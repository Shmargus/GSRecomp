#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "field_scene.h"

// The ship deck's water (map 42) is a 4-by-2 tile pattern on BG3. Validate
// the clean water strip against the captured top rows before exposing its
// repeat in the margins. Other native rows contain local water details; those
// keep the original map. The room source ends at the authored water strip.
// Recognised by that content alone, not the map number: over every saved
// capture it matches only the ship deck (local/scene-repeat-20261020/
// FINDINGS.md). Unlike the haze it is not a whole-screen repeat, so the
// shared repeating-layer rule does not cover it.
inline bool tla_ship_water_margins(const gsr::FieldScene& scene,
                                   gsr::SceneMargins& margins,
                                   const std::vector<std::uint8_t>& vram) {
    margins.layers[3].vram_height = 0;
    margins.layers[3].repeat_width = margins.layers[3].repeat_height = 0;
    if (scene.video_mode != 0 ||
        !scene.layers[3].enabled || scene.layers[3].affine)
        return false;

    constexpr int kMapTiles = 32;
    constexpr int kPatternColumns = 4;
    constexpr int kPatternRows = 2;
    constexpr int kScreenWidth = 240;
    constexpr std::size_t kMapBytes = kMapTiles * kMapTiles * 2u;

    auto reg = [&](int row, int at) {
        const auto& io = scene.row_io[row];
        return unsigned(io[at]) | (unsigned(io[at + 1]) << 8);
    };

    for (int row = 0; row < gsr::FieldScene::kRows; ++row) {
        if (!scene.row_io_valid[row] || reg(row, 0x0E) != 0x0503u)
            return false;
    }

    const std::size_t base = ((reg(0, 0x0E) >> 8) & 31u) * 0x800u;
    if (base > vram.size() || kMapBytes > vram.size() - base)
        return false;

    std::array<std::uint16_t, kPatternColumns * kPatternRows> pattern{};
    std::array<bool, kPatternColumns * kPatternRows> seen{};
    for (int y = 0; y < kPatternRows * 8; ++y) {
        const unsigned hofs = reg(y, 0x1C) & 0x1FFu;
        const unsigned vofs = reg(y, 0x1E) & 0x1FFu;
        const unsigned map_row = ((vofs + unsigned(y)) & 0xFFu) >> 3;
        auto check_column = [&](unsigned x) {
            const unsigned map_column = ((hofs + unsigned(x)) & 0xFFu) >> 3;
            const unsigned pattern_index =
                (map_row % kPatternRows) * kPatternColumns +
                (map_column % kPatternColumns);
            const std::size_t entry_at =
                base + (map_row * kMapTiles + map_column) * 2u;
            const std::uint16_t entry = std::uint16_t(vram[entry_at]) |
                (std::uint16_t(vram[entry_at + 1]) << 8);
            if (seen[pattern_index] && pattern[pattern_index] != entry)
                return false;
            pattern[pattern_index] = entry;
            seen[pattern_index] = true;
            return true;
        };
        for (int x = 0; x < kScreenWidth; x += 8)
            if (!check_column(unsigned(x))) return false;
        // The last native pixel can touch a partial tile not sampled above.
        if (!check_column(239u)) return false;
    }

    for (bool present : seen)
        if (!present) return false;

    for (unsigned row = 0; row < kPatternRows; ++row) {
        for (unsigned column = 0; column < kPatternColumns; ++column) {
            const std::size_t at = base + (row * kMapTiles + column) * 2u;
            const auto entry = std::uint16_t(vram[at]) |
                (std::uint16_t(vram[at + 1]) << 8);
            if (entry != pattern[row * kPatternColumns + column]) return false;
        }
    }
    margins.layers[3].vram_height = -1;
    margins.layers[3].repeat_width = kPatternColumns * 8;
    margins.layers[3].repeat_height = kPatternRows * 8;
    return true;
}
