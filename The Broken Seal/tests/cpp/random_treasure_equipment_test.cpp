// Host-only regression test. All locations and records are synthetic.
#include "treasure_plan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

namespace {

namespace tr = gsr::treasure;
namespace eq = gsr::random_weapons;

constexpr std::uint16_t kFirstWeapon = 1;
constexpr std::uint16_t kLastWeapon = 24;
constexpr std::uint16_t kFirstArmour = 25;
constexpr std::uint16_t kLastArmour = 44;
constexpr std::uint16_t kHerb = 45;
constexpr std::uint16_t kNut = 46;
constexpr std::uint16_t kKey = 47;
constexpr unsigned kSeeds = 2048;

void require(bool condition, const char* message, unsigned seed = 0) {
    if (condition) return;
    std::fprintf(stderr, "%s (seed %u)\n", message, seed);
    std::exit(1);
}

struct Fixture {
    std::array<tr::Item, kKey + 1> items{};
    std::vector<std::uint16_t> weapon_pool, armour_pool;
    std::vector<eq::Weapon> weapons;
    std::array<tr::Spot, 14> spots{{
        {tr::SpotKind::Field,  1, true,    15,  1, true, false, "Early coins"},
        {tr::SpotKind::Field,  2, false,    2,  1, true, false, "Early weapon"},
        {tr::SpotKind::Field,  3, false,   26,  1, true, false, "Early armour"},
        {tr::SpotKind::Field,  4, false, kHerb, 2, true, false, "Early herb"},
        {tr::SpotKind::Gift,   5, false,    5,  2, true, false, "Weapon gift"},
        {tr::SpotKind::Field,  6, false, kKey,  3, true, false, "Key"},
        {tr::SpotKind::Field,  7, false,   10,  3, true, true,  "Fixed weapon"},
        {tr::SpotKind::Field,  8, true,    99,  3, true, true,  "Fixed coins"},
        {tr::SpotKind::Field,  9, true,   250,  8, true, false, "Late coins"},
        {tr::SpotKind::Field, 10, false,   22,  8, true, false, "Late weapon"},
        {tr::SpotKind::Field, 11, false,   42,  8, true, false, "Late armour"},
        {tr::SpotKind::Gift,  12, false,   40,  8, true, false, "Armour gift"},
        {tr::SpotKind::Field, 13, false, kNut,  9, true, false, "Late nut"},
        {tr::SpotKind::Gift,  14, false, kHerb, 9, true, false, "Item gift"},
    }};

    Fixture() {
        for (std::uint16_t id = kFirstWeapon; id <= kLastWeapon; ++id) {
            items[id] = {static_cast<std::uint16_t>(100 * id), eq::kWeapon, 0, 0};
            weapon_pool.push_back(id);
            eq::Weapon weapon;
            weapon.id = id;
            weapon.type = eq::kWeapon;
            weapon.original[eq::kType] = eq::kWeapon;
            eq::set_attack(weapon.original, 10 * id);
            if (id != 2) {
                weapon.original[eq::kUnleash] = static_cast<std::uint8_t>(100 + id);
                weapon.original[eq::kChance] = id % 2 ? 7 : 8;
            }
            weapon.planned = weapon.original;
            weapons.push_back(weapon);
        }
        for (std::uint16_t id = kFirstArmour; id <= kLastArmour; ++id) {
            items[id] = {static_cast<std::uint16_t>(100 * (id - kFirstArmour + 1)), 2, 0, 0};
            armour_pool.push_back(id);
        }
        items[kHerb] = {10, 0, 0, 1};
        items[kNut] = {20, 0, 0, 1};
        items[kKey].type = tr::kPsynergyItem;
    }
};

void check_protected(const Fixture& fixture, const tr::Plan& plan, unsigned seed) {
    for (std::size_t i = 0; i < fixture.spots.size(); ++i) {
        const auto& spot = fixture.spots[i];
        const tr::Content vanilla{spot.coins, spot.value};
        if (spot.fixed || tr::psynergy_item(fixture.items, vanilla))
            require(plan.contents[i] == vanilla, "Protected treasure changed", seed);
        if (spot.kind == tr::SpotKind::Gift)
            require(!plan.contents[i].coins, "Gift holds coins", seed);
    }
}

void check_legacy_options(const Fixture& fixture) {
    const auto& spots = fixture.spots;
    const auto& items = fixture.items;
    bool items_changed = false;
    bool coin_amount_changed = false;
    for (unsigned seed = 1; seed <= kSeeds; ++seed) {
        const auto disabled = tr::make_plan(spots, items, seed, false, false, false, false);
        const auto items_only = tr::make_plan(spots, items, seed, false, true, false, false);
        const auto coins_only = tr::make_plan(spots, items, seed, false, false, true, true);
        const auto both_on = tr::make_plan(spots, items, seed, true, true, true, true);
        check_protected(fixture, items_only, seed);
        check_protected(fixture, coins_only, seed);
        check_protected(fixture, both_on, seed);
        for (std::size_t i = 0; i < spots.size(); ++i) {
            const tr::Content vanilla{spots[i].coins, spots[i].value};
            require(disabled.contents[i] == vanilla, "Disabled toggles changed treasure", seed);
            if (vanilla.coins) {
                require(items_only.contents[i] == vanilla, "Items alone changed a coin find", seed);
                require(coins_only.contents[i].coins, "Coins alone moved a coin find", seed);
                coin_amount_changed |= coins_only.contents[i] != vanilla;
            } else {
                require(coins_only.contents[i] == vanilla, "Coins alone changed an item find", seed);
                items_changed |= items_only.contents[i] != vanilla;
            }
        }
    }
    require(items_changed, "Items alone never changed an item");
    require(coin_amount_changed, "Coins alone never changed an amount");
}

using Stock = std::vector<std::vector<std::uint16_t>>;

bool weapon(const Fixture& fixture, const tr::Content& content) {
    return !content.coins && content.value < fixture.items.size() &&
           fixture.items[content.value].type == eq::kWeapon;
}

bool armour(const Fixture& fixture, const tr::Content& content) {
    return !content.coins && content.value < fixture.items.size() &&
           eq::armour_type(fixture.items[content.value].type);
}

void check_pooled_loot(const Fixture& fixture) {
    const std::array consumables{kHerb, kNut};
    const tr::ShopPools pools{consumables, fixture.weapon_pool, fixture.armour_pool};
    const Stock stock{{}, {kHerb, kNut, 1, 2, 5, 25, 26}, {kHerb, kNut, 1, 21, 23, 26, 43}};
    constexpr int top_quartile_attack = (kLastWeapon - kLastWeapon / 4 + 1) * 10;
    unsigned moved_weapon = 0, moved_coin = 0, moved_armour = 0, armour_coin = 0, early_strong = 0;
    unsigned first_weapon = 0, first_coin = 0, first_armour = 0, first_strong = 0;
    unsigned early_count = 0, late_count = 0;
    long long early_attack = 0, late_attack = 0;
    std::array<unsigned, 2> shop_weapon_count{};
    std::array<long long, 2> shop_weapon_attack{};
    std::set<std::uint16_t> early_shop_weapons, late_shop_weapons, armour_in_shops;
    bool mixed_stock = false;
    std::set<unsigned> gear_locations;
    for (unsigned seed = 1; seed <= kSeeds; ++seed) {
        const auto zero = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed,
                                          {false, false, true, false, false, false, false});
        const auto weapons_coins = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed,
                                                  {false, true, true, false, false, false, false});
        const auto armour_coins = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed,
                                                 {false, true, false, true, false, false, false});
        const auto all = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed,
                                         {true, true, true, true, true, true, true});
        check_protected(fixture, zero.plan, seed);
        check_protected(fixture, weapons_coins.plan, seed);
        check_protected(fixture, armour_coins.plan, seed);
        check_protected(fixture, all.plan, seed);
        const auto scaled = eq::make_plan(fixture.weapons, seed, true, true, false, all.weapon_strength);
        std::set<std::uint16_t> seen_gear;
        unsigned gear_locations_this_seed = 0;
        bool weapon_to_coin = false, coin_to_weapon = false, armour_to_coin = false, coin_to_armour = false;
        const auto check_gear = [&](std::uint16_t id) {
            const tr::Content content{false, id};
            require(id > 0 && id < fixture.items.size(), "Invalid synthetic gear or item", seed);
            if (!weapon(fixture, content) && !armour(fixture, content)) return;
            require(seen_gear.insert(id).second, "Gear repeated across shops, field, or gifts", seed);
            if (!weapon(fixture, content) || id == fixture.spots[6].value) return;
            const auto strength = all.weapon_strength.find(id);
            require(strength != all.weapon_strength.end(), "Pooled weapon has no strength anchor", seed);
            const auto& donor = fixture.weapons[strength->second - kFirstWeapon].original;
            const auto& planned = scaled.weapons[id - kFirstWeapon].planned;
            const auto unleash = eq::unleash_near(donor, fixture.weapons);
            require(eq::attack_of(planned) == eq::attack_of(donor) &&
                    eq::unleash_of(planned) == unleash.id && planned[eq::kChance] == unleash.chance,
                    "Scaling did not use the pooled weapon's strength and associated unleash", seed);
        };
        for (std::size_t i = 0; i < fixture.spots.size(); ++i) {
            const auto& spot = fixture.spots[i];
            const tr::Content vanilla{spot.coins, spot.value};
            const auto& w = weapons_coins.plan.contents[i];
            const auto& a = armour_coins.plan.contents[i];
            const auto& combined = all.plan.contents[i];
            if (weapon(fixture, w) && !spot.fixed) {
                for (const auto& row : stock)
                    require(std::find(row.begin(), row.end(), w.value) == row.end(),
                            "Field or gift duplicated a weapon from unchanged stock", seed);
            }
            if (armour(fixture, a) && !spot.fixed) {
                for (const auto& row : stock)
                    require(std::find(row.begin(), row.end(), a.value) == row.end(),
                            "Field or gift duplicated armour from unchanged stock", seed);
            }
            if (spot.coins) {
                require(zero.plan.contents[i] == vanilla, "Weapons alone changed a coin find", seed);
                weapon_to_coin |= weapon(fixture, w);
                armour_to_coin |= armour(fixture, a);
            } else {
                if (!weapon(fixture, vanilla))
                    require(w == vanilla, "Weapons changed a disabled treasure group", seed);
                else
                    coin_to_weapon |= w.coins;
                if (!armour(fixture, vanilla))
                    require(a == vanilla, "Armour changed a disabled treasure group", seed);
                else
                    coin_to_armour |= a.coins;
            }
            if (combined.coins) continue;
            if (!spot.fixed && (weapon(fixture, combined) || armour(fixture, combined)))
                gear_locations_this_seed |= 1u << i;
            check_gear(static_cast<std::uint16_t>(combined.value));
            if (spot.fixed || !weapon(fixture, combined)) continue;
            const int attack = eq::attack_of(scaled.weapons[combined.value - kFirstWeapon].planned);
            if (spot.section <= 2) {
                early_attack += attack;
                ++early_count;
                if (attack >= top_quartile_attack) {
                    if (!first_strong) first_strong = seed;
                    ++early_strong;
                }
            } else if (spot.section >= 8) {
                late_attack += attack;
                ++late_count;
            }
        }
        gear_locations.insert(gear_locations_this_seed);
        if (weapon_to_coin && !first_weapon) first_weapon = seed;
        if (coin_to_weapon && !first_coin) first_coin = seed;
        if (armour_to_coin && !first_armour) first_armour = seed;
        moved_weapon += weapon_to_coin;
        moved_coin += coin_to_weapon;
        moved_armour += armour_to_coin;
        armour_coin += coin_to_armour;

        for (std::size_t shop = 1; shop < stock.size(); ++shop) {
            require(weapons_coins.shops[shop].empty() && armour_coins.shops[shop].empty(),
                    "Equipment alone generated replacement shop stock", seed);
            require(all.shops[shop].size() == stock[shop].size(), "Pooled stock lost a shop slot", seed);
            unsigned groups = 0;
            auto& choices = shop == 1 ? early_shop_weapons : late_shop_weapons;
            for (const auto id : all.shops[shop]) {
                check_gear(id);
                if (weapon(fixture, {false, id})) {
                    choices.insert(id);
                    groups |= 1;
                    ++shop_weapon_count[shop - 1];
                    shop_weapon_attack[shop - 1] += eq::attack_of(scaled.weapons[id - kFirstWeapon].planned);
                } else if (armour(fixture, {false, id})) {
                    armour_in_shops.insert(id);
                    groups |= 2;
                } else {
                    groups |= 4;
                }
            }
            mixed_stock |= groups == 7;
        }
    }
    require(moved_weapon && moved_coin, "Weapons and coins never exchanged spot types");
    require(moved_armour && armour_coin, "Armour and coins never exchanged spot types");
    require(gear_locations.size() > 1, "Fair Items fixed which spots hold gear across seeds");
    require(early_count && late_count && late_attack * early_count > early_attack * late_count,
            "Late weapon locations did not average stronger than early ones");
    require(early_strong && early_strong * 4 < early_count,
            "Top-quartile attack was absent or no rarer than uniform at early weapon locations");
    require(mixed_stock && early_shop_weapons.size() > stock[1].size() &&
            late_shop_weapons.size() > stock[2].size() && armour_in_shops.size() > stock[1].size(),
            "Shop stock did not span a wider pool than its starting stock");
    require(shop_weapon_count[0] && shop_weapon_count[1] &&
            shop_weapon_attack[1] * shop_weapon_count[0] > shop_weapon_attack[0] * shop_weapon_count[1],
            "Late shops did not average stronger weapons than early shops");
    std::printf("Weapons at coin spots: %u/%u seeds (first %u); coins at weapon spots: %u/%u (first %u).\n",
                moved_weapon, kSeeds, first_weapon, moved_coin, kSeeds, first_coin);
    std::printf("Armour at coin spots: %u/%u seeds (first %u); coins at armour spots: %u/%u.\n",
                moved_armour, kSeeds, first_armour, armour_coin, kSeeds);
    std::printf("Mean attack: early %.2f (%u finds), late %.2f (%u finds); early top quartile: %u/%u (first seed %u).\n",
                static_cast<double>(early_attack) / early_count, early_count,
                static_cast<double>(late_attack) / late_count, late_count, early_strong, early_count, first_strong);
    std::printf("Distinct shop gear: %zu early weapons, %zu late weapons, %zu armour across both shops.\n",
                early_shop_weapons.size(), late_shop_weapons.size(), armour_in_shops.size());
    std::printf("Shop mean weapon attack: early %.2f (%u offers), late %.2f (%u offers).\n",
                static_cast<double>(shop_weapon_attack[0]) / shop_weapon_count[0], shop_weapon_count[0],
                static_cast<double>(shop_weapon_attack[1]) / shop_weapon_count[1], shop_weapon_count[1]);
    std::printf("Fair Items produced %zu distinct sets of gear locations.\n", gear_locations.size());
}

struct Mean {
    unsigned count = 0;
    long long sum = 0;

    void add(int value) {
        ++count;
        sum += value;
    }

    double value() const { return static_cast<double>(sum) / count; }
};

void check_artifacts_and_donor_pacing(const Fixture& fixture) {
    const std::array consumables{kHerb, kNut};
    const tr::ShopPools pools{consumables, fixture.weapon_pool, fixture.armour_pool};
    const Stock stock{{}, {1}, {kLastWeapon}, {26}};
    const std::array<std::uint16_t, 2> artifacts{1, 26};
    std::array<std::uint16_t, kKey + 1> donated_prices{};
    for (const auto id : fixture.weapon_pool)
        donated_prices[id] = fixture.items[kFirstWeapon + kLastWeapon - id].price;
    for (const auto id : fixture.armour_pool)
        donated_prices[id] = fixture.items[kFirstArmour + kLastArmour - id].price;

    std::array<Mean, 2> field_effective{}, vanilla_effective{}, shop_error{}, vanilla_shop_error{};
    for (unsigned seed = 1; seed <= kSeeds; ++seed) {
        const auto unchanged = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed,
                                               {false, true, true, true, true, false, false}, artifacts);
        const auto fair = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed,
                                          {true, true, true, true, true, true, true}, artifacts, donated_prices);
        const auto vanilla = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed,
                                             {true, true, true, true, true, true, true}, artifacts);
        require(fair.weapon_strength.find(artifacts[0]) != fair.weapon_strength.end() &&
                fair.weapon_strength.at(artifacts[0]) == artifacts[0],
                "Artifact weapon did not retain its own Scaling anchor", seed);

        std::set<std::uint16_t> seen(artifacts.begin(), artifacts.end());
        const auto reserve = [&](const tr::Content& content) {
            if (!weapon(fixture, content) && !armour(fixture, content)) return;
            require(seen.insert(static_cast<std::uint16_t>(content.value)).second,
                    "Artifact, normal stock, or field/gift gear repeated", seed);
        };
        for (std::size_t i = 0; i < fixture.spots.size(); ++i) {
            const auto& contents = fair.plan.contents[i];
            const auto& unchanged_contents = unchanged.plan.contents[i];
            const auto& vanilla_contents = vanilla.plan.contents[i];
            const auto& spot = fixture.spots[i];
            reserve(contents);
            if (weapon(fixture, unchanged_contents) || armour(fixture, unchanged_contents)) {
                for (const auto& row : stock)
                    require(std::find(row.begin(), row.end(), unchanged_contents.value) == row.end(),
                            "Unchanged stock was offered in a field spot or gift", seed);
            }
            if (spot.fixed || (spot.section > 2 && spot.section < 8)) continue;
            const std::size_t group = spot.section <= 2 ? 0 : 1;
            if (weapon(fixture, contents) || armour(fixture, contents))
                field_effective[group].add(donated_prices[contents.value]);
            if (weapon(fixture, vanilla_contents) || armour(fixture, vanilla_contents))
                vanilla_effective[group].add(donated_prices[vanilla_contents.value]);
        }
        for (std::size_t shop = 1; shop < stock.size(); ++shop) {
            for (const auto id : fair.shops[shop]) reserve({false, id});
            // Single-slot weapon shops isolate each replacement's target strength.
            if (shop > 2) continue;
            const auto old = stock[shop][0];
            const auto check_replacement = [&](const auto& plan, Mean& errors) {
                if (plan.shops[shop].empty()) return;
                const auto id = plan.shops[shop][0];
                if (id == old || !weapon(fixture, {false, id})) return;
                errors.add(std::abs(donated_prices[id] - fixture.items[old].price));
            };
            check_replacement(fair, shop_error[shop - 1]);
            check_replacement(vanilla, vanilla_shop_error[shop - 1]);
        }
    }
    require(field_effective[0].count && field_effective[1].count &&
            field_effective[0].value() < field_effective[1].value(),
            "Fair Items did not pace by reversed donated strength");
    require(vanilla_effective[0].count && vanilla_effective[1].count &&
            vanilla_effective[0].value() > vanilla_effective[1].value(),
            "Reversed synthetic donor ranking did not distinguish vanilla Fair Items");
    for (std::size_t group = 0; group < shop_error.size(); ++group)
        require(shop_error[group].count && vanilla_shop_error[group].count &&
                shop_error[group].value() < vanilla_shop_error[group].value(),
                "Fair Shops did not use donated price for its replacement stock");
    std::printf("Fair Items mean donated price: %.2f early and %.2f late, versus %.2f and %.2f with vanilla pacing.\n",
                field_effective[0].value(), field_effective[1].value(),
                vanilla_effective[0].value(), vanilla_effective[1].value());
    std::printf("Fair Shops mean donated price distance: %.2f early and %.2f late, versus %.2f and %.2f with vanilla pacing.\n",
                shop_error[0].value(), shop_error[1].value(), vanilla_shop_error[0].value(), vanilla_shop_error[1].value());
}

void check_regions(const Fixture& fixture) {
    const std::array consumables{kHerb, kNut};
    const tr::ShopPools pools{consumables, fixture.weapon_pool, fixture.armour_pool};
    const Stock stock{{}, {kHerb, 1}, {kNut, 24}};
    const tr::LootOptions fair{true, true, true, true, true, true, true};
    std::array<std::uint16_t, 14> mapped{1, 1, 1, 1, 1, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    std::array<std::uint16_t, 14> unavailable;
    unavailable.fill(tr::kNoSection);
    const std::array<tr::Spot, 4> coins{{
        {tr::SpotKind::Field, 1, true,   10, 1, true, false, "Mapped"},
        {tr::SpotKind::Field, 2, true,   20, 1, true, false, "Unmapped"},
        {tr::SpotKind::Field, 3, true,  500, 1, true, false, "Mapped"},
        {tr::SpotKind::Field, 4, true, 1000, 9, true, false, "Far"},
    }};
    const std::array coin_region{std::uint16_t{1}, tr::kNoSection, std::uint16_t{1}};
    const tr::LootOptions fair_coins{false, true, false, false, false, true, false};
    const std::array<tr::Spot, 3> scaling_spots{{
        {tr::SpotKind::Field, 1, false, kHerb, 1, true, true, "Early bound"},
        {tr::SpotKind::Field, 2, false,     2, 5, true, false, "Only weapon location"},
        {tr::SpotKind::Field, 3, false, kHerb, 9, true, true, "Late bound"},
    }};
    const std::array early_region{tr::kNoSection, std::uint16_t{1}};
    const std::array late_region{tr::kNoSection, std::uint16_t{9}};
    const tr::LootOptions weapons_only{false, false, true, false, false, false, false};
    unsigned first_contents_change = 0, first_coin_swap = 0;
    Mean early_anchor, late_anchor;
    for (unsigned seed = 1; seed <= kSeeds; ++seed) {
        const auto legacy = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed, fair);
        const auto fallback = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed, fair,
                                               {}, {}, unavailable);
        require(legacy.plan.contents == fallback.plan.contents && legacy.shops == fallback.shops &&
                legacy.weapon_strength == fallback.weapon_strength, "Missing region changed v6 output", seed);
        const auto regional = tr::pooled_loot(fixture.spots, fixture.items, pools, stock, seed, fair,
                                               {}, {}, mapped);
        check_protected(fixture, regional.plan, seed);
        if (!first_contents_change && legacy.plan.contents != regional.plan.contents)
            first_contents_change = seed;

        const auto stable = tr::pooled_loot(coins, fixture.items, pools, {}, seed, fair_coins);
        const auto tied = tr::pooled_loot(coins, fixture.items, pools, {}, seed, fair_coins, {}, {}, coin_region);
        const auto& a = stable.plan.contents;
        const auto& b = tied.plan.contents;
        require(a[1] == b[1] && a[3] == b[3], "A regional tie moved an unmapped coin spot's roll", seed);
        require((a[0] == b[0] && a[2] == b[2]) || (a[0] == b[2] && a[2] == b[0]),
                "A regional tie changed the underlying fair coin rolls", seed);
        if (!first_coin_swap && a[0] != b[0]) first_coin_swap = seed;

        const auto early = tr::pooled_loot(scaling_spots, fixture.items, pools, {}, seed, weapons_only,
                                            {}, {}, early_region);
        const auto late = tr::pooled_loot(scaling_spots, fixture.items, pools, {}, seed, weapons_only,
                                           {}, {}, late_region);
        require(early.plan.contents == late.plan.contents, "Region remapped an unfettered weapon deal", seed);
        const auto id = static_cast<std::uint16_t>(early.plan.contents[1].value);
        early_anchor.add(early.weapon_strength.at(id));
        late_anchor.add(late.weapon_strength.at(id));
    }
    require(first_contents_change, "Regional Fair Items did not alter a single seed");
    require(first_coin_swap, "Regional Fair Coins never shuffled same-region peers");
    require(early_anchor.value() < late_anchor.value(), "Weapon scaling ignored the regional anchor");
    std::printf("Regions first changed Fair Items at seed %u and swapped Fair Coins at seed %u. "
                "Mean weapon donor rank went from %.2f in the early region to %.2f in the late one.\n",
                first_contents_change, first_coin_swap, early_anchor.value(), late_anchor.value());
}

void check_native_artifacts(const Fixture& fixture) {
    auto table = fixture.items;
    const std::array<std::uint16_t, 2> consumables{kHerb, kNut};
    const std::array<std::uint16_t, 5> weapons{1, 2, 3, 4, 5};
    const std::array<std::uint16_t, 5> armour_ids{25, 26, 27, 28, 29};
    const std::array<std::uint16_t, 2> reserved{1, 26};
    for (const auto id : {1u, 3u, 4u, 26u, 27u, 28u}) table[id].flags |= tr::kNativeArtifactFlag;
    const tr::ShopPools pools{consumables, weapons, armour_ids};
    const Stock stock{{}, {1, 26}, {1, 26}, {1, 26, 1, 26}, {1, 26}, {2, 25}, {1, 26}};
    const std::array<tr::ArtifactShop, 6> shelves{{
        {}, {1, true, false}, {1, false, true}, {2, true, true}, {0, true, true}, {1, false, false},
    }};
    const tr::LootOptions all{true, false, true, true, false, false, false};
    unsigned first_weapon = 0, first_armour = 0, first_mixed = 0, first_full_fallback = 0;
    for (unsigned seed = 1; seed <= kSeeds; ++seed) {
        const auto loot = tr::pooled_loot({}, table, pools, stock, seed, all, reserved, {}, {}, shelves);
        std::set<std::uint16_t> seen(reserved.begin(), reserved.end());
        require(loot.weapon_strength.at(1) == 1, "Existing artifact lost its Scaling anchor", seed);
        for (std::size_t shop = 1; shop < stock.size(); ++shop) {
            const auto& normal = loot.shops[shop];
            const auto& additions = loot.artifact_additions[shop];
            const auto shelf = shop < shelves.size() ? shelves[shop] : tr::ArtifactShop{};
            require(additions.size() <= shelf.free_slots, "Artifact list exceeded free native slots", seed);
            require(normal.size() + additions.size() == stock[shop].size(),
                    "Moving an Artifact offer duplicated or lost stock", seed);
            for (const auto id : normal) {
                if (id == kHerb || id == kNut) continue;
                require(id == 2 || id == 25, "Unusual gear was sold on a normal shelf", seed);
                require(seen.insert(id).second, "Normal gear duplicated an existing or added Artifact", seed);
            }
            bool saw_weapon = false, saw_armour = false;
            for (const auto id : additions) {
                require(seen.insert(id).second, "Artifact repeated another offer", seed);
                require(table[id].flags & tr::kNativeArtifactFlag, "Gear without native flag became an Artifact", seed);
                if (weapon(fixture, {false, id})) {
                    require(shelf.weapons && loot.weapon_strength.contains(id),
                            "Artifact weapon entered wrong shop or missed a Scaling anchor", seed);
                    saw_weapon = true;
                } else {
                    require(shelf.armour && armour(fixture, {false, id}),
                            "Artifact armour entered the wrong shop", seed);
                    saw_armour = true;
                }
            }
            if (shop == 1 && saw_weapon && !first_weapon) first_weapon = seed;
            if (shop == 2 && saw_armour && !first_armour) first_armour = seed;
            if (shop == 3 && saw_weapon && saw_armour && !first_mixed) first_mixed = seed;
            if (shop == 4 && !first_full_fallback && std::all_of(normal.begin(), normal.end(), [](auto id) {
                    return id == kHerb || id == kNut;
                })) first_full_fallback = seed;
        }
    }
    require(first_weapon && first_armour && first_mixed && first_full_fallback,
            "Native Artifact shelves or supply fallback did not appear in the seed sweep");
    std::printf("Native Artifact offers first appeared at seeds %u (weapons), %u (armour), and %u (mixed). "
                "A full shelf switched to regular supplies at seed %u.\n",
                first_weapon, first_armour, first_mixed, first_full_fallback);
}

void check_scaling(const Fixture& fixture) {
    const eq::LocationStrengths anchors{{21, 2}, {4, 22}};
    for (unsigned seed = 1; seed <= kSeeds; ++seed) {
        const auto legacy = eq::make_plan(fixture.weapons, seed, true, true, false);
        const auto scaled = eq::make_plan(fixture.weapons, seed, true, true, false, anchors);
        const auto unscaled = eq::make_plan(fixture.weapons, seed, false, true, false, anchors);
        const auto unscaled_legacy = eq::make_plan(fixture.weapons, seed, false, true, false);
        for (std::size_t i = 0; i < scaled.weapons.size(); ++i) {
            const auto& weapon = scaled.weapons[i];
            require(unscaled.weapons[i].planned == unscaled_legacy.weapons[i].planned,
                    "Location data changed a weapon with Scaling off", seed);
            if (weapon.id == 21) {
                require(eq::attack_of(weapon.planned) == 20, "Early weapon did not take location attack", seed);
                require(eq::unleash_of(weapon.planned) == 101 && weapon.planned[eq::kChance] == 7,
                        "Early weapon did not use the nearest vanilla unleash and chance", seed);
            } else if (weapon.id == 4) {
                require(eq::attack_of(weapon.planned) == 220, "Late weapon did not take location attack", seed);
                require(eq::unleash_of(weapon.planned) == 122 && weapon.planned[eq::kChance] == 8,
                        "Late weapon did not take location unleash and chance", seed);
            } else {
                require(weapon.planned == legacy.weapons[i].planned,
                        "Location Scaling changed an unmapped weapon", seed);
            }
        }
    }
}

}  // namespace

int main() {
    const Fixture fixture;
    check_legacy_options(fixture);
    check_scaling(fixture);
    check_pooled_loot(fixture);
    check_artifacts_and_donor_pacing(fixture);
    check_regions(fixture);
    check_native_artifacts(fixture);
    std::printf("Legacy toggle, pooled loot and Scaling checks passed across %u seeds.\n", kSeeds);
}
