#pragma once

// scene_repeat.h -- continue a repeating background layer into the margins.
//
// One game-independent rule for effect layers (heat haze, mist, sunlight)
// instead of one rule per map. The evidence is the console's own picture:
// every map row the 160 screen rows show must equal the row `period` rows
// further down, moved `shift` columns, over the whole width of the map, and
// the shown rows must hold the pattern at least twice. The margins then copy
// the nearest shown period, so no row the game did not stream is ever drawn.
//
// Measured over every saved capture (local/scene-repeat-20261020/
// FINDINGS.md, tools/scene_repeat_bench.cpp): it finds Yampi's haze and Air's
// Rock's mist (8 map rows), Champa's sunlight (4 rows, 2 columns sideways)
// on every frame the per-map rules it replaces did (archived in
// Archive/2026-10-20-per-map-margin-rules). Typical cost a few microseconds
// per layer.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "field_scene.h"

namespace gsr {

struct LayerRepeat {
    bool found = false;
    int period = 0;   // pixels down
    int shift = 0;    // pixels the next copy moves right
};

namespace scene_repeat_detail {

inline unsigned row_reg(const FieldScene& scene, int y, int at) {
    return unsigned(scene.row_io[y][at]) | (unsigned(scene.row_io[y][at + 1]) << 8);
}

}  // namespace scene_repeat_detail

// Layer `bg`'s repeat, from its tile map in `vram`. Only a regular layer
// shown on every row with one map, one vertical scroll and its own
// per-row horizontal scroll (a sideways sway is allowed: Air's Rock's mist).
inline LayerRepeat find_layer_repeat(const FieldScene& scene,
                                     const std::uint8_t* vram,
                                     std::size_t vram_bytes, int bg) {
    using scene_repeat_detail::row_reg;
    LayerRepeat out;
    if (!vram || bg < 0 || bg > 3 || scene.video_mode != 0 ||
        scene.layers[bg].affine)
        return out;
    const int cnt_at = 0x08 + bg * 2, vofs_at = 0x12 + bg * 4;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        if (!scene.row_io_valid[y] || !(row_reg(scene, y, 0x00) & (0x100u << bg)) ||
            row_reg(scene, y, cnt_at) != row_reg(scene, 0, cnt_at) ||
            row_reg(scene, y, vofs_at) != row_reg(scene, 0, vofs_at))
            return out;
    }
    const unsigned cnt = row_reg(scene, 0, cnt_at);
    const int width = (cnt & 0x4000u) ? 64 : 32;   // tiles
    const int height = (cnt & 0x8000u) ? 64 : 32;
    const std::size_t base = std::size_t((cnt >> 8) & 31u) * 0x800u;
    if (base + std::size_t(width) * height * 2u > vram_bytes) return out;
    auto entry = [&](int row, int col) {
        row &= height - 1;
        col &= width - 1;
        const int block = (col >> 5) + ((row >> 5) * (width >> 5));
        const std::size_t at = base + std::size_t(block) * 0x800u +
                               std::size_t((row & 31) * 32 + (col & 31)) * 2u;
        return unsigned(vram[at]) | (unsigned(vram[at + 1]) << 8);
    };
    // The map rows the screen shows, top to bottom.
    const int vofs = int(row_reg(scene, 0, vofs_at) & 511u);
    const int first = vofs >> 3;
    const int shown = ((vofs & 7) + FieldScene::kRows - 1) / 8 + 1;
    bool varied = false;
    for (int r = 0; r < shown && !varied; ++r)
        for (int c = 0; c < width && !varied; ++c)
            varied = entry(first + r, c) != entry(first, 0);
    if (!varied) return out;   // one tile everywhere: nothing to continue
    // Smallest period first, then the smallest sideways move; a move of more
    // than one period (flatter than 45 degrees) is not looked for.
    for (int period = 1; period * 2 <= shown; ++period) {
        for (int s = 0; s <= period * 2; ++s) {
            const int shift = (s & 1) ? -((s + 1) / 2) : s / 2;
            bool same = true;
            for (int r = 0; r + period < shown && same; ++r)
                for (int c = 0; c < width && same; ++c)
                    same = entry(first + r + period, c + shift) == entry(first + r, c);
            if (same) return {true, period * 8, shift * 8};
        }
    }
    return out;
}

// Margin rows for a found repeat: each margin row draws the map row a whole
// number of periods away inside the console's 160 rows, moved by the
// repeat's shift, with the horizontal scroll of the nearest screen row (row
// 0 above, row 159 below). Taking the copied row's own scroll instead carried
// Air's Rock's sideways sway into the margin and left a seam along the old
// screen edge (replay of tla_rewind_0024). The layer's live map is used in
// the margins. `first_y` is the margin's top row (negative), `rows` the
// output height.
inline void continue_layer_repeat(const FieldScene& scene, SceneMarginLayer& margin,
                                  int bg, const LayerRepeat& repeat,
                                  int first_y, int rows) {
    using scene_repeat_detail::row_reg;
    margin.vram_height = -1;
    margin.scroll_sides = false;
    margin.scroll_first_y = first_y;
    margin.scroll.assign(std::size_t(rows), {0, 0});
    const int vofs = int(row_reg(scene, 0, 0x12 + bg * 4));
    for (int i = 0; i < rows; ++i) {
        const int y = first_y + i;
        int k = 0;   // periods between this row and its source row
        if (y < 0) k = -((-y + repeat.period - 1) / repeat.period);
        else if (y >= FieldScene::kRows)
            k = (y - FieldScene::kRows + repeat.period) / repeat.period;
        const int edge = y < 0 ? 0 : y >= FieldScene::kRows ? FieldScene::kRows - 1 : y;
        const int hofs = int(row_reg(scene, edge, 0x10 + bg * 4));
        margin.scroll[std::size_t(i)] = {std::uint16_t(hofs - k * repeat.shift),
                                         std::uint16_t(vofs - k * repeat.period)};
    }
}

// Whether layer `bg` is laid over the picture as a see-through effect on
// every row (an alpha-blend first target): light, haze or mist over the
// room, not the room's own scenery.
inline bool layer_blended_overlay(const FieldScene& scene, int bg) {
    using scene_repeat_detail::row_reg;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        if (!scene.row_io_valid[y]) return false;
        const unsigned bld = row_reg(scene, y, 0x50);
        if (((bld >> 6) & 3u) != 1u || !(bld & (1u << bg))) return false;
    }
    return true;
}

// The one margin rule for repeating layers. Every regular layer no game rule
// has already claimed (no margin scroll rows, no live-map margin) and whose
// shown picture repeats is continued with continue_layer_repeat, unless it
// is ordinary room scenery that the room source already fills
// (`room_fills(bg)`): the room knows what lies past the screen, a repeat
// only guesses. A see-through overlay is continued even then: Champa's
// sunlight matches the room's tiles on screen (96%) but the room's copy is
// not the moving light (The Lost Age FACTS, Champa sunlight 0115).
// Needs the game profile's margin_scroll_rows.
template <class RoomFills>
inline void continue_repeating_layers(const FieldScene& scene, SceneMargins& margins,
                                      const std::uint8_t* vram, std::size_t vram_bytes,
                                      RoomFills room_fills, int first_y, int rows) {
    for (int bg = 0; bg < 4; ++bg) {
        auto& margin = margins.layers[bg];
        if (margin.vram_height != 0 || !margin.scroll.empty()) continue;
        const LayerRepeat repeat = find_layer_repeat(scene, vram, vram_bytes, bg);
        if (!repeat.found) continue;
        if (room_fills(bg) && !layer_blended_overlay(scene, bg)) continue;
        continue_layer_repeat(scene, margin, bg, repeat, first_y, rows);
    }
}

}  // namespace gsr
