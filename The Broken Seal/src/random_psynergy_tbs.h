// Random Psynergy, The Broken Seal's places (rules: shared_game/
// random_psynergy.h, one copy with The Lost Age).
//
// Measured, verified ROMs (FACTS.md "Random Psynergy"):
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
// all 72 distinct saved memory captures. All zero is Off, as every existing
// save is. The bytes: shared_game/random_psynergy_format.h.
#pragma once
#include "../../shared_game/name_scroll.h"
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
// Frost, Whirlwind and Ply open puzzles (learned before they are needed, by
// anyone who is there in time). Frost and Ply may go to any hero, Whirlwind
// to one who joins no later than Ivan. Ply heals the statue at Mercury
// Lighthouse's door; Mia joins just before it (player report, seed RS66 7GRS:
// no one learned Ply). Appended last so Frost and Whirlwind keep their holders.
constexpr Key kKeys[] = {{24, 0xFu}, {78, 0x7u}, {93, 0xFu}};

// Enemy skills (FACTS.md "Enemy skills"): enemy records
// 0x08080EC8 + (id - 8) * 0x54, 0xFA of them (the getter 0x08077198 takes
// id - 8 > 0xF9 as row 0), abilities at +0x38; ability rows 0x0807EE58, 16
// bytes, getter 0x08078B9C clamps ids >= 0x208. The PP readers by their
// return address (the call + 4): the hero's menus, command checks and field
// dispatcher; the battle's spend (actor byte at [r5]) and cast check (actor
// halfword at [[sl] + 0]).
inline std::span<const PpSite> pp_sites() {
    static const PpSite sites[] = {
        {GS_ROM(0x0801C672u), PpSite::kHero}, {GS_ROM(0x0801C682u), PpSite::kHero},
        {GS_ROM(0x08022EA8u), PpSite::kHero}, {GS_ROM(0x0808E7ACu), PpSite::kHero},
        {GS_ROM(0x080A6C2Au), PpSite::kHero}, {GS_ROM(0x080A7060u), PpSite::kHero},
        {GS_ROM(0x080A8FCEu), PpSite::kHero}, {GS_ROM(0x080AD142u), PpSite::kHero},
        {GS_ROM(0x080A5ED8u), PpSite::kByteAtR5}, {GS_ROM(0x080BE7DEu), PpSite::kHalfAtR10Action},
        // 22 November (scan of all getter calls, +9 reads within 0x200
        // bytes): the field menu's cost-against-PP check, and the enemies'
        // choice (predicate 0x080BD3C8, builder 0x080BD59A/0x080BD5C8).
        {GS_ROM(0x0802773Au), PpSite::kHero},
        {GS_ROM(0x080BD3D6u), PpSite::kEnemy}, {GS_ROM(0x080BD59Eu), PpSite::kEnemy},
        {GS_ROM(0x080BD5CCu), PpSite::kEnemy},
        // 22 November, from The Lost Age's log (the scan stopped at literal
        // pools): the shared Psynergy list 0x08025xxx, getter return
        // 0x08025B2A, cost reads 0x08025BB0/0x08025BF0.
        {GS_ROM(0x08025B2Au), PpSite::kHero},
    };
    return sites;
}

inline const Game& game() {
    // Heroes Isaac, Garet, Ivan, Mia (ids 0..3) and the level each joins at
    // (the new game's hero builder, 1, 1, 4, 10).
    static const Game g{
        0x02000030u, 0x02000500u, 0x14Cu, GS_ROM(0x08078BF0u), GS_ROM(0x0807B6A8u),
        {GS_ROM(0x08084B1Cu), kClasses, false},
        4, {1, 1, 4, 10},
        kKeys, static_cast<int>(std::size(kKeys)),
        0x3u,  // Isaac and Garet, there from the start
        {GS_ROM(0x08080EC8u), 0xFA, 0x54u, 0x38u},
        {GS_ROM(0x0807EE58u), 16u, 0x208u},
        GS_ROM(0x08078B9Cu), pp_sites(),
        {},  // every enemy-only skill here is shared (random_psynergy::kIconLike)
        gs::text(0x53Au),  // descriptions: Quake 3 is base + 3 (Spain, Italy, France, Germany 0x631)
        {},  // every enemy-only skill's line is shared (random_psynergy::kLines)
    };
    return g;
}

inline Config current() { return random_psynergy::current(game()); }
inline void store(const Config& c) { random_psynergy::store(game(), c); }
inline bool on_entry(std::uint32_t pc, std::uint32_t id) {
    return random_psynergy::on_entry(game(), pc, id);
}
inline void apply_new_game() { random_psynergy::apply_new_game(game()); }
inline void pp_on_getter(std::uint32_t pc, std::uint32_t r0, std::uint32_t r5, std::uint32_t r10,
                         std::uint32_t lr) {
    random_psynergy::pp_on_getter(game(), pc, r0, r5, r10, lr);
}
// The decoder's next character of an enemy skill's description, in the
// loaded language (not a romhack, whose abilities are its own).
inline bool description_character(std::uint32_t text_id, std::uint32_t* character) {
    return random_psynergy::description_character(game(), !gs::romhack(), text_id, character);
}
// Scrolling names (shared_game/name_scroll.h; FACTS.md "Scrolling names"):
// the printer 0x0801E7C0 (veneer 0x08015080), window block
// [0x03001E8C] (tilemap shadow +0, mode byte +0xEA7, colour +0xEAE: the
// glyph routine 0x0801DE5C), font advances 0x080370D4. The lists that print
// an ability name (text 0x333 + id) and then read its cost, by return
// address: the field Psynergy list 0x0801C5F4, 0x08022E7E, the battle's
// 0x080A6C74, 0x080A8FE8 (its Psynergy list) and 0x080AD138.
inline const name_scroll::Game& scroll_game() {
    static const std::uint32_t sites[] = {GS_ROM(0x0801C5F8u), GS_ROM(0x08022E82u), GS_ROM(0x080A6C78u),
                                          GS_ROM(0x080A8FECu), GS_ROM(0x080AD13Cu), GS_ROM(0x08025BEEu)};
    static const name_scroll::Game g{
        GS_ROM(0x0801E7C0u), sites, static_cast<int>(std::size(sites)),
        gs::ram(0x03001E8Cu), 0x0u, 0xEA7u, 0xEAEu,
        gs::rom(0x080736B8u) - 0x08000000u, gs::rom(0x0803842Cu) - 0x08000000u, GS_ROM(0x080370D4u),
        gs::text(0x333u),
        [](std::uint16_t id) { return random_psynergy::scrolls(game(), id); },
        [] { return random_psynergy::reference_ids(game()); },
    };
    return g;
}
inline void scroll_on_entry(std::uint32_t pc, const std::uint32_t* r) { name_scroll::on_entry(scroll_game(), pc, r); }
inline void scroll_frame() { name_scroll::frame(scroll_game()); }
using random_psynergy::fresh_seed;

}  // namespace gsr::random_psynergy::tbs
