#pragma once

#include <string>

// Each game's identity, in one table for the selector and the game sessions.
// A game session runs in its own process (collection_main.cpp,
// --session-game) and sets session_game_index once at its start.
namespace gsr_launcher {

struct GameIdentity {
    const char* stem;            // the game's folder in the collection
    const char* title;
    const wchar_t* title_w;
    const char* rom_prompt;
    const wchar_t* rom_prompt_w;
    const char* executable;
    const wchar_t* executable_w;
    const char* library;
    // The engine: on Windows a plug-in the program runs (one window for
    // both games); on Linux the program itself.
    const char* engine;
    const char* build_stamp;
    const wchar_t* build_stamp_w;
    const char* release_stamp;
    const wchar_t* release_stamp_w;
    // The ROMs this game accepts, from the game's revision list (The Broken
    // Seal src/rom_revisions.inc, The Lost Age src/tla_rom_revisions.inc);
    // the first is USA/Europe. Each is its own game build (player_builder
    // picks by SHA-1). The flag under each cartridge picks which one the
    // launcher expects (collection_storage.h rom_language).
    const char* rom_sha1s[5];
    const char* rom_names[5];
    const char* rom_codes[5];  // config/<code>, local/<code>, language files
};

inline bool accepts_rom(const GameIdentity& game, const std::string& sha1) {
    for (const char* accepted : game.rom_sha1s)
        if (accepted && sha1 == accepted) return true;
    return false;
}

inline int rom_language_count(const GameIdentity& game) {
    int count = 0;
    while (count < 5 && game.rom_sha1s[count]) ++count;
    return count;
}

#ifdef _WIN32
#define GSR_GAME_BINARIES(exe, lib, engine) exe ".exe", L"" exe ".exe", lib ".dll", engine ".dll"
#else
#define GSR_GAME_BINARIES(exe, lib, engine) exe, L"" exe, "lib" lib ".so", exe
#endif
inline constexpr GameIdentity kGames[2] = {
    {"The Broken Seal", "Golden Sun Recompiled", L"Golden Sun Recompiled",
     "Select the Golden Sun ROM (USA/Europe, Spain, Italy, France or Germany)",
     L"Select the Golden Sun ROM (USA/Europe, Spain, Italy, France or Germany)",
     GSR_GAME_BINARIES("GoldenSunRecomp", "GoldenSunGame", "GoldenSunEngine"),
     "GoldenSunGame.build.txt", L"GoldenSunGame.build.txt",
     "GoldenSunGame.release.txt", L"GoldenSunGame.release.txt",
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator) sha1,
     {
#include "../The Broken Seal/src/rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator) name,
     {
#include "../The Broken Seal/src/rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator) code,
     {
#include "../The Broken Seal/src/rom_revisions.inc"
     }},
#undef GSR_ROM_REVISION
    {"The Lost Age", "Golden Sun: The Lost Age Recompiled",
     L"Golden Sun: The Lost Age Recompiled",
     "Select the Golden Sun: The Lost Age ROM (USA/Europe, Spain, Italy, France or Germany)",
     L"Select the Golden Sun: The Lost Age ROM (USA/Europe, Spain, Italy, France or Germany)",
     GSR_GAME_BINARIES("TLARecomp", "TLAGame", "TLAEngine"),
     "TLAGame.build.txt", L"TLAGame.build.txt",
     "TLAGame.release.txt", L"TLAGame.release.txt",
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator) sha1,
     {
#include "../The Lost Age/src/tla_rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator) name,
     {
#include "../The Lost Age/src/tla_rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator) code,
     {
#include "../The Lost Age/src/tla_rom_revisions.inc"
     }},
#undef GSR_ROM_REVISION
};
#undef GSR_GAME_BINARIES

// 0 The Broken Seal, 1 The Lost Age.
inline int session_game_index = 0;
inline const GameIdentity& session_game() { return kGames[session_game_index]; }

}  // namespace gsr_launcher
