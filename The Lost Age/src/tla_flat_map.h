#pragma once

// The flat world map (Select map; F12 tla_rewind_0072, 2026-10-21): the
// margins of a widened view show more of the map around the console's
// picture instead of the held sailing view.
//
// Measured (ROM SHA-1 b500663220cb9bf56b9f8e8c0c544f1d6fa3a824, 0072's 120
// frames):
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
// - tla_rom::unpack (0x080128A8) reproduces the 22 rows 0072 holds (20..41)
//   byte for byte. Format 1 (row 47 only) has no live row to check against.
// - The colours are the live BG palette, converted as the graphics card does
//   (round(c * 255 / 31); every native BG1 pixel of 0072's frame_100).
//
// Every frame the rows the game itself shows are compared with the unpacked
// map; any difference, file 0x23 (not measured), a window, or an alpha blend
// over BG1 (not measured) paints black margins. Black outside the map.

#include <algorithm>
#include <array>
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

// File 0x24's 48 rows, unpacked once per ROM; empty when any row fails.
inline const std::vector<std::uint8_t>& map_rows(const std::uint8_t* rom,
                                                 std::size_t rom_bytes) {
    static const std::uint8_t* for_rom = nullptr;
    static std::size_t for_bytes = 0;
    static std::vector<std::uint8_t> rows;
    if (rom == for_rom && rom_bytes == for_bytes) return rows;
    for_rom = rom;
    for_bytes = rom_bytes;
    rows.clear();
    std::size_t at = 0;
    if (!tla_rom::asset_offset(rom, rom_bytes, kFile, &at)) return rows;
    std::vector<std::uint8_t> out(std::size_t(kRows) * kRowBytes), row;
    for (int r = 0; r < kRows; ++r) {
        std::uint32_t piece = 0;
        if (!tla_rom::word(rom, rom_bytes, std::uint32_t(0x08000000u + at + 4u + r * 4u), &piece) ||
            piece >= rom_bytes - at ||
            !tla_rom::unpack(rom + at + piece, rom_bytes - at - piece, &row, kRowBytes) ||
            row.size() != kRowBytes)
            return rows;
        std::copy(row.begin(), row.end(), out.begin() + std::size_t(r) * kRowBytes);
    }
    rows = std::move(out);
    return rows;
}

inline unsigned reg16(const std::uint8_t* io, unsigned at) {
    return unsigned(io[at]) | (unsigned(io[at + 1]) << 8);
}

// The map screen's registers: mode 0, BG1 on in 256 colours, BG2 and BG3
// off, no window, on the world map (EWRAM+0x428 == 2). Ordinary world-map
// menus (DISPCNT 0x1F40) have BG2 and BG3 on.
inline bool is_map_screen(const std::vector<std::uint8_t>& io,
                          const std::vector<std::uint8_t>& ewram) {
    if (io.size() < 0x58u || ewram.size() < 0x40000u) return false;
    if (reg16(ewram.data(), 0x428u) != 2u) return false;
    const unsigned dispcnt = reg16(io.data(), 0);
    return (dispcnt & 7u) == 0u && (dispcnt & 0x0200u) && !(dispcnt & 0x0C00u) &&
           !(dispcnt & 0xE000u) && (reg16(io.data(), 0x0A) & 0x80u);
}

// Paints every pixel of `rgb` outside the centred 240x160 picture from the
// map at the game's own BG1 scroll (each row's registers; the rows above and
// below the picture use its first and last row). Black when the map cannot
// be trusted this frame. Returns true when the map was drawn.
inline bool paint_margins(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
                          const gsr::FieldScene& scene, const std::vector<std::uint8_t>& io,
                          const std::vector<std::uint8_t>& ewram,
                          const std::vector<std::uint8_t>& pal,
                          const std::uint8_t* rom, std::size_t rom_bytes) {
    if (width < 240u || height < 160u) return false;
    const std::uint32_t x0 = (width - 240u) / 2u, y0 = (height - 160u) / 2u;
    const auto row_io = [&](int y) -> const std::uint8_t* {
        y = std::clamp(y, 0, gsr::FieldScene::kRows - 1);
        return scene.row_io_valid[y] ? scene.row_io[y].data() : io.data();
    };
    const std::vector<std::uint8_t>& rows = map_rows(rom, rom_bytes);
    bool trusted = !rows.empty() && pal.size() >= 0x200u && io.size() >= 0x58u &&
                   ewram.size() >= kRing + kRingSlots * kRowBytes &&
                   reg16(ewram.data(), kFileSwitch) == 0u;
    // The rows on the console's own screen must be the game's own rows.
    for (int y = 0; y < gsr::FieldScene::kRows && trusted; ++y) {
        const std::uint8_t* r = row_io(y);
        const unsigned bldcnt = reg16(r, 0x50);
        if ((reg16(r, 0) & 0xE000u) || (((bldcnt >> 6) & 3u) == 1u && (bldcnt & 2u))) {
            trusted = false;
            break;
        }
        const int map_y = int(reg16(r, 0x16) & 0x1FFu) + y;
        if (map_y >= kHeight) continue;
        const int tile_row = map_y / 8;
        if (std::memcmp(ewram.data() + kRing + std::size_t(tile_row % kRingSlots) * kRowBytes,
                        rows.data() + std::size_t(tile_row) * kRowBytes, kRowBytes) != 0)
            trusted = false;
    }
    std::array<std::uint8_t, 256> to8{};
    for (int v = 0; v < 32; ++v) to8[v] = std::uint8_t((v * 255 + 15) / 31);
    for (std::uint32_t y = 0; y < height; ++y) {
        const int sy = int(y) - int(y0);
        const std::uint8_t* r = row_io(sy);
        const int hofs = int(reg16(r, 0x14) & 0x1FFu), vofs = int(reg16(r, 0x16) & 0x1FFu);
        const unsigned bldcnt = reg16(r, 0x50);
        const unsigned effect = (bldcnt >> 6) & 3u;
        const int evy = std::min<int>(r[0x54] & 0x1Fu, 16);
        const bool native_row = sy >= 0 && sy < 160;
        const int map_y = vofs + sy;
        std::uint8_t* dst = rgb + std::size_t(y) * width * 3u;
        for (std::uint32_t x = 0; x < width; ++x, dst += 3) {
            if (native_row && x >= x0 && x < x0 + 240u) continue;
            const int map_x = hofs + int(x) - int(x0);
            if (!trusted || map_x < 0 || map_x >= kWidth || map_y < 0 || map_y >= kHeight) {
                dst[0] = dst[1] = dst[2] = 0;
                continue;
            }
            const unsigned index = rows[std::size_t(map_y / 8) * kRowBytes +
                                        std::size_t(map_x / 8) * 64u +
                                        std::size_t(map_y % 8) * 8u + std::size_t(map_x % 8)];
            // Index 0 is the backdrop (palette 0), as on the console.
            const unsigned colour = reg16(pal.data(), index * 2u);
            int cr = int(colour & 31u);
            int g6 = int(((colour >> 4) & 62u) | (colour >> 15));
            int cb = int((colour >> 10) & 31u);
            // The graphics card's brightness step (field_scene_renderer.cpp).
            const bool target = (bldcnt >> (index ? 1u : 5u)) & 1u;
            if ((effect == 2u || effect == 3u) && evy != 0 && target) {
                if (effect == 2u) {
                    cr += ((31 - cr) * evy + 8) >> 4;
                    g6 += ((63 - g6) * evy + 8) >> 4;
                    cb += ((31 - cb) * evy + 8) >> 4;
                } else {
                    cr -= (cr * evy + 7) >> 4;
                    g6 -= (g6 * evy + 7) >> 4;
                    cb -= (cb * evy + 7) >> 4;
                }
            }
            dst[0] = to8[cr];
            dst[1] = to8[g6 >> 1];
            dst[2] = to8[cb];
        }
    }
    return trusted;
}

}  // namespace tla_flat_map
