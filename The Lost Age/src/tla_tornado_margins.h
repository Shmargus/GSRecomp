#pragma once

#include <array>

#include "field_scene.h"

// F12 0121: Air's Rock's tornado is BG0, characters 0xC000, map 0x2000.
// Its wind artwork ends at the console's top edge; below row 159 the map
// holds unrelated graphics. The margin continues its upper two tile
// rows (16 pixels) upward, with the captured horizontal shake.
// Expose only one horizontal copy, so the tilemap never repeats at the sides.
//
// The wind is drawn at fixed screen rows, the room and Felix with the camera.
// While the expanded view's camera rule holds the camera `hold` rows below
// where the game put it (tla_camera_hold_y), the room moved up that far and
// the wind did not, so it sat on Felix (Extra Wide 80 rows, F12 0017,
// 2026-10-14). Every row then shows the wind row `hold` further down; above
// the wind's row 0 the approved continuation, below its row 159 the empty
// rows 144..159. 0 changes nothing.
inline bool tla_tornado_margins(gsr::FieldScene& scene,
                                gsr::SceneMargins& margins, unsigned map,
                                int first_y, int hold = 0) {
    auto& layer = scene.layers[0];
    auto& margin = margins.layers[0];
    margin.scroll.clear();
    margin.vram_height = 0;
    // The same wind inside Air's Rock (map 84, BG0CNT 0x040E: priority 2 instead
    // of 1; the same characters, map and empty rows 144..159; F12
    // tla_rewind_0025, 2026-10-15).
    const unsigned bgcnt = map == 82 ? 0x040Du : map == 84 ? 0x040Eu : 0u;
    if (!bgcnt || scene.video_mode != 0 || !layer.enabled || layer.affine)
        return false;
    auto reg = [&](int row, int at) {
        const auto& io = scene.row_io_valid[row] ? scene.row_io[row] : scene.io_bytes;
        return unsigned(io[at]) | (unsigned(io[at + 1]) << 8);
    };
    for (int row = 0; row < gsr::FieldScene::kRows; ++row) {
        if (!scene.row_io_valid[row] || reg(row, 0x08) != bgcnt ||
            reg(row, 0x12) != 0 || (reg(row, 0x00) & 0x0107) != 0x0100)
            return false;
    }
    margin.vram_height = gsr::FieldScene::kRows;
    std::array<std::uint16_t, gsr::FieldScene::kRows> hofs{};
    for (int row = 0; row < gsr::FieldScene::kRows; ++row)
        hofs[row] = std::uint16_t(reg(row, 0x10));
    // The wind row screen row y shows.
    auto source = [&](int y) {
        const int m = y + hold;
        if (m < 0) return (-m - 1) % 16;
        return m < gsr::FieldScene::kRows ? m : 150;
    };
    if (hold != 0) {
        for (int row = 0; row < gsr::FieldScene::kRows; ++row) {
            const int src = source(row);
            auto& io = scene.row_io[row];
            io[0x10] = std::uint8_t(hofs[src]);
            io[0x11] = std::uint8_t(hofs[src] >> 8);
            const unsigned vofs = unsigned(src - row) & 0x1FFu;
            io[0x12] = std::uint8_t(vofs);
            io[0x13] = std::uint8_t(vofs >> 8);
        }
    }
    if (first_y >= 0) return true;
    margin.scroll_first_y = first_y;
    margin.scroll.resize(-first_y);
    for (int y = first_y; y < 0; ++y) {
        const int src = source(y);
        margin.scroll[y - first_y] = {hofs[src], std::uint16_t(src - y)};
    }
    return true;
}
