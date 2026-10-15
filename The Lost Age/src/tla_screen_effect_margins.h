#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "field_scene.h"

// Yampi Desert's captured heat haze is a screen-filling live BG1 tilemap.
// Continue its wrapped map only when every captured row identifies it.
// Air's Rock's mist (map 84) is the same kind of layer: BG1CNT 0x0709, or
// 0x0708 while the tornado blows, BLDCNT 0x3F42 on every row (F12
// tla_rewind_0024/0025, 2026-10-15; BG1 matches the room in 19-28% of tiles).
// No other capture has a field BG1 like this.
inline bool tla_screen_effect_margins(gsr::FieldScene& scene,
                                      gsr::SceneMargins& margins,
                                      unsigned map) {
    auto& layer = scene.layers[1];
    margins.layers[1].vram_height = 0;
    if ((map != 75 && map != 84) || scene.video_mode != 0 || !layer.enabled ||
        layer.affine)
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
    margins.layers[1].vram_height = -1;
    return true;
}

// Air's Rock's mist (map 84) is one 64-pixel tile pattern: map row r holds
// the entries of row r+8 (32x32 map at 0x3800, F12 tla_rewind_0030 frames
// 41/80/119, every filled row matched). The game fills only the rows the
// console screen will show; the rows just past its top edge hold one blank
// entry (0x5180) each, up to six rows, so the margin above showed black
// stripes. In the copy sent to the renderer, a row of one repeated entry
// takes the entries of the nearest filled row 8 rows away.
inline bool tla_mist_fill_rows(const std::vector<std::uint8_t>& vram,
                               unsigned bg1cnt,
                               std::vector<std::uint8_t>* out) {
    constexpr int kRows = 32, kRowBytes = 64;
    const std::size_t base = ((bg1cnt >> 8) & 31u) * 0x800u;
    if ((bg1cnt >> 14) != 0 || vram.size() < base + kRows * kRowBytes)
        return false;
    auto blank = [&](const std::uint8_t* row) {
        for (int i = 2; i < kRowBytes; i += 2)
            if (row[i] != row[0] || row[i + 1] != row[1]) return false;
        return true;
    };
    bool copied = false;
    for (int r = 0; r < kRows; ++r) {
        if (!blank(vram.data() + base + r * kRowBytes)) continue;
        for (int k = 1; k < 4; ++k) {
            const int from[2] = {(r + 8 * k) % kRows, (r - 8 * k + 4 * kRows) % kRows};
            const std::uint8_t* src = nullptr;
            for (int f : from)
                if (!blank(vram.data() + base + f * kRowBytes)) {
                    src = vram.data() + base + f * kRowBytes;
                    break;
                }
            if (!src) continue;
            if (!copied) { *out = vram; copied = true; }
            std::memcpy(out->data() + base + r * kRowBytes, src, kRowBytes);
            break;
        }
    }
    return copied;
}
