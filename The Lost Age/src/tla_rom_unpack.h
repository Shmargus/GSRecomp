// tla_rom_unpack.h -- the cartridge's asset table, for host-side margin
// artwork (tla_flat_map.h, tla_world_climates.h). Assets unpack with
// gsr::rom_unpack (shared_game/rom_unpack.h), the game's 0x080128A8.
//
// Evidence (ROM SHA-1 b500663220cb9bf56b9f8e8c0c544f1d6fa3a824): asset n is
// the pointer at 0x08680000 + 4 * n (0x08013300).
#pragma once

#include "tla_rom_version.h"
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../shared_game/rom_unpack.h"

namespace tla_rom {

// Little-endian word at cartridge address `addr`; false past the ROM.
inline bool word(const std::uint8_t* rom, std::size_t rom_bytes, std::uint32_t addr,
                 std::uint32_t* v) {
    const std::uint32_t off = addr - 0x08000000u;
    if ((addr >> 24) < 0x08u || std::size_t(off) + 4 > rom_bytes) return false;
    *v = rom[off] | (rom[off + 1] << 8) | (rom[off + 2] << 16) |
         (std::uint32_t(rom[off + 3]) << 24);
    return true;
}

// ROM offset of asset `n`.
inline bool asset_offset(const std::uint8_t* rom, std::size_t rom_bytes, std::uint32_t n,
                         std::size_t* off) {
    std::uint32_t p = 0;
    if (!rom || !word(rom, rom_bytes, TLA_ROM(0x08680000u) + 4u * n, &p) ||
        (p >> 24) < 0x08u || p - 0x08000000u >= rom_bytes)
        return false;
    *off = p - 0x08000000u;
    return true;
}

}  // namespace tla_rom
