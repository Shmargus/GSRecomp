// Challenge Mode's four save-payload bytes (challenge_mode.h has the rules).
// No engine dependencies, so save readers outside the game can use it too
// (The Lost Age's Golden Sun save import, gs_save.cpp).
//   +0  0xC0 | number of picks (1..4)
//   +1  first pick << 4 | second pick   (0xF = none)
//   +2  third pick << 4 | fourth pick
//   +3  check: +0 ^ +1 ^ +2 ^ 0x5A
// All zero is Off, as every existing save is.
#pragma once
#include <array>
#include <cstdint>

namespace gsr::challenge {

constexpr int kMaxPicks = 4;
constexpr std::uint8_t kNone = 0xFu;

struct Config {
    int size = 0;  // 0 = Off
    std::array<std::uint8_t, kMaxPicks> picks{kNone, kNone, kNone, kNone};
};

inline std::array<std::uint8_t, 4> encode(const Config& c) {
    if (c.size <= 0) return {0, 0, 0, 0};
    const std::uint8_t b0 = static_cast<std::uint8_t>(0xC0u | c.size);
    const std::uint8_t b1 = static_cast<std::uint8_t>(c.picks[0] << 4 | c.picks[1]);
    const std::uint8_t b2 = static_cast<std::uint8_t>(c.picks[2] << 4 | c.picks[3]);
    return {b0, b1, b2, static_cast<std::uint8_t>(b0 ^ b1 ^ b2 ^ 0x5Au)};
}

inline Config decode(const std::uint8_t b[4]) {
    Config c;
    const int size = b[0] & 0xF;
    if ((b[0] & 0xF0u) != 0xC0u || size < 1 || size > kMaxPicks ||
        b[3] != static_cast<std::uint8_t>(b[0] ^ b[1] ^ b[2] ^ 0x5Au))
        return c;
    c.picks = {static_cast<std::uint8_t>(b[1] >> 4), static_cast<std::uint8_t>(b[1] & 0xF),
               static_cast<std::uint8_t>(b[2] >> 4), static_cast<std::uint8_t>(b[2] & 0xF)};
    for (int i = 0; i < size; ++i)
        if (c.picks[i] == kNone) return {};
    c.size = size;
    return c;
}

}  // namespace gsr::challenge
