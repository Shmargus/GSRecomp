#include "gs_save.h"
#include "../../shared_game/challenge_format.h"
#include "../../shared_game/random_psynergy_format.h"
#include "../../shared_game/random_weapons_format.h"
#include "../../shared_game/save_files.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace gs_save {
namespace {
constexpr std::size_t kSectorBytes = 0x1000, kHeaderBytes = 0x10;
unsigned halfword(const std::uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }
bool valid_sector(const std::uint8_t* p) {
    if (std::memcmp(p, "CAMELOT", 7) != 0 || p[7] >= 16) return false;
    unsigned sum = 0;
    // GS1 0x08005AE0 sums 0xFE8 bytes, excluding the final eight bytes.
    for (std::size_t i = kHeaderBytes; i < 0xFF8; ++i) sum += p[i];
    return (sum & 0xFFFF) == halfword(p + 8);
}
bool character_name(const std::uint8_t* p) {
    // GS1's records start at +0x500; TLA's start at +0x520. Reject a
    // differently laid-out save rather than importing its preceding fields.
    if (p[0] == 0) return false;
    for (unsigned i = 0; i < 15; ++i) {
        if (p[i] == 0) return true;
        if (p[i] < 0x20) return false;
    }
    return false;
}

// File `number` from its two blocks (GS1 loads blocks i and i+3, 0x0802092C).
void add_cleared(std::vector<ClearSlot>& out, unsigned number,
                 const std::uint8_t* p, const std::uint8_t* second) {
    if (!p || !second || p[0x31] == 0) return;
    const auto* body = p + kHeaderBytes;
    bool names_ok = true;
    for (unsigned c = 0; c < 4; ++c)
        names_ok &= character_name(body + 0x500 + c * 0x14C);
    if (!names_ok) return;
    // Challenge Mode files stay in The Broken Seal (Jimmy, 5 November 2026);
    // its bytes are payload +0x28 (runner_main.cpp's Challenge Mode install).
    // A randomized run (payload +0x30, +0x38) goes on (7 November 2026).
    if (gsr::challenge::decode(body + 0x28).size > 0) return;
    ClearSlot slot{};
    slot.slot = number;
    slot.level = p[0x1C];
    for (unsigned n = 0; n < 12 && p[0x10 + n]; ++n) {
        const auto ch = p[0x10 + n];
        slot.name += ch >= 0x20 && ch < 0x7F ? char(ch) : '?';
    }
    // Exactly TLA's link receiver (0x02008D0E): flags/coins keep their
    // GS1 offsets; the following block moves forward 0x20 bytes.
    // Its four character records then land at +0x520, as expected by
    // TLA's common importer 0x080AE410. The game rebuilds their stats.
    std::copy_n(body, 0x254, slot.transfer.begin());
    std::copy_n(body + 0x240, 0xD20, slot.transfer.begin() + 0x260);
    out.push_back(std::move(slot));
}
}  // namespace

std::vector<ClearSlot> cleared_slots(const std::vector<std::uint8_t>& bytes,
                                     const std::vector<std::uint8_t>& more) {
    std::vector<ClearSlot> out;
    if (bytes.size() != kFileBytes) return out;
    std::array<const std::uint8_t*, 16> latest{};
    for (std::size_t i = 0; i < kFileBytes; i += kSectorBytes) {
        const auto* p = bytes.data() + i;
        if (!valid_sector(p)) continue;
        auto& previous = latest[p[7]];
        // Match GS1 0x080057C8: larger generation wins; ties keep first.
        if (!previous || halfword(p + 10) > halfword(previous + 10)) previous = p;
    }
    for (unsigned i = 0; i < 3; ++i) add_cleared(out, i + 1, latest[i], latest[i + 3]);
    // Files 4..9: a 16-byte header, then each file's two blocks as sector
    // images, in file order.
    // (The Broken Seal's save_files_tbs.h writes them; save_files.h the header.)
    namespace sf = gsr::save_files;
    constexpr std::size_t kMoreFiles = sf::kFiles - sf::kRows;
    if (more.size() >= sf::kHeaderBytes + kMoreFiles * 2 * kSectorBytes &&
        sf::header_valid(more.data())) {
        for (std::size_t f = 0; f < kMoreFiles; ++f) {
            const auto* first = more.data() + sf::kHeaderBytes + f * 2 * kSectorBytes;
            const auto* second = first + kSectorBytes;
            add_cleared(out, unsigned(f + sf::kRows + 1), valid_sector(first) ? first : nullptr,
                        valid_sector(second) ? second : nullptr);
        }
    }
    return out;
}
}  // namespace gs_save
