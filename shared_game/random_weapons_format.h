// Random Equipment's eight save-payload bytes (random_weapons.h has the
// rules; first Random Weapons).
// No engine dependencies, so save readers outside the game can use it too
// (The Lost Age's Golden Sun save import, gs_save.cpp).
//   +0..3 seed (never 0)   +4 'W'   +5 version   +6 flags   +7 check
// Flags: bit 0 Scaling, bit 1 armour, bit 2 weapons (7 November 2026: the
// two are separate choices).
// All zero is Off, as every existing save is.
#pragma once
#include <array>
#include <cstdint>

namespace gsr::random_weapons {

constexpr std::uint8_t kTag = 'W', kVersion = 1, kScalingFlag = 1, kArmourFlag = 2, kWeaponsFlag = 4;
constexpr std::uint8_t kFlags = kScalingFlag | kArmourFlag | kWeaponsFlag;

struct Config {
    std::uint32_t seed = 0;  // 0 = Off
    bool scaling = false;
    bool armour = false;
    bool weapons = false;
};

inline std::array<std::uint8_t, 8> encode(const Config& c) {
    if (c.seed == 0) return {};
    std::array<std::uint8_t, 8> b = {
        static_cast<std::uint8_t>(c.seed), static_cast<std::uint8_t>(c.seed >> 8),
        static_cast<std::uint8_t>(c.seed >> 16), static_cast<std::uint8_t>(c.seed >> 24),
        kTag, kVersion,
        static_cast<std::uint8_t>((c.scaling ? kScalingFlag : 0) | (c.armour ? kArmourFlag : 0) |
                                  (c.weapons ? kWeaponsFlag : 0)), 0};
    std::uint8_t check = 0x5Au;
    for (int i = 0; i < 7; ++i) check ^= b[i];
    b[7] = check;
    return b;
}

inline Config decode(const std::uint8_t b[8]) {
    std::uint8_t check = 0x5Au;
    for (int i = 0; i < 7; ++i) check ^= b[i];
    if (b[4] != kTag || b[5] != kVersion || (b[6] & ~kFlags) != 0 || b[7] != check) return {};
    Config c;
    c.seed = b[0] | b[1] << 8 | b[2] << 16 | static_cast<std::uint32_t>(b[3]) << 24;
    c.scaling = (b[6] & kScalingFlag) != 0;
    c.armour = (b[6] & kArmourFlag) != 0;
    c.weapons = (b[6] & kWeaponsFlag) != 0;
    return c;
}

}  // namespace gsr::random_weapons
