// Random Djinn, one copy of the rules for both runners. A randomizer run
// can tick "Djinn": every Djinni the field hands out (the ones that wait on
// the map or are fought first) gives another Djinni instead, of any
// element, and each is still found exactly once. The run's seed decides it
// (random_psynergy_format.h holds the tick).
//
// Both games hand a field Djinni over in one routine, join(r0, element,
// index) (each game's FACTS.md "Random Djinn"): unless the Djinn lesson flag
// 0x16E is set it calls the grant (element, index), plays the element's
// scene and shows "<name> joined" from the same two arguments. So the swap
// happens at join's entry and the scene and message name the new Djinni.
// The grant tests and sets flag 0x30 + element*20 + index; the map uses
// that flag to know the spot is empty, so for a swapped grant the flag
// calls take the spot's own Djinni again: flags stay "this spot is taken"
// and ownership lives in the party records (+0xF8) as always.
//
// The lesson's Djinni (Flint in The Broken Seal) is left out. A target
// already owned (a Djinni some other way gave) passes on to its own target,
// so nobody gets a Djinni twice.
//
// The look follows (each game's FACTS.md "Random Djinn", 22 November 2026):
// - On the map, a Djinni is the actor whose map object record's halfword +2
//   holds its spot's flag (join's caller finds it that way, through the
//   game's object lookup: object_flag() below). Once a frame, such an
//   actor's sprite (actor +0x50, primary layer +0x28, sprite id at layer +0)
//   showing the spot's element's Djinni sprite is switched to the given
//   Djinni's element, as the game's own layer reload and animation start
//   write the layer.
// - A Djinni fought first: the room code calls fight(n), n = element*20 +
//   index, which picks the formation from a table (one Djinni enemy per
//   element and strength); its n becomes the given element at the spot's
//   index, so the fight shows the given element at the spot's strength.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "djinn_tracker.h"
#include "flag_bank.h"
#include "gba_bus.h"
#include "random_psynergy.h"
#include "runtime_bus_bridge.h"

namespace gsr::random_djinn {

struct Djinni { std::uint8_t element, index; };

struct Game {
    const random_psynergy::Game* run;  // the seed and the tick
    std::uint32_t join_pc;             // join(r0, element, index)
    std::uint32_t join_grant_ret;      // join's call to the grant, + 4
    std::uint32_t grant_pc;            // grant(element, index)
    std::uint32_t flag_test_pc, flag_test_ret;  // the grant's flag test, its return
    std::uint32_t flag_set_pc, flag_set_ret;    // the grant's flag set, its return
    std::uint32_t records, record_size;
    int heroes;
    const Djinni* djinn;               // the shuffled set, in spot order
    int count;
    // The look.
    std::uint32_t actors;              // IWRAM word: the actor context
    int actor_first, actor_last;       // the ids join's caller searches
    std::uint32_t sprite_records;      // 20-byte field sprite records
    std::uint16_t sprites[4];          // the Djinni sprite of each element
    std::uint32_t fight_pc, fight_ret; // fight(n) and the room code's call to it, + 4
};

constexpr std::uint32_t kLessonFlag = 0x16Eu, kDjinnFlags = 0x30u, kOwned = 0xF8u;

inline std::uint32_t flag_of(Djinni d) { return kDjinnFlags + d.element * 20u + d.index; }

inline int index_of(const Game& g, Djinni d) {
    for (int i = 0; i < g.count; ++i)
        if (g.djinn[i].element == d.element && g.djinn[i].index == d.index) return i;
    return -1;
}

// target[spot]: the Djinni spot `spot` gives (both indices into g.djinn).
inline std::vector<int> plan(const Game& g, std::uint32_t seed) {
    std::vector<int> target(static_cast<std::size_t>(g.count));
    for (int i = 0; i < g.count; ++i) target[static_cast<std::size_t>(i)] = i;
    random_psynergy::Rng rng(seed, 5, 0);
    for (int i = g.count - 1; i > 0; --i)
        std::swap(target[static_cast<std::size_t>(i)], target[rng.below(static_cast<unsigned>(i + 1))]);
    return target;
}

inline bool enabled(const Game& g, random_psynergy::Config* out = nullptr) {
    const random_psynergy::Config c = random_psynergy::current(*g.run);
    if (out) *out = c;
    return c.seed != 0 && c.djinn;
}

inline bool owned(const Game& g, gba::GbaBus& b, Djinni d) {
    for (int h = 0; h < g.heroes; ++h)
        if (b.read32(g.records + h * g.record_size + kOwned + d.element * 4u) >> d.index & 1u) return true;
    return false;
}

struct State {
    std::uint32_t seed = 0;
    std::vector<int> target;
    // A swapped join on its way through the grant.
    bool active = false, armed = false;
    std::uint32_t spot_flag = 0, given_flag = 0;
    Djinni given{};
};
inline State state;

inline const std::vector<int>& targets(const Game& g, std::uint32_t seed) {
    if (state.seed != seed || state.target.size() != static_cast<std::size_t>(g.count)) {
        state.seed = seed;
        state.target = plan(g, seed);
    }
    return state.target;
}

inline bool lesson(const Game& g, gba::GbaBus& b) {
    return gsr::read_flag(b.ewram_ptr(), kLessonFlag);
}

// The Djinni spot `spot` gives now (itself when it is not shuffled).
inline Djinni given_for(const Game& g, gba::GbaBus& b, std::uint32_t seed, Djinni spot) {
    const int at = index_of(g, spot);
    if (at < 0) return spot;
    const std::vector<int>& target = targets(g, seed);
    int t = target[static_cast<std::size_t>(at)];
    for (int step = 0; step < g.count && owned(g, b, g.djinn[t]); ++step) t = target[static_cast<std::size_t>(t)];
    return g.djinn[t];
}

// At every function entry the runner watches; r: the guest registers.
inline void on_entry(const Game& g, std::uint32_t pc, std::uint32_t* r) {
    if (pc != g.join_pc && pc != g.grant_pc && pc != g.flag_test_pc && pc != g.flag_set_pc &&
        pc != g.fight_pc)
        return;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return;
    const std::uint32_t lr = r[14] & ~1u;
    if (pc == g.fight_pc) {
        random_psynergy::Config c;
        if (lr != g.fight_ret || r[0] >= 80u || !enabled(g, &c) || lesson(g, *b)) return;
        const Djinni spot{static_cast<std::uint8_t>(r[0] / 20u), static_cast<std::uint8_t>(r[0] % 20u)};
        const Djinni given = given_for(g, *b, c.seed, spot);
        if (given.element != spot.element) r[0] = given.element * 20u + spot.index;
        return;
    }
    if (pc == g.join_pc) {
        state.active = state.armed = false;
        random_psynergy::Config c;
        if (!enabled(g, &c)) return;
        if (lesson(g, *b)) return;
        if (r[1] > 3u || r[2] > 19u) return;
        const Djinni spot{static_cast<std::uint8_t>(r[1]), static_cast<std::uint8_t>(r[2])};
        if (index_of(g, spot) < 0) return;
        const Djinni given = given_for(g, *b, c.seed, spot);
        if (given.element == spot.element && given.index == spot.index) return;
        r[1] = given.element;
        r[2] = given.index;
        state.active = true;
        state.spot_flag = flag_of(spot);
        state.given_flag = flag_of(given);
        state.given = given;
        return;
    }
    if (pc == g.grant_pc) {
        state.armed = state.active && lr == g.join_grant_ret && r[0] == state.given.element &&
                      r[1] == state.given.index;
        state.active = false;
        return;
    }
    if (!state.armed || r[0] != state.given_flag) return;
    if (pc == g.flag_test_pc && lr == g.flag_test_ret) {
        r[0] = state.spot_flag;
    } else if (pc == g.flag_set_pc && lr == g.flag_set_ret) {
        r[0] = state.spot_flag;
        state.armed = false;
    }
}

inline bool ram_pointer(std::uint32_t p) {
    return (p >= 0x02000000u && p < 0x02040000u) || (p >= 0x03000000u && p < 0x03008000u);
}

// A map Djinni's layer showing sprite `from` shows sprite `to` instead: the
// fields the game's layer reload copies from the record (+4, +7, +8 frames,
// +0xC animations, frame +0x16 = 0xFF to reload), and the same animation
// started again from the new record as the game's animation start does
// (+0x10, +0x15 = 0x10, +0x14 and +2 = 0).
inline void show(const Game& g, gba::GbaBus& b, std::uint32_t layer, unsigned from, unsigned to) {
    const std::uint8_t* rom = b.rom_ptr();
    const std::size_t base = g.sprite_records - 0x08000000u;
    if (!rom || base + 20u * (std::max(from, to) + 1u) > b.rom_size()) return;
    const std::uint8_t* old_rec = rom + base + 20u * from;
    const std::uint8_t* new_rec = rom + base + 20u * to;
    const auto word = [](const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0] | p[1] << 8 | p[2] << 16) | static_cast<std::uint32_t>(p[3]) << 24;
    };
    const std::uint32_t frames = word(new_rec + 0xC), anims = word(new_rec + 0x10);
    if (frames < 0x08000000u || anims < 0x08000000u) return;
    const std::uint32_t playing = b.read32(layer + 0x10);
    std::uint32_t next = 0;
    for (unsigned k = 0; playing && k < old_rec[5] && k < new_rec[5]; ++k)
        if (b.read32(word(old_rec + 0x10) + 4u * k) == playing) { next = b.read32(anims + 4u * k); break; }
    b.write16(layer, static_cast<std::uint16_t>(to));
    b.write8(layer + 4, new_rec[4]);
    b.write8(layer + 7, new_rec[0xA]);
    b.write32(layer + 8, frames);
    b.write32(layer + 0xC, anims);
    b.write32(layer + 0x10, next);
    b.write8(layer + 0x14, 0);
    b.write8(layer + 0x15, 0x10);
    b.write16(layer + 2, 0);
    b.write8(layer + 0x16, 0xFF);
}

// The map's object records as both games' object lookup (The Broken Seal
// 0x0808D394, The Lost Age 0x080CCCB8) walks them for ids from 8: four lists
// at context +0..+0xC of 0x18-byte records, each ended by halfword -1;
// records whose s16 +0 is above 7 are numbered 8, 9, ... in order. out[id]:
// the record's halfword +2 (its flag), -1 for none.
inline void object_flags(gba::GbaBus& b, std::uint32_t context, int last, std::vector<int>& out) {
    out.assign(static_cast<std::size_t>(last + 1), -1);
    int id = 8;
    for (int list = 0; list < 4 && id <= last; ++list) {
        std::uint32_t rec = b.read32(context + 4u * list);
        if (!ram_pointer(rec)) continue;
        for (int n = 0; n < 0x100 && id <= last; ++n, rec += 0x18u) {
            const auto kind = static_cast<std::int16_t>(b.read16(rec));
            if (kind == -1) break;
            if (kind > 7) out[static_cast<std::size_t>(id++)] = static_cast<std::int16_t>(b.read16(rec + 2));
        }
    }
}

// The map Djinni of shuffled spots look like what they give.
inline void dress(const Game& g, gba::GbaBus& b, std::uint32_t seed) {
    const std::uint32_t context = b.read32(g.actors);
    if (!ram_pointer(context) || lesson(g, b)) return;
    static std::vector<int> flags;
    object_flags(b, context, g.actor_last, flags);
    for (int id = g.actor_first; id <= g.actor_last; ++id) {
        if (flags[static_cast<std::size_t>(id)] < 0) continue;
        const std::uint32_t actor = b.read32(context + 0x14u + 4u * id);
        if (!ram_pointer(actor)) continue;
        const int flag = flags[static_cast<std::size_t>(id)] - static_cast<int>(kDjinnFlags);
        if (flag < 0 || flag >= 80) continue;
        const Djinni spot{static_cast<std::uint8_t>(flag / 20), static_cast<std::uint8_t>(flag % 20)};
        if (index_of(g, spot) < 0) continue;
        // Taken: its Djinni is owned now, so the plan would name the next one.
        if (gsr::read_flag(b.ewram_ptr(), kDjinnFlags + static_cast<unsigned>(flag))) continue;
        const Djinni given = given_for(g, b, seed, spot);
        if (given.element == spot.element) continue;
        const std::uint32_t sprite = b.read32(actor + 0x50);
        if (!ram_pointer(sprite)) continue;
        const std::uint32_t layer = b.read32(sprite + 0x28);
        if (!ram_pointer(layer)) continue;
        if (b.read16(layer) == g.sprites[spot.element])
            show(g, b, layer, g.sprites[spot.element], g.sprites[given.element]);
    }
}

// Once a frame: dresses the map's Djinni and tells the Djinn Tracker what
// each spot gives and which spots are taken. A taken spot shows its planned
// Djinni: the pass-on to an owned target (given_for) cannot be told apart
// afterwards, and only a Djinni given outside the shuffle starts it.
inline void frame(const Game& g) {
    static std::uint32_t shown = 0xFFFFFFFFu;
    random_psynergy::Config c;
    const std::uint32_t seed = enabled(g, &c) ? c.seed : 0u;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (seed && g.actors && b) dress(g, *b, seed);
    for (int e = 0; e < 4; ++e) {
        std::uint32_t bits = 0;
        for (int i = 0; seed && b && i < 20; ++i) {
            const unsigned flag = kDjinnFlags + static_cast<unsigned>(e * 20 + i);
            bits |= static_cast<std::uint32_t>(gsr::read_flag(b->ewram_ptr(), flag)) << i;
        }
        djinn_tracker::spot_taken[static_cast<std::size_t>(e)].store(bits, std::memory_order_relaxed);
    }
    if (seed == shown) return;
    shown = seed;
    for (auto& v : djinn_tracker::gives) v.store(0, std::memory_order_relaxed);
    if (!seed) return;
    const std::vector<int>& target = targets(g, seed);
    for (int i = 0; i < g.count; ++i)
        djinn_tracker::gives[flag_of(g.djinn[i]) - kDjinnFlags]
            .store(static_cast<std::int16_t>(flag_of(g.djinn[target[static_cast<std::size_t>(i)]]) - kDjinnFlags + 1),
                   std::memory_order_relaxed);
}

}  // namespace gsr::random_djinn
