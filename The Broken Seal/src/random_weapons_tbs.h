// Random Equipment (first Random Weapons), The Broken Seal's places (rules: shared_game/
// random_weapons.h and random_weapons_plan.h, one copy with The Lost Age).
//
// Measured, verified ROMs (FACTS.md "Random Weapons", 5 November 2026):
// - Item records 0x0807B6A8 + id * 0x2C, ids 0..268 (269 and up are enemy text);
//   51 weapons (type 1), byte-identical in all five ROMs (each revision's
//   mapped table).
// - Descriptions are text 0x75 + id (Spain, Italy, France, Germany 0x5F),
//   unleash names the ability names 0x333 + id (0x42A); decoded with each
//   ROM's own tables. Font advances 0x080370D4 (Custom Battle's). Item names
//   (the spoiler sheet) text 0x182 + id (Spain, Italy, France, Germany 0x16C;
//   rom_addresses.inc evidence, 6 November 2026).
// - Armour: types 2, 3, 4, 5, 9 of the same table (random_weapons_plan.h),
//   84 pieces, Defense +0xA.
// - Stat rebuild 0x08077428(id) (the hero builder's, Challenge Mode's).
// - The choice lives in the save payload, RAM 0x02000038..3F: the header
//   builder 0x0801F818 never writes it, and it is zero in every saved slot
//   and all 89 F12 memory captures measured. The bytes:
//   shared_game/random_weapons_format.h.
#pragma once
#include "../../shared_game/random_weapons.h"
#include "rom_version.h"
#include "random_psynergy_tbs.h"

namespace gsr::random_weapons::tbs {

inline const Game& game() {
    static const Game g{
        0x02000038u, GS_ROM(0x0807B6A8u), 1u, 268u, {},
        gs::rom(0x080736B8u) - 0x08000000u, gs::rom(0x0803842Cu) - 0x08000000u,
        gs::text(0x75u), gs::text(0x333u), gs::text(0x182u), GS_ROM(0x080370D4u),
        0x02000500u, 0x14Cu, GS_ROM(0x08077428u), 8,
    };
    return g;
}

// Character record: HP +0x38 (max +0x34), PP +0x3A (max +0x36).
inline Config current() { return random_weapons::current(game()); }
inline void store(const Config& c) { random_weapons::store(game(), c); }
inline void frame() { random_weapons::frame(game()); }
inline void apply_new_game() { random_weapons::apply_new_game(game(), 0x38u, 0x34u, 0x3Au, 0x36u); }
inline bool text_character(std::uint32_t text_id, std::uint32_t* character) {
    return random_weapons::text_character(game(), text_id, character);
}
using random_weapons::reset;

}  // namespace gsr::random_weapons::tbs

