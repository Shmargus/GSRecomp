// Random Treasure, The Lost Age (rules: shared_game/treasure_plan.h; the
// table: tla_treasure_randomizer_data.h from The Broken Seal/tools/
// build_tla_treasure_randomizer.py). Included by tla_new_game.cpp only.
//
// Measured, verified ROMs (FACTS.md "Random Treasure"):
// - Field treasures: the treasure table 0x080F2204 (8-byte entries, +4 flag,
//   +6 item word: bit 15 coins, low 15 bits the amount; else the item) is
//   read from the ROM by the pickup routine 0x080CCEC8, so the plan is written
//   into the running ROM image (write_table; shops and prices:
//   shared_game/treasure_randomizer.h). Byte-identical in all five ROMs.
// - Coins above 0x7FFF: the pickup passes only r7 & 0x7FFF to the coin
//   message 0x08038120 (return 0x080CD254) and the coin routine 0x080AD1D8
//   (return 0x080CD262), with the flag in r9. The hooks on those two entries
//   supply the full amount instead.
// - Gifts: the gift helper 0x080D260C takes the item in r0, and its full-bag
//   handling and messages use that same r0. It is also reached from
//   0x080D296C, where an actor hands the item over. A gift is identified by
//   its scene's code file (room record +0 at 0x080F17A8 + room * 8; the room
//   number is at 0x02000420) and its original item.
// - Shops: records at 0x0810C3F4 + id * 0x42, the same layout as The Broken
//   Seal's. Evidence: the literals at 0x0810A6B4; the artifacts loop at
//   0x0810A670; the stock copy at 0x0810A6F4 (up to 24 halfwords); the kind
//   read at 0x0810A708 (+0x40). Records 1..31 hold stock; kind 3 is a mixed
//   shop. Byte-identical in all five ROMs.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../../shared_game/random_psynergy.h"
#include "../../shared_game/random_weapons.h"
#include "../../shared_game/run_sheet.h"
#include "../../shared_game/treasure_plan.h"
#include "../../shared_game/treasure_randomizer.h"
#include "tla_rom_version.h"
#include "tla_treasure_regions.h"

namespace gsr::treasure::tla {

constexpr unsigned kItemCount = 461, kShops = 32;
constexpr std::uint32_t kTableSize = 3226;  // to the 0 key, 0x080F2E9C
constexpr std::uint32_t kRoom = 0x02000420u;

inline Randomizer state{{tla_data::spots,
                         {tla_data::shop_items, tla_data::shop_weapons, tla_data::shop_armour},
                         kItemCount, kShops, tla_data::region_sections}};
inline std::vector<std::uint8_t> pristine_table;  // the treasure table

inline std::uint32_t item_table() { return TLA_ROM(0x080B2364u); }
inline std::uint32_t treasure_table() { return TLA_ROM(0x080F2204u); }
inline std::uint32_t shop_table() { return TLA_ROM(0x0810C3F4u); }

inline bool active() { return state.active(); }

// The treasure table with the plan's field contents: blocks of
// (room | 0x4000, flag) then 8-byte entries.
inline void write_table(std::uint8_t* rom) {
    const std::uint32_t table = treasure_table() - 0x08000000u;
    std::copy_n(pristine_table.data(), kTableSize, rom + table);
    if (state.field.empty()) return;
    const auto h = [&](std::uint32_t at) {
        return static_cast<std::int16_t>(pristine_table[at] | pristine_table[at + 1] << 8);
    };
    std::uint32_t at = 0;
    while (at + 2 <= kTableSize && h(at) != 0) {
        at += 4;
        while (at + 8 <= kTableSize && h(at) != 0 && !(h(at) & 0x4000)) {
            const auto it = state.field.find(static_cast<std::uint16_t>(h(at + 4)));
            if (it != state.field.end()) {
                const Content& to = it->second;
                const std::uint16_t word = to.coins
                    ? static_cast<std::uint16_t>(0x8000u | std::min<std::uint32_t>(to.value, 0x7FFFu))
                    : static_cast<std::uint16_t>(to.value);
                rom[table + at + 6] = static_cast<std::uint8_t>(word);
                rom[table + at + 7] = static_cast<std::uint8_t>(word >> 8);
            }
            at += 8;
        }
    }
}

inline bool frame(const random_psynergy::Config& c, const random_weapons::Game& equipment) {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !b->rom_ptr()) return false;
    if (!state.captured) {
        pristine_table.resize(kTableSize);
        for (std::uint32_t i = 0; i < kTableSize; ++i) pristine_table[i] = b->read8(treasure_table() + i);
    }
    return apply_frame(state, *b, equipment, c, item_table(), shop_table(), write_table);
}

// Entry hook: true when r0 was changed. pc, lr (without the Thumb bit), r9
// (the pickup's flag), r0.
inline bool on_entry(std::uint32_t pc, std::uint32_t lr, std::uint32_t r9, std::uint32_t* r0) {
    if (!active()) return false;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return false;
    if ((pc == TLA_ROM(0x08038120u) && lr == TLA_ROM(0x080CD254u)) ||
        (pc == TLA_ROM(0x080AD1D8u) && lr == TLA_ROM(0x080CD262u)))
        return state.big_coins(static_cast<std::uint16_t>(r9), 0x7FFFu, r0);
    if (pc == TLA_ROM(0x080D260Cu)) {
        const std::uint16_t room = b->read16(kRoom);
        return state.gift(b->read16(TLA_ROM(0x080F17A8u) + room * 8u), r0);
    }
    return false;
}

// The spoiler sheet's lines: where every treasure went, and the shops.
inline void sheet_lines(const random_psynergy::Config& c, std::vector<run_sheet::Section>& out,
                        const run_sheet::ItemName& item) {
    state.sheet_lines(c, out, item);
}

}  // namespace gsr::treasure::tla
