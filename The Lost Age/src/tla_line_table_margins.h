#pragma once

#include <cstddef>
#include <cstdint>

#include "field_scene.h"

// Jupiter Lighthouse's cloud wave (F12 tla_rewind_0171, map 290): ROM
// 0x080D607C builds the table the DMA below reads. Its state is at B =
// [0x03000088]: byte +0xF00 the shown buffer (two of 160 x 12 bytes from B;
// the VBlank routine 0x080D5FF8 points DMA0 at it), byte +0xF01 0 for BG1..BG3
// or else BG3 alone, u16 +0xF02 a counter it raises per build; per axis
// (HOFS, VOFS) a multiplier +0xF08/+0xF0C, a per-row step +0xF10/+0xF14 and a
// strength +0xF18/+0xF1C. Row r = base + offset(acc), acc = multiplier *
// (counter + u16 [0x03001126]) + r * step; offset = (sine * strength) >> 16
// (the IWRAM smull 0x03000220), +255 when negative, bits 8..23; sine the s16
// ROM table at 0x080F08B0 (256 entries) at (acc >> 16) & 0xFF. Step 0 writes
// the base. Shown tables used the counter 2 before the saved one (0167, 0171
// all frames), or 1 or 0 (0168: 38 and 1 of 120 frames); every row 1..159
// matched on all frames of 0167, 0168, 0171. Rows above and below the
// console continue the same rows; a table that does not reproduce every row
// is left to the row-1 rule below.
inline bool tla_cloud_wave(const gsr::FieldScene& scene, gsr::SceneMargins& margins,
                           const std::uint8_t* io, const std::uint8_t* ewram,
                           std::size_t ewram_bytes, const std::uint8_t* iwram,
                           std::size_t iwram_bytes, const std::uint8_t* rom,
                           std::size_t rom_bytes, int first_y, int rows) {
    auto le = [](const std::uint8_t* p, int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= std::uint32_t(p[i]) << (8 * i);
        return v;
    };
    if (!io || !ewram || !iwram || iwram_bytes < 0x1128u || !rom ||
        rom_bytes < 0xF08B0u + 512u || rows <= 0)
        return false;
    const std::uint32_t b = le(iwram + 0x88, 4);
    const std::uint32_t state_at = b & 0x00FFFFFFu, sad = le(io + 0xB0, 4);
    if ((b >> 24) != 0x02u || state_at + 0xF20u > ewram_bytes ||
        sad < b || sad >= b + 0xF00u)
        return false;
    const std::uint8_t* s = ewram + state_at;
    const std::uint32_t counter = le(s + 0xF02, 2), camera = le(iwram + 0x1126, 2);
    auto offset = [&](std::uint32_t acc, std::uint32_t strength) {
        const auto* p = rom + 0xF08B0u + ((acc >> 16) & 0xFFu) * 2u;
        const std::int64_t sine = std::int16_t(le(p, 2));
        std::int32_t m = std::int32_t(std::uint32_t(
            (sine * std::int32_t(strength)) >> 16));
        if (m < 0) m += 255;
        return std::uint16_t(std::uint32_t(m) >> 8);
    };
    auto reg = [&](int row, unsigned at) {
        return std::uint16_t(scene.row_io[row][at] | (scene.row_io[row][at + 1] << 8));
    };
    for (int row = 1; row < gsr::FieldScene::kRows; ++row)
        if (!scene.row_io_valid[row]) return false;
    const int first_bg = s[0xF01] ? 3 : 1;
    for (std::uint32_t back : {2u, 1u, 0u}) {
        const std::uint32_t phase = (counter - back) & 0xFFFFu;
        // Per axis: acc at row 0, step, strength.
        std::uint32_t acc0[2], step[2], strength[2];
        for (int axis = 0; axis < 2; ++axis) {
            acc0[axis] = le(s + 0xF08 + 4 * axis, 4) * (phase + camera);
            step[axis] = le(s + 0xF10 + 4 * axis, 4);
            strength[axis] = le(s + 0xF18 + 4 * axis, 4);
        }
        auto value = [&](int row, int axis, std::uint16_t base) {
            if (!step[axis]) return base;
            return std::uint16_t(base + offset(acc0[axis] +
                std::uint32_t(row) * step[axis], strength[axis]));
        };
        bool match = true;
        std::uint16_t base[4][2] = {};
        for (int bg = first_bg; bg < 4 && match; ++bg)
            for (int axis = 0; axis < 2 && match; ++axis) {
                const unsigned ofs = 0x10u + unsigned(bg) * 4u + unsigned(axis) * 2u;
                base[bg][axis] = std::uint16_t(reg(1, ofs) - value(1, axis, 0));
                for (int row = 2; row < gsr::FieldScene::kRows && match; ++row)
                    match = value(row, axis, base[bg][axis]) == reg(row, ofs);
            }
        if (!match) continue;
        bool any = false;
        for (int bg = first_bg; bg < 4; ++bg) {
            const auto& layer = scene.layers[bg];
            auto& margin = margins.layers[bg];
            if (!layer.enabled || layer.affine || margin.vram_height != 0 ||
                !margin.scroll.empty())
                continue;
            margin.scroll_sides = true;  // native pixels keep their own rows
            margin.scroll_first_y = first_y;
            margin.scroll.resize(std::size_t(rows));
            for (int row = 0; row < rows; ++row)
                margin.scroll[row] = {value(first_y + row, 0, base[bg][0]),
                                      value(first_y + row, 1, base[bg][1])};
            any = true;
        }
        return any;
    }
    return false;
}

// A layer whose scroll an HBlank DMA writes from a per-line table: the
// transfer at the end of line N loads line N+1 (runtime_bus_bridge.cpp), so
// rows 1..159 are the table's and row 0 is whatever the game left in the
// registers over VBlank. Jupiter Lighthouse map 290 (F12 tla_rewind_0167,
// 2026-10-28): DMA0 writes BG1..BG3 scroll (dest 0x04000014, 3 words,
// CNT_H 0xA660) from 0x0203528C for its BG3 cloud wave; the table's row-0
// entry 0x02035280 holds BG1 0x148,0x478, but row 0 carries the wave's
// 0xEC,0x47 in BG1 on every saved frame. Row 0 elsewhere equals that entry
// (0015, 0025, 0133). The top margin held row 0's scroll and read BG1 from
// the clouds' part of the map (grid y < 512) with BG1's tiles: a band of
// pale "banners" above the view. Rows above the console continue row 1, the
// table's first row, and so does row 0 beside the console: with only the
// top fixed, row 0's side margins kept the stale scroll, a one-pixel line
// across the margins at the console's top edge (F12 tla_rewind_0168). Margin
// pixels only; layers another rule already gave scroll rows are left alone.
// `io` is the full register file (0x400 bytes).
inline void tla_line_table_margins(const gsr::FieldScene& scene,
                                   gsr::SceneMargins& margins,
                                   const std::uint8_t* io, int first_y) {
    if (!io || !scene.row_io_valid[1]) return;
    const int first = first_y < 0 ? first_y : 0;  // through row 0's sides
    auto io16 = [&](unsigned at) {
        return unsigned(io[at]) | (unsigned(io[at + 1]) << 8);
    };
    // IO offsets each repeating HBlank channel writes, [lo, hi).
    unsigned lo[4] = {}, hi[4] = {};
    for (unsigned ch = 0; ch < 4; ++ch) {
        const unsigned base = 0xB0u + ch * 12u;
        const unsigned cnt_h = io16(base + 10u);
        if (!(cnt_h & 0x8000u) || ((cnt_h >> 12) & 3u) != 2u || !(cnt_h & 0x0200u))
            continue;
        const std::uint32_t dad = io16(base + 4u) | (std::uint32_t(io16(base + 6u)) << 16);
        if ((dad >> 24) != 0x04u) continue;
        const unsigned unit = (cnt_h & 0x0400u) ? 4u : 2u;
        const unsigned count = io16(base + 8u) ? io16(base + 8u) : (ch == 3 ? 0x10000u : 0x4000u);
        const unsigned dest_ctrl = (cnt_h >> 5) & 3u;
        const unsigned at = dad & 0x3FFu;
        const unsigned span = (dest_ctrl == 2u ? 1u : count) * unit;  // 2: fixed
        lo[ch] = dest_ctrl == 1u ? (at + unit > span ? at + unit - span : 0u) : at;
        hi[ch] = dest_ctrl == 1u ? at + unit : at + span;           // 1: decrement
    }
    auto row1 = [&](unsigned at) {
        return std::uint16_t(scene.row_io[1][at] | (scene.row_io[1][at + 1] << 8));
    };
    for (int bg = 0; bg < 4; ++bg) {
        const auto& layer = scene.layers[bg];
        auto& margin = margins.layers[bg];
        if (!layer.enabled || layer.affine || margin.vram_height != 0 ||
            !margin.scroll.empty())
            continue;
        const unsigned hofs = 0x10u + unsigned(bg) * 4u;
        bool written = false;
        for (int ch = 0; ch < 4 && !written; ++ch)
            written = lo[ch] < hofs + 4u && hi[ch] > hofs;
        if (!written) continue;
        margin.scroll_sides = true;  // native pixels keep their own rows
        margin.scroll_first_y = first;
        margin.scroll.assign(std::size_t(1 - first),
                             {row1(hofs), row1(hofs + 2u)});
    }
}
