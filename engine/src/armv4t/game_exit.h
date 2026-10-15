// End-of-game work: reports, trace flushes, joining probe threads.
//
// A standalone game program runs these at program exit, as std::atexit did.
// A game that runs as a plug-in of the player program returns to it without
// the program exiting, so the player program calls run_game_exit() as soon as
// the game returns: the reports land in that game's log and its threads stop
// before the next game starts. Last registered runs first, either way.
#pragma once
#include <cstdlib>
#include <vector>
namespace gbarecomp {

inline std::vector<void (*)()>& game_exit_list() {
    static std::vector<void (*)()> list;
    return list;
}

// Runs every registered job once, newest first.
inline void run_game_exit() {
    auto& list = game_exit_list();
    while (!list.empty()) {
        void (*fn)() = list.back();
        list.pop_back();
        fn();
    }
}

// The first job also registers run_game_exit with std::atexit, so a
// standalone program still runs them all at exit.
inline void at_game_exit(void (*fn)()) {
    static const bool registered = std::atexit(run_game_exit) == 0;
    (void)registered;
    game_exit_list().push_back(fn);
}

}  // namespace gbarecomp
