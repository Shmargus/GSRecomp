// Random Treasure: the seed's plan. Pure functions (no engine), so a host
// bench can run them on the real tables. Runtime part: each game's adapter
// (The Broken Seal's src/treasure_randomizer_tbs.h). Jimmy's design (6
// November 2026, docs/NEXT_PRIORITY_MODES.md section 5):
//  - field treasures and NPC gifts trade contents; coin spots and item spots
//    mix, and every coin spot gets a new amount (small ones common, thousands
//    sometimes, the coin cap a rare jackpot);
//  - Psynergy items (type 6) move with logic when Key Items is on (Jimmy, 7
//    November 2026: an option), else stay; story items stay;
//  - shops sell a random selection of their own kind; Bargains prices every
//    item by tier, 5 to 1000 coins;
//  - Fair Play spreads healing items and gear evenly through the story;
//  - Random Weapons / Armor finds (7 November 2026): every spot that holds a
//    weapon (or armour) once the rest is dealt gives a random one from the
//    whole game instead, any strength anywhere (Jimmy's choice).
//
// Logic (Jimmy's choice: the archived guide's story order). A Psynergy item
// moves only to a field spot the guide collects ("exact") in an earlier
// section than its own spot, or stays. Induction on sections: a spot of
// section n needs only Psynergy items the guide had by then, each now at its
// own spot or at one of an earlier section, so every spot stays reachable,
// as long as the guide's order holds. Gifts never receive Psynergy items, and
// cannot hold coins (the gift routine gives items only).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "random_weapons_plan.h"

namespace gsr::treasure {

enum class SpotKind : std::uint8_t { Field, Gift };

struct Spot {
    SpotKind kind;
    std::uint16_t key;       // field: treasure flag; gift: its scene's code pack (asset index)
    bool coins;              // field: the original holds coins
    std::uint32_t value;     // the original item, or coin amount
    std::uint16_t section;   // the guide's section (kNoSection: unknown)
    bool exact;              // the guide collects it in that section
    bool fixed;              // a story item: stays
    const char* place;
};
constexpr std::uint16_t kNoSection = 0xFFFF;

struct Content {
    bool coins = false;
    std::uint32_t value = 0;  // item id, or coin amount
    bool operator==(const Content&) const = default;
};

// What the planner needs of an item (from the item table, 0x2C per item:
// +0 price, +2 type, +3 flags, +0xC use kind).
struct Item {
    std::uint16_t price = 0;
    std::uint8_t type = 0, flags = 0, use = 0;
};
constexpr std::uint8_t kPsynergyItem = 6;
constexpr std::uint32_t kCoinCap = 999999;  // The Broken Seal 0x08079706

inline bool psynergy_item(std::span<const Item> items, const Content& c) {
    return !c.coins && c.value < items.size() && items[c.value].type == kPsynergyItem;
}

// splitmix64 with its own tag, apart from the other randomizers' streams.
struct Rng {
    std::uint64_t state;
    Rng(std::uint32_t seed, unsigned purpose)
        : state(std::uint64_t{seed} * 0x9E3779B97F4A7C15ull ^ (std::uint64_t{purpose} << 40) ^
                (std::uint64_t{0x5452u} << 48)) {}
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return n ? static_cast<std::size_t>(next() % n) : 0; }
    double unit() { return static_cast<double>(next() >> 11) / 9007199254740992.0; }
};

template <class T>
void shuffle(std::vector<T>& v, Rng& rng) {
    for (std::size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[rng.below(i)]);
}

// A coin amount. Starting shares, not measured for fun (Jimmy to judge): 75%
// 1..500, 20% 500..10,000, 4% 10,000..100,000 (each spread evenly on a log
// scale), 1% the cap.
inline std::uint32_t coin_amount(Rng& rng) {
    const double u = rng.unit();
    const auto between = [&](double lo, double hi) {
        return static_cast<std::uint32_t>(std::lround(std::exp(std::log(lo) + rng.unit() * (std::log(hi) - std::log(lo)))));
    };
    if (u < 0.75) return std::max<std::uint32_t>(1, between(1, 500));
    if (u < 0.95) return between(500, 10000);
    if (u < 0.99) return between(10000, 100000);
    return kCoinCap;
}

// Fair Play's kinds of contents.
enum Category { kHeal, kGear, kCoins, kOther, kCategories };
inline Category category(std::span<const Item> items, const Content& c) {
    if (c.coins) return kCoins;
    if (c.value >= items.size()) return kOther;
    const Item& it = items[c.value];
    if (it.type == 0 && it.use == 1) return kHeal;
    if (it.type >= 1 && it.type <= 9 && it.type != kPsynergyItem) return kGear;
    return kOther;
}

struct Plan {
    std::uint32_t seed = 0;
    bool fair_play = false;
    std::vector<Content> contents;  // per spot, in the table's order
};

// `shuffle_items`: chests, gifts (and shops) trade contents; `new_coins`:
// every coin spot a new amount; with both, coin and item spots also mix
// (Jimmy, 7 November 2026). `move_keys`: Psynergy items move (Key Items).
inline Plan make_plan(std::span<const Spot> spots, std::span<const Item> items, std::uint32_t seed,
                      bool fair_play, bool shuffle_items = true, bool new_coins = true, bool move_keys = true) {
    Plan plan;
    plan.seed = seed;
    plan.fair_play = fair_play;
    const std::size_t n = spots.size();
    plan.contents.resize(n);
    std::vector<bool> taken(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        plan.contents[i] = {spots[i].coins, spots[i].value};
        if (spots[i].fixed) taken[i] = true;
    }
    if (seed == 0) return plan;
    if (!shuffle_items) {
        // Coins only: every coin spot a new amount, nothing moves.
        Rng coins(seed, 3);
        for (std::size_t i = 0; i < n; ++i)
            if (new_coins && !spots[i].fixed && plan.contents[i].coins) plan.contents[i].value = coin_amount(coins);
        return plan;
    }
    // Items without Coins: coin spots keep their coins and stay out of the deal.
    if (!new_coins)
        for (std::size_t i = 0; i < n; ++i)
            if (spots[i].coins) taken[i] = true;

    // Psynergy items first: their own spot, or (Key Items) an exact field
    // spot of an earlier section.
    Rng keys(seed, 1);
    std::vector<std::size_t> key_spots;
    for (std::size_t i = 0; i < n; ++i)
        if (!spots[i].fixed && psynergy_item(items, {spots[i].coins, spots[i].value})) key_spots.push_back(i);
    shuffle(key_spots, keys);
    std::vector<std::size_t> home_free;  // a moved item's own spot joins the pool
    for (const std::size_t k : key_spots) {
        std::vector<std::size_t> can{k};
        if (move_keys && spots[k].section != kNoSection)
            for (std::size_t j = 0; j < n; ++j)
                if (!taken[j] && j != k && spots[j].kind == SpotKind::Field && spots[j].exact &&
                    spots[j].section < spots[k].section &&
                    !psynergy_item(items, {spots[j].coins, spots[j].value}))
                    can.push_back(j);
        const std::size_t to = can[keys.below(can.size())];
        taken[to] = true;
        if (to != k) {
            plan.contents[to] = {false, spots[k].value};
            // Its own spot is free now; the item that was at `to` joins the pool.
        }
    }

    // Everything else: the originals of the spots left, dealt again.
    std::vector<std::size_t> free;
    std::vector<Content> pool;
    for (std::size_t i = 0; i < n; ++i) {
        const Content original{spots[i].coins, spots[i].value};
        const bool key_home = !spots[i].fixed && psynergy_item(items, original);
        if (!taken[i]) free.push_back(i);
        // Pool: every non-fixed original except the Psynergy items (placed).
        if (!spots[i].fixed && !key_home && (new_coins || !original.coins)) pool.push_back(original);
    }
    // A Psynergy item that stayed home took its own spot and is not in the
    // pool; one that moved displaced an original that is in the pool, and
    // left its own spot free: the counts match.
    Rng deal(seed, 2);
    shuffle(pool, deal);
    std::sort(free.begin(), free.end(), [&](std::size_t a, std::size_t b) {
        return spots[a].section != spots[b].section ? spots[a].section < spots[b].section : a < b;
    });
    std::vector<Content> dealt;
    if (fair_play) {
        // Even spread: at each spot (in story order) the kind furthest behind
        // its share comes next.
        std::array<std::vector<Content>, kCategories> by;
        for (const Content& c : pool) by[category(items, c)].push_back(c);
        std::array<std::size_t, kCategories> total{}, given{};
        for (int c = 0; c < kCategories; ++c) total[c] = by[c].size();
        for (std::size_t k = 0; k < pool.size(); ++k) {
            int best = -1;
            double behind = -1e18;
            for (int c = 0; c < kCategories; ++c) {
                if (given[c] == total[c]) continue;
                const double d = static_cast<double>(total[c]) * static_cast<double>(k + 1) /
                                     static_cast<double>(pool.size()) - static_cast<double>(given[c]);
                if (d > behind) { behind = d; best = c; }
            }
            dealt.push_back(by[best][given[best]++]);
        }
    } else {
        dealt = pool;
    }
    for (std::size_t k = 0; k < free.size() && k < dealt.size(); ++k) plan.contents[free[k]] = dealt[k];
    // Gifts give items only: a gift dealt coins swaps with the nearest field
    // spot (in story order) dealt an item.
    for (std::size_t k = 0; k < free.size(); ++k) {
        const std::size_t g = free[k];
        if (spots[g].kind != SpotKind::Gift || !plan.contents[g].coins) continue;
        for (std::size_t d = 1; d < free.size(); ++d) {
            bool done = false;
            for (const long long at : {static_cast<long long>(k) + static_cast<long long>(d),
                                       static_cast<long long>(k) - static_cast<long long>(d)}) {
                if (at < 0 || at >= static_cast<long long>(free.size())) continue;
                const std::size_t f = free[static_cast<std::size_t>(at)];
                if (spots[f].kind == SpotKind::Field && !plan.contents[f].coins) {
                    std::swap(plan.contents[g], plan.contents[f]);
                    done = true;
                    break;
                }
            }
            if (done) break;
        }
    }
    // New coin amounts, in table order.
    Rng coins(seed, 3);
    for (std::size_t i = 0; i < n; ++i)
        if (new_coins && !spots[i].fixed && plan.contents[i].coins) plan.contents[i].value = coin_amount(coins);
    return plan;
}

// Random Weapons / Armor finds: after make_plan, the game keeps as many
// weapon (`weapons`) and armour (`armour`) finds as it had, but they move
// anywhere (Jimmy, 7 November 2026: "shuffles into the entire pool and can
// then be found anywhere"): each group's finds go to random item spots
// (field or gift; never a story spot, a Psynergy item or coins), the
// treasure there moving to the spots they left. Each find is then a piece of
// its group from the pool (every named, priced, uncursed piece of the game),
// each piece once until the pool runs out, then again.
inline void new_finds(Plan& plan, std::span<const Spot> spots, std::span<const Item> items, bool weapons,
                      bool armour, std::span<const std::uint16_t> weapon_pool,
                      std::span<const std::uint16_t> armour_pool) {
    if (plan.seed == 0 || (!weapons && !armour)) return;
    Rng rng(plan.seed, 5);
    const auto is_weapon = [&](const Content& c) {
        return !c.coins && c.value < items.size() && items[c.value].type == random_weapons::kWeapon;
    };
    const auto is_armour = [&](const Content& c) {
        return !c.coins && c.value < items.size() && random_weapons::armour_type(items[c.value].type);
    };
    std::vector<bool> placed(plan.contents.size(), false);  // a moved find: not moved again
    const auto scatter = [&](auto member) {
        std::vector<std::size_t> open, finds;
        for (std::size_t i = 0; i < spots.size() && i < plan.contents.size(); ++i) {
            const Content& c = plan.contents[i];
            if (spots[i].fixed || placed[i] || c.coins || psynergy_item(items, c)) continue;
            open.push_back(i);
            if (member(c)) finds.push_back(i);
        }
        shuffle(open, rng);
        open.resize(finds.size());  // the new homes
        std::vector<std::size_t> left, taken;  // finds' old spots not reused; new homes that held other treasure
        for (const std::size_t f : finds)
            if (std::find(open.begin(), open.end(), f) == open.end()) left.push_back(f);
        for (const std::size_t t : open)
            if (!member(plan.contents[t])) taken.push_back(t);
        shuffle(left, rng);
        const Content find = plan.contents[finds.empty() ? 0 : finds[0]];
        for (std::size_t k = 0; k < left.size() && k < taken.size(); ++k)
            plan.contents[left[k]] = plan.contents[taken[k]];
        for (const std::size_t t : open) {
            plan.contents[t] = find;  // the group's marker; the piece is drawn below
            placed[t] = true;
        }
    };
    if (weapons) scatter(is_weapon);
    if (armour) scatter(is_armour);
    struct Deck {
        std::span<const std::uint16_t> pool;
        std::vector<std::uint16_t> left;
        std::uint16_t draw(Rng& r) {
            if (left.empty()) {
                left.assign(pool.begin(), pool.end());
                shuffle(left, r);
            }
            const std::uint16_t v = left.back();
            left.pop_back();
            return v;
        }
    } weapon_deck{weapon_pool, {}}, armour_deck{armour_pool, {}};
    for (std::size_t i = 0; i < spots.size() && i < plan.contents.size(); ++i) {
        Content& c = plan.contents[i];
        if (spots[i].fixed || c.coins || c.value >= items.size()) continue;
        const std::uint8_t type = items[c.value].type;
        if (weapons && type == random_weapons::kWeapon && !weapon_pool.empty()) c.value = weapon_deck.draw(rng);
        else if (armour && random_weapons::armour_type(type) && !armour_pool.empty())
            c.value = armour_deck.draw(rng);
    }
}

// --- shops ---------------------------------------------------------------------------
// A shop record, 0x42 bytes (The Broken Seal 0x080B41AC + id * 0x42): +0x00
// 24 stock halfwords (to the first 0), +0x30 8 artifact halfwords, +0x40
// kind (0 items, 1 weapons, 2 armour).
constexpr std::size_t kShopSize = 0x42, kShopStock = 24, kShopKindAt = 0x40;

struct ShopPools {
    std::span<const std::uint16_t> items, weapons, armour;
};

// New stock for one shop: each item it sold becomes a distinct item of the
// same group (weapons, armour, items), so a mixed shop (The Lost Age's kind
// 3) stays mixed; sorted by group, then price.
inline std::vector<std::uint16_t> shop_stock(std::span<const std::uint16_t> original, const ShopPools& pools,
                                             std::span<const Item> table, Rng& rng) {
    const auto group = [&](std::uint16_t id) {
        const std::uint8_t type = id < table.size() ? table[id].type : 0;
        return type == random_weapons::kWeapon ? 0 : random_weapons::armour_type(type) ? 1 : 2;
    };
    const std::span<const std::uint16_t> by_group[3] = {pools.weapons, pools.armour, pools.items};
    std::size_t want[3] = {};
    for (const std::uint16_t id : original) ++want[group(id)];
    std::vector<std::uint16_t> out;
    for (int g = 0; g < 3; ++g) {
        std::vector<std::uint16_t> left(by_group[g].begin(), by_group[g].end());
        shuffle(left, rng);
        left.resize(std::min(want[g], left.size()));
        out.insert(out.end(), left.begin(), left.end());
    }
    std::sort(out.begin(), out.end(), [&](std::uint16_t a, std::uint16_t b) {
        const auto pa = a < table.size() ? table[a].price : 0, pb = b < table.size() ? table[b].price : 0;
        return group(a) != group(b) ? group(a) < group(b) : pa != pb ? pa < pb : a < b;
    });
    return out;
}

// Bargains: every priced item gets a tier price from 5 to 1000 coins by its
// place in the normal price order (the game's own prices rise with the
// story; equal prices share a tier), spread evenly on a log scale.
inline std::vector<std::uint16_t> bargain_prices(std::span<const Item> table) {
    std::vector<std::uint16_t> distinct;
    for (const Item& it : table)
        if (it.price) distinct.push_back(it.price);
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    std::vector<std::uint16_t> out(table.size(), 0);
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (!table[i].price) continue;
        const auto rank = static_cast<double>(std::lower_bound(distinct.begin(), distinct.end(), table[i].price) -
                                              distinct.begin());
        const double t = distinct.size() > 1 ? rank / static_cast<double>(distinct.size() - 1) : 0.0;
        out[i] = static_cast<std::uint16_t>(std::lround(5.0 * std::pow(200.0, t)));
    }
    return out;
}

}  // namespace gsr::treasure
