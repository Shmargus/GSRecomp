// rom_unpack.h -- the games' asset unpacker, both formats, read from the ROM
// on the host. The same routine in both games (The Broken Seal 0x08002544,
// The Lost Age's ARM copy 0x080128A8): a type byte, then
//   1: the byte-flag format of 0x08001B70 (gbarecomp::lz_decompress);
//   0: a bit stream read lowest bit first: 1 + 8 bits is a literal byte,
//      0 + a length code + a distance is a copy of earlier output.
// Checked byte for byte against the game's own output (two The Lost Age
// captures) and against every asset of every supported ROM of both games.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "world_map_source.h"

namespace gsr {

// Unpacks the item at `src` (at most `avail` bytes readable) into `out`.
// False on data that does not unpack, runs past `avail` or grows past
// `max_bytes`.
inline bool rom_unpack(const std::uint8_t* src, std::size_t avail,
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
            // 0x20 in as many bits as (written - 0x21) needs, 12 once that
            // reaches 0x800 (The Lost Age 0x08012A00).
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

}  // namespace gsr
