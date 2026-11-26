#pragma once
#include <cstdint>
#include "rom_revision.h"
#include "rom_pc_filter.h"

// The game library identifies its own ROM before features are installed.
// All counterparts are measured for TLA; no Broken Seal addresses apply.
// The revisions are src/tla_rom_revisions.inc, the counterparts
// src/tla_rom_addresses.inc; the lookups are the engine's (rom_revision.h).
namespace tla_rom {
// The language the loaded ROM plays as, in the row order of
// src/tla_rom_revisions.inc. A patched revision (a romhack, Portuguese)
// reports the language it is a patch of; romhack() and portuguese() tell
// them apart.
enum class Version : std::uint8_t { English, Spanish, Italian, French, German };
Version version();
// A gameplay romhack (Reloaded, Maheo Hack, Balance Age): its abilities and classes are
// its own, not the measured English ones.
bool romhack();
// Kyle the Runner's Brazilian Portuguese Beta 1 (a patch of English;
// version() is English, addresses as English). Our menus show in Portuguese.
bool portuguese();
// The Polish translation 1.1c, alone or under Polish Balance Age (patches of
// English; version() is English, addresses as English). Our menus show in
// Polish.
bool polish();
// Where the loaded ROM keeps plain text (gsr::mods::PlainText), as ROM
// offsets; false when its text is the game's own Huffman format.
bool plain_text(std::uint32_t* offsets, std::uint32_t* base);
const char* sha1();
const char* name();
bool spanish();
std::uint32_t address(std::uint32_t english);
// Every ROM address of the loaded revision that address() can return.
const gbarecomp::RomPcFilter& pc_filter();
std::uint32_t overlay(const char* image, std::uint32_t english);
std::uint32_t text(std::uint32_t english);
}

#define TLA_ROM(english) GBARECOMP_ONCE_U32(::tla_rom::address(english))
#define TLA_OVL(image, english) GBARECOMP_ONCE_U32(::tla_rom::overlay(image, english))
#define TLA_TEXT(english) GBARECOMP_ONCE_U32(::tla_rom::text(english))
