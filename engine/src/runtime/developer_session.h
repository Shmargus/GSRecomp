#pragma once

// Developer sessions and the developer-only cheats. One copy of the names
// both games and the launcher use.
//
// The developer launcher sets kDeveloperSessionEnv=1 for the game; player
// launchers remove it (launcher/session_environment.h). A game shows its
// developer-only F1 switches, and acts on them, only when it is set, so a
// player build never offers them and an old game_options.ini value is inert.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "game_options.h"

namespace gbarecomp {

inline constexpr const char* kDeveloperSessionEnv = "GSR_DEVELOPER_SESSION";

inline bool developer_session() {
    static const bool on = [] {
        const char* value = std::getenv(kDeveloperSessionEnv);
        return value && std::strcmp(value, "1") == 0;
    }();
    return on;
}

// Walk through walls (F1 Game page, developer sessions only): each game
// sends the player down its own leftover debug path, which skips the field
// walking routine's wall, cliff and people tests. Off is the game's own path.
namespace walk_through_walls {

inline constexpr const char* kOptionKey = "WalkThroughWalls";
inline constexpr const char* kSwitchLabel = "Walk through walls";
inline constexpr const char* kSwitchCaption =
    "Developer cheat. Walk through walls, cliffs and people on field maps. "
    "Only developer builds show it.";

inline bool enabled() {
    return developer_session() && game_option_get(kOptionKey, 0) != 0;
}

// The decision for either `beq` that guards a game's debug path (the debug
// byte's, then the L key's), as a conditional-branch override: not taken
// while the cheat is on; off, the game's own (its debug byte is clear).
inline int decide_branch(std::uint32_t original, std::uint32_t* out_decision) {
    if (!enabled()) return 0;
    *out_decision = 0u;
    return original != 0u;
}

}  // namespace walk_through_walls

}  // namespace gbarecomp
