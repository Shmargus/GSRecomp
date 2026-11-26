// Random Treasure: the seed's plan. These are pure functions (no engine), so
// a host bench can run them on the real tables. The runtime part is
// shared_game/treasure_randomizer.h, then each game's adapter.
//
// Design:
//  - Field treasures and NPC gifts trade contents. Coin spots and item spots
//    mix, and every coin spot gets a new amount (small ones common, thousands
//    sometimes, the coin cap a rare jackpot).
//  - Key items never move in either game. Story items (`fixed`) and Psynergy
//    items (type 6) stay at their own spots, whatever the options.
//  - Shops sell a random selection of their own kind. Bargains prices every
//    item by tier, 5 to 1000 coins.
//  - Fair Play (a switch per thing) strives for a fair progression:
//      - Items spreads healing items and gear evenly through the story, with
//        gear loosely stronger as it goes.
//      - Coins gives amounts that grow with the story, as the game's own do.
//      - Shops draws new stock near each shop's own level.
//  - Random Weapons / Armor finds: once the rest is dealt, every spot that
//    holds a weapon (or armour) gives a random one from the whole game
//    instead, any strength anywhere.
//
// With every key item at home, every spot stays as reachable as in the
// normal game. Gifts cannot hold coins (the gift routine gives items only).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
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

// A coin amount. TODO-EVIDENCE: the shares are starting values, not measured.
// 75% 1..500, 20% 500..10,000, 4% 10,000..100,000 (each spread evenly on a log
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
// share it.
// TODO-EVIDENCE: the exponent is a first guess, not measured.
constexpr double kStrengthFalloff = 2.0;

inline double price_distance_weight(std::size_t distance, std::size_t count) {
    return std::pow(1.0 - static_cast<double>(distance) / static_cast<double>(count), kStrengthFalloff);
}

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

inline double rank_weight(std::size_t rank, std::size_t anchor, std::size_t count) {
    const std::size_t distance = rank < anchor ? anchor - rank : rank - anchor;
    return price_distance_weight(distance, count);
}

inline std::size_t pick_rank(std::size_t count, std::size_t anchor, std::vector<double>& weights, Rng& rng) {
    weights.clear();
    double total = 0;
    for (std::size_t rank = 0; rank < count; ++rank) {
        weights.push_back(rank_weight(rank, anchor, count));
        total += weights.back();
    }
    return pick_weighted(weights, total, rng);
}

// Fair Items' gear pacing (loose). `gear` comes back in the order it will be
// dealt through the story. For each place in turn, a piece is drawn from
// those left. The likeliest piece is the one whose place in the price order
// matches; the further off, the rarer. So gear is mostly around the level of
// where you are, with an occasional surprise either way.
inline void pace_gear(std::vector<Content>& gear, std::span<const Item> items, Rng& rng,
                     std::span<const std::uint16_t> effective = {}) {
    const auto price = [&](const Content& c) {
        if (c.value < effective.size() && effective[c.value] != 0) return effective[c.value];
        return c.value < items.size() ? items[c.value].price : std::uint16_t{0};
    };
    std::vector<std::uint16_t> prices;
    for (const Content& c : gear) prices.push_back(price(c));
    std::sort(prices.begin(), prices.end());
    std::vector<Content> left;
    left.swap(gear);
    const auto count = left.size();
    std::vector<double> weight;
    for (std::ptrdiff_t turn = 0; !left.empty(); ++turn) {
        weight.assign(left.size(), 0.0);
        double total = 0;
        for (std::size_t k = 0; k < left.size(); ++k) {
            const auto place = std::lower_bound(prices.begin(), prices.end(), price(left[k])) - prices.begin();
            weight[k] = price_distance_weight(static_cast<std::size_t>(std::abs(place - turn)), count);
            total += weight[k];
        }
        const std::size_t k = pick_weighted(weight, total, rng);
        gear.push_back(left[k]);
        left.erase(left.begin() + static_cast<std::ptrdiff_t>(k));
    }
}

inline std::vector<Content> spread_contents(std::span<const Content> pool, std::span<const Item> items,
                                             Rng& rng, bool pooled,
                                             std::span<const std::uint16_t> effective = {}) {
    std::array<std::vector<Content>, kCategories> kinds;
    for (const Content& c : pool) kinds[category(items, c)].push_back(c);
    pace_gear(kinds[kGear], items, rng, effective);
    std::vector<Content> dealt;
    dealt.reserve(pool.size());
    if (pooled) {
        pace_gear(kinds[kHeal], items, rng);
        pace_gear(kinds[kOther], items, rng);
        // Give each category one position per equal slice of the story.
        // Its items keep their relative order, while the seed determines
        // how neighbouring categories interleave in the same stretch.
        struct Event {
            double position;
            Content content;
        };
        std::vector<Event> events;
        events.reserve(pool.size());
        for (const auto& kind : kinds)
            for (std::size_t j = 0; j < kind.size(); ++j)
                events.push_back({(static_cast<double>(j) + rng.unit()) / static_cast<double>(kind.size()), kind[j]});
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
            return a.position < b.position;
        });
        for (const Event& event : events) dealt.push_back(event.content);
        return dealt;
    }
    std::array<std::size_t, kCategories> given{};
    for (std::size_t k = 0; k < pool.size(); ++k) {
        int best = -1;
        double behind = -1e18;
        for (int c = 0; c < kCategories; ++c) {
            if (given[c] == kinds[c].size()) continue;
            const double d = static_cast<double>(kinds[c].size()) * static_cast<double>(k + 1) /
                                 static_cast<double>(pool.size()) - static_cast<double>(given[c]);
            if (d > behind) { behind = d; best = c; }
        }
        dealt.push_back(kinds[best][given[best]++]);
    }
    return dealt;
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
// every coin spot a new amount; with both, coin and item spots also mix.
// Key items stay home in every mode. `fair_play` is Fair Items, `fair_coins`
// Fair Coins.
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
    const auto dealt = fair_play ? spread_contents(pool, items, deal, false) : std::move(pool);
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

// Random Weapons / Armor finds, applied after make_plan. The game keeps as
// many weapon (`weapons`) and armour (`armour`) finds as it had, but they
// move anywhere (they shuffle into the entire pool):
//   1. Each group's finds go to random item spots (field or gift; never a
//      story spot, a Psynergy item or coins). The treasure there moves to the
//      spots they left.
//   2. Each find then becomes a piece of its group from the pool (every
//      named, priced, uncursed piece of the game). Each piece is used once
//      until the pool runs out, then again.
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

struct ShopItemOrder {
    std::span<const Item> table;

    int group(std::uint16_t id) const {
        const std::uint8_t type = id < table.size() ? table[id].type : 0;
        return type == random_weapons::kWeapon ? 0 : random_weapons::armour_type(type) ? 1 : 2;
    }

    int price(std::uint16_t id) const { return id < table.size() ? table[id].price : 0; }

    bool by_price(std::uint16_t a, std::uint16_t b) const {
        const int pa = price(a), pb = price(b);
        return pa != pb ? pa < pb : a < b;
    }

    void sort_stock(std::vector<std::uint16_t>& stock) const {
        std::sort(stock.begin(), stock.end(), [&](std::uint16_t a, std::uint16_t b) {
            const int ga = group(a), gb = group(b);
            return ga != gb ? ga < gb : by_price(a, b);
        });
    }
};

// New stock for one shop (a chance of something new, not a
// guarantee, and strong gear should be rare).
//  - Each item the shop sold is replaced only some of the time
//    (kShopReplaceChance); the rest stay as they are.
//  - A replacement comes from the same group (weapons, armour, items), so a
//    mixed shop (The Lost Age's kind 3) stays mixed, and it is never
//    something the shop already stocks.
//  - Cheaper means likelier. An item's weight is
//    (1 - place / count)^kStrengthFalloff, where place is its position
//    in its group's price order (equal prices share a place): the cheapest
//    weighs 1, the middle one 1/4 and the dearest almost nothing.
//  - Fair Shops measures that from the shop's
//    own level instead: place becomes the distance, in the same price order,
//    from the item being replaced. Something about as dear as what the shop
//    sold is likeliest, and the further off (cheaper or dearer) the rarer,
//    so a late shop is not filled with beginner gear.
// TODO-EVIDENCE: the chance is a first guess, not measured.
// The result is sorted by group, then price.
constexpr double kShopReplaceChance = 1.0 / 3.0;

inline std::vector<std::uint16_t> shop_stock(std::span<const std::uint16_t> original, const ShopPools& pools,
                                             std::span<const Item> table, Rng& rng, bool fair_play = false) {
    const ShopItemOrder order{table};
    const std::span<const std::uint16_t> by_group[3] = {pools.weapons, pools.armour, pools.items};
    std::vector<std::uint16_t> out(original.begin(), original.end());
    const auto stocked = [&](std::uint16_t id) { return std::find(out.begin(), out.end(), id) != out.end(); };
    std::vector<double> weight;
    for (std::uint16_t& slot : out) {
        if (rng.unit() >= kShopReplaceChance) continue;
        const std::span<const std::uint16_t> pool = by_group[order.group(slot)];
        std::vector<std::uint16_t> prices;
        for (const std::uint16_t id : pool) prices.push_back(order.price(id));
        std::sort(prices.begin(), prices.end());
        weight.assign(pool.size(), 0.0);
        double total = 0;
        const auto place_of = [&](std::uint16_t id) {
            return std::lower_bound(prices.begin(), prices.end(), order.price(id)) - prices.begin();
        };
        const auto own = fair_play ? place_of(slot) : 0;  // Fair Play: the replaced item's place
        for (std::size_t k = 0; k < pool.size(); ++k) {
            if (stocked(pool[k])) continue;
            const auto place = std::abs(place_of(pool[k]) - own);
            weight[k] = price_distance_weight(static_cast<std::size_t>(place), prices.size());
            total += weight[k];
        }
        if (total <= 0) continue;
        slot = pool[pick_weighted(weight, total, rng)];
    }
    order.sort_stock(out);
    return out;
}

struct LootOptions {
    bool items, coins, weapons, armour;
    bool fair_items, fair_coins, fair_shops;
};

struct ArtifactShop {
    std::size_t free_slots = 0;
    bool weapons = false, armour = false;
};
constexpr std::uint8_t kNativeArtifactFlag = 1u << 2;

struct PooledLoot {
    Plan plan;
    std::vector<std::vector<std::uint16_t>> shops;
    std::map<std::uint16_t, std::uint16_t> weapon_strength;
    std::vector<std::vector<std::uint16_t>> artifact_additions;
};

inline void pooled_coin_amounts(Plan& plan, std::span<const Spot> spots,
                                std::span<const std::size_t> story_order) {
    std::vector<std::uint32_t> scale;
    for (const Spot& spot : spots)
        if (!spot.fixed && spot.coins) scale.push_back(std::max<std::uint32_t>(1, spot.value));
    if (scale.empty()) return;
    std::sort(scale.begin(), scale.end());
    std::vector<std::size_t> coins;
    for (const auto i : story_order)
        if (plan.contents[i].coins) coins.push_back(i);
    Rng rng(plan.seed, 3);
    std::vector<double> weights;
    weights.reserve(scale.size());
    for (std::size_t j = 0; j < coins.size(); ++j) {
        const std::size_t anchor = j * scale.size() / coins.size();
        plan.contents[coins[j]].value = scale[pick_rank(scale.size(), anchor, weights, rng)];
    }
}

// The original price table is the common yardstick for equipment placement
// and for a weapon's rolled strength. Randomized stats and Bargains are
// applied later and cannot change either roll.
inline PooledLoot pooled_loot(std::span<const Spot> spots, std::span<const Item> table,
                             const ShopPools& pools, const std::vector<std::vector<std::uint16_t>>& stock,
                             std::uint32_t seed, const LootOptions& options,
                             std::span<const std::uint16_t> artifacts = {},
                             std::span<const std::uint16_t> effective_equipment = {},
                             std::span<const std::uint16_t> region_sections = {},
                             std::span<const ArtifactShop> artifact_shops = {}) {
    PooledLoot out{{seed, options.fair_items, {}},
                   std::vector<std::vector<std::uint16_t>>(stock.size()), {},
                   std::vector<std::vector<std::uint16_t>>(stock.size())};
    auto& contents = out.plan.contents;
    contents.reserve(spots.size());
    Rng finds(seed, 5), shop_rng(seed, 4), strength(seed, 6), deal(seed, 2);
    const ShopItemOrder order{table};
    const auto gear_price = [&](std::uint16_t id) {
        return id < effective_equipment.size() && effective_equipment[id] != 0
            ? effective_equipment[id] : order.price(id);
    };
    const std::array<bool, 3> enabled{options.weapons, options.armour, options.items};
    std::array<std::vector<std::uint16_t>, 3> ranked;
    const std::span<const std::uint16_t> stock_pools[] = {pools.weapons, pools.armour, pools.items};
    for (int g = 0; g < 3; ++g) {
        for (const std::uint16_t id : stock_pools[g])
            if (id != 0 && id < table.size() && !psynergy_item(table, {false, id}))
                ranked[g].push_back(id);
        std::sort(ranked[g].begin(), ranked[g].end(), [&](std::uint16_t a, std::uint16_t b) {
            return order.by_price(a, b);
        });
        ranked[g].erase(std::unique(ranked[g].begin(), ranked[g].end()), ranked[g].end());
    }
    const auto rank_for_price = [&](const std::vector<std::uint16_t>& deck, int value) {
        return static_cast<std::size_t>(std::lower_bound(deck.begin(), deck.end(), value,
            [&](std::uint16_t candidate, int target) { return order.price(candidate) < target; }) - deck.begin());
    };
    const auto tier = [&](std::size_t anchor) {
        const auto& deck = ranked[0];
        std::vector<double> weights;
        return deck[pick_rank(deck.size(), anchor, weights, strength)];
    };
    const auto shop_anchor = [&](const std::vector<std::uint16_t>& deck, std::uint16_t id) {
        return std::min(rank_for_price(deck, order.price(id)), deck.size() - 1);
    };
    const auto shop_pacing = [&](const std::vector<std::uint16_t>& deck, int value, std::size_t anchor) {
        const auto rank = std::min(rank_for_price(deck, value), deck.size() - 1);
        const double weight = rank_weight(rank, anchor, deck.size());
        return options.fair_shops ? weight * weight : weight;
    };
    std::vector<bool> used(table.size(), false);
    std::vector<bool> sold_in_vanilla_shop(table.size(), false);
    const auto is_unique = [&](std::uint16_t id) {
        const int g = order.group(id);
        return id != 0 && id < table.size() && g < 2 && enabled[g];
    };
    for (const auto id : artifacts) {
        if (!is_unique(id)) continue;
        used[id] = true;
        if (order.group(id) == 0) out.weapon_strength.emplace(id, id);
    }
    std::vector<std::size_t> free;
    std::vector<std::uint16_t> sections;
    for (std::size_t i = 0; i < spots.size(); ++i) {
        const Spot& spot = spots[i];
        const Content original{spot.coins, spot.value};
        contents.push_back(original);
        if (spot.section != kNoSection) sections.push_back(spot.section);
        const auto id = static_cast<std::uint16_t>(spot.value);
        if (!spot.fixed && !psynergy_item(table, original) && (spot.coins ? options.coins : enabled[order.group(id)]))
            free.push_back(i);
        else if (!spot.coins && is_unique(id))
            used[id] = true;
    }
    std::sort(sections.begin(), sections.end());
    sections.erase(std::unique(sections.begin(), sections.end()), sections.end());
    // Region representatives share the original section scale. An absent or
    // unknown representative leaves the spot on its own section.
    const auto has_region = [&](std::size_t i) {
        return i < region_sections.size() && region_sections[i] != kNoSection &&
            std::binary_search(sections.begin(), sections.end(), region_sections[i]);
    };
    const auto section_for = [&](std::size_t i) {
        return has_region(i) ? region_sections[i] : spots[i].section;
    };

    // Keep unchanged stock first. With shop randomization, only the first
    // kept copy of an enabled equipment ID can claim it.
    for (std::size_t shop = 1; shop < stock.size(); ++shop) {
        if (options.items) out.shops[shop].resize(stock[shop].size());
        for (std::size_t slot = 0; slot < stock[shop].size(); ++slot) {
            const auto id = stock[shop][slot];
            const bool unique = is_unique(id);
            const int g = order.group(id);
            if (unique) sold_in_vanilla_shop[id] = true;
            if (!options.items) {
                if (unique) used[id] = true;
                continue;
            }
            if (!psynergy_item(table, {false, id}) && enabled[g]) {
                if (unique && used[id]) continue;
                double replace_chance = kShopReplaceChance;
                if (options.fair_shops && g < 2 && id < effective_equipment.size() &&
                    effective_equipment[id] != 0 && !ranked[g].empty()) {
                    const auto& deck = ranked[g];
                    const double affinity = shop_pacing(deck, gear_price(id), shop_anchor(deck, id));
                    replace_chance += (1.0 - kShopReplaceChance) * (1.0 - affinity);
                }
                if (shop_rng.unit() < replace_chance) continue;
            }
            out.shops[shop][slot] = id;
            if (unique) used[id] = true;
            if (unique && order.group(id) == 0 && !ranked[0].empty())
                out.weapon_strength.emplace(id, tier(shop_anchor(ranked[0], id)));
        }
    }

    // Draw equipment without replacement from the remaining IDs. Ordinary
    // contents still come from the original eligible slots.
    std::array<std::vector<std::uint16_t>, 2> available;
    for (int g = 0; g < 2; ++g) {
        for (const auto id : ranked[g])
            if (!used[id]) available[g].push_back(id);
        shuffle(available[g], finds);
    }
    std::vector<std::uint16_t> repeatable;
    for (const auto id : ranked[2])
        if (table[id].type == 0 && table[id].use == 1)
            repeatable.push_back(id);
    std::vector<Content> pool;
    pool.reserve(free.size());
    for (const std::size_t i : free) {
        Content content = contents[i];
        if (!content.coins) {
            const int g = order.group(static_cast<std::uint16_t>(content.value));
            if (g < 2) {
                if (!available[g].empty()) {
                    const auto id = available[g].back();
                    available[g].pop_back();
                    used[id] = true;
                    content.value = id;
                } else if (options.coins && spots[i].kind == SpotKind::Field) {
                    content = {true, 1};
                } else if (!repeatable.empty()) {
                    content.value = repeatable[finds.below(repeatable.size())];
                } else {
                    content.value = 0;
                }
            }
        }
        pool.push_back(content);
    }
    shuffle(pool, deal);
    const bool fair_order = options.fair_items || (options.coins && options.fair_coins);
    const auto traversal_section = [&](std::size_t i) {
        return fair_order ? section_for(i) : spots[i].section;
    };
    std::stable_sort(free.begin(), free.end(), [&](std::size_t a, std::size_t b) {
        return traversal_section(a) < traversal_section(b);
    });
    if (fair_order && !region_sections.empty()) {
        // Leave unmapped spots in their stable positions within each tier.
        // The separate stream keeps the ordinary contents and strength rolls intact.
        Rng ties(seed, 7);
        std::vector<std::size_t> positions, members;
        for (std::size_t first = 0; first < free.size();) {
            const auto tier_section = traversal_section(free[first]);
            positions.clear();
            members.clear();
            std::size_t next = first;
            for (; next < free.size() && traversal_section(free[next]) == tier_section; ++next) {
                if (!has_region(free[next])) continue;
                positions.push_back(next);
                members.push_back(free[next]);
            }
            shuffle(members, ties);
            for (std::size_t i = 0; i < members.size(); ++i) free[positions[i]] = members[i];
            first = next;
        }
    }
    if (options.fair_items) pool = spread_contents(pool, table, deal, true, effective_equipment);

    // Coin-bearing gifts swap with the nearest eligible field item. If
    // there are more coin rolls than field slots, keep the gift an item.
    for (std::size_t i = 0; i < free.size(); ++i) {
        if (spots[free[i]].kind != SpotKind::Gift || !pool[i].coins) continue;
        std::size_t nearest = free.size(), distance = free.size();
        for (std::size_t j = 0; j < free.size(); ++j) {
            if (spots[free[j]].kind != SpotKind::Field || pool[j].coins) continue;
            const std::size_t apart = i < j ? j - i : i - j;
            if (apart <= distance) {
                nearest = j;
                distance = apart;
            }
        }
        if (nearest != free.size()) std::swap(pool[i], pool[nearest]);
        else pool[i] = {false, repeatable.empty() ? 0u : repeatable[finds.below(repeatable.size())]};
    }
    for (std::size_t i = 0; i < free.size(); ++i) {
        const Spot& spot = spots[free[i]];
        const Content content = pool[i];
        contents[free[i]] = content;
        const auto id = static_cast<std::uint16_t>(content.value);
        if (content.coins || !options.weapons || order.group(id) != 0 || ranked[0].empty()) continue;
        const auto section = std::lower_bound(sections.begin(), sections.end(), section_for(free[i]));
        const std::size_t anchor = section == sections.end()
            ? shop_anchor(ranked[0], static_cast<std::uint16_t>(spot.coins ? 0 : spot.value))
            : static_cast<std::size_t>(section - sections.begin()) * (ranked[0].size() - 1) /
                std::max<std::size_t>(1, sections.size() - 1);
        out.weapon_strength.emplace(id, tier(anchor));
    }
    if (options.coins) {
        if (options.fair_coins) pooled_coin_amounts(out.plan, spots, free);
        else new_coin_amounts(out.plan, spots, false);
    }

    for (std::size_t shop = 1; shop < stock.size() && options.items; ++shop) {
        auto& row = out.shops[shop];
        auto& additions = out.artifact_additions[shop];
        const ArtifactShop artifacts_here = shop < artifact_shops.size() ? artifact_shops[shop] : ArtifactShop{};
        const auto requires_artifact_shelf = [&](std::uint16_t id, int g) {
            return !artifact_shops.empty() && g < 2 && !sold_in_vanilla_shop[id];
        };
        for (std::size_t slot = 0; slot < row.size(); ++slot) {
            if (row[slot]) continue;
            const auto old = stock[shop][slot];
            std::vector<std::uint16_t> candidates;
            std::vector<double> weights;
            double total = 0;
            for (int g = 0; g < 3; ++g) {
                if (!enabled[g] || ranked[g].empty()) continue;
                const auto& deck = ranked[g];
                // Shops always compare equipment with the stock they replace.
                // Fair Shops makes the same comparison for ordinary items.
                const std::size_t anchor = options.fair_shops || g < 2 ? shop_anchor(deck, old) : 0;
                for (const auto id : deck) {
                    if ((is_unique(id) && used[id]) || std::find(row.begin(), row.end(), id) != row.end()) continue;
                    const bool artifact = requires_artifact_shelf(id, g);
                    // Unusual gear can only use a compatible native artifact list.
                    if (artifact && (!(table[id].flags & kNativeArtifactFlag) ||
                                     additions.size() >= artifacts_here.free_slots ||
                                     !(g == 0 ? artifacts_here.weapons : artifacts_here.armour))) continue;
                    candidates.push_back(id);
                    // Downweight the rare pool so it collectively competes with
                    // roughly one usual offer. V6 does this only for weapons.
                    const auto rarity = artifact || (g == 0 && !sold_in_vanilla_shop[id]) ? deck.size() : 1;
                    // Both prices use the same vanilla rank scale, even
                    // when Fair Shops evaluates the candidate's donor.
                    const int candidate_price = options.fair_shops && g < 2 ? gear_price(id) : order.price(id);
                    const double pacing = shop_pacing(deck, candidate_price, anchor);
                    weights.push_back(pacing / deck.size() / rarity);
                    total += weights.back();
                }
            }
            if (total <= 0) {
                if (!repeatable.empty()) row[slot] = repeatable[shop_rng.below(repeatable.size())];
                continue;
            }
            const auto id = candidates[pick_weighted(weights, total, shop_rng)];
            if (requires_artifact_shelf(id, order.group(id))) additions.push_back(id);
            else row[slot] = id;
            if (is_unique(id)) used[id] = true;
            if (options.weapons && order.group(id) == 0)
                out.weapon_strength.emplace(id, tier(shop_anchor(ranked[0], old)));
        }
        row.erase(std::remove(row.begin(), row.end(), 0), row.end());
        order.sort_stock(row);
    }
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
