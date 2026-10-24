#pragma once

#include <cstdint>
#include "local_music_m4a.h"
#include "local_music_tracks.h"

// Golden Sun's music player 1 and its four gain hooks (config/usa/main.toml);
// measured in FACTS.md "Local title music". The shared policy is in
// engine/src/runtime/local_music_m4a.h.
namespace gsr::intro_music {
inline constexpr gbarecomp::m4a_music::Player kPlayer{
    0x02004290u, 0x020044E0u, 10u, 0x50u,
    0x080F9C90u,                     // MPlayMain
    0x080FAAA4u,                     // MPlayStart's accepted header store
    {0x080F9F50u, 0u, 0x080F9F52u},  // left: BLS falls through to MOVS #255
    {0x080F9F64u, 0u, 0x080F9F66u}}; // right
inline gbarecomp::m4a_music::Replacement music{kPlayer, recordings.data(), recordings.size()};
} // namespace gsr::intro_music
