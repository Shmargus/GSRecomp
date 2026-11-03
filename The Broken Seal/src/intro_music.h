#pragma once

#include <cstdint>
#include "local_music_m4a.h"
#include "local_music_tracks.h"
#include "rom_version.h"
#include "../../shared_game/area_music.h"

// Golden Sun's music player 1 and its four gain hooks (config/usa/main.toml);
// measured in FACTS.md "Local title music". The shared policy is in
// engine/src/runtime/local_music_m4a.h.
namespace gsr::intro_music {
inline const gbarecomp::m4a_music::Player kPlayer{
    0x02004290u, 0x020044E0u, 10u, 0x50u,
    gs::rom(0x080F9C90u),                     // MPlayMain
    gs::rom(0x080FAAA4u),                     // MPlayStart's accepted header store
    {gs::rom(0x080F9F50u), 0u, gs::rom(0x080F9F52u)},  // left: BLS falls through to MOVS #255
    {gs::rom(0x080F9F64u), 0u, gs::rom(0x080F9F66u)}}; // right
inline gbarecomp::m4a_music::Replacement music{kPlayer, recordings.data(), recordings.size()};
// All five verified revisions: MPlayStart's accepted store is entry + 0x4C.
// Location-name offsets are identical; translated text starts 0x244 IDs later.
inline area_music::Controller choices{music, kPlayer, kPlayer.header_store - 0x4Cu,
    gs::rom(0x080030F8u), recordings.data(), recordings.size(),
    gs::rom_version() == gs::RomVersion::Usa ? 0x99Bu : 0xBDFu, battle_tracks};
} // namespace gsr::intro_music
