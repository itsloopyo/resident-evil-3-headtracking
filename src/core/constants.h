#pragma once

#include <cstdint>

namespace RE3HT {

inline constexpr const char* RE3HT_VERSION = "0.0.0";

inline constexpr uint16_t DEFAULT_UDP_PORT = 4242;

inline constexpr int DEFAULT_TOGGLE_KEY = 0x23;           // VK_END
inline constexpr int DEFAULT_RECENTER_KEY = 0x24;          // VK_HOME
inline constexpr int DEFAULT_POSITION_TOGGLE_KEY = 0x21;   // VK_PRIOR (Page Up)
inline constexpr int DEFAULT_RETICLE_TOGGLE_KEY = 0x2D;    // VK_INSERT
inline constexpr int DEFAULT_YAW_MODE_KEY = 0x22;          // VK_NEXT (Page Down)

// Tracking pipeline
inline constexpr int STABILIZATION_FRAME_COUNT = 30;       // Frames before auto-recenter (~0.5s at 60fps)
inline constexpr float DELTA_TIME_MAX = 0.1f;               // Clamp for frame spikes
inline constexpr float DELTA_TIME_MIN = 0.0001f;            // Clamp for near-zero deltas
inline constexpr float DELTA_TIME_DEFAULT = 0.016f;         // ~60fps fallback

} // namespace RE3HT
