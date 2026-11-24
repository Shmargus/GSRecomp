// Random Psynergy, one copy of the rules for both runners. A new game
// can tick "Random Psynergy": a fresh random seed then decides the battle
// Psynergy and the levels it is learned at, for the whole file, in one of two
// ways. By hero: each hero has one list; Djinn and class changes never change
// it. By class: every class gets its own list (class_lists() below), so the
// class a hero's Djinn make decides what they know, as in the normal game.
// Story Psynergy (Move, Mind Read, Retreat, ...) and item Psynergy are
// untouched.
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
// Fair Play ("early healing"): one hero who is in the party from the start
// (a pick, in a challenge) learns Cure or Ply at level 1, the level at which
// the normal game's classes first teach them (both games' pools: 90 Cure and
// 93 Ply at level 1). The rest of the plan is drawn as without it.
//
// Enemy skills (a choice with Random Psynergy): the skills enemies use that
// no class teaches join the pool, at the level of the first enemy that uses
// them (enemies at level 0 are unused rows). Summons, the enemies' own
// actions (Flee, calling for help, ...) and blank rows stay out. A skill
// whose ROM cost is 0 PP costs a hero what a class spell of the same kind,
// reach and strength costs (hero_cost() below); enemies keep paying nothing.
// Such a skill has no icon in the ROM either; it shows the icon of a class
// spell picked for its name (kIconLike below). pp_on_getter() puts both in
// place.
//
// The seed lives in eight save-payload bytes the game never writes
// (random_psynergy_format.h), so it saves, copies and erases with the file
// and save states carry it.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "challenge_mode.h"
#include "menu_text.h"
#include "random_psynergy_format.h"
#include "run_code.h"
#include "trace_once.h"
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

// Where the game keeps its enemies (each game's FACTS.md "Enemy skills"):
// `count` records of `stride` bytes from `address`, level byte +0xF, 8 ability
// halfwords at `abilities`.
struct EnemyTable {
    std::uint32_t address;
    int count;
    std::uint32_t stride, abilities;
};
// The ability rows: `size` bytes each, ids below `count` (the getter's
// clamp). +0 target, +1 kind, +3 effect, +4 icon (halfword), +8 range, +9 PP
// cost, +0xA power (halfword).
struct AbilityTable {
    std::uint32_t address, size;
    std::uint16_t count;
};
// A place that reads an ability's PP cost after the getter returns (lr at
// the getter's entry): the hero's menus and command checks (kHero), the
// enemies' choice (kEnemy), or the battle's spend and cast check, which serve
// both and name the actor through a register. Any other caller leaves the
// cost byte as it is: the battle's Psynergy list prints a name between its
// getter call and its cost read, and other lookups there must not undo the
// hero cost (Jimmy's build, 22 November 2026: 0 PP shown).
struct PpSite {
    enum Actor : std::uint8_t { kHero, kByteAtR5, kHalfAtR10Action, kEnemy };
    std::uint32_t ret;
    Actor actor;
};
// An enemy skill without an icon shows the icon of class spell `like`.
struct IconLike { std::uint16_t id, like; };
// An enemy skill's description line (English).
struct Line { std::uint16_t id; const char* text; };

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
    // Enemy skills.
    EnemyTable enemies;
    AbilityTable abilities;
    std::uint32_t getter_pc;     // the ability row getter (r0 = id; returns the row)
    std::span<const PpSite> pp_sites;
    std::span<const IconLike> icons;  // this game's own skills (kIconLike holds the shared ones)
    std::uint32_t description_text;   // ability description = this + id (0: none)
    std::span<const Line> lines;      // this game's own skills' lines (kLines holds the shared ones)
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

// Per seed, each hero gets a last learning level and a number of spells.
// The Broken Seal's measured ranges are 30..54 for the level and 8..16 for
// the spells; the level may go up to 60. Capping spells at 16 also keeps room
// in the list for story and item Psynergy.
constexpr int kLastLevelMin = 30, kLastLevelMax = 60;
constexpr int kCountMin = 8, kCountMax = 16;
// One new spell in ten ignores the level curve.
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
inline bool by_level(const Grant& a, const Grant& b) { return a.level < b.level; }

struct Plan {
    const Game* game = nullptr;
    std::uint32_t seed = 0;
    bool fair_play = false, enemy_skills = false, by_class = false;
    gsr::challenge::Config challenge;
    int last_level = 0, count = 0;
    // By hero: each hero's whole list. By class: only the puzzle Psynergy and
    // Fair Play's heal, which stay with the hero.
    std::array<std::vector<Grant>, kMaxHeroes> grants;
    // By class: each class's list, by class index (empty for no class).
    std::vector<std::vector<Grant>> classes;
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

// The class records in the running ROM image; null without a ROM or when the
// table runs past its end.
inline const std::uint8_t* class_records(const Game& g) {
    gba::GbaBus* b = gbarecomp::active_bus();
    const std::uint8_t* rom = b ? b->rom_ptr() : nullptr;
    const std::size_t at = g.classes.address - 0x08000000u;
    if (!rom || at + std::size_t{kClassBytes} * g.classes.count > b->rom_size()) return nullptr;
    return rom + at;
}
inline const std::uint8_t* class_record(const std::uint8_t* records, int c) {
    return records + std::size_t{kClassBytes} * c;
}
inline std::uint32_t class_type(const std::uint8_t* record) {
    return record[0] | record[1] << 8 | record[2] << 16 | static_cast<std::uint32_t>(record[3]) << 24;
}
// A class record's learn list in ROM order, blank entries left out.
inline std::vector<Grant> learn_list(const Game& g, const std::uint8_t* record) {
    std::vector<Grant> list;
    for (int k = 0; k < kLearnEntries; ++k) {
        const std::uint8_t* e = record + kLearnList + 4 * k;
        const std::uint16_t id = g.classes.wide_ability ? e[0] | e[1] << 8 : e[0];
        if (id != 0) list.push_back({g.classes.wide_ability ? e[2] : e[1], id});
    }
    return list;
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
    const std::uint8_t* records = class_records(g);
    if (!records) return cached;
    std::map<std::uint16_t, int> earliest;
    for (int c = 0; c < g.classes.count; ++c) {
        const std::uint8_t* record = class_record(records, c);
        const std::uint32_t type = class_type(record);
        if (type < 1 || type > 14) continue;
        for (const Grant& e : learn_list(g, record)) {
            const auto [it, added] = earliest.try_emplace(e.id, e.level);
            if (!added) it->second = std::min(it->second, e.level);
        }
    }
    for (const auto& [id, level] : earliest) {
        cached.spells.push_back({id, static_cast<std::uint8_t>(level)});
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

// --- enemy skills -----------------------------------------------------------------
// An enemy skill: its id, level (the earliest enemy using it, within the
// class pool's levels), what a hero pays and the ROM's own cost.
struct EnemySkill {
    std::uint16_t id;
    std::uint8_t level, cost, rom_cost;
    std::uint16_t icon, rom_icon;
};

// Icons for the enemy-only rows both games share (ids 0xD4, 0x1A4..0x206; the
// names are The Broken Seal's, The Lost Age calls the Blessings Breaths): the
// class spell whose picture fits the name. 0x63 Cure Poison stands for
// poison, 0x5A Cure for self-healing, 0xC0 Ragnarok for plain blows, 0xC1
// Cutting Edge for claws and fangs.
constexpr IconLike kIconLike[] = {
    {0x0D4, 0x063},                                  // Deadly Poison
    {0x1A4, 0x030}, {0x1A5, 0x030}, {0x1A6, 0x031},  // Fire Blessing/Breath: Fire, Fireball
    {0x1A7, 0x021}, {0x1A8, 0x022}, {0x1A9, 0x023},  // Water: Douse, Drench, Deluge
    {0x1AA, 0x018}, {0x1AB, 0x019}, {0x1AC, 0x01A},  // Ice: Frost, Tundra, Glacier
    {0x1AD, 0x07A}, {0x1AE, 0x07A},                  // Dark: Haunt
    {0x1AF, 0x037}, {0x1B0, 0x038},                  // Dark/Acid: Mad Blast, Fiery Blast
    {0x1B1, 0x04F}, {0x1B2, 0x050},                  // Storm: Tornado, Tempest
    {0x1B3, 0x04E}, {0x1B4, 0x04E}, {0x1B5, 0x04E},  // Sonic Wave, Shriek: Whirlwind
    {0x1B6, 0x07A},                                  // Banshee Howl: Haunt
    {0x1B8, 0x066}, {0x1B9, 0x07B},                  // War Cry: Impact; Wicked Howl: Curse
    {0x1BA, 0x04E}, {0x1BB, 0x04F}, {0x1BC, 0x050},  // Wing Beat/Flutter/Stroke
    {0x1BD, 0x07A}, {0x1BE, 0x063},                  // Evil Blessing: Haunt; Deadly Gas
    {0x1C2, 0x003}, {0x1C3, 0x01B}, {0x1C4, 0x04B},  // Rumble: Quake; Bone Chiller: Ice; Slice: Slash
    {0x1C5, 0x0C0}, {0x1C6, 0x02D}, {0x1C7, 0x079},  // Bone Charge; Mystic Flame: Flare; Numbing Sting: Bind
    {0x1C8, 0x0C0}, {0x1C9, 0x024}, {0x1CA, 0x07D},  // Brute Force; Sticky Goo: Froth; Cannibal Fang: Drain
    {0x1CB, 0x0C1}, {0x1CC, 0x063}, {0x1CD, 0x0C3},  // Bear Claw; Poisonous Bite; Flying Attack: Astral Blast
    {0x1CE, 0x0C0}, {0x1CF, 0x0C0}, {0x1D0, 0x024},  // Undead Sword; Ransack; Sticky Poison: Froth
    {0x1D1, 0x063}, {0x1D2, 0x042}, {0x1D3, 0x063},  // Poison Fang; Electric Bite: Bolt; Poison Tail
    {0x1D4, 0x0C0}, {0x1D5, 0x07D}, {0x1D6, 0x063},  // Onslaught; Vampiric Fang: Drain; Bacteria Rush
    {0x1D7, 0x0C1}, {0x1D8, 0x06C}, {0x1D9, 0x0C0},  // Swift Strike; Rotten Blood: Impair; Forcible Arm
    {0x1DA, 0x0C1}, {0x1DB, 0x07C}, {0x1DC, 0x0C1},  // Double Fang; Mortal Blow: Condemn; Freebite Rush
    {0x1DD, 0x0C3}, {0x1DE, 0x063}, {0x1DF, 0x036},  // Twin Beaks; Rabid Fang; Acid Bite: Blast
    {0x1E0, 0x038}, {0x1E1, 0x0C0}, {0x1E2, 0x063},  // Dynamite: Fiery Blast; Headbutt; Poison Ink
    {0x1E3, 0x0C0}, {0x1E4, 0x0C0}, {0x1E5, 0x0C0},  // Truncheon Fist; Counterstrike; Mad Dash
    {0x1E6, 0x060}, {0x1E7, 0x079}, {0x1E8, 0x068},  // Soothing Star: Wish; Spider Web: Bind; Heartrender: Dull
    {0x1E9, 0x074}, {0x1EA, 0x05A}, {0x1EB, 0x078},  // Mad Spatter: Delude; Spasm: Cure; Sleep Star: Sleep
    {0x1EC, 0x069}, {0x1ED, 0x07A}, {0x1EE, 0x05A},  // Decompose: Blunt; Haunting: Haunt; Worms: Cure
    {0x1EF, 0x066}, {0x1F0, 0x07B}, {0x1F1, 0x05B},  // Berserk: Impact; Lucid Prophecy: Curse; Recovery: Cure Well
    {0x1FE, 0x063}, {0x1FF, 0x0C1},                  // Poison Beat; Spinning Beat
    {0x200, 0x0C2}, {0x201, 0x07C},                  // Heat Flash: Heat Wave; Death Size/Scythe: Condemn
    {0x204, 0x07D}, {0x205, 0x0C0}, {0x206, 0x0C1},  // Drain Fang: Drain; Severe Blow; Thrash
};

// A hero's cost for an enemy skill the ROM gives 0 PP: what a class spell of
// the same kind, reach and strength costs. The lines are fitted to both
// games' class spells (FACTS "Enemy skills", 21 November 2026): damage
// Psynergy and breaths 2 + 0.09 power on one target, 3 + 0.12 power on
// three, 2.5 + 0.15 power on five, 3 + 0.19 power wider (Flare 15 -> 4,
// Glacier 100 -> 15, Inferno 140 -> 23); attack-plus blows 0.19 power
// (Ragnarok 35 -> 7, Odyssey 95 -> 18); attack-times blows 0.65 per power
// point on one target, half again on three (Quick Strike 18 -> 12, Shuriken
// 8 -> 8); a status effect on a damaging skill +2 (and at least 4); heals
// power / 20 on one, / 10 on more (Cure 70 -> 3, Wish 80 -> 9); effects cost
// what the class spell with the same effect costs (+2 when it reaches more),
// +1 per 20 power above it. `row(id)`: an ability row in the ROM.
template <class Row>
inline std::uint8_t hero_cost(const std::uint8_t* r, const std::vector<Spell>& classes, Row row) {
    const int kind = r[1], range = r[8], power = r[0xA] | r[0xB] << 8, status = r[3] != 0 ? 1 : 0;
    int c = 6;
    switch (kind) {
    case 0x85: case 0x86: {
        const int x100 = range == 1 ? 200 + 9 * power : range == 2 ? 300 + 12 * power
                       : range == 3 ? 250 + 15 * power : 300 + 19 * power;
        c = (x100 + 50) / 100 + 2 * status;
        break;
    }
    case 0x83: c = (19 * power + 50) / 100 + 2 * status; break;
    case 0x84: {
        const int halves = range == 1 ? 2 : range == 2 ? 3 : 4;
        c = (65 * power * halves + 100) / 200 + 2 * status;
        break;
    }
    case 0x81: c = range == 1 ? (power + 10) / 20 : (power + 5) / 10; break;
    case 0x82: {
        const std::uint8_t* like = nullptr;
        for (const Spell& s : classes) {
            const std::uint8_t* k = row(s.id);
            if (k[1] != 0x82 || k[3] != r[3] || k[9] == 0) continue;
            if (!like || (k[8] == range && like[8] != range)) like = k;
        }
        if (like)
            c = like[9] + (range >= 2 && like[8] < 2 ? 2 : 0) +
                std::max(0, power - (like[0xA] | like[0xB] << 8)) / 20;
        else
            c = 5 + (range >= 2 ? 2 : 0) + power / 20;
        break;
    }
    default: break;
    }
    const int floor = kind == 0x82 || kind == 0x81 ? 2 : 2 + 2 * status;
    return static_cast<std::uint8_t>(std::clamp(c, floor, 99));
}

inline std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

// Kinds (+1) a hero can use: 0x81..0x87 and 0x8B (attacks, heals, effects).
// 0x88 are summons, 0x89 the enemies' own actions, 0xC0 and up item uses.
inline bool hero_kind(std::uint8_t kind) { return (kind >= 0x81u && kind <= 0x87u) || kind == 0x8Bu; }

// Read once from the running ROM. Empty without a ROM.
inline const std::vector<EnemySkill>& enemy_skills(const Game& g) {
    static const Game* built_for = nullptr;
    static std::vector<EnemySkill> cached;
    if (built_for == &g) return cached;
    cached.clear();
    const Pool& classes = pool(g);
    gba::GbaBus* b = gbarecomp::active_bus();
    const std::uint8_t* rom = b ? b->rom_ptr() : nullptr;
    const AbilityTable& at = g.abilities;
    const EnemyTable& et = g.enemies;
    if (!rom || classes.spells.empty() || at.count == 0 || et.count == 0) return cached;
    const std::size_t enemies = et.address - 0x08000000u, rows = at.address - 0x08000000u;
    if (enemies + std::size_t{et.stride} * et.count > b->rom_size() ||
        rows + std::size_t{at.size} * at.count > b->rom_size())
        return cached;
    const auto row = [&](std::uint16_t id) { return rom + rows + std::size_t{at.size} * id; };
    const auto taught = [&](std::uint16_t id) {
        for (const Spell& s : classes.spells)
            if (s.id == id) return true;
        return false;
    };
    std::map<std::uint16_t, int> earliest;
    for (int e = 0; e < et.count; ++e) {
        const std::uint8_t* record = rom + enemies + std::size_t{et.stride} * e;
        if (record[0xF] == 0) continue;  // level 0: unused rows (FACTS "Enemy skills")
        for (int k = 0; k < 8; ++k) {
            const std::uint16_t id = le16(record + et.abilities + 2 * k);
            if (id <= 2 || id >= at.count || taught(id)) continue;  // none, Attack, Defend
            const std::uint8_t* r = row(id);
            const bool blank = r[3] == 0 && le16(r + 4) == 0 && le16(r + 0xA) == 0;
            if (r[0] == 0 || !hero_kind(r[1]) || blank) continue;
            const int level = std::clamp<int>(record[0xF], 1, classes.top_level);
            const auto [it, added] = earliest.try_emplace(id, level);
            if (!added) it->second = std::min(it->second, level);
        }
    }
    const auto icon_like = [&](std::uint16_t id) -> std::uint16_t {
        for (const IconLike& l : g.icons)
            if (l.id == id) return l.like;
        for (const IconLike& l : kIconLike)
            if (l.id == id) return l.like;
        return 0;
    };
    for (const auto& [id, level] : earliest) {
        const std::uint8_t* r = row(id);
        const std::uint8_t own = r[9];
        const std::uint16_t own_icon = le16(r + 4), like = icon_like(id);
        const std::uint16_t icon = own_icon || like == 0 || like >= at.count ? own_icon : le16(row(like) + 4);
        cached.push_back({id, static_cast<std::uint8_t>(level),
                          own ? own : hero_cost(r, classes.spells, row), own, icon, own_icon});
    }
    built_for = &g;
    return cached;
}

// The draw's pool: the class spells, and the enemy skills when chosen.
inline std::vector<Spell> draw_pool(const Game& g, bool with_enemy_skills) {
    std::vector<Spell> out = pool(g).spells;
    if (with_enemy_skills)
        for (const EnemySkill& e : enemy_skills(g)) out.push_back({e.id, e.level});
    return out;
}

// By class. A class line is a run of class records of one type (Squire,
// Knight, Gallant, Lord, Slayer); each tier teaches what the tier below does
// and more. Here each tier keeps the random list of the tier below and every
// entry its own list adds (an ability the tier below lacks) becomes a spell
// drawn at that entry's level, weighted by strength as the by-hero draw is
// (the normal game's level for each spell). No spell twice in a line.
inline std::vector<std::vector<Grant>> class_lists(const Game& g, std::uint32_t seed,
                                                   bool with_enemy_skills) {
    std::vector<std::vector<Grant>> out(static_cast<std::size_t>(g.classes.count));
    const std::uint8_t* records = class_records(g);
    const int top = pool(g).top_level;
    if (!records || top == 0) return out;
    const auto type_of = [&](int c) { return class_type(class_record(records, c)); };
    unsigned line = 0;
    for (int c = 0; c < g.classes.count;) {
        const std::uint32_t type = type_of(c);
        if (type == 0) { ++c; continue; }
        Rng rng(seed, 4, line++);
        std::vector<Spell> left = draw_pool(g, with_enemy_skills);
        std::vector<Grant> below_original, list;
        for (; c < g.classes.count && type_of(c) == type; ++c) {
            const std::vector<Grant> original = learn_list(g, class_record(records, c));
            for (const Grant& e : original) {
                const bool kept = std::any_of(below_original.begin(), below_original.end(),
                                              [&](const Grant& o) { return o.id == e.id; });
                if (kept || left.empty()) continue;
                list.push_back({e.level, draw(rng, left, std::clamp(e.level, 1, top), top, top)});
            }
            std::stable_sort(list.begin(), list.end(), by_level);
            out[static_cast<std::size_t>(c)] = list;
            below_original = original;
        }
    }
    return out;
}

inline Plan make_plan(const Game& g, std::uint32_t seed, const gsr::challenge::Config& challenge,
                      bool fair_play, bool with_enemy_skills = false, bool by_class = false) {
    Plan plan;
    plan.game = &g;
    plan.seed = seed;
    plan.fair_play = fair_play;
    plan.enemy_skills = with_enemy_skills;
    plan.by_class = by_class;
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

    if (by_class) {
        plan.classes = class_lists(g, seed, with_enemy_skills);
        for (auto& list : plan.grants)
            std::stable_sort(list.begin(), list.end(), by_level);
        return plan;
    }
    for (unsigned hero = 0; hero < static_cast<unsigned>(g.heroes); ++hero) {
        Rng rng(seed, 2, hero);
        std::vector<int> levels;
        for (int i = 0; i < plan.count; ++i)
            levels.push_back(1 + static_cast<int>(rng.below(static_cast<unsigned>(plan.last_level))));
        std::sort(levels.begin(), levels.end());
        std::vector<Spell> left = draw_pool(g, with_enemy_skills);
        for (const Grant& grant : plan.grants[hero])
            left.erase(std::remove_if(left.begin(), left.end(),
                                      [&](const Spell& s) { return s.id == grant.id; }),
                       left.end());
        for (const int level : levels)
            plan.grants[hero].push_back({level, draw(rng, left, level, plan.last_level, pool(g).top_level)});
        std::stable_sort(plan.grants[hero].begin(), plan.grants[hero].end(), by_level);
    }
    return plan;
}

inline const Plan& plan_for(const Game& g, const Config& c, const gsr::challenge::Config& challenge) {
    static Plan cached;
    if (cached.game != &g || cached.seed != c.seed || cached.fair_play != c.fair_psynergy ||
        cached.enemy_skills != c.enemy_skills || cached.by_class != c.by_class ||
        !same(cached.challenge, challenge))
        cached = make_plan(g, c.seed, challenge, c.fair_psynergy, c.enemy_skills, c.by_class);
    return cached;
}

// --- the rebuild ------------------------------------------------------------------
// The game's rebuild for a hero in a Random Psynergy game, with the class's
// battle list replaced by the plan's grants up to the hero's level (by
// class: the class's random list and the hero's own puzzle Psynergy). Story
// grants stay; item Psynergy keeps its room ahead of the plan. False: the
// game's own rebuild runs.
//
// Challenge Mode without Random Psynergy: the party may lack whoever teaches
// a puzzle Psynergy (The Broken Seal: no Ivan, no Whirlwind; no Mia, no Frost
// or Ply). So the leader (first pick, always in the party) knows every key
// from level 1 besides its class's own list; the other picks keep the game's
// rebuild.
inline std::vector<Grant> class_and_keys(const Game& g, unsigned cls) {
    std::vector<Grant> out;
    const std::uint8_t* records = class_records(g);
    if (records && cls < static_cast<unsigned>(g.classes.count))
        out = learn_list(g, class_record(records, static_cast<int>(cls)));
    for (int k = 0; k < g.key_count; ++k) out.push_back({1, g.keys[k].id});
    return out;
}

inline bool rebuild(const Game& g, gba::GbaBus& b, unsigned id) {
    const Config c = current(g);
    if (id >= static_cast<unsigned>(g.heroes)) return false;
    const gsr::challenge::Config challenge = gsr::challenge::current();
    const bool leader_keys = !c.psynergy && challenge.size > 0 && challenge.picks[0] == id;
    if (!c.psynergy && !leader_keys) return false;
    const std::uint32_t record = g.records + id * g.record_size;
    const unsigned cls = b.read8(record + kClass);
    if (cls == 0) return false;  // the game returns at once
    std::vector<Grant> grants;
    if (leader_keys) {
        grants = class_and_keys(g, cls);
    } else {
        const Plan& plan = plan_for(g, c, challenge);
        grants = plan.grants[id];
        if (plan.by_class && cls < plan.classes.size()) {
            grants.insert(grants.end(), plan.classes[cls].begin(), plan.classes[cls].end());
            std::stable_sort(grants.begin(), grants.end(), by_level);
        }
    }
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
    for (const Grant& grant : grants) {
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

// --- hero-only PP --------------------------------------------------------------
// The game reads a cost from the row the getter returns, for heroes and
// enemies alike, and its enemies' choice treats a skill with a cost as
// Psynergy (each game's FACTS.md "Enemy skills"). So only the hero's readers
// see the hero cost: at each getter entry for an enemy skill, its row's cost
// byte in the ROM image (written as Random Equipment writes its rows,
// random_weapons.h) is the hero cost when the caller is a hero reader, else
// the ROM's own. Each reader takes it at once, before the next getter call.
// Emulation thread.
struct PpState {
    std::vector<std::pair<std::uint32_t, std::uint8_t>> written;  // ROM offset holding our byte, its ROM byte
    void put(std::uint8_t* rom, std::uint32_t at, std::uint8_t value, std::uint8_t rom_value) {
        rom[at] = value;
        if (value != rom_value &&
            std::none_of(written.begin(), written.end(), [&](const auto& w) { return w.first == at; }))
            written.emplace_back(at, rom_value);
    }
};
inline PpState pp_state;

inline const EnemySkill* find_skill(const Game& g, std::uint16_t id) {
    for (const EnemySkill& e : enemy_skills(g))
        if (e.id == id) return &e;
    return nullptr;
}

// At the getter's entry: r0 (id, with the list's flags, which the getter
// masks off), r5, r10 and lr. The icon is the same for every caller (only the
// menus read it: FACTS "Enemy skills"); the cost is the hero's for the hero's
// readers and the ROM's for the rest.
inline void pp_on_getter(const Game& g, std::uint32_t pc, std::uint32_t r0, std::uint32_t r5,
                         std::uint32_t r10, std::uint32_t lr) {
    if (!g.getter_pc || pc != g.getter_pc) return;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !b->rom_ptr()) return;
    auto* rom = const_cast<std::uint8_t*>(b->rom_ptr());
    const Config c = current(g);
    if (!c.psynergy || !c.enemy_skills) {
        // Another file: the ROM's own costs again.
        for (const auto& [at, value] : pp_state.written) rom[at] = value;
        pp_state.written.clear();
        return;
    }
    const auto id = static_cast<std::uint16_t>(r0 & kIdMask);
    if (id >= g.abilities.count) return;
    const EnemySkill* skill = find_skill(g, id);
    if (!skill) return;
    const std::uint32_t base = g.abilities.address - 0x08000000u + g.abilities.size * id;
    if (base + 0xAu > b->rom_size()) return;
    if (skill->icon != skill->rom_icon) {
        pp_state.put(rom, base + 4u, static_cast<std::uint8_t>(skill->icon), static_cast<std::uint8_t>(skill->rom_icon));
        pp_state.put(rom, base + 5u, static_cast<std::uint8_t>(skill->icon >> 8),
                     static_cast<std::uint8_t>(skill->rom_icon >> 8));
    }
    if (skill->rom_cost == skill->cost) return;
    bool hero = false, known = false;
    for (const PpSite& site : g.pp_sites) {
        if (site.ret != (lr & ~1u)) continue;
        known = true;
        switch (site.actor) {
        case PpSite::kHero: hero = true; break;
        case PpSite::kEnemy: break;
        case PpSite::kByteAtR5: hero = b->read8(r5) < kMaxHeroes; break;
        case PpSite::kHalfAtR10Action:
            hero = static_cast<std::int16_t>(b->read16(b->read32(r10))) < kMaxHeroes;
            break;
        }
        break;
    }
    trace_once(0x1ull << 56 | std::uint64_t{lr & ~1u} << 16 | id,
               "[enemy-skills] cost id=0x%X lr=0x%08X known=%d hero=%d hero_cost=%d byte_before=%d", id, lr & ~1u,
               known, hero, skill->cost, rom[base + 9u]);
    if (!known) return;
    pp_state.put(rom, base + 9u, hero ? skill->cost : skill->rom_cost, skill->rom_cost);
}

// --- descriptions -------------------------------------------------------------
// An enemy-only skill's description reads "?" in the ROM. In a run with
// Enemy skills it gets one: its own written line (kLines, a game's lines),
// in the loaded language through the F1 translation table (menu_text: the
// game font's Latin-1), else one written in English from its row: kind
// (+1), added effect (+3), element (+2) and reach (+8). The effect words
// follow the game's own lines for abilities with the same effect byte (each
// game's FACTS.md "Enemy skills", 22 November 2026); an effect byte no such
// line names is left unsaid. The menu's element orb shows the element, so a
// skill with an effect names that instead.
inline std::string describe(const std::uint8_t* r) {
    const int kind = r[1], effect = r[3], element = r[2], target = r[0];
    const bool many = r[8] != 1;
    const char* clause = nullptr;  // {o}: it/them or the target, {p}: its/their
    switch (effect) {
    case 0x06: case 0x07: clause = "boost {p} Attack"; break;
    case 0x08: case 0x09: clause = "drop {p} Attack"; break;
    case 0x0A: case 0x0B: clause = "boost {p} Defense"; break;
    case 0x0C: case 0x0D: clause = "drop {p} Defense"; break;
    case 0x0E: case 0x0F: clause = "boost {p} Resistance"; break;
    case 0x10: case 0x11: clause = "drop {p} Resistance"; break;
    case 0x12: case 0x13: clause = "poison {o}"; break;
    case 0x14: clause = "delude {o}"; break;
    case 0x17: clause = "stun {o}"; break;
    case 0x18: clause = "put {o} to sleep"; break;
    case 0x19: case 0x43: clause = "seal {p} Psynergy"; break;
    case 0x1A: clause = "haunt {o}"; break;
    case 0x1B: clause = "try to fell {o}"; break;
    case 0x1C: clause = "curse {o}"; break;
    case 0x1F: clause = "drain {p} HP"; break;
    case 0x20: clause = "drain {p} PP"; break;
    case 0x21: clause = "remove {p} bonuses"; break;
    case 0x23: clause = "pierce {p} Defense"; break;
    case 0x35: clause = "hold {o} in place"; break;
    case 0x3A: clause = "slow {o} down"; break;
    default: break;
    }
    const auto fill = [](std::string t, const char* o, const char* p) {
        for (std::size_t at; (at = t.find("{o}")) != std::string::npos;) t.replace(at, 3, o);
        for (std::size_t at; (at = t.find("{p}")) != std::string::npos;) t.replace(at, 3, p);
        return t;
    };
    const auto capital = [](std::string t) {
        if (!t.empty() && t[0] >= 'a' && t[0] <= 'z') t[0] = static_cast<char>(t[0] - 'a' + 'A');
        return t + ".";
    };
    // Who it acts on: target +0 is 1 for foes, 2 for allies, 4 for the user.
    const char* o = target == 4 ? "yourself" : target == 2 ? (many ? "allies" : "an ally") : (many ? "foes" : "a foe");
    const char* p = target == 4 ? "your own" : target == 2 ? (many ? "allies'" : "an ally's") : (many ? "foes'" : "a foe's");
    static const char* const kElements[] = {"Venus", "Mercury", "Mars", "Jupiter"};
    const std::string el = element < 4 ? std::string(kElements[element]) + " " : std::string();
    switch (kind) {
    case 0x81: return capital(fill("restore {p} HP", o, p));
    case 0x8B: return capital(fill("restore {p} PP", o, p));
    case 0x87: return effect == 0x4A ? "Counter an enemy's attack." : "Brace for the enemy's attack.";
    case 0x82: return clause ? capital(fill(clause, o, p)) : capital(fill("strike {o} with " + el + "power", o, p));
    case 0x83: case 0x84: case 0x85: case 0x86: {
        const char* verb = kind == 0x83 ? "strike" : kind == 0x84 ? "pound" : kind == 0x85 ? "blast" : "engulf";
        if (clause)
            return capital(fill(std::string(verb) + " {o} and " + fill(clause, many ? "them" : "it", many ? "their" : "its"), o, p));
        const char* with = kind == 0x83 ? "force" : kind == 0x84 ? "a mighty blow" : kind == 0x85 ? "Psynergy" : "power";
        return capital(fill(std::string(verb) + " {o} with " + (kind == 0x84 ? std::string() : el) + with, o, p));
    }
    default: return std::string();
    }
}

// Each enemy-only skill's own line (Jimmy, 22 November 2026: "make their
// descriptions a bit more flavorful"), written from its name and what its row
// does: kind, element, reach and the effect words describe() uses. The rows
// both games share (ids 0xD4, 0x1A4..0x206) read for either name (The Broken
// Seal's Blessings are The Lost Age's Breaths). Each fits 222 pixels, the
// widest description either game prints (FACTS "Enemy skills").
constexpr Line kLines[] = {
    {0x0D4, "Strike a foe with a lethal venom."},
    {0x1A4, "Bathe foes in a scorching gout of flame."},
    {0x1A5, "Engulf foes in a roaring wave of fire."},
    {0x1A6, "Unleash an inferno upon foes."},
    {0x1A7, "Splash foes with a spray of cold water."},
    {0x1A8, "Drench foes in a rushing torrent."},
    {0x1A9, "Crash a great wave down upon foes."},
    {0x1AA, "Blow a chilling mist that frosts foes."},
    {0x1AB, "Freeze foes in a howling, icy gale."},
    {0x1AC, "Bury foes in a raging blizzard."},
    {0x1AD, "Shroud foes in darkness to delude them."},
    {0x1AF, "Spew searing fumes that eat at armor."},
    {0x1B0, "Spray foes with acid that melts armor."},
    {0x1B1, "Whip up a storm that stuns foes."},
    {0x1B2, "Unleash a tempest that stuns foes."},
    {0x1B3, "Send a shrill sound wave at a foe."},
    {0x1B4, "Blast a foe with a piercing sound wave."},
    {0x1B5, "Let out an ear-splitting shriek."},
    {0x1B6, "Wail like a banshee to stun a foe."},
    {0x1B8, "Bellow a war cry that roots a foe."},
    {0x1B9, "Howl wickedly to hold a foe in place."},
    {0x1BA, "Buffet a foe with beating wings."},
    {0x1BB, "Flutter wings to whip up a gust."},
    {0x1BC, "Batter foes with a mighty wing stroke."},
    {0x1BD, "Breathe cursed fire that haunts foes."},
    {0x1BE, "Spread a lethal gas that poisons foes."},
    {0x1C2, "Shake the ground beneath a foe."},
    {0x1C3, "Chill a foe to the bone."},
    {0x1C4, "Slice a foe with a razor-sharp gust."},
    {0x1C5, "Ram a foe with a bone-hard charge."},
    {0x1C6, "Conjure an eerie flame to burn a foe."},
    {0x1C7, "Stun a foe with a numbing sting."},
    {0x1C8, "Smash a foe with sheer brute strength."},
    {0x1C9, "Coat a foe in goo that softens armor."},
    {0x1CA, "Sink fangs into a foe and drain its HP."},
    {0x1CB, "Maul a foe with savage claws."},
    {0x1CC, "Bite a foe with venomous fangs."},
    {0x1CD, "Swoop down on a foe from above."},
    {0x1CE, "Slash a foe with a cursed blade."},
    {0x1CF, "Tear into a foe in a wild frenzy."},
    {0x1D0, "Spit toxic goo that poisons a foe."},
    {0x1D1, "Pierce a foe with poisoned fangs."},
    {0x1D2, "Bite with a jolt that stuns a foe."},
    {0x1D3, "Lash a foe with a venomous tail."},
    {0x1D4, "Assail a foe in a relentless onslaught."},
    {0x1D5, "Drain a foe's HP with vampiric fangs."},
    {0x1D6, "Infect a foe with festering germs."},
    {0x1D7, "Dart in and strike a foe in a flash."},
    {0x1D8, "Splash rotten blood that weakens armor."},
    {0x1D9, "Pummel a foe with a mighty arm."},
    {0x1DA, "Snap at a foe with twin fangs."},
    {0x1DB, "Deal a foe a savage, mortal blow."},
    {0x1DC, "Bite wildly and sap a foe's Attack."},
    {0x1DD, "Peck a foe with a pair of sharp beaks."},
    {0x1DE, "Bite a foe with rabid, poisonous fangs."},
    {0x1DF, "Bite with acid that eats at armor."},
    {0x1E0, "Hurl a lit stick of dynamite at foes."},
    {0x1E1, "Slam into a foe with a hard head."},
    {0x1E2, "Squirt a foe with toxic ink."},
    {0x1E3, "Club a foe with a heavy fist."},
    {0x1E4, "Lash back at a foe with full force."},
    {0x1E5, "Charge a foe in a reckless dash."},
    {0x1E6, "Call a gentle star to soothe allies."},
    {0x1E7, "Wrap a foe in a web to weaken armor."},
    {0x1E8, "Rend a foe's heart to sap its Attack."},
    {0x1E9, "Spatter foes with ooze that deludes."},
    {0x1EA, "Convulse wildly to restore your HP."},
    {0x1EB, "Scatter stardust to put foes to sleep."},
    {0x1EC, "Spread rot that saps foes' Attack."},
    {0x1ED, "Send a spirit to haunt a foe."},
    {0x1EE, "Squirm and wriggle to restore HP."},
    {0x1EF, "Fly into a rage and boost your Attack."},
    {0x1F0, "Foretell a foe's doom and curse it."},
    {0x1F1, "Gather your strength to restore HP."},
    {0x1FE, "Pound a foe with venom-laced blows."},
    {0x1FF, "Spin and batter a foe with blows."},
    {0x200, "Flash blinding heat to delude a foe."},
    {0x201, "Swing a reaping scythe to fell a foe."},
    {0x204, "Bite deep and drain a foe's HP."},
    {0x205, "Strike a foe with a crushing blow."},
    {0x206, "Thrash wildly at a foe."},
};

// The description line for `text_id` when it is an enemy-only skill's in a
// run with Enemy skills (`allowed`: the revision's descriptions start at
// g.description_text and its skills are the measured ones).
inline bool description(const Game& g, bool allowed, std::uint32_t text_id, std::string& out) {
    if (!allowed || !g.description_text || text_id < g.description_text) return false;
    const std::uint32_t id = text_id - g.description_text;
    if (id >= g.abilities.count) return false;
    const Config c = current(g);
    if (!c.psynergy || !c.enemy_skills) return false;
    const EnemySkill* skill = find_skill(g, static_cast<std::uint16_t>(id));
    if (!skill || skill->rom_icon != 0) return false;  // rows with an icon have their own line
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !b->rom_ptr()) return false;
    const std::size_t row = g.abilities.address - 0x08000000u + std::size_t{g.abilities.size} * id;
    if (row + g.abilities.size > b->rom_size()) return false;
    for (const Line& l : g.lines)
        if (l.id == id) return out = menu_text(l.text), true;
    for (const Line& l : kLines)
        if (l.id == id) return out = menu_text(l.text), true;
    out = describe(b->rom_ptr() + row);
    if (!out.empty()) out = menu_text(out.c_str());
    return !out.empty();
}

// Scrolling names (name_scroll.h): an enemy skill's name, in a run with
// Enemy skills, scrolls when it is wider than every class spell's name.
inline bool scrolls(const Game& g, std::uint16_t id) {
    const Config c = current(g);
    return c.psynergy && c.enemy_skills && find_skill(g, id) != nullptr;
}
inline std::vector<std::uint16_t> reference_ids(const Game& g) {
    std::vector<std::uint16_t> out;
    for (const Spell& s : pool(g).spells) out.push_back(s.id);
    return out;
}

// The same a character at a time (The Broken Seal's decoder hook): the next
// character, then the 0 that ends the line; a new line starts again. The
// line is built once, at its first character.
struct DescriptionState { std::uint32_t id = 0; int pos = -1; std::string line; };
inline DescriptionState description_state;
inline bool description_character(const Game& g, bool allowed, std::uint32_t text_id, std::uint32_t* character) {
    DescriptionState& s = description_state;
    if (s.pos < 0 || text_id != s.id) {
        s.pos = -1;
        if (!description(g, allowed, text_id, s.line)) return false;
        s.pos = 0, s.id = text_id;
    }
    const auto pos = static_cast<std::size_t>(s.pos);
    *character = pos < s.line.size() ? static_cast<unsigned char>(s.line[pos]) : 0u;
    s.pos = pos < s.line.size() ? s.pos + 1 : -1;
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
