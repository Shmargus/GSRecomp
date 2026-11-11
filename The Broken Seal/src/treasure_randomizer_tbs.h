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
//   read from the ROM, so the plan is written into the running ROM image as
//   Random Equipment does, and the pristine bytes return for other files.
// - Random Weapons / Armor finds change the same field and gift lookups
//   (treasure_plan.h new_finds); the pools are the generated ones.
// - Sol Sanctum (Jimmy, 7 November 2026: never a story item there; it is
//   left behind for good, and Skip Prologue skips it) never receives a
//   Psynergy item: the planner moves one only to an "exact" spot, and the
//   check below keeps Sol Sanctum's spots from ever being exact.
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

struct State {
    bool captured = false;
    std::vector<Item> items;                       // pristine
    std::vector<std::uint8_t> pristine_shops;      // kShops * kShopSize
    std::vector<std::uint16_t> pristine_prices;
    random_psynergy::Config applied;               // what the ROM image holds
    unsigned equipment_seen = ~0u;
    Plan plan;
    std::map<std::uint16_t, Content> field;        // by flag
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint16_t> gifts;  // (pack, item) -> item
    std::vector<std::vector<std::uint16_t>> shops;  // planned stock per shop (empty: unchanged)
};
inline State state;

inline std::uint32_t item_table() { return GS_ROM(0x0807B6A8u); }
inline std::uint32_t shop_table() { return GS_ROM(0x080B41ACu); }

inline random_psynergy::Config current() { return random_psynergy::tbs::current(); }
inline bool any(const random_psynergy::Config& c) {
    return c.treasure || c.coins || c.weapon_finds || c.armour_finds;
}
// Random Items, Coins or Weapons / Armor finds is on for the loaded file.
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
    s.pristine_shops.resize(kShops * kShopSize);
    for (std::size_t i = 0; i < s.pristine_shops.size(); ++i) s.pristine_shops[i] = b.read8(shop_table() + i);
    s.captured = true;
}

// The plan's lookups for the hooks, and the shops' stock.
inline void build(const random_psynergy::Config& c) {
    State& s = state;
    s.field.clear();
    s.gifts.clear();
    s.shops.assign(kShops, {});
    if (!any(c)) return;
    s.plan = make_plan(tbs_data::spots, s.items, c.seed, c.fair_items, c.treasure, c.coins, c.fair_coins);
    new_finds(s.plan, tbs_data::spots, s.items, c.weapon_finds, c.armour_finds, tbs_data::shop_weapons,
              tbs_data::shop_armour);
    for (std::size_t i = 0; i < std::size(tbs_data::spots); ++i) {
        const Spot& spot = tbs_data::spots[i];
        const Content& to = s.plan.contents[i];
        if (spot.kind == SpotKind::Field) s.field[spot.key] = to;
        else if (!to.coins) s.gifts[{spot.key, static_cast<std::uint16_t>(spot.value)}] = static_cast<std::uint16_t>(to.value);
    }
    Rng rng(c.seed, 4);
    for (unsigned id = 1; id < kShops && c.treasure; ++id) {
        const std::uint8_t* rec = &s.pristine_shops[id * kShopSize];
        std::vector<std::uint16_t> original;
        for (std::size_t k = 0; k < kShopStock && (rec[k * 2] | rec[k * 2 + 1] << 8); ++k)
            original.push_back(static_cast<std::uint16_t>(rec[k * 2] | rec[k * 2 + 1] << 8));
        if (original.empty()) continue;
        s.shops[id] = shop_stock(original, {tbs_data::shop_items, tbs_data::shop_weapons, tbs_data::shop_armour},
                                 s.items, rng, c.fair_shops);
    }
}

// Writes the file's shops and prices into the ROM image when they differ
// (or Random Equipment rewrote the item records, prices included).
inline void apply(gba::GbaBus& b, const random_psynergy::Config& c) {
    State& s = state;
    const bool same = c == s.applied;
    const unsigned equipment = random_weapons::state.generation;
    if (same && equipment == s.equipment_seen) return;
    std::uint8_t* rom = random_weapons::rom_image(b);
    const std::uint32_t shops = shop_table() - 0x08000000u, items = item_table() - 0x08000000u;
    if (!same) {
        build(c);
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
inline void frame() {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !b->rom_ptr()) return;
    capture(*b);
    apply(*b, current());
}

// Entry to the field pickup routine: apply the room's planned records.
inline void on_pickup(gba::GbaBus& b) {
    State& s = state;
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
    const State& s = state;
    if (!active() || record < 0x02000000u || record >= 0x02040000u) return false;
    const auto it = s.field.find(static_cast<std::uint16_t>(b.read16(record + 6u)));
    if (it == s.field.end() || !it->second.coins || it->second.value <= 0xFFFFu) return false;
    *amount = it->second.value;
    return true;
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
        const auto it = state.gifts.find({pack, static_cast<std::uint16_t>(*r0 & 0x1FFu)});
        if (it == state.gifts.end()) return false;
        *r0 = it->second;
        return true;
    }
    return false;
}

// The spoiler sheet's lines: where every treasure went, and the shops.
inline void sheet_lines(const random_psynergy::Config& c, std::vector<run_sheet::Section>& out,
                        const run_sheet::ItemName& item) {
    const State& s = state;
    if (!any(c) || s.plan.contents.size() != std::size(tbs_data::spots)) return;
    run_sheet::Section treasure{gbarecomp::ui_text("Treasure"), {}};
    for (std::size_t i = 0; i < std::size(tbs_data::spots); ++i) {
        const Spot& spot = tbs_data::spots[i];
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
