// Random Treasure: the seed's plan. Pure functions (no engine), so a host
// bench can run them on the real tables. Runtime part: each game's adapter
// (The Broken Seal's src/treasure_randomizer_tbs.h). Jimmy's design (6
// November 2026, docs/NEXT_PRIORITY_MODES.md section 5):
//  - field treasures and NPC gifts trade contents; coin spots and item spots
//    mix, and every coin spot gets a new amount (small ones common, thousands
//    sometimes, the coin cap a rare jackpot);
//  - key items never move (Jimmy, 8 November 2026: "Disable key item
//    shuffling entirely in both games"): story items (`fixed`) and Psynergy
//    items (type 6) stay at their own spots, whatever the options;
//  - shops sell a random selection of their own kind; Bargains prices every
//    item by tier, 5 to 1000 coins;
//  - Fair Play (a switch per thing, 10 November 2026) strives for a fair
//    progression: Items spreads healing items and gear evenly through the
//    story, with gear loosely stronger as it goes; Coins gives amounts that
//    grow with the story as the game's own do; Shops draws new stock near
//    each shop's own level;
//  - Random Weapons / Armor finds (7 November 2026): every spot that holds a
//    weapon (or armour) once the rest is dealt gives a random one from the
//    whole game instead, any strength anywhere (Jimmy's choice).
//
// With every key item at home, every spot stays as reachable as in the
// normal game. Gifts cannot hold coins (the gift routine gives items only).
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
    bool fair_play = false;         // Fair Items
    std::vector<Content> contents;  // per spot, in the table's order
};

// How fast likelihood falls with distance in price order: weight
// (1 - distance / count)^kStrengthFalloff. Shops and Fair Play's gear pacing
// share it. A first guess, not measured; Jimmy judges it in play.
constexpr double kStrengthFalloff = 2.0;

// The index drawn from `weight` (summing to `total` > 0); the last one with
// weight, should rounding run past the end.
inline std::size_t pick_weighted(const std::vector<double>& weight, double total, Rng& rng) {
    double at = rng.unit() * total;
    std::size_t picked = 0;
    for (std::size_t k = 0; k < weight.size(); ++k) {
        if (weight[k] <= 0) continue;
        picked = k;
        if ((at -= weight[k]) < 0) break;
    }
    return picked;
}

// Fair Items' gear pacing (Jimmy, 10 November 2026: loose). `gear` comes
// back in the order it will be dealt through the story: for each place in
// turn, a piece is drawn from those left, likeliest the one whose place in
// the price order matches and rarer the further off. So gear is mostly
// around the level of where you are, with an occasional surprise either way.
inline void pace_gear(std::vector<Content>& gear, std::span<const Item> items, Rng& rng) {
    const auto price = [&](const Content& c) { return c.value < items.size() ? items[c.value].price : 0; };
    std::vector<std::uint16_t> prices;
    for (const Content& c : gear) prices.push_back(price(c));
    std::sort(prices.begin(), prices.end());
    std::vector<Content> left;
    left.swap(gear);
    const auto count = static_cast<double>(left.size());
    std::vector<double> weight;
    for (std::ptrdiff_t turn = 0; !left.empty(); ++turn) {
        weight.assign(left.size(), 0.0);
        double total = 0;
        for (std::size_t k = 0; k < left.size(); ++k) {
            const auto place = std::lower_bound(prices.begin(), prices.end(), price(left[k])) - prices.begin();
            weight[k] = std::pow(1.0 - static_cast<double>(std::abs(place - turn)) / count, kStrengthFalloff);
            total += weight[k];
        }
        const std::size_t k = pick_weighted(weight, total, rng);
        gear.push_back(left[k]);
        left.erase(left.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

// New amounts for the spots that now hold coins. Without Fair Coins: any
// amount anywhere (coin_amount), in table order. With it: the amounts grow
// with the story as the normal game's do. The game's own coin amounts, in
// rising order, are the scale; the coin spots, in story order, walk up it,
// each taking an amount between its neighbours on the scale. No jackpots.
inline void new_coin_amounts(Plan& plan, std::span<const Spot> spots, bool fair) {
    Rng coins(plan.seed, 3);
    const std::size_t n = std::min(spots.size(), plan.contents.size());
    if (!fair) {
        for (std::size_t i = 0; i < n; ++i)
            if (!spots[i].fixed && plan.contents[i].coins) plan.contents[i].value = coin_amount(coins);
        return;
    }
    std::vector<std::uint32_t> scale;
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < n; ++i) {
        if (spots[i].fixed) continue;
        if (spots[i].coins) scale.push_back(std::max<std::uint32_t>(1, spots[i].value));
        if (plan.contents[i].coins) order.push_back(i);
    }
    if (scale.empty()) return;
    std::sort(scale.begin(), scale.end());
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return spots[a].section < spots[b].section; });
    for (std::size_t j = 0; j < order.size(); ++j) {
        const std::size_t at = j * scale.size() / order.size();
        const double lo = scale[at ? at - 1 : 0], hi = scale[std::min(at + 1, scale.size() - 1)];
        plan.contents[order[j]].value = static_cast<std::uint32_t>(
            std::lround(std::exp(std::log(lo) + coins.unit() * (std::log(hi) - std::log(lo)))));
    }
}

// `shuffle_items`: chests, gifts (and shops) trade contents; `new_coins`:
// every coin spot a new amount; with both, coin and item spots also mix
// (Jimmy, 7 November 2026). Key items stay home in every mode. `fair_play`
// is Fair Items, `fair_coins` Fair Coins.
inline Plan make_plan(std::span<const Spot> spots, std::span<const Item> items, std::uint32_t seed,
                      bool fair_play, bool shuffle_items = true, bool new_coins = true,
                      bool fair_coins = false) {
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
        if (new_coins) new_coin_amounts(plan, spots, fair_coins);
        return plan;
    }
    // Items without Coins: coin spots keep their coins and stay out of the deal.
    if (!new_coins)
        for (std::size_t i = 0; i < n; ++i)
            if (spots[i].coins) taken[i] = true;

    // Psynergy items (key items) keep their own spots.
    for (std::size_t i = 0; i < n; ++i)
        if (psynergy_item(items, {spots[i].coins, spots[i].value})) taken[i] = true;

    // Everything else: the originals of the spots left, dealt again.
    std::vector<std::size_t> free;
    std::vector<Content> pool;
    for (std::size_t i = 0; i < n; ++i)
        if (!taken[i]) {
            free.push_back(i);
            pool.push_back({spots[i].coins, spots[i].value});
        }
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
        pace_gear(by[kGear], items, deal);
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
    if (new_coins) new_coin_amounts(plan, spots, fair_coins);
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

// New stock for one shop (Jimmy, 7 November 2026: a chance of something new,
// not a guarantee, and strong gear should be rare).
//  - Each item the shop sold is replaced only some of the time
//    (kShopReplaceChance); the rest stay as they are.
//  - A replacement comes from the same group (weapons, armour, items), so a
//    mixed shop (The Lost Age's kind 3) stays mixed, and it is never
//    something the shop already stocks.
//  - Cheaper means likelier. An item's weight is
//    (1 - place / count)^kStrengthFalloff, where place is its position
//    in its group's price order (equal prices share a place): the cheapest
//    weighs 1, the middle one 1/4 and the dearest almost nothing.
//  - Fair Shops (Jimmy, 10 November 2026) measures that from the shop's own
//    level instead: place becomes the distance, in the same price order,
//    from the item being replaced. Something about as dear as what the shop
//    sold is likeliest, and the further off (cheaper or dearer) the rarer,
//    so a late shop is not filled with beginner gear.
// The chance is a first guess, not measured; Jimmy judges it in play.
// The result is sorted by group, then price.
constexpr double kShopReplaceChance = 1.0 / 3.0;

inline std::vector<std::uint16_t> shop_stock(std::span<const std::uint16_t> original, const ShopPools& pools,
                                             std::span<const Item> table, Rng& rng, bool fair_play = false) {
    const auto group = [&](std::uint16_t id) {
        const std::uint8_t type = id < table.size() ? table[id].type : 0;
        return type == random_weapons::kWeapon ? 0 : random_weapons::armour_type(type) ? 1 : 2;
    };
    const auto price = [&](std::uint16_t id) { return id < table.size() ? table[id].price : 0; };
    const std::span<const std::uint16_t> by_group[3] = {pools.weapons, pools.armour, pools.items};
    std::vector<std::uint16_t> out(original.begin(), original.end());
    const auto stocked = [&](std::uint16_t id) { return std::find(out.begin(), out.end(), id) != out.end(); };
    std::vector<double> weight;
    for (std::uint16_t& slot : out) {
        if (rng.unit() >= kShopReplaceChance) continue;
        const std::span<const std::uint16_t> pool = by_group[group(slot)];
        std::vector<std::uint16_t> prices;
        for (const std::uint16_t id : pool) prices.push_back(price(id));
        std::sort(prices.begin(), prices.end());
        weight.assign(pool.size(), 0.0);
        double total = 0;
        const auto place_of = [&](std::uint16_t id) {
            return std::lower_bound(prices.begin(), prices.end(), price(id)) - prices.begin();
        };
        const auto own = fair_play ? place_of(slot) : 0;  // Fair Play: the replaced item's place
        for (std::size_t k = 0; k < pool.size(); ++k) {
            if (stocked(pool[k])) continue;
            const auto place = std::abs(place_of(pool[k]) - own);
            weight[k] = std::pow(1.0 - static_cast<double>(place) / static_cast<double>(prices.size()),
                                 kStrengthFalloff);
            total += weight[k];
        }
        if (total <= 0) continue;
        slot = pool[pick_weighted(weight, total, rng)];
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
