#pragma once

#include <cstdint>

namespace RE3HT {

inline constexpr const char* RE3HT_VERSION = "0.0.0";

inline constexpr uint16_t DEFAULT_UDP_PORT = 4242;

inline constexpr int DEFAULT_TOGGLE_KEY = 0x23;           // VK_END
inline constexpr int DEFAULT_POSITION_TOGGLE_KEY = 0x21;   // VK_PRIOR (Page Up)
inline constexpr int DEFAULT_YAW_MODE_KEY = 0x22;          // VK_NEXT (Page Down)

// Seed value for the first frame, before FrameClock has an interval to report.
inline constexpr float DELTA_TIME_DEFAULT = 0.016f;         // ~60fps fallback

} // namespace RE3HT
