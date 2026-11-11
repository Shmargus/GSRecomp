// Random Equipment (first Random Weapons): the seed's plan and the new
// descriptions. Pure functions over ROM bytes (no engine), so a host bench
// can run them on the real ROMs. Runtime part: random_weapons.h. Jimmy's
// design (2, 5 and 6 November 2026, docs/NEXT_PRIORITY_MODES.md):
//  - every weapon takes the stat line of a random OTHER weapon (no newly
//    rolled numbers) and a random unleash;
//  - armour is folded in (6 November): every piece takes the stat line of a
//    random other piece of the same kind (body, arm, head, feet, undershirt);
//  - who can equip it, its name, look, price, curse and use stay its own;
//  - Scaling on: the other piece's stat line is scaled so its Attack (a
//    weapon) or Defense (armour) is the piece's own; its other numbers scale
//    by the same ratio.
//
// Item records, 0x2C bytes, measured in both games (each game's FACTS.md
// "Random Weapons"; the weapon records are byte-identical in all five ROMs
// of a game): +2 type (1 = weapon), +3 flags, +4 equip mask, +6 icon,
// +8 Attack (i16), +0xB unleash chance in 5% steps (7 = 35%, 8 = 40%; 0
// with no unleash), +0xC use kind, +0xE unleash ability (u16), +0x14
// element (4 = none), +0x18 four bonuses of 4 bytes (u8 kind, i8 amount,
// two zero bytes), +0x28 use ability. Armour, measured the same way (6
// November 2026, both games' item tables): types 2 body, 3 arm (shields,
// gloves, armlets), 4 head, 5 feet, 9 undershirts; Defense +0xA (u8, up to
// 53); some carry Attack +8 (Spiked Armor 10); no unleash (+0xB, +0xE zero);
// element +0x14 always 4. Rings (type 8) carry only a use and stay.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gs_text_encode.h"

namespace gsr::random_weapons {

constexpr std::uint32_t kRecordSize = 0x2Cu;
constexpr std::uint32_t kType = 2, kMask = 4, kAttack = 8, kDefense = 0xA, kChance = 0xB,
                        kUse = 0xC, kUnleash = 0xE, kElement = 0x14, kBonuses = 0x18;
constexpr int kBonusCount = 4;
constexpr std::uint32_t kBonusSize = 4;
constexpr std::uint8_t kWeapon = 1;
constexpr std::uint8_t kArmourTypes[] = {2, 3, 4, 5, 9};
constexpr std::uint8_t kUsable = 2;  // use kind: the item casts its use ability

inline bool armour_type(std::uint8_t type) {
    for (const std::uint8_t t : kArmourTypes)
        if (t == type) return true;
    return false;
}

using Record = std::array<std::uint8_t, kRecordSize>;

struct Unleash {
    std::uint16_t id = 0;
    std::uint8_t chance = 0;
};

// One weapon's or armour piece's pristine record and what the plan makes of it.
struct Weapon {
    std::uint16_t id = 0;
    std::uint8_t type = 0;
    Record original{}, planned{};
    std::uint16_t donor = 0;  // whose stat line it took
    Unleash unleash;
};

struct Plan {
    std::uint32_t seed = 0;
    bool scaling = false, deal_weapons = false, armour = false;
    std::vector<Weapon> weapons;
};

inline int attack_of(const Record& r) {
    return static_cast<std::int16_t>(r[kAttack] | r[kAttack + 1] << 8);
}
inline std::uint16_t unleash_of(const Record& r) {
    return static_cast<std::uint16_t>(r[kUnleash] | r[kUnleash + 1] << 8);
}
inline int defense_of(const Record& r) { return r[kDefense]; }
inline std::uint16_t mask_of(const Record& r) {
    return static_cast<std::uint16_t>(r[kMask] | r[kMask + 1] << 8);
}

// splitmix64, one stream per purpose. The tag in the top bits keeps these
// streams apart from Random Psynergy's, which shares the run's seed.
struct Rng {
    std::uint64_t state;
    Rng(std::uint32_t seed, unsigned purpose)
        : state(std::uint64_t{seed} * 0x9E3779B97F4A7C15ull ^ (std::uint64_t{purpose} << 40) ^
                (std::uint64_t{0x5745u} << 48)) {}
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }
};

// The weapons and armour of a pristine item table: type-1 and armour-type
// records with ids first..last, minus `excluded`.
inline std::vector<Weapon> weapons_of(std::span<const std::uint8_t> table, unsigned first,
                                      unsigned last, std::span<const std::uint16_t> excluded) {
    std::vector<Weapon> out;
    for (unsigned id = first; id <= last; ++id) {
        if ((id + 1u) * kRecordSize > table.size()) break;
        const std::uint8_t type = table[id * kRecordSize + kType];
        if (type != kWeapon && !armour_type(type)) continue;
        if (std::find(excluded.begin(), excluded.end(), id) != excluded.end()) continue;
        Weapon w;
        w.id = static_cast<std::uint16_t>(id);
        w.type = type;
        std::copy_n(table.begin() + id * kRecordSize, kRecordSize, w.original.begin());
        w.planned = w.original;
        out.push_back(w);
    }
    return out;
}

// `value` * own / donor, rounded; a non-zero bonus never scales to nothing.
inline std::int8_t scaled(std::int8_t value, int own, int donor) {
    if (value == 0 || donor == 0) return value;
    const double v = std::round(static_cast<double>(value) * own / donor);
    int r = static_cast<int>(std::clamp(v, -128.0, 127.0));
    if (r == 0) r = value > 0 ? 1 : -1;
    return static_cast<std::int8_t>(r);
}

// The same for Attack (i16) on armour.
inline int scaled_int(int value, int own, int donor) {
    if (value == 0 || donor == 0) return value;
    int r = static_cast<int>(std::lround(static_cast<double>(value) * own / donor));
    if (r == 0) r = value > 0 ? 1 : -1;
    return r;
}

// One cycle over n (Sattolo's shuffle): no index keeps its own place and
// every place is used exactly once.
inline std::vector<std::size_t> one_cycle(std::size_t n, Rng& rng) {
    std::vector<std::size_t> to(n);
    for (std::size_t i = 0; i < n; ++i) to[i] = i;
    for (std::size_t i = n - 1; i > 0 && n > 1; --i) std::swap(to[i], to[rng.below(i)]);
    return to;
}

inline void set_attack(Record& r, int attack) {
    r[kAttack] = static_cast<std::uint8_t>(attack);
    r[kAttack + 1] = static_cast<std::uint8_t>(static_cast<unsigned>(attack) >> 8);
}

// `from`'s bonuses into `r`, scaled by own / other when Scaling.
inline void copy_bonuses(Record& r, const Record& from, bool scaling, int own, int other) {
    for (int k = 0; k < kBonusCount; ++k) {
        const std::uint32_t at = kBonuses + static_cast<std::uint32_t>(k) * kBonusSize;
        for (std::uint32_t j = 0; j < kBonusSize; ++j) r[at + j] = from[at + j];
        if (scaling && r[at] != 0)
            r[at + 1] = static_cast<std::uint8_t>(scaled(static_cast<std::int8_t>(r[at + 1]), own, other));
    }
}

// `weapons`, `armour`: which of the two are dealt (separate choices since 7
// November 2026).
inline Plan make_plan(std::vector<Weapon> all, std::uint32_t seed, bool scaling, bool weapons,
                      bool armour) {
    Plan plan;
    plan.seed = seed;
    plan.scaling = scaling;
    plan.deal_weapons = weapons;
    plan.armour = armour;
    if (seed == 0) {
        plan.weapons = std::move(all);
        return plan;
    }
    // Weapons: donors in one cycle, then unleashes.
    std::vector<std::size_t> weapon_at;
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].type == kWeapon) weapon_at.push_back(i);
    const std::size_t n = weapon_at.size();
    if (weapons && n >= 2) {
        Rng lines(seed, 1);
        const std::vector<std::size_t> donor = one_cycle(n, lines);

        // Unleashes: the game's own, each with its own chance, dealt from a
        // shuffled deck that is shuffled again when it runs out.
        std::vector<Unleash> pool;
        for (const std::size_t i : weapon_at) {
            const Weapon& w = all[i];
            const std::uint16_t u = unleash_of(w.original);
            if (u == 0 || w.original[kChance] == 0) continue;
            const bool seen = std::any_of(pool.begin(), pool.end(),
                                          [u](const Unleash& x) { return x.id == u; });
            if (!seen) pool.push_back({u, w.original[kChance]});
        }
        std::sort(pool.begin(), pool.end(), [](const Unleash& a, const Unleash& b) { return a.id < b.id; });
        Rng deal(seed, 2);
        std::vector<Unleash> deck;
        const auto draw = [&]() -> Unleash {
            if (pool.empty()) return {};
            if (deck.empty()) {
                deck = pool;
                for (std::size_t i = deck.size() - 1; i > 0; --i) std::swap(deck[i], deck[deal.below(i + 1)]);
            }
            const Unleash u = deck.back();
            deck.pop_back();
            return u;
        };

        for (std::size_t k = 0; k < n; ++k) {
            Weapon& w = all[weapon_at[k]];
            const Weapon& d = all[weapon_at[donor[k]]];
            w.donor = d.id;
            Record r = w.original;
            const int own = attack_of(w.original), other = attack_of(d.original);
            set_attack(r, scaling ? own : other);
            r[kElement] = d.original[kElement];
            copy_bonuses(r, d.original, scaling, own, other);
            w.unleash = draw();
            r[kChance] = w.unleash.chance;
            r[kUnleash] = static_cast<std::uint8_t>(w.unleash.id);
            r[kUnleash + 1] = static_cast<std::uint8_t>(w.unleash.id >> 8);
            w.planned = r;
        }
    }
    // Armour: one cycle per kind; the stat line is Attack, Defense, element
    // and bonuses. Scaling keeps the piece's own Defense.
    for (const std::uint8_t type : kArmourTypes) {
        if (!armour) break;
        std::vector<std::size_t> at;
        for (std::size_t i = 0; i < all.size(); ++i)
            if (all[i].type == type) at.push_back(i);
        if (at.size() < 2) continue;
        Rng lines(seed, 0x10u + type);
        const std::vector<std::size_t> donor = one_cycle(at.size(), lines);
        for (std::size_t k = 0; k < at.size(); ++k) {
            Weapon& w = all[at[k]];
            const Weapon& d = all[at[donor[k]]];
            w.donor = d.id;
            Record r = w.original;
            const int own = defense_of(w.original), other = defense_of(d.original);
            r[kDefense] = scaling ? w.original[kDefense] : d.original[kDefense];
            const int attack = attack_of(d.original);
            set_attack(r, scaling ? scaled_int(attack, own, other) : attack);
            r[kElement] = d.original[kElement];
            copy_bonuses(r, d.original, scaling, own, other);
            w.planned = r;
        }
    }
    plan.weapons = std::move(all);
    return plan;
}

// --- descriptions -------------------------------------------------------------------
// A weapon's description names its unleash ("Long Sword: Unleashes Acid
// Bath"; Spanish "Espada larga. Libera Baño ácido."). Each ROM's own wording
// is learnt from its own lines: a weapon without an unleash shows its kind
// alone ("Long Sword"), which starts the lines of the weapons with one; what
// follows the kind up to the unleash's name, and after it, is the wording.
struct Texts {
    std::size_t text_table, tree_table;   // ROM offsets (gs_text_encode.h)
    std::uint32_t description;            // item description = this + id
    std::uint32_t ability_name;           // ability name = this + id
    std::span<const std::uint8_t> advances;  // 224 font advances, glyph - 32
};

inline unsigned text_width(const std::string& s, std::span<const std::uint8_t> advances) {
    unsigned w = 0;
    for (const unsigned char c : s)
        if (c >= 32u && c - 32u < advances.size()) w += advances[c - 32u];
    return w;
}

// The weapons' new description lines, written into `out`; a weapon whose
// wording cannot be learnt keeps the game's own line (absent from the map).
inline void weapon_descriptions(mods::TextEncoder& decoder, const Texts& t,
                                const std::vector<Weapon>& weapons,
                                std::map<std::uint16_t, std::string>& out) {
    const auto line = [&](std::uint32_t n) {
        std::string s;
        return decoder.decode(n, &s) ? s : std::string();
    };
    const auto unknown = [](const std::string& s) {
        return s.empty() || std::all_of(s.begin(), s.end(), [](char c) { return c == '?'; });
    };
    std::map<std::uint16_t, std::string> own, name;
    for (const Weapon& w : weapons) {
        own[w.id] = line(t.description + w.id);
        if (const std::uint16_t u = unleash_of(w.original)) name[u] = line(t.ability_name + u);
        if (w.unleash.id) name[w.unleash.id] = line(t.ability_name + w.unleash.id);
    }
    // Kinds: the lines of weapons without an unleash that begin another
    // weapon's unleash line.
    std::vector<std::string> kinds;
    for (const Weapon& w : weapons) {
        const std::string& d = own[w.id];
        if (unleash_of(w.original) != 0 || unknown(d)) continue;
        for (const Weapon& v : weapons) {
            const std::uint16_t u = unleash_of(v.original);
            const std::string& e = own[v.id];
            if (u && e.size() > d.size() && e.compare(0, d.size(), d) == 0 &&
                e.find(name[u]) != std::string::npos &&
                std::find(kinds.begin(), kinds.end(), d) == kinds.end()) {
                kinds.push_back(d);
                break;
            }
        }
    }
    const auto kind_of = [&](const std::string& d) {
        std::string best;
        for (const std::string& k : kinds)
            if (k.size() > best.size() && d.compare(0, k.size(), k) == 0) best = k;
        return best;
    };
    // Wordings (between the kind and the name, after the name), commonest first.
    std::map<std::pair<std::string, std::string>, int> counts;
    unsigned widest = 0;
    for (const Weapon& w : weapons) {
        const std::string& d = own[w.id];
        widest = std::max(widest, text_width(d, t.advances));
        const std::uint16_t u = unleash_of(w.original);
        if (!u || name[u].empty()) continue;
        const std::size_t at = d.find(name[u]);
        const std::string k = kind_of(d);
        if (at == std::string::npos || k.empty() || at < k.size()) continue;
        ++counts[{d.substr(k.size(), at - k.size()), d.substr(at + name[u].size())}];
    }
    std::vector<std::pair<std::string, std::string>> wordings;
    for (const auto& [w, c] : counts) wordings.push_back(w);
    std::stable_sort(wordings.begin(), wordings.end(),
                     [&](const auto& a, const auto& b) { return counts[a] > counts[b]; });

    for (const Weapon& w : weapons) {
        const std::string& d = own[w.id];
        const std::string& n = name[w.unleash.id];
        if (!w.unleash.id || n.empty()) continue;
        std::vector<std::string> options;
        // Its own line with the name swapped keeps its own extras ("(cursed)",
        // "Cursed Long Sword"), but only with the commonest wording or no
        // kind: other wordings can carry an article for the old name
        // (Spanish "Libera el Espejismo.").
        std::string swapped;
        const std::uint16_t u = unleash_of(w.original);
        std::string k = kind_of(d);
        if (u && !name[u].empty()) {
            const std::size_t at = d.find(name[u]);
            if (at != std::string::npos) {
                const bool common = !wordings.empty() && at >= k.size() &&
                                    d.compare(k.size(), at - k.size(), wordings.front().first) == 0;
                if (k.empty() || common) swapped = d.substr(0, at) + n + d.substr(at + name[u].size());
            }
        }
        if (!swapped.empty()) options.push_back(swapped);
        if (k.empty()) {
            // No kind of its own ("?" lines of unused weapons, "Cursed
            // Sword"): the kind of the first weapon worn by the same heroes.
            for (const Weapon& v : weapons)
                if (mask_of(v.original) == mask_of(w.original) && !(k = kind_of(own[v.id])).empty()) break;
        }
        if (!k.empty())
            for (const auto& [lead, tail] : wordings) options.push_back(k + lead + n + tail);
        if (options.empty()) continue;
        // The first that fits as wide as the game's own widest weapon line,
        // else the narrowest.
        std::string pick;
        for (const std::string& o : options)
            if (text_width(o, t.advances) <= widest) { pick = o; break; }
        if (pick.empty())
            pick = *std::min_element(options.begin(), options.end(), [&](const auto& a, const auto& b) {
                return text_width(a, t.advances) < text_width(b, t.advances);
            });
        out[w.id] = pick;
    }
}

// Armour lines name the piece's kind, then its bonuses ("Armor: Resists
// Water & Fire"; Spanish "Ropa. Resiste el agua."). Kinds are learnt as for
// weapons: the line of a plain piece (no bonuses, no Attack, no use) that
// starts other lines. A piece takes its own kind and its donor's tail; a
// donor whose line is about its use (use kind 2) gives no tail, and a piece
// with a use of its own keeps its own line (the use stays its own). Lines
// that cannot be split keep the game's own.
inline void armour_descriptions(mods::TextEncoder& decoder, const Texts& t,
                                const std::vector<Weapon>& pieces,
                                std::map<std::uint16_t, std::string>& out) {
    const auto line = [&](std::uint32_t n) {
        std::string s;
        return decoder.decode(n, &s) ? s : std::string();
    };
    std::map<std::uint16_t, std::string> own;
    std::map<std::uint16_t, const Weapon*> by_id;
    for (const Weapon& w : pieces) {
        own[w.id] = line(t.description + w.id);
        by_id[w.id] = &w;
    }
    const auto plain = [](const Record& r) {
        if (attack_of(r) != 0 || r[kUse] == kUsable) return false;
        for (int k = 0; k < kBonusCount; ++k)
            if (r[kBonuses + static_cast<std::uint32_t>(k) * kBonusSize] != 0) return false;
        return true;
    };
    std::vector<std::string> kinds;
    for (const Weapon& w : pieces) {
        const std::string& d = own[w.id];
        if (d.empty() || d.find('?') != std::string::npos || !plain(w.original)) continue;
        if (std::find(kinds.begin(), kinds.end(), d) != kinds.end()) continue;
        for (const Weapon& v : pieces)
            if (v.id != w.id && own[v.id].size() > d.size() && own[v.id].compare(0, d.size(), d) == 0) {
                kinds.push_back(d);
                break;
            }
    }
    const auto kind_of = [&](const std::string& d) {
        std::string best;
        for (const std::string& k : kinds)
            if (k.size() > best.size() && d.compare(0, k.size(), k) == 0) best = k;
        return best;
    };
    unsigned widest = 0;
    for (const Weapon& w : pieces) widest = std::max(widest, text_width(own[w.id], t.advances));
    for (const Weapon& w : pieces) {
        if (w.donor == 0 || w.original[kUse] == kUsable) continue;
        const std::string mine = kind_of(own[w.id]);
        const auto donor = by_id.find(w.donor);
        if (mine.empty() || donor == by_id.end()) continue;
        std::string tail;
        if (donor->second->original[kUse] != kUsable) {
            const std::string& d = own[w.donor];
            const std::string theirs = kind_of(d);
            if (theirs.empty()) continue;  // the donor's line cannot be split
            tail = d.substr(theirs.size());
        }
        std::string pick = mine + tail;
        if (text_width(pick, t.advances) > widest) pick = mine;
        if (pick != own[w.id]) out[w.id] = pick;
    }
}

// New description per item id (weapons and armour).
inline std::map<std::uint16_t, std::string> descriptions(std::span<const std::uint8_t> rom,
                                                         const Texts& t, const Plan& plan) {
    std::map<std::uint16_t, std::string> out;
    if (plan.seed == 0) return out;
    mods::TextEncoder decoder(rom, t.text_table, t.tree_table);
    std::vector<Weapon> weapons;
    for (const Weapon& w : plan.weapons)
        if (w.type == kWeapon) weapons.push_back(w);
    if (plan.deal_weapons) weapon_descriptions(decoder, t, weapons, out);
    for (const std::uint8_t type : kArmourTypes) {
        if (!plan.armour) break;
        std::vector<Weapon> pieces;
        for (const Weapon& w : plan.weapons)
            if (w.type == type) pieces.push_back(w);
        armour_descriptions(decoder, t, pieces, out);
    }
    return out;
}

}  // namespace gsr::random_weapons
