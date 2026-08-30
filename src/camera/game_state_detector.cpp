#include "pch.h"
#include "game_state_detector.h"

#include <cameraunlock/reframework/gameplay_gate.h>
#include <cameraunlock/reframework/log_callback.h>
#include <cameraunlock/reframework/managed_utils.h>

#include <reframework/API.hpp>

namespace RE3HT {

namespace ref = cameraunlock::reframework;

// RE3 (offline.*) game-state signals, verified against the live TDB:
//   PlayerManager.get_CurrentPlayer()                      null => menu / loading
//   <player>.getComponent(SurvivorCondition).get_IsEvent() true => cutscene
//   GUIMaster.get_IsOpenPause()                            true => pause / inventory
//
// Named outright rather than probed. The generic manager probing the RE7/RE8/
// Requiem detectors use binds whichever candidate resolves first, which is the
// right trade only where nothing has been confirmed; here these have been, and
// swapping them for a probe would trade a verified binding for a guess.
static constexpr const char* kPlayerManager = "offline.PlayerManager";
static constexpr const char* kSurvivorCondition = "offline.survivor.SurvivorCondition";
static constexpr const char* kGuiMaster = "offline.gui.GUIMaster";

static struct {
    reframework::API::Method* getCurrentPlayer = nullptr;
    reframework::API::Method* getComponent = nullptr;
    void* survivorConditionType = nullptr;  // System.Type for getComponent
    reframework::API::Method* getIsEvent = nullptr;
    reframework::API::Method* getIsOpenPause = nullptr;
    bool available = false;
} g_checks;

static void Discover() {
    auto tdb = reframework::API::get()->tdb();

    auto pmType = tdb->find_type(kPlayerManager);
    if (pmType) g_checks.getCurrentPlayer = pmType->find_method("get_CurrentPlayer");
    auto goType = tdb->find_type("via.GameObject");
    if (goType) g_checks.getComponent = goType->find_method("getComponent");
    auto condType = tdb->find_type(kSurvivorCondition);
    if (condType) {
        g_checks.getIsEvent = condType->find_method("get_IsEvent");
        g_checks.survivorConditionType = condType->get_runtime_type();
    }
    auto guiType = tdb->find_type(kGuiMaster);
    if (guiType) g_checks.getIsOpenPause = guiType->find_method("get_IsOpenPause");

    g_checks.available =
        g_checks.getCurrentPlayer && g_checks.getComponent &&
        g_checks.survivorConditionType && g_checks.getIsEvent && g_checks.getIsOpenPause;

    ref::LogInfo("Game state detection %s: player=%p, getComponent=%p, condType=%p, isEvent=%p, pause=%p",
        g_checks.available ? "ready" : "unavailable",
        g_checks.getCurrentPlayer, g_checks.getComponent,
        g_checks.survivorConditionType, g_checks.getIsEvent, g_checks.getIsOpenPause);
}

// The managed calls, guarded together. A probe that faults reports no
// suppression rather than a state: the game is running, this detector is not,
// and dropping tracking on the detector's own failure would be the worse of the
// two errors.
static const char* SuppressReason(const reframework::API* api) {
    __try {
        auto pmgr = api->get_managed_singleton(kPlayerManager);
        if (!pmgr) return "no PlayerManager";

        auto player = ref::CallMethod(g_checks.getCurrentPlayer, pmgr);
        if (!player) return "no player (menu/loading)";

        auto condition = ref::CallMethodArg(g_checks.getComponent, player, g_checks.survivorConditionType);
        if (condition && ref::CallMethodBool(g_checks.getIsEvent, condition)) {
            return "cutscene";
        }

        auto gui = api->get_managed_singleton(kGuiMaster);
        if (gui && ref::CallMethodBool(g_checks.getIsOpenPause, gui)) {
            return "paused";
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

static bool Check(void* primaryCamera, bool diag, const char** reason) {
    (void)primaryCamera;
    (void)diag;
    if (!g_checks.available) return true;

    const char* suppress = SuppressReason(reframework::API::get().get());
    if (!suppress) return true;
    *reason = suppress;
    return false;
}

static ref::GameplayGate g_gate{&Discover, &Check};

ref::GameplayGate* GameplayGateInstance() { return &g_gate; }

bool IsInGameplay() { return g_gate.IsInGameplay(); }

} // namespace RE3HT
