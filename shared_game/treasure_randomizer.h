// Random Treasure at run time: the part both games share (the rules are in
// treasure_plan.h). Each game's adapter supplies its tables and addresses and
// keeps its own hooks: The Broken Seal's src/treasure_randomizer_tbs.h, The
// Lost Age's src/tla_treasure_randomizer.h.
//
// Per loaded file: the plan's lookups for the hooks (field treasure by flag,
// gift by (scene code pack or file, original item)), each shop's new stock,
// and Bargains prices. Shops and prices are read by the game from the ROM,
// so they are written into the running ROM image (as Random Equipment does)
// and the pristine bytes return for other files.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "host_config_ui.h"  // ui_text
#include "random_psynergy_format.h"
#include "run_sheet.h"
#include "treasure_plan.h"
#include "treasure_tracker_types.h"

namespace gsr::treasure {

struct GameTables {
    std::span<const Spot> spots;
    ShopPools pools;
    unsigned item_count, shops;  // item records; shop records (0 unused)
};

struct Randomizer {
    explicit Randomizer(const GameTables& t) : tables(t) {}

    GameTables tables;
    bool captured = false;
    std::vector<Item> items;                   // pristine
    std::vector<std::uint8_t> pristine_shops;  // shops * kShopSize
    std::vector<std::uint16_t> pristine_prices;
    random_psynergy::Config applied;           // what the ROM image holds
    unsigned equipment_seen = ~0u;
    Plan plan;
    std::map<std::uint16_t, Content> field;    // by flag
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint16_t> gifts;  // (pack or file, item) -> item
    std::vector<std::vector<std::uint16_t>> shops;  // planned stock per shop (empty: unchanged)

    static bool any(const random_psynergy::Config& c) {
        return c.treasure || c.coins || c.weapon_finds || c.armour_finds;
    }
    // Random Items, Coins or Weapons / Armor finds is on for the loaded file.
    bool active() const { return any(applied); }

    // Once, from the bus: the item records (0x2C each) and the shop records.
    template <class Bus>
    void capture(Bus& b, std::uint32_t item_table, std::uint32_t shop_table) {
        if (captured) return;
        items.resize(tables.item_count);
        pristine_prices.resize(tables.item_count);
        for (unsigned i = 0; i < tables.item_count; ++i) {
            const std::uint32_t r = item_table + i * 0x2Cu;
            items[i] = {b.read16(r), b.read8(r + 2), b.read8(r + 3), b.read8(r + 0xC)};
            pristine_prices[i] = items[i].price;
        }
        pristine_shops.resize(tables.shops * kShopSize);
        for (std::size_t i = 0; i < pristine_shops.size(); ++i) pristine_shops[i] = b.read8(shop_table + i);
        captured = true;
    }

    // The plan's lookups for the hooks, and the shops' stock.
    void build(const random_psynergy::Config& c) {
        field.clear();
        gifts.clear();
        shops.assign(tables.shops, {});
        if (!any(c)) return;
        plan = make_plan(tables.spots, items, c.seed, c.fair_items, c.treasure, c.coins, c.fair_coins);
        new_finds(plan, tables.spots, items, c.weapon_finds, c.armour_finds, tables.pools.weapons,
                  tables.pools.armour);
        for (std::size_t i = 0; i < tables.spots.size(); ++i) {
            const Spot& spot = tables.spots[i];
            const Content& to = plan.contents[i];
            if (spot.kind == SpotKind::Field) field[spot.key] = to;
            else if (!to.coins) gifts[{spot.key, static_cast<std::uint16_t>(spot.value)}] = static_cast<std::uint16_t>(to.value);
        }
        Rng rng(c.seed, 4);
        for (unsigned id = 1; id < tables.shops && c.treasure; ++id) {
            const std::uint8_t* rec = &pristine_shops[id * kShopSize];
            std::vector<std::uint16_t> original;
            for (std::size_t k = 0; k < kShopStock && (rec[k * 2] | rec[k * 2 + 1] << 8); ++k)
                original.push_back(static_cast<std::uint16_t>(rec[k * 2] | rec[k * 2 + 1] << 8));
            if (original.empty()) continue;
            shops[id] = shop_stock(original, tables.pools, items, rng, c.fair_shops);
        }
    }

    // Writes the file's shops and prices into the ROM image (rom: its byte 0)
    // when they differ, or when Random Equipment rewrote the item records
    // (prices included; equipment: its generation). True when the plan was
    // rebuilt, so the game writes its own planned tables too.
    bool apply(std::uint8_t* rom, unsigned equipment, std::uint32_t item_table, std::uint32_t shop_table,
               const random_psynergy::Config& c) {
        const bool same = c == applied;
        if (same && equipment == equipment_seen) return false;
        const std::uint32_t shop_at = shop_table - 0x08000000u, item_at = item_table - 0x08000000u;
        if (!same) {
            build(c);
            for (unsigned id = 0; id < tables.shops; ++id) {
                std::uint8_t* rec = rom + shop_at + id * kShopSize;
                std::copy_n(&pristine_shops[id * kShopSize], kShopSize, rec);
                const auto& stock = shops[id];
                if (stock.empty()) continue;
                for (std::size_t k = 0; k < kShopStock; ++k) {
                    const std::uint16_t v = k < stock.size() ? stock[k] : 0;
                    rec[k * 2] = static_cast<std::uint8_t>(v);
                    rec[k * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
                }
            }
        }
        const std::vector<std::uint16_t> bargain = c.treasure && c.bargains ? bargain_prices(items)
                                                                            : std::vector<std::uint16_t>{};
        for (unsigned i = 0; i < tables.item_count; ++i) {
            const std::uint16_t v = bargain.empty() ? pristine_prices[i] : bargain[i];
            rom[item_at + i * 0x2Cu] = static_cast<std::uint8_t>(v);
            rom[item_at + i * 0x2Cu + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        applied = c;
        equipment_seen = equipment;
        return !same;
    }

    // The gift for (scene code pack or file, original item), if planned.
    bool gift(std::uint16_t scene, std::uint32_t* r0) const {
        const auto it = gifts.find({scene, static_cast<std::uint16_t>(*r0 & 0x1FFu)});
        if (it == gifts.end()) return false;
        *r0 = it->second;
        return true;
    }

    // The field treasure with this flag, when it now holds more coins than
    // the game's own amount field can carry (above limit).
    bool big_coins(std::uint16_t flag, std::uint32_t limit, std::uint32_t* amount) const {
        const auto it = field.find(flag);
        if (it == field.end() || !it->second.coins || it->second.value <= limit) return false;
        *amount = it->second.value;
        return true;
    }

    // The Treasure Tracker's reward for the field treasure with this flag:
    // the planned item ID or kCoins | amount; 0 when nothing is planned.
    unsigned tracker_reward(std::uint16_t flag) const {
        if (!active()) return 0;
        const auto it = field.find(flag);
        if (it == field.end()) return 0;
        return it->second.coins ? treasure_tracker::kCoins | it->second.value : it->second.value;
    }

    // The spoiler sheet's lines: where every treasure went, and the shops.
    void sheet_lines(const random_psynergy::Config& c, std::vector<run_sheet::Section>& out,
                     const run_sheet::ItemName& item) const {
        if (!any(c) || plan.contents.size() != tables.spots.size()) return;
        run_sheet::Section treasure{gbarecomp::ui_text("Treasure"), {}};
        for (std::size_t i = 0; i < tables.spots.size(); ++i) {
            const Spot& spot = tables.spots[i];
            if (spot.fixed) continue;
            const Content& to = plan.contents[i];
            std::string row = std::string(spot.place) + ": " +
                              (to.coins ? std::to_string(to.value) + " " + gbarecomp::ui_text("coins")
                                        : item(static_cast<std::uint16_t>(to.value)));
            if (spot.kind == SpotKind::Gift) row += std::string(" (") + gbarecomp::ui_text("gift") + ")";
            treasure.lines.push_back(row);
        }
        std::stable_sort(treasure.lines.begin(), treasure.lines.end());
        out.push_back(std::move(treasure));
        run_sheet::Section shop_lines{gbarecomp::ui_text("Shops"), {}};
        for (unsigned id = 1; id < tables.shops; ++id) {
            if (shops[id].empty()) continue;
            std::string row = std::string(gbarecomp::ui_text("Shop")) + " " + std::to_string(id) + ":";
            for (std::size_t k = 0; k < shops[id].size(); ++k) row += (k ? ", " : " ") + item(shops[id][k]);
            shop_lines.lines.push_back(row);
        }
        if (!shop_lines.lines.empty()) out.push_back(std::move(shop_lines));
    }
};

}  // namespace gsr::treasure
