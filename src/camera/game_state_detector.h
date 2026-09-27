#pragma once

namespace cameraunlock::reframework { class GameplayGate; }

namespace RE3HT {

// The gate the camera pipeline consults before writing the camera.
cameraunlock::reframework::GameplayGate* GameplayGateInstance();

// True while the player is in active gameplay (not paused, in a menu, loading,
// or in a cutscene).
bool IsInGameplay();

// True while the game's aim camera is up (offline.camera.CameraSystem
// get_IsHoldWeaponCamera), polled once per gameplay frame. False on any frame it
// cannot read.
bool IsAiming();

} // namespace RE3HT
