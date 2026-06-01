#pragma once

#include "constants.h"
#include "config.h"
#include <cameraunlock/protocol/udp_receiver.h>
#include <cameraunlock/processing/tracking_processor.h>
#include <cameraunlock/processing/pose_interpolator.h>
#include <cameraunlock/processing/position_processor.h>
#include <cameraunlock/processing/position_interpolator.h>

namespace RE3HT {

// Page Up / Ctrl+Shift+G cycles through these in order, wrapping back to Normal.
enum class TrackingMode {
    Normal,        // rotation + position
    RotationOnly,  // position disabled
    PositionOnly   // rotation disabled
};

class Mod {
public:
    static Mod& Instance();

    bool Initialize();
    void Shutdown();

    bool IsEnabled() const { return m_enabled.load(); }
    void SetEnabled(bool enabled);
    void Toggle();

    void Recenter();
    void CycleTrackingMode();
    void ToggleReticle();
    void ToggleYawMode();

    Config& GetConfig() { return m_config; }
    const Config& GetConfig() const { return m_config; }

    bool GetProcessedRotation(float& yaw, float& pitch, float& roll);
    bool GetPositionOffset(float& x, float& y, float& z);

    float GetLastDeltaTime() const { return m_lastDeltaTime; }

    bool IsReticleEnabled() const { return m_reticleEnabled; }
    bool IsPositionEnabled() const { return m_trackingMode != TrackingMode::RotationOnly; }
    bool IsRotationEnabled() const { return m_trackingMode != TrackingMode::PositionOnly; }
    bool IsWorldSpaceYaw() const { return m_worldSpaceYaw; }

    Mod(const Mod&) = delete;
    Mod& operator=(const Mod&) = delete;

private:
    Mod() = default;
    ~Mod() = default;

    bool LoadConfig();

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_initialized{false};

    Config m_config;
    cameraunlock::UdpReceiver m_udpReceiver;
    cameraunlock::PoseInterpolator m_poseInterpolator;
    cameraunlock::TrackingProcessor m_processor;
    int64_t m_lastReceiveTimestamp = 0;

    cameraunlock::PositionProcessor m_positionProcessor;
    cameraunlock::PositionInterpolator m_positionInterpolator;
    TrackingMode m_trackingMode = TrackingMode::Normal;
    bool m_reticleEnabled = true;
    bool m_worldSpaceYaw = true;

    uint64_t m_lastProcessTime = 0;
    float m_lastDeltaTime = DELTA_TIME_DEFAULT;

    float m_cachedYaw = 0.0f;
    float m_cachedPitch = 0.0f;
    float m_cachedRoll = 0.0f;
    bool m_cachedValid = false;
    bool m_hasCentered = false;
    int m_stabilizationFrames = 0;

    // Previous raw values for new-sample detection (data change, not just packet arrival)
    float m_lastRawYaw = 0.0f;
    float m_lastRawPitch = 0.0f;
    float m_lastRawRoll = 0.0f;
};

} // namespace RE3HT
