#pragma once

#include <cstddef>
#include <cstdint>

#include "field_scene.h"

// A slow-scrolling backdrop continued left of its own artwork.
//
// A field layer that scrolls slower than the camera (layer record speed below
// 1.0) reaches its grid region's column 0 before the camera reaches the
// room's left edge. With the camera at the edge, the wide view's left margin
// is still inside the room but asks that layer for pixels left of column 0,
// where the grid has nothing, so they drew black. Tret's forest (map 46, measured in a
// capture): BG3 at half speed, camera x 120 -> BG3 x 60; a
// 480-wide view needs BG3 x -60..-1, black on every row that holds the room
// position (57..159 and below).
//
// The game's own grid says what lies there when the artwork repeats: Tret's
// BG3 region (grid columns 0..63, rows 0..31) repeats every 16 columns
// (256 px) on all 32 rows; it first differs at column 64, BG2's region.
// Measured here per frame from the grid, over the grid rows the view can
// show and the columns as wide as the view, and handed to the renderer's
// existing art repeat (SceneMarginLayer::art_repeat_x), which only touches
// source pixels left of column 0. A layer whose art does not repeat keeps
// its black edge.
//
// Layer records: 0x30 bytes each from 0x02030DD0, BG3 then BG2 then BG1
// (field_game_profile.cpp); +0x10 is horizontal speed (16.16), +0x2C the
// region's first grid cell. Grid: 128x128 u32 at 0x02010000, low 12 bits
// the metatile id.
inline bool golden_sun_backdrop_margins(const gsr::FieldScene& scene,
                                        gsr::SceneMargins& margins,
                                        const std::uint8_t* ewram,
                                        std::size_t ewram_bytes,
                                        int view_width, int view_height) {
    for (auto& layer : margins.layers) layer.art_repeat_x = 0;
    constexpr std::size_t kRecords = 0x30DD0u, kRecordBytes = 0x30u;
    constexpr std::size_t kGrid = 0x10000u;
    constexpr int kColumns = 128, kCell = 16;
    if (!ewram || ewram_bytes < kGrid + kColumns * kColumns * 4u ||
        ewram_bytes < kRecords + 3 * kRecordBytes || scene.video_mode != 0 ||
        view_width <= 240)
        return false;
    auto word = [&](std::size_t at) {
        return std::uint32_t(ewram[at]) | (std::uint32_t(ewram[at + 1]) << 8) |
               (std::uint32_t(ewram[at + 2]) << 16) |
               (std::uint32_t(ewram[at + 3]) << 24);
    };
    auto cell = [&](int x, int y) {
        return word(kGrid + std::size_t(y * kColumns + x) * 4u) & 0xFFFu;
    };
    const int first_y = -(view_height - 160) / 2;
    bool found = false;
    for (int record = 0; record < 3; ++record) {
        const int bg = 3 - record;
        const auto& layer = scene.layers[bg];
        if (!layer.enabled || layer.affine) continue;
        const std::size_t at = kRecords + std::size_t(record) * kRecordBytes;
        if (word(at + 0x10) >= 0x10000u) continue;   // keeps up with the camera
        // Only a region starting at grid column 0 can be asked for x < 0.
        const std::uint32_t grid = word(at + 0x2C);
        if (grid < 0x02010000u || grid >= 0x02020000u || (grid & 3u) ||
            ((grid - 0x02010000u) / 4u) % kColumns != 0u)
            continue;
        // The rows this layer shows at a room position (rows whose scroll
        // has no place in the 2048-px map are a sky accumulator instead).
        int vofs_lo = 1 << 30, vofs_hi = -(1 << 30);
        for (int y = 0; y < gsr::FieldScene::kRows; ++y) {
            const std::uint8_t* io = scene.registers_for_row(y);
            const int hofs = std::int16_t(io[0x10 + bg * 4] | (io[0x11 + bg * 4] << 8));
            const int vofs = std::int16_t(io[0x12 + bg * 4] | (io[0x13 + bg * 4] << 8));
            if (hofs + 240 <= 0 || hofs >= 2048 || vofs + 160 <= 0 || vofs >= 2048)
                continue;
            if (vofs < vofs_lo) vofs_lo = vofs;
            if (vofs > vofs_hi) vofs_hi = vofs;
        }
        if (vofs_lo > vofs_hi) continue;
        int row_lo = (vofs_lo + first_y) / kCell;
        int row_hi = (vofs_hi + first_y + view_height - 1) / kCell;
        if (row_lo < 0) row_lo = 0;
        if (row_hi > kColumns - 1) row_hi = kColumns - 1;
        if (row_lo > row_hi) continue;
        // Smallest period whose copy holds across the view's width.
        const int across = (view_width + kCell - 1) / kCell + 1;
        for (int period = 1; period * 2 <= kColumns; ++period) {
            const int need = across > period ? across : period;
            if (need + period > kColumns) break;
            bool same = true;
            for (int y = row_lo; y <= row_hi && same; ++y)
                for (int x = 0; x < need && same; ++x)
                    same = cell(x + period, y) == cell(x, y);
            if (same) {
                margins.layers[bg].art_repeat_x = period * kCell;
                found = true;
                break;
            }
        }
    }
    return found;
}
