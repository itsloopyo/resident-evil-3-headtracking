#include "pch.h"

#include <reframework/API.hpp>

#include "core/mod.h"
#include "core/logger.h"
#include "camera/camera_hook.h"

#include <cameraunlock/input/hotkey_poller.h>
#include <cameraunlock/input/chord_hotkeys.h>
#include <cameraunlock/reframework/log_callback.h>

using cameraunlock::input::NavGuarded;
using cameraunlock::input::ChordGuarded;

static cameraunlock::input::HotkeyPoller g_hotkeyPoller;

// --- REFramework application entry callbacks ---

static void OnPreBeginRendering() {
    RE3HT::OnPreBeginRendering();
}

static void OnPostBeginRendering() {
    RE3HT::OnPostBeginRendering();
}

static bool OnPreGuiDrawElement(void* element, void* context) {
    return RE3HT::OnPreGuiDrawElement(element, context);
}

// --- REFramework plugin exports ---

extern "C" __declspec(dllexport)
void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    version->major = REFRAMEWORK_PLUGIN_VERSION_MAJOR;
    version->minor = REFRAMEWORK_PLUGIN_VERSION_MINOR;
    version->patch = REFRAMEWORK_PLUGIN_VERSION_PATCH;
    version->game_name = nullptr;
}

extern "C" __declspec(dllexport)
bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param) return false;

    // Initialize REFramework SDK wrapper
    reframework::API::initialize(param);

    // Set up logging via REFramework's log functions
    RE3HT::Logger::Instance().SetREFunctions(
        param->functions->log_info,
        param->functions->log_warn,
        param->functions->log_error
    );

    // Bridge shared library logging to REFramework's log functions
    cameraunlock::reframework::SetLogCallback([](cameraunlock::reframework::LogLevel level, const char* msg) {
        switch (level) {
            case cameraunlock::reframework::LogLevel::Warning:
                RE3HT::Logger::Instance().Warning("%s", msg); break;
            case cameraunlock::reframework::LogLevel::Error:
                RE3HT::Logger::Instance().Error("%s", msg); break;
            default:
                RE3HT::Logger::Instance().Info("%s", msg); break;
        }
    });

    RE3HT::Logger::Instance().Info("RE3 Head Tracking v%s - Plugin loaded", RE3HT::RE3HT_VERSION);

    // Initialize mod (tracking pipeline, UDP receiver)
    if (!RE3HT::Mod::Instance().Initialize()) {
        RE3HT::Logger::Instance().Error("Mod initialization failed");
        return false;
    }

    // Register REFramework callbacks
    param->functions->on_pre_application_entry("BeginRendering", OnPreBeginRendering);
    param->functions->on_post_application_entry("BeginRendering", OnPostBeginRendering);
    param->functions->on_pre_gui_draw_element(OnPreGuiDrawElement);

    // Set up hotkeys
    auto& config = RE3HT::Mod::Instance().GetConfig();

    // Nav-cluster bindings. Suppressed when Ctrl+Shift is held so the chord
    // path (below) is the sole trigger for Ctrl+Shift+<nav> combos.
    // Recenter and mode-cycle mutate render-thread-owned session state, so the
    // hotkey thread only requests them; the render frame runs them.
    g_hotkeyPoller.SetToggleKey(config.toggleKey, NavGuarded([] { RE3HT::Mod::Instance().Toggle(); }));
    g_hotkeyPoller.SetRecenterKey(config.recenterKey, NavGuarded([] { RE3HT::Mod::Instance().RequestRecenter(); }));
    g_hotkeyPoller.AddHotkey(config.positionToggleKey, NavGuarded([] { RE3HT::Mod::Instance().RequestCycleTrackingMode(); }));
    g_hotkeyPoller.AddHotkey(config.reticleToggleKey, NavGuarded([] { RE3HT::Mod::Instance().ToggleReticle(); }));
    g_hotkeyPoller.AddHotkey(config.yawModeKey, NavGuarded([] { RE3HT::Mod::Instance().ToggleYawMode(); }));

    // Ctrl+Shift+<letter> chord bindings (CLAUDE.md T/Y/U/G/H/J cluster).
    g_hotkeyPoller.AddHotkey('T', ChordGuarded([] { RE3HT::Mod::Instance().RequestRecenter(); }));
    g_hotkeyPoller.AddHotkey('Y', ChordGuarded([] { RE3HT::Mod::Instance().Toggle(); }));
    g_hotkeyPoller.AddHotkey('G', ChordGuarded([] { RE3HT::Mod::Instance().RequestCycleTrackingMode(); }));
    g_hotkeyPoller.AddHotkey('H', ChordGuarded([] { RE3HT::Mod::Instance().ToggleYawMode(); }));
    g_hotkeyPoller.AddHotkey('U', ChordGuarded([] { RE3HT::Mod::Instance().ToggleReticle(); }));
    g_hotkeyPoller.Start();

    RE3HT::Logger::Instance().Info("Plugin initialization complete");
    return true;
}
