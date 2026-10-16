#pragma once

#include "field_scene.h"

// The game's sine veneer 0x08002096: rounds the angle by 32, indexes the
// signed-halfword ROM asset at 0x080020A8 and multiplies by four.
template<class ReadSine>
int tla_sine(ReadSine read_sine, unsigned angle) {
    return int(read_sine(((angle + 32u) & 0xFFFFu) >> 6)) * 4;
}

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
        auto sine = [&](unsigned angle) { return tla_sine(read_sine, angle); };
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

// The port's water (map 98, F12 tla_rewind_0040..0044): the room's RAM
// routine 0x02008A98 fills a 160-row BG1VOFS table, row i =
// base + (sine((counter - i - base + 160) << 9) >> 15), base = the s16 at
// [[0x03000020] + 0x17E], counter the byte at 0x0300122C (the ripple's
// phase); 0x02008A44 hands it to HBlank DMA0 at VBlank, row 0 written
// directly. Rows 1..159 of all 600 captured port frames match with base
// within 4 of row 1 (the current phase in 582, the one before in 18).
// Only extend a wave that reproduces EVERY row 1..159.
template<class ReadSine>
bool tla_port_wave(gsr::FieldScene& scene, gsr::SceneMargins& margins,
                   unsigned phase, ReadSine read_sine, int first_y,
                   int rows) {
    auto& layer = scene.layers[1];
    auto& margin = margins.layers[1];
    margin.scroll.clear();
    if (scene.video_mode != 0 || layer.affine || !layer.enabled || rows <= 0)
        return false;
    auto reg = [&](int row, int at) {
        return unsigned(scene.row_io[row][at]) | (unsigned(scene.row_io[row][at + 1]) << 8);
    };
    const unsigned x = reg(1, 0x14);
    bool varying = false;
    for (int row = 1; row < gsr::FieldScene::kRows; ++row) {
        if (!scene.row_io_valid[row] || reg(row, 0x14) != x) return false;
        varying |= reg(row, 0x16) != reg(1, 0x16);
    }
    if (!varying) return false;
    auto wave = [&](int row, unsigned p, int base) {
        const unsigned angle = unsigned(int(p) - row - base + 160) << 9;
        return std::uint16_t(base + (tla_sine(read_sine, angle) >> 15));
    };
    const int near = int(std::int16_t(reg(1, 0x16)));
    for (unsigned p : {phase, (phase - 1u) & 0xFFu})
        for (int base = near - 4; base <= near + 4; ++base) {
            bool match = true;
            for (int row = 1; row < gsr::FieldScene::kRows && match; ++row)
                match = wave(row, p, base) == reg(row, 0x16);
            if (!match) continue;
            margin.scroll_first_y = first_y;
            margin.scroll.resize(rows);
            for (int row = 0; row < rows; ++row)
                margin.scroll[row] = {std::uint16_t(x), wave(first_y + row, p, base)};
            return true;
        }
    return false;
}
