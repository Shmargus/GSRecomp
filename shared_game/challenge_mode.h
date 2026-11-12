// Challenge Mode, one copy of the rules for both runners (docs/
// NEXT_PRIORITY_MODES.md "Challenge Mode", Jimmy's design of 4 November 2026).
//
// - Chosen per save file at New Game and locked for that file: Solo, Duo or
//   Triple (The Lost Age adds a four-character choice, No 2nd party).
// - The picks are the party from the first scene, whatever the story's join
//   order. Every other character is permanently dead the moment the story
//   adds them, and no revive can target them.
// - The first pick always leads the party (so the walking character, which
//   follows the leader by default, is the first pick).
//
// The choice lives in the game's own save payload, four bytes the game never
// writes (each game's address is measured, see its install), so it saves,
// loads, copies and erases with the file and save states carry it. The
// bytes: challenge_format.h.
#pragma once
#include <array>
#include <cstdint>

#include "challenge_format.h"
#include "gba_bus.h"
#include "runtime_bus_bridge.h"

namespace gsr::challenge {

// A game's measured places.
struct Game {
    std::uint32_t config;        // the four payload bytes
    std::uint32_t joined;        // flag bank: bit id of byte 0 = character id joined
    std::uint32_t order;         // party order bytes (count = joined characters)
    std::uint32_t records;       // character record 0
    std::uint32_t record_size;
    std::uint32_t hp_offset;     // current HP, u16; 0 = down
    std::uint32_t hp_share_offset; // HP as a share of max (0x4000 = full); battle reads it
    std::uint32_t field_context; // RAM word, nonzero on the field
    std::uint32_t battle_context;// RAM word, nonzero in battle
    int heroes;                  // character ids 0..heroes-1
    int max_party;               // members at most (Jimmy, 4 November: TBS 4)
    // A revive kind beyond the three both games share (TLA 0x49); 0 = none.
    std::uint32_t extra_revive_kind = 0;
    // The picks take the first places in the order, in pick order (TLA: the
    // first four fight, the rest wait behind). False: only the leader moves.
    bool picks_lead = false;
};

inline const Game* game = nullptr;
inline void install(const Game& g) { game = &g; }

inline gba::GbaBus* bus() { return gbarecomp::active_bus(); }
inline std::uint32_t record(unsigned id) { return game->records + id * game->record_size; }

inline Config current() {
    gba::GbaBus* b = bus();
    if (!game || !b) return {};
    std::uint8_t raw[4];
    for (int i = 0; i < 4; ++i) raw[i] = b->read8(game->config + i);
    return decode(raw);
}

inline void store(const Config& c) {
    gba::GbaBus* b = bus();
    if (!game || !b) return;
    const auto raw = encode(c);
    for (int i = 0; i < 4; ++i) b->write8(game->config + i, raw[i]);
}

inline bool picked(const Config& c, unsigned id) {
    for (int i = 0; i < c.size; ++i)
        if (c.picks[i] == id) return true;
    return false;
}

// A character no revive may reach: a hero the challenge left out.
inline bool excluded(unsigned id) {
    if (!game || id >= static_cast<unsigned>(game->heroes)) return false;
    const Config c = current();
    return c.size > 0 && !picked(c, id);
}

// Both games: effect kinds 5, 0x38 and 0x39 revive (TBS 0x08079EF8); The
// Lost Age adds 0x49 (0x080B0740).
inline bool revive_kind(std::uint32_t kind) {
    return kind == 5u || kind == 0x38u || kind == 0x39u ||
           (game && game->extra_revive_kind != 0 && kind == game->extra_revive_kind);
}

inline bool joined(gba::GbaBus& b, unsigned id) {
    return (b.read8(game->joined + id / 8u) >> (id & 7u)) & 1u;
}

inline int joined_count(gba::GbaBus& b) {
    int n = 0;
    for (unsigned id = 0; id < 8u; ++id) n += joined(b, id);
    return n;
}

// The game's join (TBS 0x0807961C): count first, set the flag, append the id
// to the order unless it is already there.
inline void join(gba::GbaBus& b, unsigned id) {
    const int count = joined_count(b);
    b.write8(game->joined + id / 8u,
             static_cast<std::uint8_t>(b.read8(game->joined + id / 8u) | (1u << (id & 7u))));
    for (int i = 0; i < count; ++i)
        if (b.read8(game->order + i) == id) return;
    b.write8(game->order + count, static_cast<std::uint8_t>(id));
}

// The game's leave (TBS 0x08079664): clear the flag, close the gap.
inline void leave(gba::GbaBus& b, unsigned id) {
    const int count = joined_count(b);
    b.write8(game->joined + id / 8u,
             static_cast<std::uint8_t>(b.read8(game->joined + id / 8u) & ~(1u << (id & 7u))));
    int at = 0;
    while (at < count && b.read8(game->order + at) != id) ++at;
    for (int i = at; i + 1 < count; ++i) b.write8(game->order + i, b.read8(game->order + i + 1));
}

// A Djinni's new owner. The game's grant (TBS 0x0807A0F4) gives it to the
// member with the fewest Djinn in total, first in party order on a tie,
// skipping anyone holding more than 9 of its element. Left-out members hold
// none, so every Djinni would go to the dead (F12 0089: Flint to Isaac, and
// the Djinn lesson battle never ends). The same rule over the picks only;
// `counts_offset` is the record's four per-element counts. -1: no pick can
// take it (the game's own choice stands).
inline int djinn_recipient(unsigned element, std::uint32_t counts_offset) {
    gba::GbaBus* b = bus();
    if (!game || !b || element > 3u) return -1;
    const Config c = current();
    if (c.size == 0) return -1;
    const int count = joined_count(*b);
    int best = -1, best_total = 0;
    for (int i = 0; i < count; ++i) {
        const unsigned id = b->read8(game->order + i);
        if (!picked(c, id)) continue;
        const std::uint32_t counts = record(id) + counts_offset;
        if (b->read8(counts + element) > 9u) continue;
        int total = 0;
        for (unsigned e = 0; e < 4u; ++e) total += b->read8(counts + e);
        if (best < 0 || total < best_total) best = static_cast<int>(id), best_total = total;
    }
    return best;
}

// The Djinn menu's give check (TBS 0x080AE778): after one Djinni moves from
// `from` to `to`, every two members' Djinn totals must be within one. The
// same rule over the picks only (Jimmy, 5 November): left-out heroes may
// take or hand over any number. `ids` and `totals` are the menu's members.
inline bool djinn_give_balanced(const unsigned* ids, const int* totals, int n,
                                int from, int to) {
    int t[8];
    if (n > 8) n = 8;
    for (int i = 0; i < n; ++i) t[i] = totals[i];
    if (from >= 0 && from < n) --t[from];
    if (to >= 0 && to < n) ++t[to];
    for (int i = 0; i < n; ++i) {
        if (excluded(ids[i])) continue;
        for (int j = i + 1; j < n; ++j) {
            if (excluded(ids[j])) continue;
            const int d = t[i] - t[j];
            if (d < -1 || d > 1) return false;
        }
    }
    return true;
}

// A story join refused: the party is full and the newcomer is not a pick
// (TBS: Jenna would join as a fifth member). The game's join returns the
// member count.
inline bool refuse_join(unsigned id, std::uint32_t* count_out) {
    gba::GbaBus* b = bus();
    if (!game || !b || id >= 8u) return false;
    const Config c = current();
    if (c.size == 0 || picked(c, id) || joined(*b, id)) return false;
    const int count = joined_count(*b);
    if (count < game->max_party) return false;
    *count_out = static_cast<std::uint32_t>(count);
    return true;
}

// A story revival that names its heroes (TBS 0x08077F70 wakes Isaac and
// Garet after the opening knocks the party out) wakes the picks instead:
// full HP and PP, shares full (as that routine leaves them). Max HP/PP
// offsets: +0x34/+0x36 TBS; current PP and its share follow HP's.
inline void wake_picks(std::uint32_t max_hp_offset, std::uint32_t max_pp_offset,
                       std::uint32_t pp_offset, std::uint32_t pp_share_offset) {
    gba::GbaBus* b = bus();
    if (!game || !b) return;
    const Config c = current();
    for (int i = 0; i < c.size; ++i) {
        const std::uint32_t r = record(c.picks[i]);
        b->write16(r + game->hp_offset, b->read16(r + max_hp_offset));
        b->write16(r + pp_offset, b->read16(r + max_pp_offset));
        b->write16(r + game->hp_share_offset, 0x4000u);
        b->write16(r + pp_share_offset, 0x4000u);
    }
}

// A left-out hero the story needs alive for now (TBS: Isaac in the Djinn
// lesson); -1 none. Set by the game each frame before frame().
inline int spared = -1;

// The left-out members down now. Also called by each game just before battle
// lists who is alive, so a heal earlier in the same frame cannot count.
inline void keep_down(gba::GbaBus& b, const Config& c) {
    for (unsigned id = 0; id < static_cast<unsigned>(game->heroes); ++id) {
        if (picked(c, id) || !joined(b, id) || static_cast<int>(id) == spared) continue;
        const std::uint32_t r = record(id);
        if (b.read16(r + game->hp_offset) != 0) b.write16(r + game->hp_offset, 0);
        if (b.read16(r + game->hp_share_offset) != 0) b.write16(r + game->hp_share_offset, 0);
    }
}
inline void keep_down() {
    gba::GbaBus* b = bus();
    if (!game || !b) return;
    const Config c = current();
    if (c.size > 0) keep_down(*b, c);
}

// The battle's party list without the left-out heroes (Jimmy, 12 November:
// hidden in battle only). The game's builder (TBS 0x080B6A60) takes the first
// `cap` members in party order, writes their ids as u16s to `out` (if not 0)
// with an 0xFF end, marks each record's `battle_mark` byte 2 and returns the
// count. The same over the members who fight: picks, and a spared hero.
// The Lost Age's waiting list (0x0811A0B0) is the members after the first
// four, none when 3 may fight: `skip` 4 there.
inline std::uint32_t battle_party(gba::GbaBus& b, const Config& c, std::uint32_t out, int cap,
                                  std::uint32_t battle_mark, int skip = 0) {
    const int count = joined_count(b);
    int n = 0, fighters = 0;
    for (int i = 0; i < count && n < cap; ++i) {
        const unsigned id = b.read8(game->order + i);
        if (id < static_cast<unsigned>(game->heroes) && !picked(c, id) &&
            static_cast<int>(id) != spared) continue;
        if (fighters++ < skip) continue;
        if (out) { b.write16(out, static_cast<std::uint16_t>(id)); out += 2; }
        b.write8(record(id) + battle_mark, 2);
        ++n;
    }
    if (out) b.write16(out, 0xFFu);
    return static_cast<std::uint32_t>(n);
}

// Once a game frame. Every left-out member who is in the party stays down,
// as the game's own HP setter leaves them (TBS 0x080782A0): HP 0 and HP share
// 0 (story joins, inns and full heals included). On the field, outside
// battle, the picks join and the first pick leads. The F1 HP/PP cheats are
// held off in a challenge (they would revive the left-out members).
inline void frame() {
    gba::GbaBus* b = bus();
    if (!game || !b) return;
    const Config c = current();
    gbarecomp::set_cheats_blocked(c.size > 0);
    if (c.size == 0) return;
    keep_down(*b, c);
    if (b->read32(game->field_context) == 0 || b->read32(game->battle_context) != 0) return;
    for (int i = 0; i < c.size; ++i)
        if (!joined(*b, c.picks[i])) join(*b, c.picks[i]);
    // Over the limit (a file from before the join rule): guests outside the
    // heroes leave first.
    for (unsigned id = 7; joined_count(*b) > game->max_party && id >= static_cast<unsigned>(game->heroes); --id)
        if (joined(*b, id) && !picked(c, id)) leave(*b, id);
    const int count = joined_count(*b);
    // Pick p to place `to`: the members between move one place back.
    const auto place = [&](std::uint8_t p, int to) {
        for (int i = to; i < count; ++i) {
            if (b->read8(game->order + i) != p) continue;
            for (int j = i; j > to; --j) b->write8(game->order + j, b->read8(game->order + j - 1));
            b->write8(game->order + to, p);
            return;
        }
    };
    if (b->read8(game->order) != c.picks[0]) place(c.picks[0], 0);
    if (!game->picks_lead) return;
    for (int i = 1; i < c.size && i < count; ++i)
        if (b->read8(game->order + i) != c.picks[i]) place(c.picks[i], i);
}

// Level 1 for the picks (Jimmy, 4 November). A new game builds each hero at
// level 0 and raises it to its story level (TBS: Isaac and Garet 1, Ivan 4,
// Mia 10) before the questions are asked. Each game keeps the level-0 record
// from just before that raise; once the picks are known, a pick above level
// 1 gets it back and the game raises it to level 1 the same way.
struct StartRecords {
    std::array<std::array<std::uint8_t, 0x200>, 8> bytes{};
    std::array<bool, 8> kept{};
};
inline StartRecords g_start;

inline void keep_start_record(unsigned id) {
    gba::GbaBus* b = bus();
    if (!game || !b || id >= 8u || game->record_size > 0x200u) return;
    for (std::uint32_t i = 0; i < game->record_size; ++i) g_start.bytes[id][i] = b->read8(record(id) + i);
    g_start.kept[id] = true;
}

// Puts the kept level-0 record back for each pick above `start_level` (TBS
// 1; TLA Felix's level); returns the ids changed (bit per id) for the game to
// raise and recalculate. Bytes before `level_offset` (the name) stay as they
// are.
inline unsigned restore_start_records(const Config& c, std::uint32_t level_offset,
                                      unsigned start_level = 1) {
    gba::GbaBus* b = bus();
    if (!game || !b) return 0;
    unsigned changed = 0;
    for (int i = 0; i < c.size; ++i) {
        const unsigned id = c.picks[i];
        if (id >= 8u || !g_start.kept[id]) continue;
        const std::uint32_t r = record(id);
        if (b->read8(r + level_offset) <= start_level) continue;
        for (std::uint32_t k = level_offset; k < game->record_size; ++k)
            b->write8(r + k, g_start.bytes[id][k]);
        changed |= 1u << id;
    }
    g_start = {};
    return changed;
}

// New-game questions: the size, then the picks one by one.
struct Prompt {
    int size = 0;     // picks wanted
    Config chosen;    // picks so far in chosen.picks, count in chosen.size
};

}  // namespace gsr::challenge
