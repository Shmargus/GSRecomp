// Random Treasure, The Broken Seal (rules: shared_game/treasure_plan.h; the
// table: treasure_randomizer_data.h from tools/build_treasure_randomizer.py).
//
// Measured, verified ROMs (FACTS.md "Random Treasure", 6 November 2026):
// - Field pickups all pass 0x0808D9A4(actor). The room's records are 12
//   bytes at [[0x03001EBC] + 0x10] up to 0xFFFFFFFF (0x0808D48C): +0 low
//   nibble 3 = an actor's interaction, +4 actor, +6 flag (s16), +8 payload;
//   payload & 0xFFF00000 == 0x100000 gives the item (low 16 bits) with
//   0x08077030, 0x200000 the coins (low 16 bits) with 0x08077230 ->
//   0x08079700 (capped at 999,999, 0x08079706). Availability is the flag
//   alone for both (0x0808D458 only answers for 0x500000). So a planned
//   spot's payload is rewritten in RAM on entry to 0x0808D9A4; an amount
//   above 0xFFFF is passed to the coin message (0x08015120 from 0x0808DC58)
//   and the coin routine (from 0x0808DC66) by their entries, r7 still the
//   record.
// - Gifts: the gift helper 0x08091A58(item) (full-bag handling and the
//   messages use the same r0). A gift is its scene's code pack, room record
//   +0 at 0x0809F1A8 + room * 8 (room 0x02000400), and its original item;
//   the room tables are byte-identical in the five ROMs.
// - Shops: 0x080B41AC + id * 0x42 (treasure_plan.h's layout), 35 records,
//   1..34 used; prices are item record +0 (0x0807B6A8 + id * 0x2C). Both are
//   read from the ROM (shared_game/treasure_randomizer.h writes them).
// - Random Weapons / Armor finds change the same field and gift lookups
//   (treasure_plan.h new_finds); the pools are the generated ones.
// - Sol Sanctum never receives a Psynergy item. It holds no story item, it
//   is left behind for good, and Skip Prologue skips it. The planner moves a
//   Psynergy item only to an "exact" spot, and the check below keeps Sol
//   Sanctum's spots from ever being exact.
#pragma once
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include <algorithm>
#include <string>
#include <string_view>

#include "../../shared_game/run_sheet.h"
#include "../../shared_game/treasure_plan.h"
#include "../../shared_game/treasure_randomizer.h"
#include "random_psynergy_tbs.h"
#include "random_weapons_tbs.h"
#include "rom_version.h"
#include "treasure_randomizer_data.h"

namespace gsr::treasure::tbs {

constexpr bool sol_sanctum_takes_no_psynergy_items() {
    for (const Spot& s : tbs_data::spots)
        if (std::string_view(s.place) == "Sol Sanctum" && s.exact) return false;
    return true;
}
static_assert(sol_sanctum_takes_no_psynergy_items(), "a Psynergy item could be placed in Sol Sanctum");

constexpr unsigned kItemCount = 269, kShops = 35;
constexpr std::uint32_t kRoom = 0x02000400u;

inline Randomizer state{{tbs_data::spots,
                         {tbs_data::shop_items, tbs_data::shop_weapons, tbs_data::shop_armour},
                         kItemCount, kShops}};

inline std::uint32_t item_table() { return GS_ROM(0x0807B6A8u); }
inline std::uint32_t shop_table() { return GS_ROM(0x080B41ACu); }

inline random_psynergy::Config current() { return random_psynergy::tbs::current(); }
inline bool active() { return state.active(); }

// Once a game frame, after Random Equipment's frame.
inline void frame() {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !b->rom_ptr()) return;
    state.capture(*b, item_table(), shop_table());
    state.apply(random_weapons::rom_image(*b), random_weapons::state.generation, item_table(), shop_table(),
                current());
}

// Entry to the field pickup routine: apply the room's planned records.
inline void on_pickup(gba::GbaBus& b) {
    const Randomizer& s = state;
    if (s.field.empty() || !active()) return;
    const std::uint32_t context = b.read32(gs::ram(0x03001EBCu));
    std::uint32_t at = context ? b.read32(context + 0x10u) : 0;
    if (at < 0x02000000u || at >= 0x02040000u) return;
    for (int guard = 0; guard < 512; ++guard, at += 12u) {
        const std::uint32_t word = b.read32(at);
        if (word == 0xFFFFFFFFu) break;
        if ((word & 0xFu) != 3u) continue;
        const std::uint32_t payload = b.read32(at + 8u);
        const std::uint32_t kind = payload & 0xFFF00000u;
        if (kind != 0x100000u && kind != 0x200000u) continue;
        const auto it = s.field.find(static_cast<std::uint16_t>(b.read16(at + 6u)));
        if (it == s.field.end()) continue;
        const Content& to = it->second;
        const std::uint32_t value = to.coins ? std::min<std::uint32_t>(to.value, 0xFFFFu) : to.value;
        b.write32(at + 8u, (to.coins ? 0x200000u : 0x100000u) | (value & 0xFFFFu));
    }
}

// An amount above 0xFFFF for the record in r7 (the coin message and the coin
// routine), else none.
inline bool big_coins(gba::GbaBus& b, std::uint32_t record, std::uint32_t* amount) {
    if (!active() || record < 0x02000000u || record >= 0x02040000u) return false;
    return state.big_coins(static_cast<std::uint16_t>(b.read16(record + 6u)), 0xFFFFu, amount);
}

// Entry hook: true when r0 was changed. pc, lr, r0, r7 from the entry.
inline bool on_entry(std::uint32_t pc, std::uint32_t lr, std::uint32_t r7, std::uint32_t* r0) {
    if (!active()) return false;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return false;
    if (pc == GS_ROM(0x0808D9A4u)) {
        on_pickup(*b);
        return false;
    }
    if ((pc == GS_ROM(0x08015120u) && lr == (GS_ROM(0x0808DC5Cu) | 1u)) ||
        (pc == GS_ROM(0x08079700u) && lr == (GS_ROM(0x0808DC6Au) | 1u)))
        return big_coins(*b, r7, r0);
    if (pc == GS_ROM(0x08091A58u)) {
        const std::uint16_t room = b->read16(kRoom);
        const std::uint16_t pack = b->read16(GS_ROM(0x0809F1A8u) + room * 8u);
        return state.gift(pack, r0);
    }
    return false;
}

// The spoiler sheet's lines: where every treasure went, and the shops.
inline void sheet_lines(const random_psynergy::Config& c, std::vector<run_sheet::Section>& out,
                        const run_sheet::ItemName& item) {
    state.sheet_lines(c, out, item);
}

}  // namespace gsr::treasure::tbs

// The run's F1 page (run_sheet.h): the code and the spoiler sheet.
namespace gsr::run_sheet::tbs {
inline const Game& game() {
    static constexpr const char* kNames[] = {"Isaac", "Garet", "Ivan", "Mia"};
    static const Game g{&random_psynergy::tbs::game(), &random_weapons::tbs::game(), kNames,
                        &treasure::tbs::sheet_lines};
    return g;
}
inline void install() { run_sheet::install(game()); }
}  // namespace gsr::run_sheet::tbs
