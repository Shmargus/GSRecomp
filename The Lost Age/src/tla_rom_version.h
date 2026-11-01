#pragma once
#include <cstdint>
#include "rom_revision.h"

// The game library identifies its own ROM before features are installed.
// All counterparts are measured for TLA; no Broken Seal addresses apply.
// The revisions are src/tla_rom_revisions.inc, the counterparts
// src/tla_rom_addresses.inc; the lookups are the engine's (rom_revision.h).
namespace tla_rom {
// In the row order of src/tla_rom_revisions.inc.
enum class Version : std::uint8_t { English, Spanish, Italian, French, German };
Version version();
const char* sha1();
const char* name();
bool spanish();
std::uint32_t address(std::uint32_t english);
std::uint32_t overlay(const char* image, std::uint32_t english);
std::uint32_t text(std::uint32_t english);
}

#define TLA_ROM(english) GBARECOMP_ONCE_U32(::tla_rom::address(english))
#define TLA_OVL(image, english) GBARECOMP_ONCE_U32(::tla_rom::overlay(image, english))
#define TLA_TEXT(english) GBARECOMP_ONCE_U32(::tla_rom::text(english))
