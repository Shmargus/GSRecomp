#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace gsr::custom_battle {

// The native backdrop loader expands a 128-colour palette and 15 rows of
// 32 eight-pixel tiles. Its delta stream is already in picture order.
inline constexpr int kArenaWidth = 32 * 8, kArenaHeight = 15 * 8;
inline constexpr unsigned kArenaPaletteBytes = 128 * 2;

inline std::vector<std::uint32_t> decode_arena(std::span<const std::uint8_t> asset) {
    if (asset.size() < kArenaPaletteBytes) return {};
    std::uint32_t palette[128];
    for (unsigned i = 0; i < 128; ++i) {
        const unsigned colour = asset[2 * i] | (unsigned(asset[2 * i + 1]) << 8);
        const auto expand = [](unsigned v) { return (v << 3) | (v >> 2); };
        palette[i] = 0xFF000000u | (expand(colour & 31u) << 16)
                    | (expand((colour >> 5) & 31u) << 8) | expand((colour >> 10) & 31u);
    }
    std::vector<std::uint32_t> picture;
    picture.reserve(kArenaWidth * kArenaHeight);
    std::size_t next = kArenaPaletteBytes;
    unsigned bits = 0, available = 0, colour = 0;
    for (int pixel = 0; pixel < kArenaWidth * kArenaHeight; ++pixel) {
        while (available < 10 && next < asset.size()) {
            bits |= unsigned(asset[next++]) << available;
            available += 8;
        }
        const unsigned code = bits & 7u;
        unsigned used = 2;
        switch (code) {
        case 1: case 5: {
            const unsigned delta = 1 + ((bits >> 3) & 1u);
            colour = code == 1 ? colour + delta : colour - delta;
            used = 4;
            break;
        }
        case 3: case 7: {
            const unsigned delta = 3 + ((bits >> 3) & 7u);
            colour = code == 3 ? colour + delta : colour - delta;
            used = 6;
            break;
        }
        case 2: {
            const unsigned delta = 11 + ((bits >> 4) & 15u);
            colour = bits & 8u ? colour - delta : colour + delta;
            used = 8;
            break;
        }
        case 6:
            colour = (bits >> 3) & 127u;
            used = 10;
            break;
        }
        if (available < used) return {};
        bits >>= used;
        available -= used;
        colour &= 127u;
        picture.push_back(palette[colour]);
    }
    return picture;
}

}  // namespace gsr::custom_battle
