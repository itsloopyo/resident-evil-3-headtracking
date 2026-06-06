#pragma once

#include "config.h"
#include <atomic>
#include <cameraunlock/input/deferred_actions.h>
#include <cameraunlock/protocol/udp_receiver.h>
#include <cameraunlock/tracking/head_tracking_session.h>

namespace RE3HT {

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

    // Hotkey callbacks fire on the HotkeyPoller's background thread, but
    // Recenter and CycleTrackingMode mutate the session's non-atomic
    // processor/interpolator smoothing state owned by the render thread. The
    // hotkey thread only requests the action; ProcessDeferredActions() runs it
    // on the render thread at the start of each frame.
    void RequestRecenter() { m_recenterRequested.Request(); }
    void RequestCycleTrackingMode() { m_cycleModeRequested.Request(); }
    void ProcessDeferredActions();

    Config& GetConfig() { return m_config; }
    const Config& GetConfig() const { return m_config; }

    // Advance interpolation + smoothing pipelines once per render frame.
    // Every in-frame consumer (camera matrix, crosshair projection, GUI
    // compensation) then reads identical cached values.
    void TickFrame();

    bool GetProcessedRotation(float& yaw, float& pitch, float& roll);
    bool GetPositionOffset(float& x, float& y, float& z);

    float GetLastDeltaTime() const { return m_lastDeltaTime; }

    bool IsWorldSpaceYaw() const { return m_worldSpaceYaw.load(std::memory_order_relaxed); }

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
    cameraunlock::HeadTrackingSession<cameraunlock::UdpReceiver> m_session{m_udpReceiver};

    // Read on the render thread, toggled on the hotkey thread.
    std::atomic<bool> m_reticleEnabled{true};
    std::atomic<bool> m_worldSpaceYaw{false};

    cameraunlock::input::DeferredAction m_recenterRequested;
    cameraunlock::input::DeferredAction m_cycleModeRequested;

    uint64_t m_lastFrameTickTime = 0;
    float m_lastDeltaTime = DELTA_TIME_DEFAULT;
};

} // namespace RE3HT
