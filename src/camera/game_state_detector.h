#pragma once

namespace RE3HT {

// Returns true if the player is in active gameplay (not paused, menu, loading, etc.)
bool IsInGameplay();

// Call periodically to refresh cached game state
void RefreshGameState();

// Returns true once after transitioning from non-gameplay to gameplay (for auto-recenter)
bool ShouldRecenter();

} // namespace RE3HT
