// Both games share session source; their state and identity stay separate.
#define GSR_LAUNCHER_TLA 1
#define GSR_SESSION_ENTRY gsr_lost_age_session_main
#define gsr_launcher gsr_lost_age_launcher
#define gsr_collection gsr_lost_age_collection
#define gsr_online gsr_lost_age_online
#define gsr gsr_lost_age_policy
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
