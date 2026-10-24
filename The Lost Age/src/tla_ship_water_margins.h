#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "field_scene.h"
#include "scene_repeat.h"

// The ship deck's water (map 42) is a 4-by-2 tile pattern on BG3. Validate
// the clean water strip against the captured top rows before exposing its
// repeat in the margins. Other native rows contain local water details; those
// keep the original map. The room source ends at the authored water strip.
// Recognised by that content alone, not the map number: over every saved
// capture it matches only the ship deck (local/scene-repeat-20261020/
// FINDINGS.md). Unlike the haze it is not a whole-screen repeat, so the
// shared repeating-layer rule does not cover it.
//
// The open sea elsewhere uses the same water (Atteka inlet, map 234, F12
// 0137), where the room has islands and coast in the margins and the repeat
// painted water over them. So the room's own BG3 across the whole expanded
// view must hold nothing but water in the pattern's colours, see-through
// tiles and one filler entry where its water ends (deck 0070: only 0x200, 160 cells; local water
// details 0x33DC/0x33DD keep palette 3); 0137 has dozens of other entries.
// `ewram` (the 256 KB work RAM: id grid 0x02010000, atlas 0x02020000, as the
// field profile) may be null: then the room is not consulted.
inline bool tla_ship_water_margins(const gsr::FieldScene& scene,
                                   gsr::SceneMargins& margins,
                                   const std::vector<std::uint8_t>& vram,
                                   const std::uint8_t* ewram, std::size_t ewram_bytes,
                                   int view_width, int view_height) {
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
    // The deck's eight pattern cells are eight different tiles. A plain strip
    // also fits a 4-by-2 repeat: the black top of cave map 178 (one tile,
    // F12 0126) and the open water of shrine map 169 (four tiles, 0121/0122),
    // where repeating it painted over the rest of the room.
    for (std::size_t i = 0; i < pattern.size(); ++i)
        for (std::size_t j = i + 1; j < pattern.size(); ++j)
            if (pattern[i] == pattern[j]) return false;

    for (unsigned row = 0; row < kPatternRows; ++row) {
        for (unsigned column = 0; column < kPatternColumns; ++column) {
            const std::size_t at = base + (row * kMapTiles + column) * 2u;
            const auto entry = std::uint16_t(vram[at]) |
                (std::uint16_t(vram[at + 1]) << 8);
            if (entry != pattern[row * kPatternColumns + column]) return false;
        }
    }

    if (ewram && ewram_bytes >= 0x20000u + 0x1000u * 8u) {
        auto half = [&](std::size_t at) {
            return std::uint16_t(ewram[at] | (ewram[at + 1] << 8));
        };
        const unsigned bank = pattern[0] >> 12;
        const unsigned bgcnt = reg(0, 0x0E);
        const int hofs = int(reg(0, 0x1C)), vofs = int(reg(0, 0x1E));
        const int left = -(view_width - kScreenWidth) / 2;
        const int top = -(view_height - gsr::FieldScene::kRows) / 2;
        bool have_filler = false;
        std::uint16_t filler = 0;
        for (int y = top; y < top + view_height; y += 8) {
            for (int x = left; x < left + view_width; x += 8) {
                const int sx = (x + hofs) & 2047, sy = (y + vofs) & 2047;
                const std::size_t cell = std::size_t((sy >> 4) * 128 + (sx >> 4));
                const unsigned id = half(0x10000u + cell * 4u) & 0x0FFFu;
                const unsigned sub = unsigned(((sy >> 3) & 1) * 2 + ((sx >> 3) & 1));
                const std::uint16_t entry = half(0x20000u + id * 8u + sub * 2u);
                if (unsigned(entry >> 12) == bank ||
                    gsr::background_tile_blank(vram.data(), vram.size(), bgcnt, entry))
                    continue;
                if (have_filler && entry != filler) return false;
                have_filler = true;
                filler = entry;
            }
        }
    }
    margins.layers[3].vram_height = -1;
    margins.layers[3].repeat_width = kPatternColumns * 8;
    margins.layers[3].repeat_height = kPatternRows * 8;
    return true;
}
