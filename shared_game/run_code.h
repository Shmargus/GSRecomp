// The run code, both games. One nine-character code carries a randomizer
// run's seed and its choices, so a friend who types it at New Game plays the
// same run. The choices are Weapons, Armor, Items, Coins, Psynergy (by hero or
// by class), Enemy skills, Djinn, Stats, Scaling, Bargains and Fair Play.
// Difficulty and Challenge Mode are separate choices, not in the code.
//
// Notes on the choices:
//   - Weapons and Armor are the ticks; Stats says the pieces' stats shuffle
//     too.
//   - Fair Play is four choices, one per randomized thing: Psynergy, Items,
//     Coins, Shops. Their bits come out of the seed's share of the code (the
//     layout below).
//
// The code is 45 bits:
//   - seed (25 bits, never 0), choices (16), check (4);
//   - mixed by a reversible 45-bit scramble, so neighbouring seeds give
//     unrelated codes;
//   - written as nine symbols of five bits, from an alphabet without
//     look-alikes (no I, O, 0, 1).
// Older codes have eight symbols: 40 bits, with choices (12) and check (3).
// They still decode (no Enemy skills).
//
// Choice bit 15 marks the pooled treasure planner. Older codes leave it clear.
// Pooled codes for the regional artifact planner flip the highest check bit.
//
// The letter grid (Entry) is the shared logic of the code's typing screen;
// each game paints it with its own window and font.
#pragma once
#include <cstdint>
#include <string>

namespace gsr::run_code {

enum Choice : std::uint8_t {
    kPsynergy = 1u, kWeapons = 2u, kScaling = 4u, kFairPsynergy = 8u, kItems = 16u, kBargains = 32u,
    kArmor = 64u, kCoins = 128u,
};
constexpr std::uint16_t kStats = 256u, kFairItems = 512u, kFairCoins = 1024u, kFairShops = 2048u;
constexpr std::uint16_t kEnemySkills = 4096u;  // with Psynergy: enemy skills join the draw
constexpr std::uint16_t kByClass = 8192u;      // with Psynergy: every class has its own list
constexpr std::uint16_t kDjinn = 16384u;       // Random Djinn
constexpr std::uint16_t kPooledLoot = 32768u;  // pooled treasure planner
constexpr std::uint16_t kFairBits = kFairPsynergy | kFairItems | kFairCoins | kFairShops;
constexpr std::uint16_t kChoiceBits = 0x7FFFu, kOldChoiceBits = 0xFFFu;
constexpr std::uint32_t kSeedBits = 0x01FFFFFFu;

struct Run {
    std::uint32_t seed = 0;
    std::uint16_t choices = 0;
    bool pooled_loot = false;
    bool short_code = false;  // preserve the layout of decoded eight-symbol codes
    bool regional_artifacts = false;
};

constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int kSymbols = 32, kLength = 9, kOldLength = 8;

// One code layout: its width in bits, choice and check bits, and the
// scramble's odd multiplier (with its inverse modulo 2^bits) and shift
// (more than half the width, so one xor undoes itself).
struct Layout {
    int bits, choice_bits, check_bits, shift;
    std::uint64_t mul;
    constexpr std::uint64_t mask() const { return (std::uint64_t{1} << bits) - 1u; }
    constexpr std::uint64_t mul_inv() const {
        std::uint64_t x = mul;  // Newton: x = x * (2 - a x), doubling bits each step
        for (int i = 0; i < 6; ++i) x = (x * (2u - mul * x)) & mask();
        return x;
    }
};
constexpr Layout kLayout{45, 16, 4, 23, 0x9E3779B97F4Bull & ((std::uint64_t{1} << 45) - 1u)};
constexpr Layout kOldLayout{40, 12, 3, 21, 0x9E3779B97Full & ((std::uint64_t{1} << 40) - 1u)};
constexpr std::uint8_t kRegionalCheckBit = 1u << (kLayout.check_bits - 1);
static_assert(((kLayout.mul * kLayout.mul_inv()) & kLayout.mask()) == 1u, "scramble must be reversible");
static_assert(((kOldLayout.mul * kOldLayout.mul_inv()) & kOldLayout.mask()) == 1u, "scramble must be reversible");
static_assert(25 + kLayout.choice_bits + kLayout.check_bits == kLayout.bits && kLayout.bits == 5 * kLength);
static_assert(25 + kOldLayout.choice_bits + kOldLayout.check_bits == kOldLayout.bits &&
              kOldLayout.bits == 5 * kOldLength);

constexpr std::uint8_t check_of(std::uint32_t seed, std::uint16_t choices, int check_bits) {
    std::uint32_t h = seed ^ (std::uint32_t{choices} * 0x45D9F3Bu);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return static_cast<std::uint8_t>(h & ((1u << check_bits) - 1u));
}

constexpr std::uint64_t scramble(std::uint64_t v, const Layout& l) {
    v = (v * l.mul) & l.mask();
    v ^= v >> l.shift;
    v = (v * l.mul) & l.mask();
    return v ^ (v >> l.shift);
}
constexpr std::uint64_t unscramble(std::uint64_t v, const Layout& l) {
    v ^= v >> l.shift;
    v = (v * l.mul_inv()) & l.mask();
    v ^= v >> l.shift;
    return (v * l.mul_inv()) & l.mask();
}

inline std::string encode(const Run& r) {
    const bool old = r.short_code && !r.pooled_loot && (r.choices & ~kOldChoiceBits) == 0;
    const Layout& l = old ? kOldLayout : kLayout;
    const std::uint16_t choices = (r.choices & kChoiceBits) | (r.pooled_loot ? kPooledLoot : 0u);
    const std::uint32_t seed = r.seed & kSeedBits;
    const std::uint8_t check = check_of(seed, choices, l.check_bits) ^
                               (r.pooled_loot && r.regional_artifacts ? kRegionalCheckBit : 0u);
    std::uint64_t v = std::uint64_t{seed} << (l.choice_bits + l.check_bits) |
                      std::uint64_t{choices} << l.check_bits | check;
    v = scramble(v, l);
    std::string out;
    for (int i = l.bits / 5 - 1; i >= 0; --i) out += kAlphabet[(v >> (i * 5)) & 31u];
    return out;
}

inline int symbol_of(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (int i = 0; i < kSymbols; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

// Nine symbols, or an older code's eight. False for another length, a
// symbol outside the alphabet, a failed check or seed 0.
inline bool decode(const std::string& code, Run* out) {
    const bool old = code.size() == static_cast<std::size_t>(kOldLength);
    if (!old && code.size() != static_cast<std::size_t>(kLength)) return false;
    const Layout& l = old ? kOldLayout : kLayout;
    std::uint64_t v = 0;
    for (const char c : code) {
        const int s = symbol_of(c);
        if (s < 0) return false;
        v = v << 5 | static_cast<std::uint64_t>(s);
    }
    v = unscramble(v, l);
    const auto seed = static_cast<std::uint32_t>(v >> (l.choice_bits + l.check_bits));
    const auto choices = static_cast<std::uint16_t>((v >> l.check_bits) & ((1u << l.choice_bits) - 1u));
    const bool pooled = (choices & kPooledLoot) != 0;
    const auto check = v & ((1u << l.check_bits) - 1u);
    const auto expected = check_of(seed, choices, l.check_bits);
    const bool regional = pooled && check == (expected ^ kRegionalCheckBit);
    if (seed == 0 || (check != expected && !regional)) return false;
    if (out) *out = {seed, static_cast<std::uint16_t>(choices & kChoiceBits), pooled, old, regional};
    return true;
}

// A code as people type or paste it: spaces, dashes and line breaks are
// ignored, so "ABC DEF GHJ" copied from the screen reads as ABCDEFGHJ.
inline bool decode_typed(const std::string& text, Run* out) {
    std::string code;
    for (const char c : text)
        if (c != ' ' && c != '-' && c != '\t' && c != '\r' && c != '\n') code += c;
    return decode(code, out);
}

// "ABC DEF GHJ": the code in threes, as the screens show it.
inline std::string spaced(const std::string& code) {
    return code.size() == static_cast<std::size_t>(kLength)
               ? code.substr(0, 3) + " " + code.substr(3, 3) + " " + code.substr(6)
               : code;
}

// --- the letter grid ------------------------------------------------------------
// Four rows of eight symbols, then a row of three actions. D-pad moves
// (wrapping), A types the symbol or does the action, B deletes the last
// symbol (or, with none typed, leaves without a change), Start is OK.
constexpr int kColumns = 8, kGridRows = 4, kActions = 3;
enum Action { kDelete, kRandom, kOk };
constexpr const char* kActionLabels[kActions] = {"Erase", "New seed", "OK"};

enum class Result { None, Typed, Cancel, Accepted, Random, Invalid };

struct Entry {
    std::string typed;
    int row = 0, column = 0;  // row kGridRows: the actions, column 0..2
    bool invalid = false;     // the last OK was refused (shown until a change)

    void open() { *this = {}; }
    int columns() const { return row == kGridRows ? kActions : kColumns; }
    void move(int dx, int dy) {
        if (dy) {
            const int before_columns = columns();
            row = (row + dy + kGridRows + 1) % (kGridRows + 1);
            // Moving between the letters and the three actions keeps the side.
            if (columns() != before_columns)
                column = column * columns() / before_columns;
        }
        if (dx) column = (column + dx + columns()) % columns();
    }
    // A, B or Start; Accepted fills *out.
    Result press(bool a, bool b, bool start, Run* out) {
        if (b) {
            if (typed.empty()) return Result::Cancel;
            typed.pop_back();
            invalid = false;
            return Result::Typed;
        }
        const bool ok = start || (a && row == kGridRows && column == kOk);
        if (ok) {
            if (decode(typed, out)) return Result::Accepted;
            invalid = true;
            return Result::Invalid;
        }
        if (!a) return Result::None;
        if (row == kGridRows) {
            if (column == kRandom) return Result::Random;
            if (!typed.empty()) typed.pop_back();
            invalid = false;
            return Result::Typed;
        }
        if (typed.size() < static_cast<std::size_t>(kLength)) {
            typed += kAlphabet[row * kColumns + column];
            invalid = false;
            // The last symbol moves the cursor to OK.
            if (typed.size() == static_cast<std::size_t>(kLength)) {
                row = kGridRows;
                column = kOk;
            }
        }
        return Result::Typed;
    }
    // The typed symbols with the rest as underscores, in threes.
    std::string shown() const {
        std::string s = typed;
        while (s.size() < static_cast<std::size_t>(kLength)) s += '_';
        return spaced(s);
    }
};

}  // namespace gsr::run_code
