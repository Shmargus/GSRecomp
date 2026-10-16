#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "field_scene.h"

namespace tla_haze {

constexpr int kRows = 32, kRowBytes = 64, kCycle = 8;

inline unsigned reg(const gsr::FieldScene& scene, int y, int at) {
    return unsigned(scene.row_io[y][at]) | (unsigned(scene.row_io[y][at + 1]) << 8);
}

// BG1's map in video memory, or npos when it is not a 32x32 map in range.
inline std::size_t map_base(const std::vector<std::uint8_t>& vram,
                            const gsr::FieldScene& scene) {
    const unsigned bg1cnt = reg(scene, 0, 0x0A);
    const std::size_t base = ((bg1cnt >> 8) & 31u) * 0x800u;
    if ((bg1cnt >> 14) != 0 || vram.size() < base + kRows * kRowBytes)
        return std::size_t(-1);
    return base;
}

// The map rows the console's screen rows from `first` on show (BG1VOFS per
// row).
inline void shown_rows(const gsr::FieldScene& scene, bool (&shown)[kRows],
                       int first = 0) {
    for (int r = 0; r < kRows; ++r) shown[r] = false;
    for (int y = first; y < gsr::FieldScene::kRows; ++y)
        if (scene.row_io_valid[y])
            shown[((reg(scene, y, 0x16) & 0x1FFu) + unsigned(y)) / 8u % kRows] = true;
}

}  // namespace tla_haze

// Yampi Desert's captured heat haze is a screen-filling live BG1 tilemap.
// Continue its wrapped map only when every captured row identifies it.
// Map 69 carries the same haze (F12 tla_rewind_0031, 2026-10-16: every row
// BG1CNT 0x0709, BLDCNT 0x3F42, decoded rows already repeat every eight).
// Air's Rock's mist (map 84) is the same kind of layer: BG1CNT 0x0709, or
// 0x0708 while the tornado blows, BLDCNT 0x3F42 on every row (F12
// tla_rewind_0024/0025, 2026-10-15; BG1 matches the room in 19-28% of tiles).
// The registers alone are not enough: Madra's town (map 44, F12
// tla_rewind_0054) has a see-through room layer with the very same BG1CNT,
// DISPCNT and BLDCNT. What sets the haze apart is its content: map row r
// equals row r + 8 on every pair of rows the console shows, in every frame
// of every haze capture (tla_rewind_0003..0055: Yampi maps 68, 69, 71, 74,
// 75, 78, Air's Rock 84) and in none of Madra's 120. A list of maps left the
// haze in a 240x160 box on 74 (0048) and 78 (0050).
inline bool tla_screen_effect_margins(gsr::FieldScene& scene,
                                      gsr::SceneMargins& margins,
                                      unsigned map,
                                      const std::vector<std::uint8_t>& vram) {
    auto& layer = scene.layers[1];
    margins.layers[1].vram_height = 0;
    if (scene.video_mode != 0 || !layer.enabled || layer.affine)
        return false;
    for (int row = 0; row < gsr::FieldScene::kRows; ++row) {
        if (!scene.row_io_valid[row]) return false;
        const auto& io = scene.row_io[row];
        auto reg = [&](int at) {
            return unsigned(io[at]) | (unsigned(io[at + 1]) << 8);
        };
        const unsigned bg1cnt = reg(0x0A);
        if ((bg1cnt != 0x0709 && !(map == 84 && bg1cnt == 0x0708)) ||
            (reg(0x00) & 0xE207) != 0x0200 || reg(0x50) != 0x3F42)
            return false;
    }
    using namespace tla_haze;
    const std::size_t base = map_base(vram, scene);
    if (base == std::size_t(-1)) return false;
    bool shown[kRows];
    shown_rows(scene, shown);
    for (int r = 0; r < kRows; ++r) {
        const int next = (r + kCycle) % kRows;
        if (shown[r] && shown[next] &&
            std::memcmp(vram.data() + base + r * kRowBytes,
                        vram.data() + base + next * kRowBytes, kRowBytes) != 0)
            return false;
    }
    margins.layers[1].vram_height = -1;
    return true;
}

// The haze and mist are one 64-pixel tile pattern: eight different map rows,
// row r the same as row r + 8 (32x32 map at 0x3800; Air's Rock map 84, F12
// tla_rewind_0030 frames 41/80/119; Yampi map 69, 0031 f119; map 75, 0036
// visible rows). The game rewrites only the rows the console screen shows as
// the layer drifts. Rows out of its view keep what they held: blank entries
// (0x5180) above Air's Rock's screen, black stripes in the margin; rows one
// step out of the cycle in the Yampi cave (0036 f119: rows 2..11 one row
// behind), seams at the old screen edge. In the copy sent to the renderer,
// every row the console does not show takes the visible row at the same
// place in the eight-row cycle.
//
// Alhafra's port water (map 98, tla_port_wave) is the same kind of layer:
// the room's BG1 is the 0x3E0..0x3E7 pattern, four map rows a cycle, and
// the game streams only the rows its camera covers. The wave lifts screen
// rows 0 and 1 by a pixel into a row it never streamed, which still holds
// the last room's tiles: a pale line along the top (F12 tla_rewind_0053:
// map row 25, tiles 0x200/0x220). Over all 720 map-98 frames (0040..0044,
// 0053) only screen rows 0 and 1 ever showed a non-water row, so for it
// `first` = 2: rows 2..159 name the streamed rows, and the rest, row 0's
// included, take the cycle.
inline bool tla_haze_fill_rows(const std::vector<std::uint8_t>& vram,
                               const gsr::FieldScene& scene,
                               std::vector<std::uint8_t>* out,
                               int first = 0) {
    using namespace tla_haze;
    const std::size_t base = map_base(vram, scene);
    if (base == std::size_t(-1)) return false;
    bool shown[kRows];
    shown_rows(scene, shown, first);
    int source[kCycle];
    for (int c = 0; c < kCycle; ++c) {
        source[c] = -1;
        for (int r = c; r < kRows && source[c] < 0; r += kCycle)
            if (shown[r]) source[c] = r;
        if (source[c] < 0) return false;
    }
    bool copied = false;
    for (int r = 0; r < kRows; ++r) {
        if (shown[r]) continue;
        const std::uint8_t* src = vram.data() + base + source[r % kCycle] * kRowBytes;
        const std::size_t at = base + r * kRowBytes;
        if (std::memcmp(vram.data() + at, src, kRowBytes) == 0) continue;
        if (!copied) { *out = vram; copied = true; }
        std::memcpy(out->data() + at, src, kRowBytes);
    }
    return copied;
}
