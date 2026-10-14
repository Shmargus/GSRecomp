// Isolated game state, using the same maintained session implementation.
#define GSR_SESSION_ENTRY gsr_broken_seal_session_main
#define gsr_launcher gsr_broken_seal_launcher
#define gsr_collection gsr_broken_seal_collection
#define gsr_online gsr_broken_seal_online
#define gsr gsr_broken_seal_policy
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
