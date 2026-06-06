#include "pch.h"
#include "mod.h"
#include "logger.h"

#include <cameraunlock/time/qpc_clock.h>

namespace RE3HT {

using cameraunlock::TrackingMode;

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

    cameraunlock::SensitivitySettings sensitivity;
    sensitivity.yaw = m_config.yawMultiplier;
    sensitivity.pitch = m_config.pitchMultiplier;
    sensitivity.roll = m_config.rollMultiplier;
    m_session.GetProcessor().SetSensitivity(sensitivity);

    Logger::Instance().Info("Sensitivity: yaw=%.2f pitch=%.2f roll=%.2f",
                            sensitivity.yaw, sensitivity.pitch, sensitivity.roll);

    m_session.SetMode(m_config.positionEnabled ? TrackingMode::RotationAndPosition
                                               : TrackingMode::RotationOnly);
    m_session.SetStabilizationFrames(STABILIZATION_FRAME_COUNT);
    m_reticleEnabled = m_config.reticleEnabled;
    m_worldSpaceYaw = m_config.worldSpaceYaw;

    cameraunlock::PositionSettings posSettings(
        m_config.positionSensitivityX, m_config.positionSensitivityY, m_config.positionSensitivityZ,
        m_config.positionLimitX, m_config.positionLimitY, m_config.positionLimitZ, m_config.positionLimitZBack,
        m_config.positionSmoothing,
        m_config.positionInvertX, m_config.positionInvertY, m_config.positionInvertZ
    );
    m_session.GetPositionProcessor().SetSettings(posSettings);
    // The previous per-mod pipeline never engaged tracker pivot compensation
    // (it passed radians to a degrees API, zeroing the artifact). Keep that
    // tuning until pivot compensation is verified in game.
    m_session.GetPositionProcessor().SetTrackerPivotForward(0.0f);

    Logger::Instance().Info("Position: %s, sens=%.1f/%.1f/%.1f",
                            m_session.IsPositionActive() ? "6DOF" : "3DOF",
                            posSettings.sensitivity_x, posSettings.sensitivity_y, posSettings.sensitivity_z);

    // Route receiver bind/retry diagnostics to our logger. The callback runs
    // on the background retry thread; Logger forwards to REFramework's
    // thread-safe log functions.
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
    m_session.Recenter();
    m_lastFrameTickTime = 0;
    Logger::Instance().Info("View recentered");
}

void Mod::ToggleReticle() {
    m_reticleEnabled = !m_reticleEnabled;
    Logger::Instance().Info("Reticle %s", m_reticleEnabled ? "enabled" : "disabled");
}

void Mod::CycleTrackingMode() {
    switch (m_session.CycleMode()) {
        case TrackingMode::RotationAndPosition:
            Logger::Instance().Info("Tracking mode: normal (rotation + position)");
            break;
        case TrackingMode::RotationOnly:
            Logger::Instance().Info("Tracking mode: rotation only");
            break;
        case TrackingMode::PositionOnly:
            Logger::Instance().Info("Tracking mode: position only");
            break;
    }
}

void Mod::ProcessDeferredActions() {
    if (!m_initialized.load()) return;
    if (m_recenterRequested.Consume()) Recenter();
    if (m_cycleModeRequested.Consume()) CycleTrackingMode();
}

void Mod::TickFrame() {
    if (!m_initialized.load()) return;

    uint64_t now = cameraunlock::time::QpcNowMicros();
    float deltaTime = DELTA_TIME_DEFAULT;
    if (m_lastFrameTickTime > 0) {
        deltaTime = (now - m_lastFrameTickTime) / 1000000.0f;
        if (deltaTime > DELTA_TIME_MAX) deltaTime = DELTA_TIME_MAX;
        if (deltaTime < DELTA_TIME_MIN) deltaTime = DELTA_TIME_MIN;
    }
    m_lastFrameTickTime = now;
    m_lastDeltaTime = deltaTime;

    m_session.Update(deltaTime);
}

bool Mod::GetProcessedRotation(float& yaw, float& pitch, float& roll) {
    return m_session.GetRotation(yaw, pitch, roll);
}

bool Mod::GetPositionOffset(float& x, float& y, float& z) {
    return m_session.GetPositionOffset(x, y, z);
}

void Mod::ToggleYawMode() {
    bool now = !m_worldSpaceYaw.load(std::memory_order_relaxed);
    m_worldSpaceYaw.store(now, std::memory_order_relaxed);
    Logger::Instance().Info("Yaw mode: %s", now ? "world-space (horizon-locked)" : "camera-local");
}

} // namespace RE3HT
