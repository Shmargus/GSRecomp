#pragma once

// The flat world map (Select map). The margins of a widened view show more of
// the map around the console's picture, instead of the held sailing view.
//
// Measured (ROM SHA-1 b500663220cb9bf56b9f8e8c0c544f1d6fa3a824, over a
// recorded 120 frames):
// - The screen is mode 0 with BG0, BG1 and objects (DISPCNT 0x1340), BG1CNT
//   0x8682 (256 colours), EWRAM+0x428 == 2. BG1 is a fixed 32x24 grid of
//   tiles 0x100..0x3FF; the game scrolls by rewriting their graphics, so
//   video memory holds only a 256x192 window of a 512x384 map.
// - Map pixel (X, Y) shows at console pixel (X - BG1HOFS, Y - BG1VOFS): every
//   BG1 tile on screen equals the ring tile at map column X/8, ring slot
//   (Y/8) mod 24 (0 of 651 differ; one column or row off, all 651 differ).
// - The ring at EWRAM 0x02010000 holds whole 512-pixel map rows of 8-bit
//   tiles, 0x1000 bytes a tile row (64 tiles of 64 bytes), map row r in slot
//   r mod 24. The row loader 0x080ECF74 takes file 0x24 (0x23 when halfword
//   0x0202A640 != 0) from the table at 0x08680000 (0x08013300), unpacks row r
//   from file + [file + 4 + r * 4] with 0x0801587C (a RAM copy of the ARM
//   routine 0x080128A8, size 0x2C8) into the ring.
// - File 0x24 (ROM 0x086B1688): word 0 is 0xC4, then 48 row offsets
//   (0x2A4..0x1BD5E); rows 0..46 are format 0, row 47 format 1; every row
//   unpacks to 4096 bytes. 0x080ECE40 clamps the scroll to x <= 0x110,
//   y <= 0xE0, i.e. a 512x384 map.
// - tla_rom::unpack (0x080128A8) reproduces the 22 rows the capture holds (20..41)
//   byte for byte. Format 1 (row 47 only) has no live row to check against.
// - The colours are the live BG palette, converted as the graphics card does
//   (round(c * 255 / 31); every native BG1 pixel of one measured frame).
//
// Every frame, the rows the game itself shows are compared with the unpacked
// map. Black margins are painted if any of these holds:
//   - the rows differ,
//   - file 0x23 is in use (not measured),
//   - a window is active,
//   - an alpha blend is over BG1 (not measured).
// Outside the map the margins are black.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "field_scene.h"
#include "tla_rom_unpack.h"

namespace tla_flat_map {

constexpr unsigned kFile = 0x24u;
constexpr int kRows = 48;
constexpr int kWidth = 512, kHeight = kRows * 8;
constexpr std::size_t kRowBytes = 0x1000u;          // 64 tiles of 64 bytes
constexpr std::size_t kRing = 0x10000u;             // EWRAM 0x02010000
constexpr int kRingSlots = 24;
constexpr std::size_t kFileSwitch = 0x2A640u;       // EWRAM 0x0202A640

// True if `src` matches no row in the map table. Return false if the table
// is unreadable. Row r starts at file + [file + 4 + r * 4]. The first word
// gives (word 0 - 4) / 4 rows (48 for file 0x24, 22 for 0x23 in all five ROMs).
// At VOFS 0xE0, the initial loader (0x080ECE40, call at 0x080ECF18) and
// scroll loader (0x080ECF74, call at 0x080ECFD0) both ask for row 48 of 0x24.
// That reads file + 0x3EB90000 (open bus) into slot 24, past the ring.
// The row is offscreen.
inline bool past_table(const std::uint8_t* rom, std::size_t rom_bytes, unsigned file,
                       std::uint32_t src) {
    std::size_t at = 0;
    std::uint32_t first = 0, piece = 0;
    if (!tla_rom::asset_offset(rom, rom_bytes, file, &at) ||
        !tla_rom::word(rom, rom_bytes, std::uint32_t(0x08000000u + at), &first) ||
        first < 4u || (first & 3u))
        return false;
    for (std::uint32_t r = 0; r < (first - 4u) / 4u; ++r) {
        if (!tla_rom::word(rom, rom_bytes, std::uint32_t(0x08000000u + at + 4u + r * 4u), &piece))
            return false;
        if (src == std::uint32_t(0x08000000u + at + piece)) return false;
    }
    return true;
}

// File 0x24's 48 rows, unpacked once per ROM; empty when any row fails.
inline const std::vector<std::uint8_t>& map_rows(const std::uint8_t* rom,
                                                 std::size_t rom_bytes,
                                                 std::uint32_t* out_generation = nullptr) {
    static const std::uint8_t* for_rom = nullptr;
    static std::size_t for_bytes = 0;
    static std::vector<std::uint8_t> rows;
    static std::uint32_t generation = 0;
    if (rom != for_rom || rom_bytes != for_bytes) {
        for_rom = rom;
        for_bytes = rom_bytes;
        rows.clear();
        std::size_t at = 0;
        if (tla_rom::asset_offset(rom, rom_bytes, kFile, &at)) {
            std::vector<std::uint8_t> out(std::size_t(kRows) * kRowBytes), row;
            bool ok = true;
            for (int r = 0; r < kRows; ++r) {
                std::uint32_t piece = 0;
                if (!tla_rom::word(rom, rom_bytes, std::uint32_t(0x08000000u + at + 4u + r * 4u), &piece) ||
                    piece >= rom_bytes - at ||
                    !gsr::rom_unpack(rom + at + piece, rom_bytes - at - piece, &row, kRowBytes) ||
                    row.size() != kRowBytes) {
                    ok = false;
                    break;
                }
                std::copy(row.begin(), row.end(), out.begin() + std::size_t(r) * kRowBytes);
            }
            if (ok) {
                rows = std::move(out);
                ++generation;
            }
        }
    }
    if (out_generation) *out_generation = rows.empty() ? 0u : generation;
    return rows;
}

inline unsigned reg16(const std::uint8_t* io, unsigned at) {
    return unsigned(io[at]) | (unsigned(io[at + 1]) << 8);
}

// The map screen's registers: mode 0, BG1 on in 256 colours, BG2 and BG3
// off, no window, on the world map (EWRAM+0x428 == 2). Ordinary world-map
// menus (DISPCNT 0x1F40) have BG2 and BG3 on.
inline bool is_map_screen(const std::uint8_t* io, std::size_t io_bytes,
                          const std::uint8_t* ewram, std::size_t ewram_bytes) {
    if (!io || !ewram || io_bytes < 0x58u || ewram_bytes < 0x40000u) return false;
    if (reg16(ewram, 0x428u) != 2u) return false;
    const unsigned dispcnt = reg16(io, 0);
    return (dispcnt & 7u) == 0u && (dispcnt & 0x0200u) && !(dispcnt & 0x0C00u) &&
           !(dispcnt & 0xE000u) && (reg16(io, 0x0Au) & 0x80u);
}

inline bool is_map_screen(const std::vector<std::uint8_t>& io,
                          const std::vector<std::uint8_t>& ewram) {
    return is_map_screen(io.data(), io.size(), ewram.data(), ewram.size());
}

// True when the rows on the console's own screen match the unpacked map and
// no window, file 0x23, or alpha blend over BG1 invalidates the margins.
inline bool is_trusted(const gsr::FieldScene& scene,
                       const std::uint8_t* io, std::size_t io_bytes,
                       const std::uint8_t* ewram, std::size_t ewram_bytes,
                       const std::uint8_t* rom, std::size_t rom_bytes,
                       std::uint32_t* out_generation = nullptr) {
    const std::vector<std::uint8_t>& rows = map_rows(rom, rom_bytes, out_generation);
    if (rows.empty() || !io || !ewram || scene.palette.size() < 256u ||
        io_bytes < 0x58u || ewram_bytes < kRing + kRingSlots * kRowBytes ||
        reg16(ewram, kFileSwitch) != 0u)
        return false;
    for (int y = 0; y < gsr::FieldScene::kRows; ++y) {
        const std::uint8_t* r = scene.row_io_valid[y] ? scene.row_io[y].data() : io;
        const unsigned bldcnt = reg16(r, 0x50u);
        if ((reg16(r, 0) & 0xE000u) || (((bldcnt >> 6) & 3u) == 1u && (bldcnt & 2u)))
            return false;
        const int map_y = int(reg16(r, 0x16u) & 0x1FFu) + y;
        if (map_y >= kHeight) continue;
        const int tile_row = map_y / 8;
        if (std::memcmp(ewram + kRing + std::size_t(tile_row % kRingSlots) * kRowBytes,
                        rows.data() + std::size_t(tile_row) * kRowBytes, kRowBytes) != 0)
            return false;
    }
    return true;
}

}  // namespace tla_flat_map
