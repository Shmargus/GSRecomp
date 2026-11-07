// Autosave, both games (Jimmy, 7 November 2026): each time the party enters
// a new area, the game is saved to file 9, shown first in the file list as
// "Autosave" (save_files.h). Manual saves never reach it, so the player's
// own files are never overwritten. F1 > Game switch, off by default.
//
// The moment: the field button poll's entry (radial_psynergy.h), reached only
// while the player has control on the field, with the room number changed
// since the last poll. A loaded file or state starts a new baseline instead
// of saving. Each game supplies the save itself: its own header and
// map-object steps, then the record written straight into file 9.
#pragma once
#include <cstdint>
#include "game_options.h"
#include "gba_bus.h"
#include "runtime_bus_bridge.h"

namespace gsr::autosave {

constexpr const char* kOptionKey = "Autosave";
constexpr const char* kSwitchLabel = "Autosave";
constexpr const char* kSwitchCaption =
    "Each time you enter a new area, save the game to the Autosave file at "
    "the top of the file list. Your own save files are never touched.";

struct Game {
    std::uint32_t poll;   // field button poll entry
    std::uint32_t field;  // RAM word: nonzero while the field runs
    std::uint32_t room;   // RAM s16: the current room
    void (*save)();
};

inline const Game* game = nullptr;
inline int last_room = -1;  // -1: no baseline yet

inline void install(const Game& g) { game = &g; }
inline void reset() { last_room = -1; }

inline void on_entry(std::uint32_t pc) {
    if (!game || pc != game->poll) return;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || b->read32(game->field) == 0) return;
    const int room = b->read16(game->room);
    if (room == last_room) return;
    const bool baseline = last_room < 0;
    last_room = room;
    if (baseline || gbarecomp::game_option_get(kOptionKey, 0) == 0) return;
    game->save();
}

}  // namespace gsr::autosave
