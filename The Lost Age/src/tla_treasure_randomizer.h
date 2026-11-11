// Random Treasure, The Lost Age (rules: shared_game/treasure_plan.h; the
// table: tla_treasure_randomizer_data.h from The Broken Seal/tools/
// build_tla_treasure_randomizer.py). Included by tla_new_game.cpp only.
//
// Measured, verified ROMs (FACTS.md "Random Treasure", 7 November 2026):
// - Field treasures: the treasure table 0x080F2204 (8-byte entries, +4 flag,
//   +6 item word: bit 15 coins, low 15 bits the amount; else the item) is
//   read from the ROM by the pickup routine 0x080CCEC8, so the plan is written
//   into the running ROM image (as Random Equipment does) and the pristine
//   bytes return for other files. Byte-identical in all five ROMs.
// - Coins above 0x7FFF: the pickup gives r7 & 0x7FFF through the coin message
//   0x08038120 (return 0x080CD254) and the coin routine 0x080AD1D8 (return
//   0x080CD262), the flag in r9; their entries take the full amount.
// - Gifts: the gift helper 0x080D260C(item) (full-bag handling and messages
//   use the same r0); also reached from 0x080D296C (an actor hands it over).
//   A gift is its scene's code file (room record +0 at 0x080F17A8 + room *
//   8; room 0x02000420) and its original item.
// - Shops: 0x0810C3F4 + id * 0x42, as The Broken Seal's (literals at
//   0x0810A6B4 artifacts loop 0x0810A670, 0x0810A6F4 stock copy up to 24
//   halfwords, 0x0810A708 kind +0x40); records 1..31 hold stock, kind 3
//   mixed. Byte-identical in all five ROMs.
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
#include "tla_rom_version.h"
#include "tla_treasure_randomizer_data.h"

namespace gsr::treasure::tla {

constexpr unsigned kItemCount = 461, kShops = 32;
constexpr std::uint32_t kTableSize = 3226;  // to the 0 key, 0x080F2E9C
constexpr std::uint32_t kRoom = 0x02000420u;

struct State {
    bool captured = false;
    std::vector<Item> items;                   // pristine
    std::vector<std::uint8_t> pristine_table;  // the treasure table
    std::vector<std::uint8_t> pristine_shops;  // kShops * kShopSize
    std::vector<std::uint16_t> pristine_prices;
    random_psynergy::Config applied;           // what the ROM image holds
    unsigned equipment_seen = ~0u;
    Plan plan;
    std::map<std::uint16_t, Content> field;    // by flag
    std::map<std::pair<std::int16_t, std::uint16_t>, std::uint16_t> gifts;  // (code file, item) -> item
    std::vector<std::vector<std::uint16_t>> shops;  // planned stock per shop (empty: unchanged)
};
inline State state;

inline std::uint32_t item_table() { return TLA_ROM(0x080B2364u); }
inline std::uint32_t treasure_table() { return TLA_ROM(0x080F2204u); }
inline std::uint32_t shop_table() { return TLA_ROM(0x0810C3F4u); }

inline bool any(const random_psynergy::Config& c) {
    return c.treasure || c.coins || c.weapon_finds || c.armour_finds;
}
inline bool active() { return any(state.applied); }

inline void capture(gba::GbaBus& b) {
    State& s = state;
    if (s.captured) return;
    s.items.resize(kItemCount);
    s.pristine_prices.resize(kItemCount);
    for (unsigned i = 0; i < kItemCount; ++i) {
        const std::uint32_t r = item_table() + i * 0x2Cu;
        s.items[i] = {b.read16(r), b.read8(r + 2), b.read8(r + 3), b.read8(r + 0xC)};
        s.pristine_prices[i] = s.items[i].price;
    }
    s.pristine_table.resize(kTableSize);
    for (std::uint32_t i = 0; i < kTableSize; ++i) s.pristine_table[i] = b.read8(treasure_table() + i);
    s.pristine_shops.resize(kShops * kShopSize);
    for (std::size_t i = 0; i < s.pristine_shops.size(); ++i) s.pristine_shops[i] = b.read8(shop_table() + i);
    s.captured = true;
}

inline void build(const random_psynergy::Config& c) {
    State& s = state;
    s.field.clear();
    s.gifts.clear();
    s.shops.assign(kShops, {});
    if (!any(c)) return;
    s.plan = make_plan(tla_data::spots, s.items, c.seed, c.fair_items, c.treasure, c.coins, c.fair_coins);
    new_finds(s.plan, tla_data::spots, s.items, c.weapon_finds, c.armour_finds, tla_data::shop_weapons,
              tla_data::shop_armour);
    for (std::size_t i = 0; i < std::size(tla_data::spots); ++i) {
        const Spot& spot = tla_data::spots[i];
        const Content& to = s.plan.contents[i];
        if (spot.kind == SpotKind::Field) s.field[spot.key] = to;
        else if (!to.coins)
            s.gifts[{static_cast<std::int16_t>(spot.key), static_cast<std::uint16_t>(spot.value)}] =
                static_cast<std::uint16_t>(to.value);
    }
    Rng rng(c.seed, 4);
    for (unsigned id = 1; id < kShops && c.treasure; ++id) {
        const std::uint8_t* rec = &s.pristine_shops[id * kShopSize];
        std::vector<std::uint16_t> original;
        for (std::size_t k = 0; k < kShopStock && (rec[k * 2] | rec[k * 2 + 1] << 8); ++k)
            original.push_back(static_cast<std::uint16_t>(rec[k * 2] | rec[k * 2 + 1] << 8));
        if (original.empty()) continue;
        s.shops[id] = shop_stock(original, {tla_data::shop_items, tla_data::shop_weapons, tla_data::shop_armour},
                                 s.items, rng, c.fair_shops);
    }
}

// Writes the file's treasure table, shops and prices into the ROM image when
// they differ (or Random Equipment rewrote the item records, prices included).
inline void apply(gba::GbaBus& b, const random_psynergy::Config& c) {
    State& s = state;
    const bool same = c == s.applied;
    const unsigned equipment = random_weapons::state.generation;
    if (same && equipment == s.equipment_seen) return;
    std::uint8_t* rom = random_weapons::rom_image(b);
    const std::uint32_t table = treasure_table() - 0x08000000u, shops = shop_table() - 0x08000000u,
                        items = item_table() - 0x08000000u;
    if (!same) {
        build(c);
        // The table: blocks of (room | 0x4000, flag) then 8-byte entries.
        std::copy_n(s.pristine_table.data(), kTableSize, rom + table);
        if (!s.field.empty()) {
            const auto h = [&](std::uint32_t at) {
                return static_cast<std::int16_t>(s.pristine_table[at] | s.pristine_table[at + 1] << 8);
            };
            std::uint32_t at = 0;
            while (at + 2 <= kTableSize && h(at) != 0) {
                at += 4;
                while (at + 8 <= kTableSize && h(at) != 0 && !(h(at) & 0x4000)) {
                    const auto it = s.field.find(static_cast<std::uint16_t>(h(at + 4)));
                    if (it != s.field.end()) {
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
        for (unsigned id = 0; id < kShops; ++id) {
            std::uint8_t* rec = rom + shops + id * kShopSize;
            std::copy_n(&s.pristine_shops[id * kShopSize], kShopSize, rec);
            const auto& stock = s.shops[id];
            if (stock.empty()) continue;
            for (std::size_t k = 0; k < kShopStock; ++k) {
                const std::uint16_t v = k < stock.size() ? stock[k] : 0;
                rec[k * 2] = static_cast<std::uint8_t>(v);
                rec[k * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
            }
        }
    }
    const std::vector<std::uint16_t> bargain = c.treasure && c.bargains ? bargain_prices(s.items)
                                                                        : std::vector<std::uint16_t>{};
    for (unsigned i = 0; i < kItemCount; ++i) {
        const std::uint16_t v = bargain.empty() ? s.pristine_prices[i] : bargain[i];
        rom[items + i * 0x2Cu] = static_cast<std::uint8_t>(v);
        rom[items + i * 0x2Cu + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    s.applied = c;
    s.equipment_seen = equipment;
}

// Once a game frame, after Random Equipment's frame.
inline void frame(const random_psynergy::Config& c) {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !b->rom_ptr()) return;
    capture(*b);
    apply(*b, c);
}

// Entry hook: true when r0 was changed. pc, lr (without the Thumb bit), r9
// (the pickup's flag), r0.
inline bool on_entry(std::uint32_t pc, std::uint32_t lr, std::uint32_t r9, std::uint32_t* r0) {
    if (!active()) return false;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return false;
    const State& s = state;
    if ((pc == TLA_ROM(0x08038120u) && lr == TLA_ROM(0x080CD254u)) ||
        (pc == TLA_ROM(0x080AD1D8u) && lr == TLA_ROM(0x080CD262u))) {
        const auto it = s.field.find(static_cast<std::uint16_t>(r9));
        if (it == s.field.end() || !it->second.coins || it->second.value <= 0x7FFFu) return false;
        *r0 = it->second.value;
        return true;
    }
    if (pc == TLA_ROM(0x080D260Cu)) {
        const std::uint16_t room = b->read16(kRoom);
        const auto file = static_cast<std::int16_t>(b->read16(TLA_ROM(0x080F17A8u) + room * 8u));
        const auto it = s.gifts.find({file, static_cast<std::uint16_t>(*r0 & 0x1FFu)});
        if (it == s.gifts.end()) return false;
        *r0 = it->second;
        return true;
    }
    return false;
}

// The spoiler sheet's lines: where every treasure went, and the shops.
inline void sheet_lines(const random_psynergy::Config& c, std::vector<run_sheet::Section>& out,
                        const run_sheet::ItemName& item) {
    const State& s = state;
    if (!any(c) || s.plan.contents.size() != std::size(tla_data::spots)) return;
    run_sheet::Section treasure{gbarecomp::ui_text("Treasure"), {}};
    for (std::size_t i = 0; i < std::size(tla_data::spots); ++i) {
        const Spot& spot = tla_data::spots[i];
        if (spot.fixed) continue;
        const Content& to = s.plan.contents[i];
        std::string row = std::string(spot.place) + ": " +
                          (to.coins ? std::to_string(to.value) + " " + gbarecomp::ui_text("coins")
                                    : item(static_cast<std::uint16_t>(to.value)));
        if (spot.kind == SpotKind::Gift) row += std::string(" (") + gbarecomp::ui_text("gift") + ")";
        treasure.lines.push_back(row);
    }
    std::stable_sort(treasure.lines.begin(), treasure.lines.end());
    out.push_back(std::move(treasure));
    run_sheet::Section shops{gbarecomp::ui_text("Shops"), {}};
    for (unsigned id = 1; id < kShops; ++id) {
        if (s.shops[id].empty()) continue;
        std::string row = std::string(gbarecomp::ui_text("Shop")) + " " + std::to_string(id) + ":";
        for (std::size_t k = 0; k < s.shops[id].size(); ++k) row += (k ? ", " : " ") + item(s.shops[id][k]);
        shops.lines.push_back(row);
    }
    if (!shops.lines.empty()) out.push_back(std::move(shops));
}

}  // namespace gsr::treasure::tla
