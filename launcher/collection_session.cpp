// The game session, compiled once for both games: collection_main.cpp
// passes the game to gsr_session_main (launcher_game.h holds each game's
// names, files and ROM checksum).
#ifdef _WIN32
#ifndef GSR_COLLECTION_DEV
#define GSR_RELEASE_LAUNCHER 1
#endif
#include "session_windows.cpp"
#else
#define SDL_MAIN_HANDLED
#ifdef GSR_COLLECTION_DEV
#define GSR_LINUX_DEV_LAUNCHER 1
#endif
#include "session_linux.cpp"
#endif
