#pragma once

// Seamless mode: The Broken Seal saves its Clear Data at the end of the game
// and The Lost Age starts a new game with it, without the player going
// through either title screen. The launcher passes the save between them.
// One copy of the names both games and the launcher use.

namespace gbarecomp::seamless {

// The Broken Seal's game_options.ini key, 0/1. The F1 Game page, the new-game
// question and the launcher's switch all read and write this one value.
inline constexpr const char* kOptionKey = "Seamless";

// The Broken Seal exits with kExitCodeBase + the saved slot (0..2) once its
// Clear Data is written; the launcher then starts The Lost Age at once. Any
// value the game never returns otherwise works (it returns 0, or 1 when the
// save could not be written); the launcher reports 127 itself.
inline constexpr int kExitCodeBase = 75;
inline constexpr int kSlotCount = 3;

// The slot a Seamless exit code names, or -1 for any other code.
inline constexpr int slot_of_exit_code(int code) {
    return code >= kExitCodeBase && code < kExitCodeBase + kSlotCount
        ? code - kExitCodeBase : -1;
}

// The launcher's --seamless-slot value: one digit naming a slot, else -1.
template <class Char>
inline int parse_slot(const Char* text) {
    return text && text[0] >= '0' && text[0] - '0' < kSlotCount && !text[1]
        ? int(text[0] - '0') : -1;
}

// Set by the launcher on The Lost Age: The Broken Seal's save file on every
// launch where it exists (Seamless mode imports it; a new game started by
// hand offers it first in the file browser), and in
// Seamless mode also the slot to import (1..3, as the import lists them).
inline constexpr const char* kSaveEnv = "GSR_SEAMLESS_GS_SAVE";
inline constexpr const char* kSlotEnv = "GSR_SEAMLESS_GS_SLOT";

}  // namespace gbarecomp::seamless
