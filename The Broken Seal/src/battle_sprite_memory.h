#pragma once

#include <cstdint>

namespace gsr::custom_battle::sprites {

// GS1 08003FA4 / 08003F3C: 96 {u16 size, u16 byte offset} grants.
// The original allocator owns the first page; only its failed fighter
// requests use the engine's existing second 32 KiB page. FACTS, F12 0063.
// The allocator's ROM addresses are GS1's and live with its caller
// (custom_battle_tbs.cpp); this header is shared with The Lost Age.
inline constexpr std::uint32_t kGrants = 0x03001B10u;
inline constexpr unsigned kOwners = 0x60u;
inline constexpr unsigned kPageStart = 0x8000u, kPageEnd = 0x10000u;
inline constexpr unsigned kBlock = 64u;

// Palette-bank bits are unused by 8bpp OBJ. Carry the extra tile bit in
// bit 15 so it follows the game's own OAM buffering, sorting and savestates.
// Only our extended fighter grants write this marker; 4bpp is untouched.
inline constexpr unsigned kPageMarker = 0x8000u;
inline int tile(std::uint16_t attr0, std::uint16_t attr2) {
    return (attr0 & 0x2000u) && (attr2 & kPageMarker)
        ? int(0x400u | (attr2 & 0x3FFu)) : -1;
}

inline unsigned rounded(unsigned bytes) { return (bytes + kBlock - 1) / kBlock * kBlock; }

// A fighter request the extra page may serve.
inline bool fighter_request(unsigned owner, unsigned bytes) {
    return owner < kOwners && bytes > 0x10u && bytes <= 0x2000u;
}

// `owner`'s grant is held (not the empty size or the 0xFFFF free offset).
template<class Read16>
bool live_grant(unsigned owner, Read16 read, std::uint32_t grants = kGrants) {
    const unsigned entry = grants + owner * 4u;
    return read(entry) > 0x10u && read(entry + 2u) != 0xFFFFu;
}

// `owner`'s grant is held and lies on the extra page.
template<class Read16>
bool extra_grant(unsigned owner, Read16 read, std::uint32_t grants = kGrants) {
    return live_grant(owner, read, grants) && read(grants + owner * 4u + 2u) >= kPageStart;
}

template<class Read16>
int first_fit(unsigned owner, unsigned bytes, Read16 read, std::uint32_t grants = kGrants) {
    if (!fighter_request(owner, bytes)) return -1;
    const unsigned need = rounded(bytes);
    for (unsigned at = kPageStart; at + need <= kPageEnd;) {
        unsigned next = at;
        for (unsigned other = 0; other < kOwners; ++other) {
            if (other == owner || !extra_grant(other, read, grants)) continue;
            const unsigned entry = grants + other * 4u;
            const unsigned size = read(entry), from = read(entry + 2u);
            const unsigned end = from + rounded(size);
            if (at < end && from < at + need && end > next) next = end;
        }
        if (next == at) return int(at);
        at = next;
    }
    return -1;
}

}  // namespace gsr::custom_battle::sprites
