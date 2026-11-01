#pragma once

#include <cstdint>

// The world map is map number 2 ([0x02000428]). The ending's flyover over the
// lighthouses (F12 tla_rewind_0181..0184, 2026-10-29) is map 3: the same
// world, the same BG3/BG2 rings in blocks 8/10, and its loaded pieces match
// the whole-map unpacking (world map agreement 97-100% on 23 of its 24
// mode-2 frames; the 24th is the frame the game starts its snapshot).
constexpr std::uint32_t kTlaWorldMapNumber = 2u;
constexpr std::uint32_t kTlaCreditsWorldMapNumber = 3u;

inline bool tla_world_map(unsigned map) {
    return map == kTlaWorldMapNumber || map == kTlaCreditsWorldMapNumber;
}

// A lighthouse's beam in the ending (F12 tla_rewind_0181..0184): the game
// freezes the camera and swaps the world map for its 256x160 snapshot in
// BG1 (mode 0, BG1CNT 0x0682: 256 colours; the affine registers unchanged;
// the snapshot is the last mode-2 frame's picture, 0183 f7 against f8) and
// adds BG0's glow to it (BLDCNT 0x3F41: alpha, BG0 alone the first target;
// BLDALPHA 0x100F/0x1010). The glow's colours ramp to white for the fade
// (15-16 of the 256 BG colours change, the snapshot's stay). The world
// map's quake uses the same snapshot without blending (0151: BLDCNT 0).
// The glow is one 256x128 picture with a uniform outer colour (F12 0191).
// Its border extends the fade into the margins without repeating the light.
// `io` is the register file, `map` the map number.
inline bool tla_beam_glow_frame(const std::uint8_t* io, unsigned map) {
    if (!io || !tla_world_map(map)) return false;
    auto rd = [&](unsigned at) { return unsigned(io[at] | (io[at + 1] << 8)); };
    const unsigned dispcnt = rd(0x00), bg1 = rd(0x0A), bg2 = rd(0x0C), bg3 = rd(0x0E);
    const unsigned bldcnt = rd(0x50);
    return (dispcnt & 7u) == 0u && (dispcnt & 0x0300u) == 0x0300u &&
           (bg1 & 0x80u) != 0u && ((bg2 >> 8) & 0x1Fu) == 10u &&
           ((bg3 >> 8) & 0x1Fu) == 8u && (bldcnt & 0xFFu) == 0x41u;
}
