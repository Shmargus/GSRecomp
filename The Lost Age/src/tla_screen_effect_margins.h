#pragma once

#include "field_scene.h"

// Yampi Desert's captured heat haze is a screen-filling live BG1 tilemap.
// Continue its wrapped map only when every captured row identifies it.
inline bool tla_screen_effect_margins(gsr::FieldScene& scene,
                                      gsr::SceneMargins& margins,
                                      unsigned map) {
    auto& layer = scene.layers[1];
    margins.layers[1].vram_height = 0;
    if (map != 75 || scene.video_mode != 0 || !layer.enabled || layer.affine)
        return false;
    for (int row = 0; row < gsr::FieldScene::kRows; ++row) {
        if (!scene.row_io_valid[row]) return false;
        const auto& io = scene.row_io[row];
        auto reg = [&](int at) {
            return unsigned(io[at]) | (unsigned(io[at + 1]) << 8);
        };
        if (reg(0x0A) != 0x0709 || (reg(0x00) & 0xE207) != 0x0200 ||
            reg(0x50) != 0x3F42)
            return false;
    }
    margins.layers[1].vram_height = -1;
    return true;
}
