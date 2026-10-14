#pragma once

#include "field_scene.h"

// F12 0118/0119: the RAM routine at 0x020082A4 builds BG2HOFS using
// 3*sin((y+row)*0x3300+phase*1024) + 7*sin((y+row)*0x1320-phase*1024),
// shifted right 19. Its sine veneer reaches 0x08002096, which rounds the
// angle by 32, indexes the signed-halfword ROM asset at 0x080020A8 and
// multiplies by four. No ROM data is embedded here.
// Only extend a wave that reproduces EVERY captured row. The live phase
// may already have advanced at VBlank; try it and its preceding value.
template<class ReadSine>
bool tla_water_ripple(gsr::FieldScene& scene, gsr::SceneMargins& margins,
                      unsigned phase, ReadSine read_sine, int first_y,
                      int rows) {
    auto& layer = scene.layers[2];
    auto& margin = margins.layers[2];
    margin.scroll.clear();
    if (scene.video_mode != 0 || layer.affine || !layer.enabled || rows <= 0)
        return false;
    auto reg = [&](int row, int at) {
        const auto& io = scene.row_io_valid[row] ? scene.row_io[row] : scene.io_bytes;
        return unsigned(io[at]) | (unsigned(io[at + 1]) << 8);
    };
    const unsigned y = reg(0, 0x1A);
    bool varying = false;
    for (int row = 0; row < gsr::FieldScene::kRows; ++row) {
        if (!scene.row_io_valid[row] || reg(row, 0x1A) != y) return false;
        varying |= reg(row, 0x18) != reg(0, 0x18);
    }
    if (!varying) return false;
    auto delta = [&](int row, unsigned p) {
        auto sine = [&](unsigned angle) {
            return int(read_sine(((angle + 32u) & 0xFFFFu) >> 6)) * 4;
        };
        const unsigned position = y + unsigned(row);
        const int sum = 3 * sine(position * 0x3300u + (p << 10)) +
                        7 * sine(position * 0x1320u - (p << 10));
        // ARM ASR rounds negative values down, not toward zero.
        return sum >= 0 ? sum / 0x80000 : -((-sum + 0x7FFFF) / 0x80000);
    };
    for (unsigned p : {phase, (phase - 1u) & 0xFFu}) {
        const int x = int(reg(0, 0x18)) - delta(0, p);
        bool match = true;
        for (int row = 1; row < gsr::FieldScene::kRows; ++row)
            if (std::uint16_t(x + delta(row, p)) != reg(row, 0x18)) {
                match = false;
                break;
            }
        if (!match) continue;
        margin.scroll_first_y = first_y;
        margin.scroll.resize(rows);
        for (int row = 0; row < rows; ++row)
            margin.scroll[row] = {std::uint16_t(x + delta(first_y + row, p)),
                                        std::uint16_t(y)};
        return true;
    }
    return false;
}
