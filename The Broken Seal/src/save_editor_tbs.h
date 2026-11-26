// F1 > Cheats > Save editor, The Broken Seal's places (rules:
// shared_game/save_editor.h). Verified USA ROM, FACTS.md "Save editor":
// - Coins 0x02000250, clamped to 999,999 by the game's own adder 0x08079700.
// - Hero n has joined when story flag n is set (bank 0x02000040).
// - Character record 0x02000500 + id * 0x14C: level byte +0xF, experience
//   +0x124 (u32), 15 item halfwords +0xD8.
// - 0x080792FC(id, level) calls the level-up 0x0807905C once per level, then
//   the stat rebuild 0x08077428. The level-up stops at 99 (cmp 0x62), adds
//   the class's random growth and raises experience to the new level's
//   0x08079008(id, level).
// - Give 0x08078588(id, item): slot or -1.
// - Remove 0x080788C4(id, slot): one fewer of a stack, else the slot is
//   cleared and the bag packed to the front; then 0x08077428.
// - Djinn (FACTS.md "Save editor routines"): owned bits
//   +0xF8 + element*4, owned count +0x118 + element, owned flag 0x30 +
//   element*20 + index, names text 0x45F + element*20 + index, 7 per
//   element. The grant gives with 0x0807A1B4 (refuses at a count above 9)
//   then 0x0807A458 (the Djinn list); the move 0x0807A498 takes from the
//   giver with 0x0807A350 (unset), clears the bit and count itself, then
//   0x0807A3A8 (off the list).
#pragma once
#include "../../shared_game/save_editor.h"
#include "random_weapons_tbs.h"
#include "rom_version.h"

namespace gsr::save_editor::tbs {

inline const Game& game() {
    static const Game g{
        &random_weapons::tbs::game(), 0x02000250u, 999999u, 0x02000040u, 0x0Fu, 0x124u, 99u,
        GS_ROM(0x080792FCu), GS_ROM(0x08078588u), GS_ROM(0x080788C4u),
        0xF8u, 0x118u, 0x30u, gs::text(0x45Fu), 7u,
        GS_ROM(0x0807A1B4u), GS_ROM(0x0807A458u), GS_ROM(0x0807A350u), GS_ROM(0x0807A3A8u),
    };
    return g;
}

inline void install() { save_editor::install(game()); }

}  // namespace gsr::save_editor::tbs
