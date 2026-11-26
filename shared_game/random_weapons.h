// Random Equipment (first Random Weapons): one copy of the running rules for
// both games. The plan and the descriptions are in random_weapons_plan.h.
//
// A new game can tick "Random Equipment". The run's seed (run_code.h) and the
// Scaling answer then decide, for the whole file, every weapon's stat line
// and unleash, and every armour piece's stat line.
//
// The game reads its item records straight from the ROM, so:
//   - The plan is written into the running ROM image (the runtime's own copy
//     in memory, never the ROM file) whenever the loaded file's choice
//     differs from what is there.
//   - The pristine records come back for a file without it.
//   - Every reader (menus, shops, battles) then sees the same weapons.
//   - A weapon's description line is answered from the plan through each
//     game's text hook.
//
// The choice lives in eight save-payload bytes the game never writes
// (random_weapons_format.h), so it saves, copies and erases with the file,
// and save states carry it.
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "random_weapons_format.h"
#include "random_weapons_plan.h"
#include "gba_bus.h"
#include "host_config_ui.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif

namespace gsr::random_weapons {

// A game's measured places.
struct Game {
    std::uint32_t config;             // the eight payload bytes
    std::uint32_t item_table;         // ROM address of item 0
    unsigned first_item, last_item;   // the ids that can be weapons
    std::span<const std::uint16_t> excluded;  // weapons left as they are
    std::size_t text_table, tree_table;       // ROM offsets (gs_text_encode.h)
    std::uint32_t description_text;   // item description = this + id
    std::uint32_t ability_name_text;  // ability name = this + id
    std::uint32_t item_name_text;     // item name = this + id (the spoiler sheet)
    std::uint32_t glyph_widths;       // ROM address, 224 advances (glyph - 32)
    std::uint32_t records, record_size;  // character records
    std::uint32_t stats_pc;           // the game's stat rebuild (character)
    int heroes;                       // character ids 0..heroes-1
};

constexpr std::uint32_t kItems = 0xD8u;
constexpr unsigned kItemSlots = 15;
constexpr std::uint16_t kEquipped = 0x200u, kItemMask = 0x1FFu;

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

// Emulation thread only.
struct State {
    bool captured = false;
    std::vector<Weapon> pristine;
    Config applied;  // what the ROM image holds now
    Plan plan;
    std::vector<std::uint16_t> donor_prices;  // indexed by the item receiving the stat line
    unsigned revision = 0;
    std::map<std::uint16_t, std::string> text;  // description per weapon id
    // The description being handed out a character at a time.
    std::uint32_t text_id = 0;
    int text_pos = -1;
};
inline State state;

inline std::span<const std::uint8_t> rom_of(gba::GbaBus& b) {
    return {b.rom_ptr(), b.rom_size()};
}

// The ROM image the runtime loaded into memory (a non-const buffer it keeps
// for the whole run; the bus only hands out a const view of it).
inline std::uint8_t* rom_image(gba::GbaBus& b) { return const_cast<std::uint8_t*>(b.rom_ptr()); }

inline bool capture(const Game& g, gba::GbaBus& b) {
    if (state.captured) return true;
    const std::uint32_t table = g.item_table - 0x08000000u;
    if (table + (g.last_item + 1u) * kRecordSize > b.rom_size()) return false;
    state.pristine = weapons_of(rom_of(b).subspan(table), g.first_item, g.last_item, g.excluded);
    state.captured = true;
    return true;
}

// Writes the file's choice into the ROM image; true when anything changed.
inline bool apply(const Game& g, const Config& c, const LocationStrengths& locations,
                  bool treasure_changed) {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !b->rom_ptr() || !capture(g, *b)) return false;
    const std::uint32_t table = g.item_table - 0x08000000u;
    State& s = state;
    const bool uses_locations = c.seed && c.scaling && c.weapons;
    if (c.seed == s.applied.seed && c.scaling == s.applied.scaling && c.armour == s.applied.armour &&
        c.weapons == s.applied.weapons && !(uses_locations && treasure_changed))
        return false;
    static const LocationStrengths no_locations;
    s.plan = make_plan(s.pristine, c.seed, c.scaling, c.weapons, c.armour,
                       uses_locations ? locations : no_locations);
    std::uint8_t* rom = rom_image(*b);
    // The first two bytes hold the price, which Random Treasure owns.
    for (const Weapon& w : s.plan.weapons)
        std::copy(w.planned.begin() + kType, w.planned.end(), rom + table + w.id * kRecordSize + kType);
    s.applied = c;
    ++s.revision;
    s.donor_prices.clear();
    if (c.seed && !c.scaling && (c.weapons || c.armour)) {
        s.donor_prices.resize(g.last_item + 1u);
        for (const Weapon& w : s.plan.weapons) {
            if (w.donor == 0) continue;
            const auto donor = std::lower_bound(s.pristine.begin(), s.pristine.end(), w.donor,
                [](const Weapon& item, std::uint16_t id) { return item.id < id; });
            if (donor != s.pristine.end() && donor->id == w.donor)
                s.donor_prices[w.id] = static_cast<std::uint16_t>(donor->original[0] |
                                                               donor->original[1] << 8);
        }
    }
    s.text.clear();
    s.text_pos = -1;
    if (c.seed != 0) {
        const std::uint32_t widths = g.glyph_widths - 0x08000000u;
        const Texts t{g.text_table, g.tree_table, g.description_text, g.ability_name_text,
                      rom_of(*b).subspan(widths, 224)};
        s.text = descriptions(rom_of(*b), t, s.plan);
    }
    return true;
}

struct FairStrengths {
    std::span<const std::uint16_t> prices;
    unsigned revision = 0;
    bool equipment_changed = false;
};

// An unscaled stat shuffle needs no treasure plan, so it can be installed
// before treasure uses its donor prices to place equipment.
inline FairStrengths prepare_fair_strengths(const Game& g, const Config& c, bool fair_pooled) {
    if (!fair_pooled || !c.seed || c.scaling || (!c.weapons && !c.armour)) return {};
    const bool changed = apply(g, c, {}, false);
    if (state.donor_prices.empty()) return {};
    return {state.donor_prices, state.revision, changed};
}

// The installed plan for the spoiler sheet (emulation thread).
inline const Plan& applied_plan() { return state.plan; }

// After installing the final new-game plan, rebuild the stats of each hero
// wearing equipment. Preserve full HP and PP if a bonus changes the maximum.
inline void rebuild_equipped_stats(const Game& g, std::uint32_t hp, std::uint32_t max_hp,
                                   std::uint32_t pp, std::uint32_t max_pp) {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return;
    const std::uint32_t table = g.item_table;
    for (int id = 0; id < g.heroes; ++id) {
        const std::uint32_t r = g.records + static_cast<std::uint32_t>(id) * g.record_size;
        bool armed = false;
        for (unsigned slot = 0; slot < kItemSlots && !armed; ++slot) {
            const std::uint16_t w = b->read16(r + kItems + slot * 2u);
            const std::uint8_t type = b->read8(table + (w & kItemMask) * kRecordSize + kType);
            armed = (w & kEquipped) && (type == kWeapon || armour_type(type));
        }
        if (!armed) continue;
        const bool full = b->read16(r + hp) == b->read16(r + max_hp) &&
                          b->read16(r + pp) == b->read16(r + max_pp);
        std::uint32_t result = 0;
        gbarecomp::guest_call(g.stats_pc | 1u, static_cast<std::uint32_t>(id), 0, 0, 0, &result);
        if (full) {
            b->write16(r + hp, b->read16(r + max_hp));
            b->write16(r + pp, b->read16(r + max_pp));
        }
    }
}

// A weapon's description line, whole (The Lost Age's text hook).
inline bool line_text(const Game& g, std::uint32_t text_id, std::string& out) {
    if (text_id < g.description_text) return false;
    const auto it = state.text.find(static_cast<std::uint16_t>(text_id - g.description_text));
    if (it == state.text.end()) return false;
    out = it->second;
    return true;
}

// The same a character at a time (The Broken Seal's decoder hook): true with
// the next character, then the 0 that ends the line; a new line, or the same
// line again after its end, starts at the first character.
inline bool text_character(const Game& g, std::uint32_t text_id, std::uint32_t* character) {
    State& s = state;
    std::string line;
    if (!line_text(g, text_id, line)) {
        s.text_pos = -1;
        return false;
    }
    if (s.text_pos < 0 || text_id != s.text_id) {
        s.text_pos = 0;
        s.text_id = text_id;
    }
    const auto pos = static_cast<std::size_t>(s.text_pos);
    *character = pos < line.size() ? static_cast<unsigned char>(line[pos]) : 0u;
    s.text_pos = pos < line.size() ? s.text_pos + 1 : -1;
    return true;
}

// Savestate loaded: a description half handed out starts again.
inline void reset() { state.text_pos = -1; }

}  // namespace gsr::random_weapons
