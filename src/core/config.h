#pragma once

#include <cameraunlock/reframework/plugin_config.h>

namespace RE3HT {

using Config = cameraunlock::reframework::PluginConfig;

// The game's name as cameraunlock-core's data/games.json spells it, written at
// the top of CameraUnlock.ini.
inline constexpr const char* kGameName = "Resident Evil 3";

// RE3's INI schema: the [Position] Invert keys it has always exposed, and the
// 2x position sensitivity default - RE Engine's native head-bob range is narrow
// enough that 1:1 reads as no movement at typical tracker range. Neither is a
// setting any more: the canonical file has no sensitivity or inversion row, so
// PluginMod applies SetDefaults' values, the 2x and no inversion, whatever the
// file holds.
//
// canonicalConfig: settings live in reframework\plugins\CameraUnlock.ini, and
// HeadTracking.ini, the file every earlier build read, is imported once while
// CameraUnlock.ini is absent and never written.
inline constexpr cameraunlock::reframework::PluginConfigSchema kConfigSchema{
    /*title*/ "RE3 Head Tracking",
    /*positionInvertKeys*/ true,
    /*flashlight*/ false,
    /*diagnosticMarkerKey*/ false,
    /*positionSensitivity*/ 2.0f,
    /*modId*/ "re3",
    /*canonicalConfig*/ true,
};

} // namespace RE3HT
