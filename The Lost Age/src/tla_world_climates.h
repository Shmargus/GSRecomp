// tla_world_climates.h -- each world-map margin cell in its own climate.
//
// The world map has four terrain sets (BG3 and BG2 tile graphics plus the
// background palette, swapped together), and the game loads ONE for the
// whole screen, chosen by the 16-pixel cell under the ship. Past the
// console's screen that put a snowy island in green (F12 0073/0074). This
// unpacks all four from the cartridge, once, and names each cell's set the
// game's way, so the renderer draws a margin cell whose set is not the
// loaded one with its own artwork and colours. The loaded set's cells, and
// everything inside the 240x160, stay on video memory.
//
// Evidence (21 October 2026, kanban snow investigation, ROM SHA-1 b5006632):
//  * selector, Thumb 0x0802C58C..0x0802C6B2: cell word at 0x02020000 (the
//    first layer's ring); bits 24..29 divided by 10 (0x08002054): 3 -> set 3,
//    2 -> set 2, 4 -> set 1, else set 0; set record 0x0802ED34 + 36 * set,
//    stored at [[0x03000020] + 0x120].
//  * a set record is nine 32-bit asset numbers, each a pointer at
//    0x08680000 + 4 * n (tla_rom_unpack.h). Asset 0 is the palette (0x0801591C,
//    format of 0x08001658), DMA'd as 0x70 words keeping colour 0 (0x0802C71C);
//    assets 3..6 the character blocks, staged at 0x02038000/A000/C000/E000
//    and copied to VRAM 0x8000/A000/C000/E000 (0x0802C76E..0x0802C7D6),
//    unpacked by 0x0801587C (format of ARM 0x080128A8).
//  * both unpackers checked byte for byte: 0073 frame 0 (snow, set 1) and
//    frame 60 (set 0), all eight staged assets equal EWRAM, the palette
//    equals PAL 1..223. Over 207 frames of 0073/0074 no colour 1..223 ever
//    differs from the loaded set's (no colour cycling), so a palette that
//    does differ (a fade, a menu) turns this off for the frame.
//  * the snow set's tiles never change in video memory; set 0 animates 54
//    tiles (its asset 1 script), which other sets' cells show unanimated.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "field_scene_renderer.h"
#include "tla_rom_unpack.h"
#include "world_map_source.h"

class TlaWorldClimates {
public:
    static constexpr int kSets = 4;
    static_assert(kSets <= gsr::FieldSceneRenderer::kWorldArtSets);

    // Hands the renderer this frame's cell artwork, or turns it off. Call
    // after upload_world_map with the map that was uploaded, and after
    // upload_palette with the same `palette` (256 background colours).
    void apply(gsr::FieldSceneRenderer& renderer, bool map_uploaded,
               const gbarecomp::WorldMapSource& map, const std::uint16_t* palette,
               const std::uint8_t* iwram, std::size_t iwram_bytes,
               const std::uint8_t* rom, std::size_t rom_bytes) {
        bool loading = false;
        const int loaded = map_uploaded ? loaded_set(iwram, iwram_bytes, &loading) : -1;
        if (loaded < 0 || !decode(rom, rom_bytes) ||
            map.cell_attributes().empty() || !palette_is_set(palette, loaded)) {
            renderer.upload_world_cell_art(nullptr, 0, nullptr, nullptr, 0, 0);
            return;
        }
        // While the game is still loading the new set's tiles into video
        // memory, it holds part of the old set: no margin cell reads it then
        // (F12 0116 frame 46: the snowy island garbled for a few frames).
        const int in_vram = loading ? -1 : loaded;
        if (map.generation() != cells_map_ || in_vram != cells_loaded_) {
            const auto& attributes = map.cell_attributes();
            cells_.resize(attributes.size());
            for (std::size_t i = 0; i < attributes.size(); ++i) {
                const int set = set_of(attributes[i]);
                cells_[i] = set == in_vram ? 0xFF : std::uint8_t(set);
            }
            cells_map_ = map.generation();
            cells_loaded_ = in_vram;
            ++cells_generation_;
        }
        for (int s = 0; s < kSets; ++s) {
            std::uint16_t* row = palettes_.data() + s * 256;
            for (int c = 0; c < 256; ++c)
                row[c] = c >= 1 && c < kSetColours ? colours_[s][c] : palette[c];
        }
        renderer.upload_world_cell_art(cells_.data(), cells_generation_, art_.data(),
                                       palettes_.data(), kSets, 1);
    }

    // The set the game would load for a cell's attribute byte (bits 24..29
    // of its word), as the selector at 0x0802C662..0x0802C68A.
    static int set_of(std::uint8_t attribute) {
        switch ((attribute & 0x3Fu) / 10u) {
            case 3: return 3;
            case 2: return 2;
            case 4: return 1;
            default: return 0;
        }
    }

private:
    static constexpr std::uint32_t kSetRecords = 0x0802ED34u;
    static constexpr int kSetColours = 0x70 * 2;  // 224: DMA of 0x70 words
    static constexpr std::size_t kBlock = 0x2000;  // one staged asset, 8 KB

    // The set record the game last chose, and whether its 12-step load is
    // still running: the word [S+0x958] counts the steps down, and the
    // loader runs only while it is non-zero (ldr/cmp at 0x0802C6C2..C6C6;
    // F12 0116 shows 10..0 after each switch).
    static int loaded_set(const std::uint8_t* iwram, std::size_t bytes, bool* loading) {
        const auto word = [&](std::uint32_t addr, std::uint32_t* v) {
            const std::uint32_t off = addr - 0x03000000u;
            if ((addr >> 24) != 0x03u || off + 4 > bytes) return false;
            *v = iwram[off] | (iwram[off + 1] << 8) | (iwram[off + 2] << 16) |
                 (std::uint32_t(iwram[off + 3]) << 24);
            return true;
        };
        std::uint32_t state = 0, record = 0, steps = 0;
        if (!iwram || !word(0x03000020u, &state) || !word(state + 0x120u, &record) ||
            !word(state + 0x958u, &steps))
            return -1;
        *loading = steps != 0;
        const std::uint32_t at = record - kSetRecords;
        return at % 36u == 0 && at / 36u < kSets ? int(at / 36u) : -1;
    }

    bool palette_is_set(const std::uint16_t* palette, int set) const {
        for (int c = 1; c < kSetColours; ++c)
            if (palette[c] != colours_[set][c]) return false;
        return true;
    }

    bool decode(const std::uint8_t* rom, std::size_t rom_bytes) {
        if (rom == rom_ && rom_bytes == rom_bytes_) return decoded_;
        rom_ = rom;
        rom_bytes_ = rom_bytes;
        decoded_ = false;
        art_.assign(std::size_t(kSets) * 4 * kBlock, 0);
        const auto asset = [&](int set, int k, std::size_t* off) {
            std::uint32_t n = 0;
            return tla_rom::word(rom, rom_bytes, kSetRecords + 36u * set + 4u * k, &n) &&
                   tla_rom::asset_offset(rom, rom_bytes, n, off);
        };
        std::vector<std::uint8_t> out;
        for (int s = 0; s < kSets; ++s) {
            std::size_t off = 0;
            if (!asset(s, 0, &off) ||
                !gbarecomp::lz_decompress(rom + off, rom_bytes - off, &out, 0x200) ||
                out.size() < std::size_t(kSetColours) * 2)
                return false;
            for (int c = 0; c < kSetColours; ++c)
                colours_[s][c] = std::uint16_t(out[c * 2] | (out[c * 2 + 1] << 8));
            for (int k = 0; k < 4; ++k) {
                if (!asset(s, 3 + k, &off) ||
                    !tla_rom::unpack(rom + off, rom_bytes - off, &out, kBlock) ||
                    out.size() != kBlock)
                    return false;
                std::copy(out.begin(), out.end(),
                          art_.begin() + (std::size_t(s) * 4 + k) * kBlock);
            }
        }
        decoded_ = true;
        return true;
    }

    const std::uint8_t* rom_ = nullptr;
    std::size_t rom_bytes_ = 0;
    bool decoded_ = false;
    std::vector<std::uint8_t> art_;
    std::array<std::array<std::uint16_t, kSetColours>, kSets> colours_{};
    std::array<std::uint16_t, 256 * kSets> palettes_{};
    std::vector<std::uint8_t> cells_;
    std::uint32_t cells_map_ = 0;
    int cells_loaded_ = -1;
    std::uint32_t cells_generation_ = 0;
};
