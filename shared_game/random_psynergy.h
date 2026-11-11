// Random Psynergy, one copy of the rules for both runners (The Broken Seal's
// docs/features/SEEDED_PSYNERGY.md; Jimmy's design of 2 and 4 November
// 2026). A new game can tick "Random Psynergy": a fresh random seed then
// decides every hero's battle Psynergy and the levels they learn it at, for
// the whole file. Djinn and class changes never change it; story Psynergy
// (Move, Mind Read, Retreat, ...) and item Psynergy are untouched.
//
// Both games rebuild a character's 32 Psynergy words (record +0x58, every 4
// bytes, id in the low half) in one routine (each game's measured address):
// class 0 returns at once; otherwise the words flagged 0x8000 (class) and
// 0x4000 (item) are dropped, the class list's entries the level reaches come
// back as id | 0x8000 and each equipped (0x200) item's ability (item record
// +0xC == 3, ability u16 +0x28) as id | 0x4000. The rebuild here keeps the
// unflagged (story) words and the items, and takes the plan's grants in place
// of the class list. Each game: its FACTS.md "Random Psynergy".
//
// Fair Play (6 November 2026, Jimmy: "early healing"): one hero there from
// the start (a pick, in a challenge) learns Cure or Ply at level 1, the level
// the normal game's classes first teach them (both games' pools: 90 Cure and
// 93 Ply at level 1). The rest of the plan is drawn as without it.
//
// The seed lives in eight save-payload bytes the game never writes
// (random_psynergy_format.h), so it saves, copies and erases with the file
// and save states carry it.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <vector>

#include "challenge_mode.h"
#include "random_psynergy_format.h"
#include "run_code.h"
#include "gba_bus.h"
#include "host_config_ui.h"
#include "runtime_bus_bridge.h"
#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif

namespace gsr::random_psynergy {

// An ability a class teaches, with the earliest level any class teaches it
// (the strength ranking: the normal game's own order).
struct Spell { std::uint16_t id; std::uint8_t level; };

// Where the game's classes keep their learn lists; the pool is read from the
// player's ROM (pool() below), never shipped. `count` class records of 0x54
// bytes from `address`: type word +0 (1..14 a character class, The Lost
// Age's 15..17 item classes), 16 entries at +0x10 of 4 bytes, the ability
// (one byte in The Broken Seal, a halfword in The Lost Age) then the level
// byte. Each game's FACTS.md "Random Psynergy".
struct ClassTable {
    std::uint32_t address;
    int count;
    bool wide_ability;
};
constexpr std::uint32_t kClassBytes = 0x54u, kLearnList = 0x10u;
constexpr int kLearnEntries = 16;

// A Psynergy that opens puzzles: whoever holds it knows it by the time they
// join. `holders`: bit per hero id who may hold it. In a challenge only picks
// hold them, from level 1.
struct Key { std::uint16_t id; std::uint8_t holders; };

constexpr int kMaxHeroes = 8;

// A game's measured places and its pool.
struct Game {
    std::uint32_t config;        // the eight payload bytes
    std::uint32_t records, record_size;
    std::uint32_t rebuild_pc;    // the game's list rebuild (character)
    std::uint32_t item_table;    // item records, 0x2C bytes each
    ClassTable classes;          // the pool's source (pool() below)
    int heroes;                  // character ids 0..heroes-1
    std::array<int, kMaxHeroes> join_level;  // the level each hero joins at
    const Key* keys;
    int key_count;
    std::uint8_t starters;       // bit per hero in the party from the start
};

// Fair Play's early heals: Cure, Ply (the same ids in both games, FACTS
// "Random Psynergy"), taught at the level of the normal game.
constexpr std::uint16_t kEarlyHeals[] = {90, 93};

constexpr std::uint32_t kLevel = 0xFu, kClass = 0x129u, kAbilities = 0x58u, kItems = 0xD8u;
constexpr unsigned kSlots = 32, kItemSlots = 15;
constexpr std::uint16_t kFromClass = 0x8000u, kFromItem = 0x4000u, kEquipped = 0x200u;
constexpr std::uint16_t kIdMask = 0x3FFFu, kItemMask = 0x1FFu;
constexpr std::uint32_t kItemSize = 0x2Cu, kItemKind = 0xCu, kItemAbility = 0x28u;
constexpr std::uint8_t kItemGrantsAbility = 3;

// Per seed: the last learning level and spells per hero. The Broken Seal's
// measured ranges (30..54, 8..16), the level up to 60 (Jimmy, 4 November).
// 16 also keeps the list's room for story and item Psynergy.
constexpr int kLastLevelMin = 30, kLastLevelMax = 60;
constexpr int kCountMin = 8, kCountMax = 16;
// One new spell in ten ignores the level curve (Jimmy, 4 November).
constexpr unsigned kLuckyOneIn = 10;

inline Config current(const Game& g) {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return {};
    std::uint8_t raw[8];
    for (int i = 0; i < 8; ++i) raw[i] = b->read8(g.config + i);
    return decode(raw);
}

inline void store(const Game& g, const Config& c) {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return;
    const auto raw = encode(c);
    for (int i = 0; i < 8; ++i) b->write8(g.config + i, raw[i]);
}

// A new seed for a new game: never 0, which means Off; 25 bits, as the run
// code carries (run_code.h).
inline std::uint32_t fresh_seed() {
    std::random_device device;
    const auto now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint32_t seed = 0;
    while (seed == 0)
        seed = (device() ^ static_cast<std::uint32_t>(now) ^ static_cast<std::uint32_t>(now >> 32)) &
               run_code::kSeedBits;
    return seed;
}

// --- the plan -----------------------------------------------------------------
// splitmix64, one independent stream per purpose and hero: the game's own
// random numbers are never used, so nothing in play can change the plan.
struct Rng {
    std::uint64_t state;
    Rng(std::uint32_t seed, unsigned purpose, unsigned hero)
        : state(seed * 0x9E3779B97F4A7C15ull ^ (std::uint64_t{purpose} << 40 | hero)) {}
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    unsigned below(unsigned n) { return static_cast<unsigned>((next() >> 32) * n >> 32); }
};

struct Grant { int level; std::uint16_t id; };

struct Plan {
    const Game* game = nullptr;
    std::uint32_t seed = 0;
    bool fair_play = false;
    gsr::challenge::Config challenge;
    int last_level = 0, count = 0;
    std::array<std::vector<Grant>, kMaxHeroes> grants;
};

inline bool same(const gsr::challenge::Config& a, const gsr::challenge::Config& b) {
    return a.size == b.size && a.picks == b.picks;
}

// One spell for a grant at `level` from `left` (removed from it). Stronger
// spells (taught later in the normal game) grow likelier as levels rise: the
// level maps onto the normal game's 1..top_level so the last learning level
// reaches the latest class spell; spells the normal game teaches by then are
// drawn, weighted by that level. None yet: the earliest left. One in ten: any
// spell at all.
inline std::uint16_t draw(Rng& rng, std::vector<Spell>& left, int level, int last_level,
                          int top_level) {
    std::size_t pick = 0;
    if (rng.below(kLuckyOneIn) == 0) {
        pick = rng.below(static_cast<unsigned>(left.size()));
    } else {
        const int target = std::max(1, level * top_level / last_level);
        unsigned total = 0;
        for (const Spell& s : left)
            if (s.level <= target) total += s.level;
        if (total > 0) {
            unsigned at = rng.below(total);
            for (pick = 0; pick < left.size(); ++pick) {
                if (left[pick].level > target) continue;
                if (at < left[pick].level) break;
                at -= left[pick].level;
            }
        } else {
            int earliest = 99;
            for (const Spell& s : left) earliest = std::min<int>(earliest, s.level);
            std::vector<std::size_t> first;
            for (std::size_t i = 0; i < left.size(); ++i)
                if (left[i].level == earliest) first.push_back(i);
            pick = first[rng.below(static_cast<unsigned>(first.size()))];
        }
    }
    const std::uint16_t id = left[pick].id;
    left.erase(left.begin() + static_cast<std::ptrdiff_t>(pick));
    return id;
}

// Every ability a character class teaches, by id, with the earliest level
// any class teaches it, and the latest such level: read once from the
// running ROM's class table. Empty without a ROM.
struct Pool {
    std::vector<Spell> spells;
    int top_level = 0;
};
inline const Pool& pool(const Game& g) {
    static const Game* built_for = nullptr;
    static Pool cached;
    if (built_for == &g && !cached.spells.empty()) return cached;
    cached = {};
    gba::GbaBus* b = gbarecomp::active_bus();
    const std::uint8_t* rom = b ? b->rom_ptr() : nullptr;
    const std::size_t at = g.classes.address - 0x08000000u;
    if (!rom || at + std::size_t{kClassBytes} * g.classes.count > b->rom_size()) return cached;
    std::map<std::uint16_t, std::uint8_t> earliest;
    for (int c = 0; c < g.classes.count; ++c) {
        const std::uint8_t* record = rom + at + std::size_t{kClassBytes} * c;
        const std::uint32_t type = record[0] | record[1] << 8 | record[2] << 16 |
                                   static_cast<std::uint32_t>(record[3]) << 24;
        if (type < 1 || type > 14) continue;
        for (int k = 0; k < kLearnEntries; ++k) {
            const std::uint8_t* entry = record + kLearnList + 4 * k;
            const std::uint16_t id = g.classes.wide_ability ? entry[0] | entry[1] << 8 : entry[0];
            const std::uint8_t level = g.classes.wide_ability ? entry[2] : entry[1];
            if (id == 0) continue;
            const auto [it, added] = earliest.try_emplace(id, level);
            if (!added) it->second = std::min(it->second, level);
        }
    }
    for (const auto& [id, level] : earliest) {
        cached.spells.push_back({id, level});
        cached.top_level = std::max<int>(cached.top_level, level);
    }
    built_for = &g;
    return cached;
}

inline int pool_level(const Game& g, std::uint16_t id) {
    for (const Spell& s : pool(g).spells)
        if (s.id == id) return s.level;
    return 0;
}

inline Plan make_plan(const Game& g, std::uint32_t seed, const gsr::challenge::Config& challenge,
                      bool fair_play) {
    Plan plan;
    plan.game = &g;
    plan.seed = seed;
    plan.fair_play = fair_play;
    plan.challenge = challenge;
    Rng run(seed, 0, 0);
    plan.last_level = kLastLevelMin + static_cast<int>(run.below(kLastLevelMax - kLastLevelMin + 1));
    plan.count = kCountMin + static_cast<int>(run.below(kCountMax - kCountMin + 1));

    // The puzzle Psynergy: a holder each, known by the time they join.
    Rng keys(seed, 1, 0);
    for (int k = 0; k < g.key_count; ++k) {
        std::vector<unsigned> can;
        if (challenge.size > 0) {
            for (int i = 0; i < challenge.size; ++i) can.push_back(challenge.picks[i]);
        } else {
            for (unsigned id = 0; id < static_cast<unsigned>(g.heroes); ++id)
                if (g.keys[k].holders & (1u << id)) can.push_back(id);
        }
        if (can.empty()) continue;
        const unsigned hero = can[keys.below(static_cast<unsigned>(can.size()))];
        if (hero >= static_cast<unsigned>(g.heroes)) continue;
        const int level = challenge.size > 0 ? 1
            : 1 + static_cast<int>(keys.below(static_cast<unsigned>(g.join_level[hero])));
        plan.grants[hero].push_back({level, g.keys[k].id});
    }

    // Fair Play: a starter (a pick) learns one early heal at its usual level.
    if (fair_play) {
        Rng heal(seed, 3, 0);
        std::vector<unsigned> can;
        if (challenge.size > 0) {
            for (int i = 0; i < challenge.size; ++i) can.push_back(challenge.picks[i]);
        } else {
            for (unsigned id = 0; id < static_cast<unsigned>(g.heroes); ++id)
                if (g.starters & (1u << id)) can.push_back(id);
        }
        const std::uint16_t id = kEarlyHeals[heal.below(static_cast<unsigned>(std::size(kEarlyHeals)))];
        const int level = pool_level(g, id);
        if (!can.empty() && level > 0) {
            const unsigned hero = can[heal.below(static_cast<unsigned>(can.size()))];
            if (hero < static_cast<unsigned>(g.heroes)) plan.grants[hero].push_back({level, id});
        }
    }

    for (unsigned hero = 0; hero < static_cast<unsigned>(g.heroes); ++hero) {
        Rng rng(seed, 2, hero);
        std::vector<int> levels;
        for (int i = 0; i < plan.count; ++i)
            levels.push_back(1 + static_cast<int>(rng.below(static_cast<unsigned>(plan.last_level))));
        std::sort(levels.begin(), levels.end());
        std::vector<Spell> left = pool(g).spells;
        for (const Grant& grant : plan.grants[hero])
            left.erase(std::remove_if(left.begin(), left.end(),
                                      [&](const Spell& s) { return s.id == grant.id; }),
                       left.end());
        for (const int level : levels)
            plan.grants[hero].push_back({level, draw(rng, left, level, plan.last_level, pool(g).top_level)});
        std::stable_sort(plan.grants[hero].begin(), plan.grants[hero].end(),
                         [](const Grant& a, const Grant& b) { return a.level < b.level; });
    }
    return plan;
}

inline const Plan& plan_for(const Game& g, const Config& c, const gsr::challenge::Config& challenge) {
    static Plan cached;
    if (cached.game != &g || cached.seed != c.seed || cached.fair_play != c.fair_psynergy ||
        !same(cached.challenge, challenge))
        cached = make_plan(g, c.seed, challenge, c.fair_psynergy);
    return cached;
}

// --- the rebuild ------------------------------------------------------------------
// The game's rebuild for a hero in a Random Psynergy game, with the class's
// battle list replaced by the plan's grants up to the hero's level. Story
// grants stay; item Psynergy keeps its room ahead of the plan. False: the
// game's own rebuild runs.
inline bool rebuild(const Game& g, gba::GbaBus& b, unsigned id) {
    const Config c = current(g);
    if (!c.psynergy || id >= static_cast<unsigned>(g.heroes)) return false;
    const std::uint32_t record = g.records + id * g.record_size;
    if (b.read8(record + kClass) == 0) return false;  // the game returns at once
    const Plan& plan = plan_for(g, c, gsr::challenge::current());
    const auto has = [](const std::vector<std::uint16_t>& list, std::uint16_t ability) {
        for (const std::uint16_t w : list)
            if ((w & kIdMask) == ability) return true;
        return false;
    };

    std::vector<std::uint16_t> out;
    for (unsigned i = 0; i < kSlots; ++i) {
        const std::uint16_t w = b.read16(record + kAbilities + i * 4u);
        if (w != 0 && !(w & (kFromClass | kFromItem))) out.push_back(w);
    }
    std::vector<std::uint16_t> items;
    for (unsigned slot = 0; slot < kItemSlots; ++slot) {
        const std::uint16_t w = b.read16(record + kItems + slot * 2u);
        if (w == 0 || !(w & kEquipped)) continue;
        const std::uint32_t item = g.item_table + (w & kItemMask) * kItemSize;
        if (b.read8(item + kItemKind) != kItemGrantsAbility) continue;
        const std::uint16_t ability = b.read16(item + kItemAbility);
        if (!has(out, ability) && !has(items, ability)) items.push_back(ability);
    }
    const int level = b.read8(record + kLevel);
    std::size_t room = kSlots > out.size() + items.size() ? kSlots - out.size() - items.size() : 0;
    for (const Grant& grant : plan.grants[id]) {
        if (grant.level > level || has(out, grant.id)) continue;
        const auto item = std::find(items.begin(), items.end(), grant.id);
        if (item != items.end()) {
            items.erase(item);  // already has room
        } else if (room == 0) {
            continue;
        } else {
            --room;
        }
        out.push_back(static_cast<std::uint16_t>(grant.id | kFromClass));
    }
    for (const std::uint16_t ability : items)
        if (out.size() < kSlots) out.push_back(static_cast<std::uint16_t>(ability | kFromItem));
    for (unsigned i = 0; i < kSlots; ++i)
        b.write16(record + kAbilities + i * 4u, i < out.size() ? out[i] : 0);
    return true;
}

// Entry to the game's rebuild: true when ours ran instead (the caller returns
// 0, as the game's does).
inline bool on_entry(const Game& g, std::uint32_t pc, std::uint32_t id) {
    if (pc != g.rebuild_pc) return false;
    gba::GbaBus* b = gbarecomp::active_bus();
    return b && rebuild(g, *b, id);
}

// After the new-game questions: rebuilds every hero's list from the plan (the
// game built them before the questions, from their classes).
inline void apply_new_game(const Game& g) {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return;
    for (unsigned id = 0; id < static_cast<unsigned>(g.heroes); ++id) rebuild(g, *b, id);
}

}  // namespace gsr::random_psynergy
