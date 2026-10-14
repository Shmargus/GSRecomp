#pragma once

// Both player launchers share one UI; game identity stays explicit.
#ifdef GSR_LAUNCHER_TLA
#define GSR_GAME_TITLE "Golden Sun: The Lost Age Recompiled"
#define GSR_GAME_TITLE_W L"Golden Sun: The Lost Age Recompiled"
#define GSR_ROM_PROMPT "Select the Golden Sun: The Lost Age USA/Europe ROM"
#define GSR_ROM_PROMPT_W L"Select the Golden Sun: The Lost Age USA/Europe ROM"
#define GSR_GAME_EXE_W L"TLARecomp.exe"
#define GSR_GAME_DEV_BUILD_DIR_W L"build/tla"
#define GSR_GAME_BUILD "TLAGame.build.txt"
#define GSR_GAME_BUILD_W L"TLAGame.build.txt"
#define GSR_GAME_RELEASE "TLAGame.release.txt"
#define GSR_GAME_RELEASE_W L"TLAGame.release.txt"
#else
#define GSR_GAME_TITLE "Golden Sun Recompiled"
#define GSR_GAME_TITLE_W L"Golden Sun Recompiled"
#define GSR_ROM_PROMPT "Select the Golden Sun USA/Europe ROM"
#define GSR_ROM_PROMPT_W L"Select the Golden Sun USA/Europe ROM"
#define GSR_GAME_EXE_W L"GoldenSunRecomp.exe"
#define GSR_GAME_DEV_BUILD_DIR_W L"build/gs011_opt"
#define GSR_GAME_BUILD "GoldenSunGame.build.txt"
#define GSR_GAME_BUILD_W L"GoldenSunGame.build.txt"
#define GSR_GAME_RELEASE "GoldenSunGame.release.txt"
#define GSR_GAME_RELEASE_W L"GoldenSunGame.release.txt"
#endif
