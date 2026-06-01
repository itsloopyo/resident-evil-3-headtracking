#include "pch.h"
#include "mod.h"
#include "logger.h"

namespace RE3HT {

static uint64_t GetTimeMicros() {
    static const double freqReciprocal = []() {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        return 1000000.0 / static_cast<double>(freq.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart * freqReciprocal);
}

Mod& Mod::Instance() {
    static Mod instance;
    return instance;
}

bool Mod::Initialize() {
    if (m_initialized.load()) {
        Logger::Instance().Warning("Mod already initialized");
        return true;
    }

    Logger::Instance().Info("RE3 Head Tracking v%s initializing...", RE3HT_VERSION);

    if (!LoadConfig()) {
        Logger::Instance().Warning("Using default configuration");
    }

    // Initialize TrackingProcessor
    cameraunlock::SensitivitySettings sensitivity;
    sensitivity.yaw = m_config.yawMultiplier;
    sensitivity.pitch = m_config.pitchMultiplier;
    sensitivity.roll = m_config.rollMultiplier;
    m_processor.SetSensitivity(sensitivity);

    Logger::Instance().Info("Sensitivity: yaw=%.2f pitch=%.2f roll=%.2f",
                            sensitivity.yaw, sensitivity.pitch, sensitivity.roll);

    // Initialize position processor
    m_trackingMode = m_config.positionEnabled ? TrackingMode::Normal : TrackingMode::RotationOnly;
    m_reticleEnabled = m_config.reticleEnabled;
    m_worldSpaceYaw = m_config.worldSpaceYaw;

    cameraunlock::PositionSettings posSettings(
        m_config.positionSensitivityX, m_config.positionSensitivityY, m_config.positionSensitivityZ,
        m_config.positionLimitX, m_config.positionLimitY, m_config.positionLimitZ, m_config.positionLimitZBack,
        m_config.positionSmoothing,
        m_config.positionInvertX, m_config.positionInvertY, m_config.positionInvertZ
    );
    m_positionProcessor.SetSettings(posSettings);

    Logger::Instance().Info("Position: %s, sens=%.1f/%.1f/%.1f",
                            IsPositionEnabled() ? "6DOF" : "3DOF",
                            posSettings.sensitivity_x, posSettings.sensitivity_y, posSettings.sensitivity_z);

    // Route receiver bind/retry diagnostics to our logger.
    m_udpReceiver.SetLog([](const std::string& msg) {
        Logger::Instance().Info("UDP: %s", msg.c_str());
    });

    // Start UDP receiver. A busy port is not fatal: the receiver schedules a
    // background retry and binds once the port frees (e.g. when another RE
    // Engine head-tracking mod sharing 4242 exits). Initialization continues
    // so the camera hook and render callbacks install regardless.
    if (m_udpReceiver.Start(m_config.udpPort)) {
        Logger::Instance().Info("UDP receiver started on port %d", m_config.udpPort);
    } else {
        Logger::Instance().Warning("UDP port %d busy - retrying in background", m_config.udpPort);
    }

    if (m_config.autoEnable) {
        m_enabled.store(true);
        Logger::Instance().Info("Head tracking auto-enabled");
    }

    m_initialized.store(true);
    Logger::Instance().Info("Initialization complete");
    return true;
}

void Mod::Shutdown() {
    if (!m_initialized.load()) return;

    Logger::Instance().Info("Shutting down...");
    m_udpReceiver.Stop();
    m_initialized.store(false);
    Logger::Instance().Info("Shutdown complete");
}

bool Mod::LoadConfig() {
    // Config file is in the same directory as the DLL (reframework/plugins/)
    HMODULE hModule = nullptr;
    char dllPath[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&Mod::Instance, &hModule)) {
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);
    }

    std::string configPath;
    if (dllPath[0] != '\0') {
        configPath = dllPath;
        auto lastSlash = configPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            configPath = configPath.substr(0, lastSlash + 1);
        }
    }
    configPath += "HeadTracking.ini";

    if (!m_config.Load(configPath.c_str())) {
        m_config.SetDefaults();
        m_config.Save(configPath.c_str());
        return false;
    }
    return true;
}

void Mod::SetEnabled(bool enabled) {
    bool wasEnabled = m_enabled.exchange(enabled);
    if (wasEnabled != enabled) {
        Logger::Instance().Info("Head tracking %s", enabled ? "enabled" : "disabled");
    }
}

void Mod::Toggle() {
    SetEnabled(!m_enabled.load());
}

void Mod::Recenter() {
    m_udpReceiver.Recenter();
    m_processor.Reset();
    m_poseInterpolator.Reset();
    m_lastProcessTime = 0;

    float px, py, pz;
    if (m_udpReceiver.GetPosition(px, py, pz)) {
        cameraunlock::PositionData posCenter(px, py, pz);
        m_positionProcessor.SetCenter(posCenter);
    }
    m_positionInterpolator.Reset();

    Logger::Instance().Info("View recentered");
}

void Mod::ToggleReticle() {
    m_reticleEnabled = !m_reticleEnabled;
    Logger::Instance().Info("Reticle %s", m_reticleEnabled ? "enabled" : "disabled");
}

void Mod::CycleTrackingMode() {
    switch (m_trackingMode) {
        case TrackingMode::Normal:       m_trackingMode = TrackingMode::RotationOnly; break;
        case TrackingMode::RotationOnly: m_trackingMode = TrackingMode::PositionOnly; break;
        case TrackingMode::PositionOnly: m_trackingMode = TrackingMode::Normal;       break;
    }

    if (!IsPositionEnabled()) {
        m_positionProcessor.Reset();
        m_positionInterpolator.Reset();
    }

    const char* label =
        m_trackingMode == TrackingMode::Normal       ? "normal (rotation + position)" :
        m_trackingMode == TrackingMode::RotationOnly ? "rotation only (position off)" :
                                                       "position only (rotation off)";
    Logger::Instance().Info("Tracking mode: %s", label);
}

bool Mod::GetProcessedRotation(float& yaw, float& pitch, float& roll) {
    uint64_t now = GetTimeMicros();
    if (m_lastProcessTime > 0 && (now - m_lastProcessTime) < CACHE_VALIDITY_US) {
        yaw = m_cachedYaw;
        pitch = m_cachedPitch;
        roll = m_cachedRoll;
        return m_cachedValid;
    }

    float rawYaw, rawPitch, rawRoll;
    if (!m_udpReceiver.GetRotation(rawYaw, rawPitch, rawRoll)) {
        m_lastProcessTime = now;
        m_cachedValid = false;
        return false;
    }

    // Wait for stabilization before auto-recentering (skip noisy initial frames)
    if (!m_hasCentered) {
        m_stabilizationFrames++;
        if (m_stabilizationFrames >= STABILIZATION_FRAME_COUNT) {
            m_hasCentered = true;
            Recenter();
            Logger::Instance().Info("Auto-recentered after %d frames", m_stabilizationFrames);
        }
        // Still process data below so smoothing settles
    }

    float deltaTime = DELTA_TIME_DEFAULT;
    if (m_lastProcessTime > 0) {
        deltaTime = (now - m_lastProcessTime) / 1000000.0f;
        if (deltaTime > DELTA_TIME_MAX) deltaTime = DELTA_TIME_MAX;
        if (deltaTime < DELTA_TIME_MIN) deltaTime = DELTA_TIME_MIN;
    }
    m_lastProcessTime = now;
    m_lastDeltaTime = deltaTime;

    int64_t receiveTs = m_udpReceiver.GetLastReceiveTimestamp();
    bool isNewPacket = (receiveTs != m_lastReceiveTimestamp);
    m_lastReceiveTimestamp = receiveTs;

    bool isNewSample = isNewPacket &&
        (rawYaw != m_lastRawYaw || rawPitch != m_lastRawPitch || rawRoll != m_lastRawRoll);
    if (isNewPacket) {
        m_lastRawYaw = rawYaw;
        m_lastRawPitch = rawPitch;
        m_lastRawRoll = rawRoll;
    }

    cameraunlock::InterpolatedPose interpolated = m_poseInterpolator.Update(
        rawYaw, rawPitch, rawRoll, isNewSample, deltaTime);

    cameraunlock::TrackingPose processed = m_processor.Process(
        interpolated.yaw, interpolated.pitch, interpolated.roll, deltaTime);

    yaw = processed.yaw;
    pitch = processed.pitch;
    roll = processed.roll;

    m_cachedYaw = yaw;
    m_cachedPitch = pitch;
    m_cachedRoll = roll;
    m_cachedValid = true;

    return true;
}

bool Mod::GetPositionOffset(float& x, float& y, float& z) {
    if (!IsPositionEnabled()) {
        x = y = z = 0.0f;
        return false;
    }

    float rawX, rawY, rawZ;
    if (!m_udpReceiver.GetPosition(rawX, rawY, rawZ)) {
        x = y = z = 0.0f;
        return false;
    }

    float deltaTime = m_lastDeltaTime;
    cameraunlock::PositionData rawPos(rawX, rawY, rawZ);
    cameraunlock::PositionData interpolatedPos = m_positionInterpolator.Update(rawPos, deltaTime);

    cameraunlock::math::Quat4 headRotQ = cameraunlock::math::Quat4::FromYawPitchRoll(
        m_cachedYaw * static_cast<float>(cameraunlock::math::kDegToRad),
        m_cachedPitch * static_cast<float>(cameraunlock::math::kDegToRad),
        m_cachedRoll * static_cast<float>(cameraunlock::math::kDegToRad));

    cameraunlock::math::Vec3 offset = m_positionProcessor.Process(interpolatedPos, headRotQ, deltaTime);

    x = offset.x;
    y = offset.y;
    z = offset.z;
    return true;
}

void Mod::ToggleYawMode() {
    m_worldSpaceYaw = !m_worldSpaceYaw;
    Logger::Instance().Info("Yaw mode: %s", m_worldSpaceYaw ? "world-space (horizon-locked)" : "camera-local");
}

} // namespace RE3HT
