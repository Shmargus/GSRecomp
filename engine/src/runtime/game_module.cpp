// game_module.cpp -- the way in when a game's engine is a plug-in
// (GoldenSunEngine.dll, TLAEngine.dll) run by a player program
// (player_main.cpp; one window for both games, STATUS.md).
//
// window_in: a window the game opens into instead of making its own (null:
// make one). window_out: when not null, the game leaves its window open when
// it ends and hands it back here, for the next game.
#include <cstdio>

#include "game_exit.h"
#include "host_window.h"

extern "C" int main(int argc, char** argv);  // the game's runner

extern "C"
#if defined(_WIN32)
__declspec(dllexport)
#endif
int gsr_game_main(int argc, char** argv, void* window_in, void** window_out) {
    gbarecomp::HostWindow::give_window(window_in);
    gbarecomp::HostWindow::keep_window_on_close(window_out != nullptr);
    const int code = main(argc, argv);
    gbarecomp::run_game_exit();
    std::fflush(stdout);
    std::fflush(stderr);
    if (window_out) *window_out = gbarecomp::HostWindow::take_kept_window();
    return code;
}
