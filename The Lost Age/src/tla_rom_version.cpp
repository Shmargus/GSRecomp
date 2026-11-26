#include "tla_rom_version.h"

#include <cstring>
#include <iterator>

extern "C" const char gsr_game_rom_sha1[];

namespace tla_rom {
namespace {
namespace rr = gbarecomp::rom_revision;
constexpr const char* kTag = "tla-rom";
constexpr rr::Revision kRevisions[] = {
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language, romhack) \
    {code, name, sha1, language},
#include "tla_rom_revisions.inc"
#undef GSR_ROM_REVISION
};
constexpr bool kRomhacks[] = {
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language, romhack) romhack,
#include "tla_rom_revisions.inc"
#undef GSR_ROM_REVISION
};
// Every revision after English has a column in tla_rom_addresses.inc.
constexpr unsigned kColumns = std::size(kRevisions) - 1;
using Row = rr::RowN<kColumns>;
using OverlayRow = rr::OverlayRowN<kColumns>;
constexpr Row addresses[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de, rl, pt, mh, pl, ba) {en, {es, it, fr, de, rl, pt, mh, pl, ba}},
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de, rl, pt, mh, pl, ba)
#define TLA_TEXT_ID(en, es, it, fr, de, rl, pt, mh, pl, ba)
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
constexpr OverlayRow overlays[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de, rl, pt, mh, pl, ba)
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de, rl, pt, mh, pl, ba) {#image, en, {es, it, fr, de, rl, pt, mh, pl, ba}},
#define TLA_TEXT_ID(en, es, it, fr, de, rl, pt, mh, pl, ba)
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
constexpr Row texts[] = {
#define TLA_ROM_ADDR(en, es, it, fr, de, rl, pt, mh, pl, ba)
#define TLA_OVERLAY_ADDR(image, en, es, it, fr, de, rl, pt, mh, pl, ba)
#define TLA_TEXT_ID(en, es, it, fr, de, rl, pt, mh, pl, ba) {en, {es, it, fr, de, rl, pt, mh, pl, ba}},
#include "tla_rom_addresses.inc"
#undef TLA_ROM_ADDR
#undef TLA_OVERLAY_ADDR
#undef TLA_TEXT_ID
};
unsigned revision() {
    static const unsigned index = rr::identify(kRevisions, gsr_game_rom_sha1, kTag);
    return index;
}

// The language row the loaded revision plays as (romhacks: English).
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
bool romhack() { return kRomhacks[revision()]; }
bool portuguese() { return std::strcmp(kRevisions[revision()].code, "pt") == 0; }
bool polish() {
    const char* code = kRevisions[revision()].code;
    return std::strcmp(code, "pl") == 0 || std::strcmp(code, "ba") == 0;
}

// Where a plain-text patch keeps its text. Measured: the literals of the
// patched, copied routine 0x0803857C (ROM 0x080385A4 / 0x080385A8) name the
// offset table and the text base (FACTS.md "Portuguese (Kyle the Runner)",
// "Polish 1.1c and Balance Age 1.34hf").
bool plain_text(std::uint32_t* offsets, std::uint32_t* base) {
    static constexpr struct { const char* code; std::uint32_t offsets, base; } kTables[] = {
        {"pt", 0x00F80000u, 0x00F8C300u},
        {"pl", 0x00FA0000u, 0x00FAC300u},
        {"ba", 0x01000000u, 0x0100C300u},
    };
    for (const auto& table : kTables)
        if (std::strcmp(kRevisions[revision()].code, table.code) == 0) {
            *offsets = table.offsets;
            *base = table.base;
            return true;
        }
    return false;
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
