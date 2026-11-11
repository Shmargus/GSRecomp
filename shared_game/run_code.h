// The run code, both games (Jimmy, 6 November 2026; docs/NEXT_PRIORITY_MODES.md
// section 5): one eight-character code carries a randomizer run's seed and
// its choices (Weapons, Armor, Items, Coins, Psynergy, Stats, Scaling,
// Bargains, Fair Play), so a friend who types it at New Game
// plays the same run. Difficulty and Challenge Mode are separate choices,
// not in it. Weapons and Armor are the ticks; Stats says the pieces' stats
// shuffle too. Fair Play is four choices, one per randomized thing (10
// November 2026): Psynergy, Items, Coins, Shops. They took two bits from the
// seed, so codes made before that day no longer read.
//
// 40 bits: seed (25, never 0), choices (12), check (3), mixed by a reversible
// 40-bit scramble so neighbouring seeds give unrelated codes; eight symbols
// of five bits from an alphabet without look-alikes (no I, O, 0, 1).
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
constexpr std::uint16_t kFairBits = kFairPsynergy | kFairItems | kFairCoins | kFairShops;
constexpr std::uint16_t kChoiceBits = 0xFFFu;
constexpr std::uint32_t kSeedBits = 0x01FFFFFFu;

struct Run {
    std::uint32_t seed = 0;
    std::uint16_t choices = 0;
};

constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
constexpr int kSymbols = 32, kLength = 8;
constexpr std::uint64_t kMask = (std::uint64_t{1} << 40) - 1u;
// Odd multiplier and its inverse modulo 2^40.
constexpr std::uint64_t kMul = 0x9E3779B97Full & kMask;

constexpr std::uint64_t inverse_mul() {
    std::uint64_t x = kMul;  // Newton: x = x * (2 - a x), doubling bits each step
    for (int i = 0; i < 6; ++i) x = (x * (2u - kMul * x)) & kMask;
    return x;
}
constexpr std::uint64_t kMulInv = inverse_mul();
static_assert(((kMul * kMulInv) & kMask) == 1u, "scramble must be reversible");

constexpr std::uint8_t check_of(std::uint32_t seed, std::uint16_t choices) {
    std::uint32_t h = seed ^ (std::uint32_t{choices} * 0x45D9F3Bu);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return static_cast<std::uint8_t>(h & 7u);
}

constexpr std::uint64_t scramble(std::uint64_t v) {
    v = (v * kMul) & kMask;
    v ^= v >> 21;
    v = (v * kMul) & kMask;
    return v ^ (v >> 21);
}
constexpr std::uint64_t unscramble(std::uint64_t v) {
    v ^= v >> 21;  // undoes v ^= v >> 21 for 40 bits (21 * 2 > 40)
    v = (v * kMulInv) & kMask;
    v ^= v >> 21;
    return (v * kMulInv) & kMask;
}

inline std::string encode(const Run& r) {
    const std::uint16_t choices = r.choices & kChoiceBits;
    const std::uint32_t seed = r.seed & kSeedBits;
    std::uint64_t v = std::uint64_t{seed} << 15 | std::uint64_t{choices} << 3 | check_of(seed, choices);
    v = scramble(v);
    std::string out;
    for (int i = kLength - 1; i >= 0; --i) out += kAlphabet[(v >> (i * 5)) & 31u];
    return out;
}

inline int symbol_of(char c) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    for (int i = 0; i < kSymbols; ++i)
        if (kAlphabet[i] == c) return i;
    return -1;
}

// False for a wrong length, a symbol outside the alphabet, a failed check or
// seed 0.
inline bool decode(const std::string& code, Run* out) {
    if (code.size() != static_cast<std::size_t>(kLength)) return false;
    std::uint64_t v = 0;
    for (const char c : code) {
        const int s = symbol_of(c);
        if (s < 0) return false;
        v = v << 5 | static_cast<std::uint64_t>(s);
    }
    v = unscramble(v);
    const auto seed = static_cast<std::uint32_t>(v >> 15);
    const auto choices = static_cast<std::uint16_t>((v >> 3) & kChoiceBits);
    if (seed == 0 || check_of(seed, choices) != (v & 7u)) return false;
    if (out) *out = {seed, choices};
    return true;
}

// A code as people type or paste it: spaces, dashes and line breaks are
// ignored, so "ABCD EFGH" copied from the screen reads as ABCDEFGH.
inline bool decode_typed(const std::string& text, Run* out) {
    std::string code;
    for (const char c : text)
        if (c != ' ' && c != '-' && c != '\t' && c != '\r' && c != '\n') code += c;
    return decode(code, out);
}

// "ABCD EFGH": the code in two halves, as the screens show it.
inline std::string spaced(const std::string& code) {
    return code.size() == static_cast<std::size_t>(kLength) ? code.substr(0, 4) + " " + code.substr(4) : code;
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
    // The typed symbols with the rest as underscores, in two halves.
    std::string shown() const {
        std::string s = typed;
        while (s.size() < static_cast<std::size_t>(kLength)) s += '_';
        return spaced(s);
    }
};

}  // namespace gsr::run_code
