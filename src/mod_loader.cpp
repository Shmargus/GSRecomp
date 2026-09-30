// The mod loader. For now it holds one thing: a launcher test ("Mod test:
// Earth Surge", env GSR_MOD_FIELD_TEST) that adds a Psynergy in one of the
// game's empty slots by changing data tables in memory. Design:
// docs/features/MODDING.md.
//
// Evidence (FACTS.md, 2026-09-30):
//  - Psynergy data: one getter, 0x08078B9C, returns 0x0807EE58 + id * 0x10.
//    +1 move type (0x85 Psynergy), +2 element, +4 menu icon (confirmed in
//    play), +8 range, +9 PP, +0xA power. Empty slots are all zero, named "?".
//  - Battle effect: 0x080BF100 loads 0x080C2DA0 + id * 4 into the action's
//    +0x58 (confirmed in play: changing an entry changes the animation). Bit
//    0x4000 sends it to the weapon-swing starter 0x080E3A3C; the low bits
//    are the swing style (a normal attack uses 0x4000 | style from
//    0x080C23E8; Isaac's sword is style 1).
//  - Class learn lists: 0x08084B1C + class * 0x54, 16 u32 entries at
//    +0x10..+0x4F, byte 0 Psynergy id (0 = unused), byte 1 level (Plasma at
//    8, confirmed). 0x08078BF0 rebuilds a character's Psynergy list (+0x58)
//    from them, only when the class is recalculated (0x08079AE8: level-up,
//    Djinn change), not on load. Class type (+0) 1 is pure Venus, which only
//    Isaac reaches (types from 0x08088DB8).
// ROM offset = address - 0x08000000. Only the in-memory copy is changed; the
// ROM file on disk and its SHA-1 check are untouched.
#include "mod_loader.h"

#include <map>
#include <string>
#include <vector>

#include "gs_text_encode.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gsr::mods {
namespace {

constexpr std::size_t kPsynergyTable = 0x7EE58;
constexpr std::size_t kPsynergyStride = 0x10;
constexpr std::size_t kPsynergyEffectTable = 0xC2DA0;  // u32 per Psynergy
constexpr std::size_t kClassTable = 0x84B1C;
constexpr std::size_t kClassStride = 0x54;
constexpr std::size_t kClassLearnFirst = 0x10;
constexpr std::size_t kClassLearnEnd = 0x50;
// Classes before the first entry whose type word is not a small number
// (0xCC entries); the scan below stops at the first such entry anyway.
constexpr std::size_t kClassScanLimit = 0x100;

// Earth Surge, test version: a copy of Spire in empty slot 18 that plays
// Isaac's sword attack. Learned at level 1 so it shows at once.
constexpr unsigned kEarthSurge = 18;
constexpr unsigned kSpire = 6;
constexpr std::uint16_t kGaiaIcon = 7;  // Gaia's +4
constexpr std::uint32_t kSwordAttackEffect = 0x4001;
// The resolver (0x080BF11E) takes an ability's move kind from a byte table at
// 0x080C2B98 (checked for ids up to 0x205) when the byte is non-zero. Kinds 5
// and 9 both go to 0x080B9EC0 (r1 = 1 / 0), which plays the caster flourish
// (0x080C1798) and then reads the effect word: 0x4001 goes to the weapon-
// swing starter; its tail restores the display. Kind 5 (summon) also makes
// the caster leave the scene (played 2026-09-30), so Earth Surge uses 9, the
// kind of Gaia, Rockfall and Procne.
constexpr std::size_t kMoveKindTable = 0xC2B98;
constexpr std::uint8_t kCastKind = 9;
constexpr std::uint8_t kLearnLevel = 1;
constexpr std::uint32_t kPureVenusClassType = 1;

bool test_enabled() {
    const char* v = std::getenv("GSR_MOD_FIELD_TEST");
    return v && v[0] && !(v[0] == '0' && v[1] == '\0');
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void write_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

bool all_zero(const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

}  // namespace

// Psynergy name and description lines (FACTS.md 2026-09-30): name 0x333 + id,
// description 0x53A + id. Each description candidate is tried in order until
// one can be encoded with the game's own trees.
constexpr std::uint32_t kPsynergyNameLine = 0x333;
constexpr std::uint32_t kPsynergyDescriptionLine = 0x53A;
constexpr const char* kEarthSurgeName = "Earth Surge";
constexpr const char* kEarthSurgeDescriptions[] = {
    "Blast a foe with erupting earth.",
    "Attack with a surge of earth.",
    "Earth erupts beneath the foe.",
};

// Gives Psynergy 18 its name and description by rebuilding the two text
// blocks they live in as extensions of the in-memory ROM. A line that cannot
// be encoded is logged and left alone.
void patch_text(std::vector<std::uint8_t>* rom) {
    TextEncoder encoder(*rom);
    std::map<std::uint32_t, std::map<std::uint32_t, std::vector<std::uint8_t>>>
        by_block;  // block -> line -> encoded bytes
    auto add = [&](std::uint32_t line, const char* text) {
        std::vector<std::uint8_t> bytes;
        std::string why;
        if (!encoder.encode(text, &bytes, &why)) {
            std::fprintf(stderr, "[mod-test] text line 0x%X \"%s\": %s\n",
                         line, text, why.c_str());
            return false;
        }
        by_block[line >> 8][line] = std::move(bytes);
        return true;
    };
    add(kPsynergyNameLine + kEarthSurge, kEarthSurgeName);
    for (const char* text : kEarthSurgeDescriptions) {
        if (add(kPsynergyDescriptionLine + kEarthSurge, text)) {
            std::fprintf(stderr, "[mod-test] description: \"%s\"\n", text);
            break;
        }
    }
    // The encoder reads the ROM; rebuild only after all encoding is done.
    for (const auto& [block, lines] : by_block) {
        std::string why;
        if (rebuild_text_block(rom, block, lines, &why))
            std::fprintf(stderr,
                         "[mod-test] text block 0x%X rebuilt (%zu line(s)); "
                         "ROM now %zu bytes\n",
                         block, lines.size(), rom->size());
        else
            std::fprintf(stderr, "[mod-test] text block 0x%X not rebuilt: %s\n",
                         block, why.c_str());
    }
}

void patch_rom(std::vector<std::uint8_t>* rom_vector) {
    if (!rom_vector || !test_enabled()) return;
    std::uint8_t* rom = rom_vector->data();
    const std::size_t size = rom_vector->size();
    if (kClassTable + kClassScanLimit * kClassStride > size ||
        kPsynergyEffectTable + (kEarthSurge + 1) * 4 > size) {
        std::fprintf(stderr, "[mod-test] ROM too small; nothing changed\n");
        return;
    }

    // The slot must still be empty, or this is not the ROM we measured.
    std::uint8_t* entry = rom + kPsynergyTable + kEarthSurge * kPsynergyStride;
    std::uint8_t* effect = rom + kPsynergyEffectTable + kEarthSurge * 4;
    if (!all_zero(entry, kPsynergyStride) || read_u32(effect) != 0) {
        std::fprintf(stderr,
                     "[mod-test] Psynergy slot %u is not empty; nothing "
                     "changed\n", kEarthSurge);
        return;
    }

    std::memcpy(entry, rom + kPsynergyTable + kSpire * kPsynergyStride,
                kPsynergyStride);
    entry[4] = static_cast<std::uint8_t>(kGaiaIcon & 0xFF);
    entry[5] = static_cast<std::uint8_t>(kGaiaIcon >> 8);
    write_u32(effect, kSwordAttackEffect);
    if (kMoveKindTable + kEarthSurge >= size ||
        rom[kMoveKindTable + kEarthSurge] != 0) {
        std::fprintf(stderr,
                     "[mod-test] move kind slot %u is not free; kind left "
                     "alone\n", kEarthSurge);
    } else {
        rom[kMoveKindTable + kEarthSurge] = kCastKind;
        std::fprintf(stderr, "[mod-test] move kind table[%u] = %u (summon)\n",
                     kEarthSurge, static_cast<unsigned>(kCastKind));
    }
    std::fprintf(stderr,
                 "[mod-test] Psynergy %u: copy of Spire, icon %u, effect "
                 "0x%X\n", kEarthSurge, static_cast<unsigned>(kGaiaIcon),
                 static_cast<unsigned>(kSwordAttackEffect));

    // Add it to every pure-Venus class list, in its first free place.
    int added = 0;
    for (std::size_t c = 0; c < kClassScanLimit; ++c) {
        std::uint8_t* cls = rom + kClassTable + c * kClassStride;
        const std::uint32_t type = read_u32(cls);
        if (type > 0x40) break;  // past the end of the class table
        if (type != kPureVenusClassType) continue;
        for (std::size_t off = kClassLearnFirst; off < kClassLearnEnd;
             off += 4) {
            if (read_u32(cls + off) != 0) continue;
            write_u32(cls + off,
                      kEarthSurge | (std::uint32_t(kLearnLevel) << 8));
            ++added;
            break;
        }
    }
    std::fprintf(stderr,
                 "[mod-test] Psynergy %u learned at level %u in %d pure-Venus "
                 "classes\n", kEarthSurge, static_cast<unsigned>(kLearnLevel),
                 added);

    patch_text(rom_vector);  // may grow the vector; `rom` is stale after this
}

}  // namespace gsr::mods
