// Golden Sun's narrow, evidence-backed expanded-view policy.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gsr::widescreen {

// The 360x240 expanded option is deliberately bounded here even though the
// generic PPU can support a larger horizontal envelope.  These constants are
// presentation policy, not guest camera coordinates: the atlas provider may
// read only the measured 60px/40px margins around the authentic canvas.
inline constexpr std::int32_t kExpandedExtraX = 60;

// Golden Sun hides an unused sprite by moving it off the visible screen
// rather than clearing its enable bit, and it parks every one of them at a
// SINGLE position: raw `attr1 & 0x1FF == 192`, `attr0 & 0xFF == 192`, which
// resolves to (192, -64) -- just above the screen. Measured 2026-09-11 over
// logs/objrec_20260911_101026: 19,264 of 22,528 published OAM slots across 180
// field frames sit on exactly that pair, 85.5% of the table and about 107 of
// the 128 live entries every frame. It is one sentinel, not a spread, which is
// what makes an exact test safe.
//
// This matters only for the expanded view. -64 is off-screen on hardware, but
// it is INSIDE a 40-row top margin, so without this test the parked dummies
// rain into the widened picture.
inline constexpr unsigned kGoldenSunObjParkRawX = 192u;
inline constexpr unsigned kGoldenSunObjParkRawY = 192u;

inline constexpr bool golden_sun_obj_is_parked(std::uint16_t attr0,
                                               std::uint16_t attr1) {
    return (attr1 & 0x01FFu) == kGoldenSunObjParkRawX &&
           (attr0 & 0x00FFu) == kGoldenSunObjParkRawY;
}

}  // namespace gsr::widescreen
