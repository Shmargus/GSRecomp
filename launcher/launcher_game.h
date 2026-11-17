#pragma once

#include <string>

// Each game's identity, in one table for the selector and the game sessions.
// A game session runs in its own process (collection_main.cpp,
// --session-game) and sets session_game_index once at its start.
namespace gsr_launcher {

// Revisions a game may list: five languages and one supported patch.
inline constexpr int kMaxRevisions = 6;

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
    // One build per supported ROM revision (see each game's rom_revisions.inc).
    // The order is USA/Europe, other languages, then supported patches. The
    // cartridge's flag or patch badge selects the expected revision. The
    // builder identifies the ROM by SHA-1.
    const char* rom_sha1s[kMaxRevisions];
    const char* rom_names[kMaxRevisions];
    const char* rom_codes[kMaxRevisions];      // config/<code>, local/<code>, language files
    const char* rom_languages[kMaxRevisions];  // the language row's code each plays as
};

inline bool accepts_rom(const GameIdentity& game, const std::string& sha1) {
    for (const char* accepted : game.rom_sha1s)
        if (accepted && sha1 == accepted) return true;
    return false;
}

// Every revision the game lists (one launcher button each).
inline int rom_revision_count(const GameIdentity& game) {
    int count = 0;
    while (count < kMaxRevisions && game.rom_sha1s[count]) ++count;
    return count;
}

// The revision with this SHA-1, or -1.
inline int rom_revision_of(const GameIdentity& game, const std::string& sha1) {
    for (int i = 0; i < rom_revision_count(game); ++i)
        if (sha1 == game.rom_sha1s[i]) return i;
    return -1;
}

// Base language for this revision. Reloaded uses USA/Europe.
inline int rom_language_row(const GameIdentity& game, int revision) {
    for (int i = 0; i < rom_revision_count(game); ++i)
        if (std::string(game.rom_codes[i]) == game.rom_languages[revision]) return i;
    return 0;
}

inline bool rom_is_language(const GameIdentity& game, int revision) {
    return rom_language_row(game, revision) == revision;
}

#ifdef _WIN32
#define GSR_GAME_BINARIES(exe, lib, engine, android) exe ".exe", L"" exe ".exe", lib ".dll", engine ".dll"
#elif defined(__ANDROID__)
// On Android this path is a link to the engine in the app's library folder.
#define GSR_GAME_BINARIES(exe, lib, engine, android) "lib" android ".so", L"lib" android ".so", "lib" lib ".so", "lib" android ".so"
#else
#define GSR_GAME_BINARIES(exe, lib, engine, android) exe, L"" exe, "lib" lib ".so", exe
#endif
inline constexpr GameIdentity kGames[2] = {
    {"The Broken Seal", "Golden Sun Recompiled", L"Golden Sun Recompiled",
     "Select the Golden Sun ROM (USA/Europe, Spain, Italy, France, Germany or Reloaded 1.83a)",
     L"Select the Golden Sun ROM (USA/Europe, Spain, Italy, France, Germany or Reloaded 1.83a)",
     GSR_GAME_BINARIES("GoldenSunRecomp", "GoldenSunGame", "GoldenSunEngine", "gs_engine"),
     "GoldenSunGame.build.txt", L"GoldenSunGame.build.txt",
     "GoldenSunGame.release.txt",
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) sha1,
     {
#include "../The Broken Seal/src/rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) name,
     {
#include "../The Broken Seal/src/rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) code,
     {
#include "../The Broken Seal/src/rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) language,
     {
#include "../The Broken Seal/src/rom_revisions.inc"
     }},
#undef GSR_ROM_REVISION
    {"The Lost Age", "Golden Sun: The Lost Age Recompiled",
     L"Golden Sun: The Lost Age Recompiled",
     "Select the Golden Sun: The Lost Age ROM (USA/Europe, Spain, Italy, France, Germany or Reloaded 1.83)",
     L"Select the Golden Sun: The Lost Age ROM (USA/Europe, Spain, Italy, France, Germany or Reloaded 1.83)",
     GSR_GAME_BINARIES("TLARecomp", "TLAGame", "TLAEngine", "tla_engine"),
     "TLAGame.build.txt", L"TLAGame.build.txt",
     "TLAGame.release.txt",
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) sha1,
     {
#include "../The Lost Age/src/tla_rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) name,
     {
#include "../The Lost Age/src/tla_rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) code,
     {
#include "../The Lost Age/src/tla_rom_revisions.inc"
     },
#undef GSR_ROM_REVISION
#define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) language,
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
