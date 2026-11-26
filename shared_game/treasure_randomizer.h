// Random Treasure at run time: the part both games share (the rules are in
// treasure_plan.h). Each game's adapter supplies its tables and addresses and
// keeps its own hooks: The Broken Seal's src/treasure_randomizer_tbs.h, The
// Lost Age's src/tla_treasure_randomizer.h.
//
// Per loaded file it holds:
//   - the plan's lookups for the hooks: field treasure by flag, and gifts by
//     (scene code pack or file, original item);
//   - each shop's new stock; and
//   - the Bargains prices.
// The game reads shops and prices from the ROM, so they are written into the
// running ROM image (as Random Equipment does). The pristine bytes return for
// other files.
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
#include "random_weapons.h"
#include "run_sheet.h"
#include "treasure_plan.h"
#include "treasure_tracker_types.h"

namespace gsr::treasure {

struct GameTables {
    std::span<const Spot> spots;
    ShopPools pools;
    unsigned item_count, shops;  // item records; shop records (0 unused)
    std::span<const std::uint16_t> region_sections = {};
};

struct Randomizer {
    explicit Randomizer(const GameTables& t) : tables(t) {}

    GameTables tables;
    bool captured = false;
    std::vector<Item> items;                   // pristine
    std::vector<std::uint8_t> pristine_shops;  // shops * kShopSize
    random_psynergy::Config applied;           // what the ROM image holds
    unsigned applied_equipment_revision = 0;  // shuffled donor prices used for pacing
    Plan plan;
    std::map<std::uint16_t, std::uint16_t> weapon_strength;
    std::map<std::uint16_t, Content> field;    // by flag
    std::map<std::pair<std::uint16_t, std::uint16_t>, std::uint16_t> gifts;  // (pack or file, item) -> item
    std::vector<std::vector<std::uint16_t>> shops;  // planned stock per shop (empty: unchanged)
    std::vector<std::vector<std::uint16_t>> shop_artifacts;  // new native one-copy offers

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
        for (unsigned i = 0; i < tables.item_count; ++i) {
            const std::uint32_t r = item_table + i * 0x2Cu;
            items[i] = {b.read16(r), b.read8(r + 2), b.read8(r + 3), b.read8(r + 0xC)};
        }
        pristine_shops.resize(tables.shops * kShopSize);
        for (std::size_t i = 0; i < pristine_shops.size(); ++i) pristine_shops[i] = b.read8(shop_table + i);
        captured = true;
    }

    // The plan's lookups for the hooks, and the shops' stock.
    void build(const random_psynergy::Config& c, std::span<const std::uint16_t> effective_equipment = {}) {
        field.clear();
        gifts.clear();
        shops.assign(tables.shops, {});
        shop_artifacts.assign(tables.shops, {});
        weapon_strength.clear();
        if (!any(c)) return;
        std::vector<std::vector<std::uint16_t>> original(tables.shops);
        std::vector<std::uint16_t> artifacts;
        std::vector<ArtifactShop> artifact_shops(c.regional_artifacts ? tables.shops : 0);
        if (c.treasure || c.pooled_loot) {
            for (unsigned id = 1; id < tables.shops; ++id) {
                const std::uint8_t* rec = &pristine_shops[id * kShopSize];
                for (std::size_t k = 0; k < kShopStock && (rec[k * 2] | rec[k * 2 + 1] << 8); ++k)
                    original[id].push_back(static_cast<std::uint16_t>(rec[k * 2] | rec[k * 2 + 1] << 8));
                if (c.pooled_loot) {
                    std::size_t free_slots = 0;
                    for (std::size_t at = kShopStock * 2; at < kShopKindAt; at += 2) {
                        if (const auto item = static_cast<std::uint16_t>(rec[at] | rec[at + 1] << 8))
                            artifacts.push_back(item);
                        else
                            ++free_slots;
                    }
                    if (c.regional_artifacts) {
                        const int kind = rec[kShopKindAt] | rec[kShopKindAt + 1] << 8;
                        artifact_shops[id] = {free_slots, kind == 1 || kind == 3, kind == 2 || kind == 3};
                    }
                }
            }
        }
        if (c.pooled_loot) {
            auto pooled = pooled_loot(tables.spots, items, tables.pools, original, c.seed,
                                      {c.treasure, c.coins, c.weapon_finds, c.armour_finds,
                                       c.fair_items, c.fair_coins, c.fair_shops}, artifacts, effective_equipment,
                                      c.regional_artifacts ? tables.region_sections : std::span<const std::uint16_t>{},
                                      artifact_shops);
            plan = std::move(pooled.plan);
            shops = std::move(pooled.shops);
            if (c.regional_artifacts) shop_artifacts = std::move(pooled.artifact_additions);
            weapon_strength = std::move(pooled.weapon_strength);
        } else {
            plan = make_plan(tables.spots, items, c.seed, c.fair_items, c.treasure, c.coins, c.fair_coins);
            new_finds(plan, tables.spots, items, c.weapon_finds, c.armour_finds, tables.pools.weapons,
                      tables.pools.armour);
            Rng rng(c.seed, 4);
            for (unsigned id = 1; id < tables.shops && c.treasure; ++id)
                if (!original[id].empty()) shops[id] = shop_stock(original[id], tables.pools, items, rng, c.fair_shops);
        }
        for (std::size_t i = 0; i < tables.spots.size(); ++i) {
            const Spot& spot = tables.spots[i];
            const Content& to = plan.contents[i];
            if (spot.kind == SpotKind::Field) field[spot.key] = to;
            else if (!to.coins) gifts[{spot.key, static_cast<std::uint16_t>(spot.value)}] = static_cast<std::uint16_t>(to.value);
        }
    }

    // Writes the loaded file's normal stock and prices. Equipment owns the
    // remaining item fields. True when the game needs to update field tables.
    bool apply(std::uint8_t* rom, std::uint32_t item_table, std::uint32_t shop_table,
               const random_psynergy::Config& c, std::span<const std::uint16_t> effective_equipment = {},
               unsigned equipment_revision = 0) {
        if (!c.pooled_loot || !(c.fair_items || c.fair_shops)) effective_equipment = {};
        if (effective_equipment.empty()) equipment_revision = 0;
        if (c == applied && equipment_revision == applied_equipment_revision) return false;
        const std::uint32_t shop_at = shop_table - 0x08000000u, item_at = item_table - 0x08000000u;
        build(c, effective_equipment);
        for (unsigned id = 0; id < tables.shops; ++id) {
            std::uint8_t* rec = rom + shop_at + id * kShopSize;
            std::copy_n(&pristine_shops[id * kShopSize], kShopSize, rec);
            const auto& stock = shops[id];
            if (stock.empty() && !(id != 0 && c.pooled_loot && c.treasure)) continue;
            for (std::size_t k = 0; k < kShopStock; ++k) {
                const std::uint16_t v = k < stock.size() ? stock[k] : 0;
                rec[k * 2] = static_cast<std::uint8_t>(v);
                rec[k * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
            }
            // Discovery of this merchant adds the listed pieces only once.
            // Preserve every vanilla artifact word and leave the shop kind alone.
            const auto& added = shop_artifacts[id];
            std::size_t next = 0;
            for (std::size_t at = kShopStock * 2; at < kShopKindAt && next < added.size(); at += 2) {
                if (rec[at] || rec[at + 1]) continue;
                const auto item = added[next++];
                rec[at] = static_cast<std::uint8_t>(item);
                rec[at + 1] = static_cast<std::uint8_t>(item >> 8);
            }
        }
        const std::vector<std::uint16_t> bargain = c.treasure && c.bargains ? bargain_prices(items)
                                                                            : std::vector<std::uint16_t>{};
        for (unsigned i = 0; i < tables.item_count; ++i) {
            const std::uint16_t v = bargain.empty() ? items[i].price : bargain[i];
            rom[item_at + i * 0x2Cu] = static_cast<std::uint8_t>(v);
            rom[item_at + i * 0x2Cu + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        applied = c;
        applied_equipment_revision = equipment_revision;
        return true;
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

    // The Treasure Tracker's reward for the gift (scene code pack or file,
    // original item): the planned item ID; 0 when nothing is planned.
    unsigned tracker_gift(std::uint16_t scene, std::uint16_t item) const {
        if (!active()) return 0;
        const auto it = gifts.find({scene, item});
        return it == gifts.end() ? 0 : it->second;
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
            const std::string shop = std::string(gbarecomp::ui_text("Shop")) + " " + std::to_string(id);
            if (!shops[id].empty()) {
                std::string row = shop + ":";
                for (std::size_t k = 0; k < shops[id].size(); ++k) row += (k ? ", " : " ") + item(shops[id][k]);
                shop_lines.lines.push_back(std::move(row));
            }
            for (const auto artifact : shop_artifacts[id])
                shop_lines.lines.push_back(shop + " (" + gbarecomp::ui_text("Artifacts") + "): 1 × " + item(artifact));
        }
        if (!shop_lines.lines.empty()) out.push_back(std::move(shop_lines));
    }
};

// Fair placement uses unscaled equipment prices. Scaling uses the treasure plan.
inline bool apply_frame(Randomizer& state, gba::GbaBus& bus,
                        const random_weapons::Game& equipment, const random_psynergy::Config& config,
                        std::uint32_t item_table, std::uint32_t shop_table,
                        void (*write_treasure_table)(std::uint8_t*) = nullptr) {
    state.capture(bus, item_table, shop_table);
    if (!random_weapons::capture(equipment, bus)) return false;
    const auto equipment_config = random_weapons::current(equipment);
    const auto fair = random_weapons::prepare_fair_strengths(
        equipment, equipment_config, config.pooled_loot && (config.fair_items || config.fair_shops));
    auto* rom = random_weapons::rom_image(bus);
    const bool changed = state.apply(rom, item_table, shop_table, config, fair.prices, fair.revision);
    if (changed && write_treasure_table) write_treasure_table(rom);
    if (!fair.prices.empty()) return fair.equipment_changed;
    return random_weapons::apply(equipment, equipment_config, state.weapon_strength, changed);
}

}  // namespace gsr::treasure
