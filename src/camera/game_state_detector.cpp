#include "pch.h"
#include "game_state_detector.h"
#include "ref_utils.h"
#include "core/logger.h"

#include <reframework/API.hpp>

namespace RE3HT {

// RE3 (offline.*) game-state signals, confirmed at runtime:
//   PlayerManager.get_CurrentPlayer()             null => menu / loading
//   <player>.getComponent(SurvivorCondition).get_IsEvent() => cutscene
//   GUIMaster.get_IsOpenPause()                   true => pause / inventory
static constexpr const char* kPlayerManager = "offline.PlayerManager";
static constexpr const char* kSurvivorCondition = "offline.survivor.SurvivorCondition";
static constexpr const char* kGuiMaster = "offline.gui.GUIMaster";

static struct {
    bool inGameplay = false;
    uint64_t lastCheckTime = 0;
    static constexpr uint64_t CHECK_INTERVAL_MS = 100;

    bool typesInitialized = false;

    // Tier 1: camera existence
    reframework::API::Method* getMainView = nullptr;
    reframework::API::Method* getPrimaryCamera = nullptr;

    // Tier 2: gameplay-vs-not signals
    reframework::API::Method* getCurrentPlayer = nullptr;
    reframework::API::Method* getComponent = nullptr;
    void* survivorConditionType = nullptr;  // System.Type for getComponent
    reframework::API::Method* getIsEvent = nullptr;
    reframework::API::Method* getIsOpenPause = nullptr;
    bool stateMethodsAvailable = false;

    bool wasInGameplay = false;
    bool pendingRecenter = false;
} g_state;

void RefreshGameState() {
    uint64_t now = GetTickCount64();
    if (now - g_state.lastCheckTime < g_state.CHECK_INTERVAL_MS) return;
    g_state.lastCheckTime = now;

    const auto& api = reframework::API::get();
    if (!api) {
        g_state.inGameplay = false;
        return;
    }

    if (!g_state.typesInitialized) {
        g_state.typesInitialized = true;
        auto tdb = api->tdb();

        auto smType = tdb->find_type("via.SceneManager");
        if (smType) g_state.getMainView = smType->find_method("get_MainView");
        auto svType = tdb->find_type("via.SceneView");
        if (svType) g_state.getPrimaryCamera = svType->find_method("get_PrimaryCamera");

        auto pmType = tdb->find_type(kPlayerManager);
        if (pmType) g_state.getCurrentPlayer = pmType->find_method("get_CurrentPlayer");
        auto goType = tdb->find_type("via.GameObject");
        if (goType) g_state.getComponent = goType->find_method("getComponent");
        auto condType = tdb->find_type(kSurvivorCondition);
        if (condType) {
            g_state.getIsEvent = condType->find_method("get_IsEvent");
            g_state.survivorConditionType = condType->get_runtime_type();
        }
        auto guiType = tdb->find_type(kGuiMaster);
        if (guiType) g_state.getIsOpenPause = guiType->find_method("get_IsOpenPause");

        g_state.stateMethodsAvailable =
            g_state.getCurrentPlayer && g_state.getComponent &&
            g_state.survivorConditionType && g_state.getIsEvent && g_state.getIsOpenPause;

        Logger::Instance().Info("Game state detection %s: player=%p, getComponent=%p, condType=%p, isEvent=%p, pause=%p",
            g_state.stateMethodsAvailable ? "ready" : "unavailable",
            g_state.getCurrentPlayer, g_state.getComponent,
            g_state.survivorConditionType, g_state.getIsEvent, g_state.getIsOpenPause);
    }

    bool newState = false;
    const char* suppressReason = nullptr;

    do {
        // Tier 1: camera must exist
        if (!g_state.getMainView || !g_state.getPrimaryCamera) break;

        auto sceneManager = api->get_native_singleton("via.SceneManager");
        if (!sceneManager) break;
        auto mainView = g_state.getMainView->call<void*>(api->get_vm_context(), sceneManager);
        if (!mainView) break;
        auto camera = g_state.getPrimaryCamera->call<void*>(api->get_vm_context(), mainView);
        if (!camera) break;

        // Tier 2: suppress outside active gameplay
        if (g_state.stateMethodsAvailable) {
            __try {
                auto pmgr = api->get_managed_singleton(kPlayerManager);
                if (!pmgr) { suppressReason = "no PlayerManager"; __leave; }

                auto player = InvokePtr(g_state.getCurrentPlayer, pmgr);
                if (!player) { suppressReason = "no player (menu/loading)"; __leave; }

                auto condition = InvokePtrArg(g_state.getComponent, player, g_state.survivorConditionType);
                if (condition && InvokeBool(g_state.getIsEvent, condition)) {
                    suppressReason = "cutscene";
                    __leave;
                }

                auto gui = api->get_managed_singleton(kGuiMaster);
                if (gui && InvokeBool(g_state.getIsOpenPause, gui)) {
                    suppressReason = "paused";
                    __leave;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                suppressReason = nullptr;
            }

            if (suppressReason) break;
        }

        newState = true;
    } while (false);

    g_state.inGameplay = newState;

    if (g_state.inGameplay && !g_state.wasInGameplay) {
        g_state.pendingRecenter = true;
        Logger::Instance().Info("Game state: entered gameplay - pending recenter");
    } else if (!g_state.inGameplay && g_state.wasInGameplay) {
        Logger::Instance().Info("Game state: left gameplay (%s)",
            suppressReason ? suppressReason : "no camera");
    }
    g_state.wasInGameplay = g_state.inGameplay;
}

bool IsInGameplay() {
    RefreshGameState();
    return g_state.inGameplay;
}

bool ShouldRecenter() {
    if (g_state.pendingRecenter) {
        g_state.pendingRecenter = false;
        return true;
    }
    return false;
}

} // namespace RE3HT
