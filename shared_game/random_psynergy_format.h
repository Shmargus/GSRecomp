// The run's eight save-payload bytes (first Random Psynergy's): the run's
// seed and which seed-driven modes it has besides Random Equipment (whose
// own block is random_weapons_format.h). No engine dependencies, so save
// readers outside the game can use it too (The Lost Age's Golden Sun save
// import, gs_save.cpp).
//   +0..3 seed (never 0)   +4 'R'   +5 version (low nibble), more flags
//   (high nibble)   +6 flags   +7 check
// Flags (6 and 7 November 2026): bit 0 Fair Play, bit 1 Random Psynergy,
// bit 2 Random Items (chests, gifts, shops), bit 3 Bargains (shop prices),
// bit 4 Random Coins, bit 5 Scaled difficulty (every battle's enemies rise
// to the party's level), bit 6 Random Weapons finds, bit 7 Random Armor
// finds (chests and gifts holding a weapon, or armour, give a random piece).
// More flags (version 3, 7 November 2026): bit 0 was Key Items, retired 8
// November 2026 (key items never move): read and ignored, never written, so
// a save made with it still loads. Version 2 bytes read with no more flags.
// Version 4 (10 November 2026): Fair Play is a switch per randomized thing.
// Flags bit 0 is Fair Psynergy; more flags bit 1 Fair Items, bit 2 Fair
// Coins, bit 3 Fair Shops. In older versions bit 0 was the one Fair Play
// switch and reads as all four. All zero
// is Off, as every existing save is. A randomized Broken Seal run goes on to
// The Lost Age with these bytes (Jimmy, 7 November 2026).
#pragma once
#include <array>
#include <cstdint>

namespace gsr::random_psynergy {

constexpr std::uint8_t kTag = 'R', kVersion = 4, kKeyItemsVersion = 3, kOldVersion = 2;
constexpr std::uint8_t kFairPlayFlag = 1, kPsynergyFlag = 2, kTreasureFlag = 4, kBargainsFlag = 8,
                       kCoinsFlag = 16, kScaledFlag = 32, kWeaponFindsFlag = 64, kArmourFindsFlag = 128;
constexpr std::uint8_t kFlags = kFairPlayFlag | kPsynergyFlag | kTreasureFlag | kBargainsFlag | kCoinsFlag |
                                kScaledFlag | kWeaponFindsFlag | kArmourFindsFlag;
// Byte +5's high nibble.
constexpr std::uint8_t kRetiredKeyItemsFlag = 1, kFairItemsFlag = 2, kFairCoinsFlag = 4, kFairShopsFlag = 8;

struct Config {
    std::uint32_t seed = 0;  // 0 = no run
    bool psynergy = false;
    bool fair_psynergy = false;  // Fair Play: a hero learns an early heal
    bool treasure = false;  // Random Items: chests, gifts and shops
    bool bargains = false;
    bool coins = false;     // Random Coins
    bool scaled = false;    // Scaled difficulty
    bool weapon_finds = false, armour_finds = false;  // Random Weapons / Armor finds
    // Fair Play's other switches (treasure_plan.h): chests and gifts, coin
    // amounts, shop stock.
    bool fair_items = false, fair_coins = false, fair_shops = false;
    bool operator==(const Config&) const = default;
};

inline std::array<std::uint8_t, 8> encode(const Config& c) {
    if (c.seed == 0) return {};
    const auto flags = static_cast<std::uint8_t>((c.fair_psynergy ? kFairPlayFlag : 0) |
                                                 (c.psynergy ? kPsynergyFlag : 0) |
                                                 (c.treasure ? kTreasureFlag : 0) |
                                                 (c.bargains ? kBargainsFlag : 0) |
                                                 (c.coins ? kCoinsFlag : 0) | (c.scaled ? kScaledFlag : 0) |
                                                 (c.weapon_finds ? kWeaponFindsFlag : 0) |
                                                 (c.armour_finds ? kArmourFindsFlag : 0));
    const auto more = static_cast<std::uint8_t>((c.fair_items ? kFairItemsFlag : 0) |
                                                (c.fair_coins ? kFairCoinsFlag : 0) |
                                                (c.fair_shops ? kFairShopsFlag : 0));
    std::array<std::uint8_t, 8> b = {
        static_cast<std::uint8_t>(c.seed), static_cast<std::uint8_t>(c.seed >> 8),
        static_cast<std::uint8_t>(c.seed >> 16), static_cast<std::uint8_t>(c.seed >> 24),
        kTag, static_cast<std::uint8_t>(kVersion | more << 4), flags, 0};
    std::uint8_t check = 0x5Au;
    for (int i = 0; i < 7; ++i) check ^= b[i];
    b[7] = check;
    return b;
}

inline Config decode(const std::uint8_t b[8]) {
    std::uint8_t check = 0x5Au;
    for (int i = 0; i < 7; ++i) check ^= b[i];
    const std::uint8_t version = b[5] & 0xFu, more = b[5] >> 4;
    const std::uint8_t allowed = version == kVersion ? kFairItemsFlag | kFairCoinsFlag | kFairShopsFlag
                               : version == kKeyItemsVersion ? kRetiredKeyItemsFlag
                               : version == kOldVersion ? 0 : 0xFF;
    if (b[4] != kTag || allowed == 0xFF || (b[6] & ~kFlags) != 0 || (more & ~allowed) != 0 || b[7] != check)
        return {};
    Config c;
    c.seed = b[0] | b[1] << 8 | b[2] << 16 | static_cast<std::uint32_t>(b[3]) << 24;
    c.fair_psynergy = (b[6] & kFairPlayFlag) != 0;
    // Before version 4 the one Fair Play switch covered everything.
    const bool each = version == kVersion;
    c.fair_items = each ? (more & kFairItemsFlag) != 0 : c.fair_psynergy;
    c.fair_coins = each ? (more & kFairCoinsFlag) != 0 : c.fair_psynergy;
    c.fair_shops = each ? (more & kFairShopsFlag) != 0 : c.fair_psynergy;
    c.psynergy = (b[6] & kPsynergyFlag) != 0;
    c.treasure = (b[6] & kTreasureFlag) != 0;
    c.bargains = (b[6] & kBargainsFlag) != 0;
    c.coins = (b[6] & kCoinsFlag) != 0;
    c.scaled = (b[6] & kScaledFlag) != 0;
    c.weapon_finds = (b[6] & kWeaponFindsFlag) != 0;
    c.armour_finds = (b[6] & kArmourFindsFlag) != 0;
    return c;
}

}  // namespace gsr::random_psynergy
