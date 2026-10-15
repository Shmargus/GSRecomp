#pragma once

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
    const char* rom_sha1;        // the only ROM this game accepts
};

#ifdef _WIN32
#define GSR_GAME_BINARIES(exe, lib, engine) exe ".exe", L"" exe ".exe", lib ".dll", engine ".dll"
#else
#define GSR_GAME_BINARIES(exe, lib, engine) exe, L"" exe, "lib" lib ".so", exe
#endif
inline constexpr GameIdentity kGames[2] = {
    {"The Broken Seal", "Golden Sun Recompiled", L"Golden Sun Recompiled",
     "Select the Golden Sun USA/Europe ROM",
     L"Select the Golden Sun USA/Europe ROM",
     GSR_GAME_BINARIES("GoldenSunRecomp", "GoldenSunGame", "GoldenSunEngine"),
     "GoldenSunGame.build.txt", L"GoldenSunGame.build.txt",
     "GoldenSunGame.release.txt", L"GoldenSunGame.release.txt",
     "5c4695205413df7db52b9a184815a07783999971"},
    {"The Lost Age", "Golden Sun: The Lost Age Recompiled",
     L"Golden Sun: The Lost Age Recompiled",
     "Select the Golden Sun: The Lost Age USA/Europe ROM",
     L"Select the Golden Sun: The Lost Age USA/Europe ROM",
     GSR_GAME_BINARIES("TLARecomp", "TLAGame", "TLAEngine"),
     "TLAGame.build.txt", L"TLAGame.build.txt",
     "TLAGame.release.txt", L"TLAGame.release.txt",
     "b500663220cb9bf56b9f8e8c0c544f1d6fa3a824"},
};
#undef GSR_GAME_BINARIES

// 0 The Broken Seal, 1 The Lost Age.
inline int session_game_index = 0;
inline const GameIdentity& session_game() { return kGames[session_game_index]; }

}  // namespace gsr_launcher
