#include "rom_version.h"

#include <cstring>
#include <iterator>

// Written into the game library from the ROM it was generated from
// (CMakeLists.txt in a dev build, player_builder in a player package).
extern "C" const char gsr_game_rom_sha1[];

namespace gs {
namespace {

namespace rr = gbarecomp::rom_revision;
constexpr const char* kTag = "rom_version";

constexpr rr::Revision kRevisions[] = {
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) \
    {code, name, sha1, language},
#include "rom_revisions.inc"
#undef GSR_ROM_REVISION
};

// Every revision after USA/Europe has a column in rom_addresses.inc.
constexpr unsigned kColumns = std::size(kRevisions) - 1;
using Row = rr::RowN<kColumns>;
using OverlayRow = rr::OverlayRowN<kColumns>;

constexpr Row kRomAddrs[] = {
#define GS_RAM_ADDR(usa, germany)
#define GS_ROM_ADDR(usa, spain, italy, france, germany, reloaded) {usa, {spain, italy, france, germany, reloaded}},
#define GS_OVL_ADDR(id, usa, spain, italy, france, germany, reloaded)
#define GS_TEXT_ID(usa, spain, italy, france, germany, reloaded)
#include "rom_addresses.inc"
#undef GS_ROM_ADDR
#undef GS_OVL_ADDR
#undef GS_TEXT_ID
#undef GS_RAM_ADDR
};
constexpr OverlayRow kOvlAddrs[] = {
#define GS_RAM_ADDR(usa, germany)
#define GS_ROM_ADDR(usa, spain, italy, france, germany, reloaded)
#define GS_OVL_ADDR(id, usa, spain, italy, france, germany, reloaded) {#id, usa, {spain, italy, france, germany, reloaded}},
#define GS_TEXT_ID(usa, spain, italy, france, germany, reloaded)
#include "rom_addresses.inc"
#undef GS_ROM_ADDR
#undef GS_OVL_ADDR
#undef GS_TEXT_ID
#undef GS_RAM_ADDR
};
constexpr Row kTextIds[] = {
#define GS_RAM_ADDR(usa, germany)
#define GS_ROM_ADDR(usa, spain, italy, france, germany, reloaded)
#define GS_OVL_ADDR(id, usa, spain, italy, france, germany, reloaded)
#define GS_TEXT_ID(usa, spain, italy, france, germany, reloaded) {usa, {spain, italy, france, germany, reloaded}},
#include "rom_addresses.inc"
#undef GS_ROM_ADDR
#undef GS_OVL_ADDR
#undef GS_TEXT_ID
#undef GS_RAM_ADDR
};

unsigned revision() {
    static const unsigned index = rr::identify(kRevisions, gsr_game_rom_sha1, kTag);
    return index;
}

// The language row the loaded revision plays as.
unsigned language_row() {
    static const unsigned row = rr::language_row(kRevisions, revision(), kTag);
    return row;
}

template <typename Row>
std::uint32_t counterpart(const Row* row, const char* what, std::uint32_t usa) {
    return rr::counterpart(row, revision(), kTag, rom_version_name(), what, usa);
}

}  // namespace

const char* game_rom_sha1() { return gsr_game_rom_sha1; }
RomVersion rom_version() { return static_cast<RomVersion>(language_row()); }
bool reloaded() { return std::strcmp(kRevisions[revision()].code, "rl") == 0; }
const char* rom_version_name() { return kRevisions[revision()].name; }

std::uint32_t rom(std::uint32_t usa) {
    if (revision() == 0) return usa;
    return counterpart(rr::find_sorted(kRomAddrs, usa), "ROM address", usa);
}

std::uint32_t ovl(const char* overlay_id, std::uint32_t usa) {
    if (revision() == 0) return usa;
    return counterpart(rr::find(kOvlAddrs, overlay_id, usa), overlay_id, usa);
}

std::uint32_t text(std::uint32_t usa_text_id) {
    if (revision() == 0) return usa_text_id;
    return counterpart(rr::find(kTextIds, usa_text_id), "text ID", usa_text_id);
}

std::uint32_t ram(std::uint32_t usa) {
    if (rom_version() != RomVersion::Germany) return usa;
    switch (usa) {
#define GS_ROM_ADDR(usa, spain, italy, france, germany, reloaded)
#define GS_OVL_ADDR(id, usa, spain, italy, france, germany, reloaded)
#define GS_TEXT_ID(usa, spain, italy, france, germany, reloaded)
#define GS_RAM_ADDR(usa, germany) case usa: return germany;
#include "rom_addresses.inc"
#undef GS_ROM_ADDR
#undef GS_OVL_ADDR
#undef GS_TEXT_ID
#undef GS_RAM_ADDR
    }
    rr::unmapped(kTag, rom_version_name(), "RAM slot", usa);
}

}  // namespace gs
