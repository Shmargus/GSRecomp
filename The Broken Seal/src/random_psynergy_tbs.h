// Random Psynergy, The Broken Seal's places (rules: shared_game/
// random_psynergy.h, one copy with The Lost Age; design docs/features/
// SEEDED_PSYNERGY.md).
//
// Measured, verified ROMs (FACTS.md "Random Psynergy", 4 November 2026):
// - The game rebuilds a character's 32 Psynergy words (record +0x58, every
//   4 bytes, id in the low half) in 0x08078BF0(id), from the class update
//   0x08079AE8, the level-up 0x0807905C, equipping 0x08078708 and 0x08078E28's
//   caller. It returns 0 at once for class 0; otherwise it clears the words
//   flagged 0x8000 (class) and 0x4000 (item), packs the rest (story grants
//   such as Move), adds each class entry (class table 0x08084B1C + class *
//   0x54 + 0x10, 16 words: ability byte, level byte) the level reaches as
//   ability | 0x8000, then each equipped (0x200) item's ability (item table
//   0x0807B6A8 + item * 0x2C: +0xC == 3, ability u16 +0x28) as | 0x4000, and
//   packs again. Nothing else reads the class lists, so the level-up message
//   compares the rebuilt list (gameplay to confirm).
// - Class lists teach 74 different abilities: 71 battle spells plus Frost,
//   Whirlwind and Avoid. Classes teach 8..16 of them, the last at level 30..54;
//   Grand Gaia's 54 is the latest. Same tables in all five ROMs.
//
// The seed lives in the save payload, RAM 0x02000030..37: the header builder
// 0x0801F818 never writes it, and it is zero in every saved first block and
// all 72 distinct F12 memory captures. All zero is Off, as every existing
// save is. The bytes: shared_game/random_psynergy_format.h.
#pragma once
#include <iterator>

#include "../../shared_game/random_psynergy.h"
#include "rom_version.h"

namespace gsr::random_psynergy::tbs {

// The pool: every ability classes 0..84 teach, with the earliest level any
// of them teaches it (74 abilities, the latest at 54), read from the
// player's ROM (shared_game/random_psynergy.h pool()). The class table is
// byte-identical in all five ROMs; each revision's address is a reviewed
// override (config/<lang>/address-overrides.toml).
constexpr int kClasses = 85;

// Fair Play's early heals, Cure 90 and Ply 93, are in this pool at level 1.
// Frost and Whirlwind open puzzles (Jimmy: learned before they are needed,
// by anyone who is there in time). Frost may go to any hero, Whirlwind to one
// who joins no later than Ivan.
constexpr Key kKeys[] = {{24, 0xFu}, {78, 0x7u}};

inline const Game& game() {
    // Heroes Isaac, Garet, Ivan, Mia (ids 0..3) and the level each joins at
    // (the new game's hero builder, FACTS "Challenge Mode": 1, 1, 4, 10).
    static const Game g{
        0x02000030u, 0x02000500u, 0x14Cu, GS_ROM(0x08078BF0u), GS_ROM(0x0807B6A8u),
        {GS_ROM(0x08084B1Cu), kClasses, false},
        4, {1, 1, 4, 10},
        kKeys, static_cast<int>(std::size(kKeys)),
        0x3u,  // Isaac and Garet, there from the start
    };
    return g;
}

inline Config current() { return random_psynergy::current(game()); }
inline void store(const Config& c) { random_psynergy::store(game(), c); }
inline bool on_entry(std::uint32_t pc, std::uint32_t id) {
    return random_psynergy::on_entry(game(), pc, id);
}
inline void apply_new_game() { random_psynergy::apply_new_game(game()); }
using random_psynergy::fresh_seed;

}  // namespace gsr::random_psynergy::tbs
