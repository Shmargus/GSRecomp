#include "tla_rom_version.h"

#include <cstring>
#include <iterator>

extern "C" const char gsr_game_rom_sha1[];

namespace tla_rom {
namespace {
namespace rr = gbarecomp::rom_revision;
constexpr const char* kTag = "tla-rom";
constexpr rr::Revision kRevisions[] = {
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) \
    {code, name, sha1, language},
#include "tla_rom_revisions.inc"
#undef GSR_ROM_REVISION
};
// Every revision after English has a column in tla_rom_addresses.inc.
constexpr unsigned kColumns = std::size(kRevisions) - 1;
using Row = rr::RowN<kColumns>;
using OverlayRow = rr::OverlayRowN<kColumns>;
constexpr Row addresses[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de, rl, pt) {en, {es, it, fr, de, rl, pt}},
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de, rl, pt)
#define TLA_TEXT_ID(en, es, it, fr, de, rl, pt)
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
constexpr OverlayRow overlays[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de, rl, pt)
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de, rl, pt) {#image, en, {es, it, fr, de, rl, pt}},
#define TLA_TEXT_ID(en, es, it, fr, de, rl, pt)
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
constexpr Row texts[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de, rl, pt)
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de, rl, pt)
#define TLA_TEXT_ID(en, es, it, fr, de, rl, pt) {en, {es, it, fr, de, rl, pt}},
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
unsigned revision() {
    static const unsigned index = rr::identify(kRevisions, gsr_game_rom_sha1, kTag);
    return index;
}

// The language row the loaded revision plays as (Reloaded: English).
unsigned language_row() {
    static const unsigned row = rr::language_row(kRevisions, revision(), kTag);
    return row;
}

template <typename Row>
std::uint32_t counterpart(const Row* row, const char* kind, std::uint32_t english) {
    return rr::counterpart(row, revision(), kTag, name(), kind, english);
}

}  // namespace

const char* sha1() { return gsr_game_rom_sha1; }
Version version() { return static_cast<Version>(language_row()); }
bool reloaded() { return std::strcmp(kRevisions[revision()].code, "rl") == 0; }
bool portuguese() { return std::strcmp(kRevisions[revision()].code, "pt") == 0; }

// Measured 19 November 2026: the patched copied routine 0x0803857C's
// literals (ROM 0x080385A4 / 0x080385A8) name the offset table 0x08F80000
// and the text base 0x08F8C300 (FACTS.md "Portuguese (Kyle the Runner)").
bool plain_text(std::uint32_t* offsets, std::uint32_t* base) {
    if (!portuguese()) return false;
    *offsets = 0x00F80000u;
    *base = 0x00F8C300u;
    return true;
}
bool spanish() { return version() == Version::Spanish; }
const char* name() { return kRevisions[revision()].name; }

std::uint32_t address(std::uint32_t english) {
    if (revision() == 0) return english;
    return counterpart(rr::find(addresses, english), "ROM address", english);
}

const gbarecomp::RomPcFilter& pc_filter() {
    static const gbarecomp::RomPcFilter filter = [] {
        gbarecomp::RomPcFilter f;
        const unsigned index = revision();
        for (const Row& row : addresses)
            f.add(index == 0 ? row.base : row.localized[index - 1]);
        return f;
    }();
    return filter;
}

std::uint32_t text(std::uint32_t english) {
    if (revision() == 0) return english;
    return counterpart(rr::find(texts, english), "text ID", english);
}

std::uint32_t overlay(const char* image, std::uint32_t english) {
    if (revision() == 0) return english;
    return counterpart(rr::find(overlays, image, english), image, english);
}

}  // namespace tla_rom
