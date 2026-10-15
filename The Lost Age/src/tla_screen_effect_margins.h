#pragma once

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
