// crc32.h — the engine's one IEEE 802.3 CRC-32 (poly 0xEDB88320, init and
// final XOR 0xFFFFFFFF), the same flavor PNG / zip / Ethernet use. Header
// only and in armv4t (the lowest engine library) so every layer can include
// it without linking anything. Pairs with sha1.h for asset validation: the
// BIOS / ROM pickers quote both CRC32 and SHA-1 when an asset doesn't match;
// the recompiler, self-heal and runtime guards use it to fingerprint code.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gba {

namespace crc32_detail {

struct Table {
    uint32_t v[256];
    constexpr Table() : v{} {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            v[i] = c;
        }
    }
};

inline constexpr Table kTable{};

}  // namespace crc32_detail

// Continue a CRC-32 over `len` more bytes, zlib semantics: crc32_update(0,
// buf) is the CRC of buf, and crc32_update(crc32_update(0, a), b) equals the
// CRC of a followed by b.
inline uint32_t crc32_update(uint32_t crc, const void* data, std::size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = ~crc;
    for (std::size_t i = 0; i < len; ++i) {
        c = crc32_detail::kTable.v[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return ~c;
}

// Hash a byte buffer with CRC32-IEEE. Result matches `crc32` from `zlib`
// and the `crc32` field in PNG / zip headers.
inline uint32_t crc32(const void* data, std::size_t len) {
    return crc32_update(0, data, len);
}

}  // namespace gba
