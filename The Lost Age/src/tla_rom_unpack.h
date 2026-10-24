// tla_rom_unpack.h -- the cartridge's asset table and its unpacker, for
// host-side margin artwork (tla_flat_map.h, tla_world_climates.h).
//
// Evidence (ROM SHA-1 b500663220cb9bf56b9f8e8c0c544f1d6fa3a824):
//  * asset n is the pointer at 0x08680000 + 4 * n (0x08013300).
//  * 0x0801587C is a RAM copy of the ARM unpacker 0x080128A8. Checked byte
//    for byte against the game's own output: the 22 flat-map rows of F12
//    0072 and the eight staged climate blocks of F12 0073 (21 October 2026).
//    On 22 October two independent ports agreed on all 64 inputs (file 0x24's
//    48 rows, the four climate sets' blocks 3..6); this is the shorter one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "world_map_source.h"

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
    if (!rom || !word(rom, rom_bytes, 0x08680000u + 4u * n, &p) ||
        (p >> 24) < 0x08u || p - 0x08000000u >= rom_bytes)
        return false;
    *off = p - 0x08000000u;
    return true;
}

// The 0x080128A8 unpacker: a type byte, 1 for the byte-flag format of
// 0x08001B70, 0 for a bit stream read lowest bit first: 1 + 8 bits is a
// literal; 0 + a length code + a distance is a copy (see below).
inline bool unpack(const std::uint8_t* src, std::size_t avail,
                   std::vector<std::uint8_t>* out, std::size_t max_bytes) {
    out->clear();
    if (avail < 1) return false;
    if (src[0] == 1) return gbarecomp::lz_decompress(src + 1, avail - 1, out, max_bytes);
    if (src[0] != 0) return false;
    std::size_t pos = 1;
    std::uint32_t buf = 0;
    int have = 0;
    bool ok = true;
    const auto bits = [&](int n) -> unsigned {
        while (have < n) {
            if (pos >= avail) { ok = false; return 0; }
            buf |= std::uint32_t(src[pos++]) << have;
            have += 8;
        }
        const unsigned v = buf & ((1u << n) - 1u);
        buf >>= n;
        have -= n;
        return v;
    };
    for (;;) {
        if (bits(1)) {
            out->push_back(std::uint8_t(bits(8)));
        } else {
            unsigned length;
            if (!bits(1)) length = 2;
            else if (!bits(1)) length = 3;
            else if (!bits(1)) length = 4;
            else if (!bits(1)) length = 5;
            else if (!bits(1)) length = bits(1) ? 7 : 6;
            else if (const unsigned two = bits(2)) length = two + 7;
            else if (const unsigned seven = bits(7)) length = seven + 10;
            else return ok;  // the end marker
            // Distance back from the last byte written: 5 bits, or past
            // 0x20 in as many bits as (written - 0x21) needs, 12 once
            // that reaches 0x800 (0x08012A00).
            std::size_t back;
            if (bits(1)) {
                back = bits(5);
            } else {
                const std::size_t w = out->size() - 0x21;
                int n = 12;
                if (out->size() >= 0x21 && w < 0x800)
                    for (n = 0; (w >> n) != 0; ++n) {}
                back = bits(n) + 0x20;
            }
            if (!ok || back >= out->size()) return false;
            for (unsigned i = 0; i < length; ++i)
                out->push_back((*out)[out->size() - 1 - back]);
        }
        if (!ok || out->size() > max_bytes) return false;
    }
}

}  // namespace tla_rom
