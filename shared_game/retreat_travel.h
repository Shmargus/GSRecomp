// Overworld Retreat: one visited-town picker for both games. Each runner
// supplies measured native destinations and its own room-transition data.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "flag_bank.h"
#include "game_options.h"
#include "gba_bus.h"
#include "menu_text.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tracker_text.h"

namespace gsr::retreat_travel {
struct Cast {
    std::uint32_t flag, argument, message, close, confirm, ability, pp_return, effect;
    unsigned stack, effect_stack;
};
struct Destination {
    const char* name;
    unsigned flag, route;
    std::uint16_t room, entrance;
    unsigned mooring = 0;
    bool debug = false;  // F1 > Cheats "Debug rooms in Retreat"
};
// A leftover test room, named as the game's own location table names it.
// Every one has a single entrance, 0 (each game's FACTS.md, "Debug rooms in
// Retreat"), so travel asks for that.
struct DebugRoom { const char* name; std::uint16_t room; };
// Where a debug-room warp left the world map, so Retreat inside the room
// puts the party back on that exact spot. Both games keep a scene snapshot
// (what the field Save stores) and a saved spot (x, y, z, facing; 32-bit
// each); with flag 0x109 set, the next room load restores the snapshot and
// skips the door's spawn, then clears the flag (each game's FACTS.md,
// "Debug rooms in Retreat").
struct Spot {
    std::uint32_t snapshot;      // guest routine that fills the snapshot
    std::uint32_t begin, end;    // the snapshot's EWRAM bytes
    std::uint32_t fields;        // the saved spot
    std::uint32_t ship;          // halfword "party on the ship", 0 if none
    std::uint32_t (*actor)(gba::GbaBus*);  // the character the player moves
};
constexpr unsigned kRestoreSceneFlag = 0x109u;
struct Game {
    std::uint32_t dispatcher, caller_return, frame, sound;
    std::uint32_t map_number, room, state_slot, party;
    unsigned exit_offset, cost;
    bool (*enabled)();
    std::vector<Destination> (*destinations)();
    bool (*arrive)(const Destination&);
    void (*prepare_font)() = nullptr;
    // Optional complete native travel coroutine. TBS keeps its working resolver.
    std::uint32_t native_arrival = 0;
    unsigned native_cost = 0;
    const Cast* cast = nullptr;
    bool (*valid_route)(unsigned) = nullptr;
    std::span<const DebugRoom> debug_rooms{};
    // Native Retreat's return map/entrance, copied to `room` on its exit.
    std::uint32_t return_point = 0;
    const Spot* spot = nullptr;
};
inline const Game* game = nullptr;
inline std::vector<Destination> destinations;
inline bool showing = false, waiting = false, swallowing = false, busy = false;
inline std::uint16_t held = 0;
inline std::uint32_t hold_sp = 0;
inline int cursor = 0, chosen = -1;
inline std::vector<unsigned> sounds;
// Kept across state loads: the last departure is still a real world spot.
struct Departure {
    bool valid = false;
    std::uint16_t entrance = 0, ship = 0;
    std::array<std::uint32_t, 4> spot{};
    std::vector<std::uint8_t> scene;
};
inline Departure departure;
inline bool debug_retreat = false;  // Retreat confirmed inside a debug room

// This uses the already measured Custom Battle list geometry: six 12-pixel
// rows inside a 28x15-tile native window. Long names are fitted by glyph width.
constexpr int kTileX = 1, kTileY = 2, kTiles = 28, kRows = 15, kVisible = 6;
constexpr unsigned kA = 1, kB = 2, kStart = 8, kRight = 16, kLeft = 32;
constexpr unsigned kUp = 64, kDown = 128, kR = 256, kL = 512;
constexpr unsigned kMove = 0x6F, kSelect = 0x70, kBack = 0x71;

// A real, balanced guest stack frame keeps the continuation in savestates.
// Native frame work uses the stack BELOW this record. A loaded picker state
// cancels at its next frame entry and restores the saved caller/stack; it
// cannot leave the game looping in a host-only continuation.
using Hold = std::array<std::uint32_t, 8>;
constexpr std::uint32_t kHoldTag = 0x54525452u; // "RTRT"
constexpr unsigned kHoldBytes = sizeof(Hold);
inline bool stack_address(std::uint32_t sp) {
    return (sp & 3u) == 0 && sp >= 0x03007000u && sp <= 0x03007F00u - kHoldBytes;
}
inline bool field_address(std::uint32_t p) {
    return (p & 3u) == 0 && p >= 0x02000000u && p < 0x02040000u - 0x170u;
}
inline bool read_hold_at(gba::GbaBus* b, Hold& record, std::uint32_t sp) {
    // The native frame restores its return PC from the stack, NOT into LR.
    // LR still holds an internal call's return on the next frame entry.
    // Identify our continuation by its complete owned stack record instead.
    if (!stack_address(sp)) return false;
    for (unsigned i = 0; i < record.size(); ++i) record[i] = b->read32(sp + i * 4u);
    const auto selected_route = (record[5] & 0x7FFFFFFFu) >> 2;
    const bool native_route = game->native_arrival && (record[5] & 0x80000000u) &&
        (game->valid_route ? game->valid_route(selected_route) :
            (selected_route >= 0x08000000u &&
             std::uint64_t(selected_route - 0x08000000u) + 20u <= b->rom_size()));
    return record[0] == kHoldTag && record[1] == ~kHoldTag &&
        record[2] == sp + kHoldBytes && record[3] == game->caller_return &&
        field_address(record[4]) && (record[5] == 2u ||
            native_route) &&
        (record[6] & 0xFFFFu) < 8u &&
        record[7] == game->dispatcher;
}
inline unsigned caster(const Hold& h) { return h[6] & 0xFFFFu; }
inline unsigned phase(const Hold& h) { return h[5] & 3u; }
inline unsigned route(const Hold& h) { return (h[5] & 0x7FFFFFFFu) >> 2; }
inline void set_phase(gba::GbaBus* b, std::uint32_t sp, const Hold& h, unsigned p) {
    b->write32(sp + 20u, (h[5] & ~3u) | p);
}
inline bool read_hold(gba::GbaBus* b, Hold& record) {
    return read_hold_at(b, record, g_cpu.R[13]);
}
inline bool visited(const Destination& d) {
    auto* b = gbarecomp::active_bus();
    return d.debug || (b && gsr::read_flag(b->ewram_ptr(), d.flag));
}
inline bool in_debug_room(gba::GbaBus* b) {
    const auto room = b->read16(game->room);
    return std::any_of(game->debug_rooms.begin(), game->debug_rooms.end(),
        [=](const auto& r) { return r.room == room; });
}
inline bool world(gba::GbaBus* b) {
    return game->enabled() && b->read16(game->map_number) == 2u &&
        b->read16(game->room) == 2u && field_address(b->read32(game->state_slot));
}
inline std::uint32_t pp_address(unsigned caster) {
    // Both original unit getters use 0x14C-byte records; both dispatchers
    // compare/deduct current PP at +0x3A. The record bases differ by game.
    return game->party + caster * 0x14Cu + 0x3Au;
}
inline void reset() {
    showing = waiting = swallowing = busy = debug_retreat = false;
    held = 0; hold_sp = 0; cursor = 0; chosen = -1;
    destinations.clear(); sounds.clear();
}
inline void install(const Game& g) { game = &g; reset(); }
inline bool call(std::uint32_t pc, std::uint32_t r0, std::uint32_t r1,
                 std::uint32_t* result = nullptr) {
    const bool prior = busy; busy = true;
    std::uint32_t value = 0;
    const bool ok = gbarecomp::guest_call(pc | 1u, r0, r1, 0, 0, &value);
    busy = prior;
    if (result) *result = value;
    return ok;
}

inline void remember_departure(gba::GbaBus* b) {
    departure.valid = false;
    const auto* s = game->spot;
    if (!s || !call(s->snapshot, 0u, 0u)) return;
    const auto actor = s->actor(b);
    if (!field_address(actor)) return;
    departure.spot = {b->read32(actor + 8u), b->read32(actor + 12u),
                      b->read32(actor + 16u), b->read16(actor + 6u)};
    departure.entrance = b->read16(game->room + 2u);
    departure.ship = s->ship ? b->read16(s->ship) : 0u;
    departure.scene.resize(s->end - s->begin);
    for (std::size_t i = 0; i < departure.scene.size(); ++i)
        departure.scene[i] = b->read8(s->begin + static_cast<std::uint32_t>(i));
    departure.valid = true;
}
inline void restore_departure(gba::GbaBus* b) {
    const auto* s = game->spot;
    for (std::size_t i = 0; i < departure.scene.size(); ++i)
        b->write8(s->begin + static_cast<std::uint32_t>(i), departure.scene[i]);
    for (unsigned i = 0; i < departure.spot.size(); ++i)
        b->write32(s->fields + i * 4u, departure.spot[i]);
    if (s->ship) b->write16(s->ship, departure.ship);
    constexpr auto at = 0x02000000u + gsr::kFlagBank + (kRestoreSceneFlag >> 3);
    b->write8(at, static_cast<std::uint8_t>(b->read8(at) | (1u << (kRestoreSceneFlag & 7u))));
}
// Run the original Retreat dispatcher, including its actual effect and cleanup.
// Only its already answered dungeon prompt/restriction and early dungeon exit
// are replaced. Every hook requires our complete owned guest frame.
inline bool cast_entry(std::uint32_t pc) {
    if (!game || !game->cast || busy) return false;
    const auto& c = *game->cast;
    if (pc != game->dispatcher && pc != c.effect && pc != c.flag &&
        pc != c.argument && pc != c.message && pc != c.close &&
        pc != c.confirm && pc != c.ability) return false;
    auto* b = gbarecomp::active_bus();
    if (!b) return false;
    // Retreat inside a debug room. Those rooms keep the world map's flag
    // 0x144 (Retreat refused here), so answer it clear for the dispatcher's
    // own check. Entering a room makes it its own Retreat point, so point
    // native Retreat at the departure's world entrance; once confirmed, the
    // frame hook in on_entry restores the exact spot. Without a departure
    // (game started inside the room) the game's own refusal stands.
    if (pc == c.flag && g_cpu.R[0] == 0x144u && departure.valid && in_debug_room(b) &&
        g_cpu.R[14] > game->dispatcher && g_cpu.R[14] < game->dispatcher + 0x200u) {
        b->write16(game->return_point, 2u);
        b->write16(game->return_point + 2u, departure.entrance);
        debug_retreat = true;
        gbarecomp::guest_return_now(pc, 0u);
        return true;
    }
    Hold h{};
    if (pc == game->dispatcher && read_hold(b, h) && h[5] != 2u && phase(h) == 0u) {
        set_phase(b, g_cpu.R[13], h, 1u);
        g_cpu.R[0] = 0x95u | (caster(h) << 10);
        g_cpu.R[14] = game->frame | 1u;
    } else if (pc == c.effect && read_hold_at(b, h, g_cpu.R[13] + c.effect_stack) &&
               h[5] != 2u && phase(h) == 1u) {
        set_phase(b, g_cpu.R[13] + c.effect_stack, h, 3u);
    } else if (read_hold_at(b, h, g_cpu.R[13] + c.stack) && h[5] != 2u && phase(h) == 1u) {
        if ((pc == c.flag && g_cpu.R[0] == 0x144u) || pc == c.argument ||
            pc == c.message || pc == c.close || pc == c.confirm) {
            gbarecomp::guest_return_now(pc, 0u);
            return true;
        }
        if (pc == c.ability && g_cpu.R[14] == c.pp_return) {
            // Native Retreat has just copied its dungeon return. Keep the
            // current world loaded until the effect finishes, then travel.
            b->write32(game->room, 2u | (h[6] & 0xFFFF0000u));
            b->write16(h[4] + game->exit_offset, 0u);
        }
    }
    return false;
}

// True when this entry has already been returned by the picker.
inline bool on_entry(std::uint32_t pc) {
    if (!game || busy || (pc != game->frame && pc != game->dispatcher)) return false;
    auto* b = gbarecomp::active_bus();
    if (!b) return false;
    // Native Retreat has copied the world return into the current room:
    // the confirmation was answered, and the field reload comes next.
    if (debug_retreat && pc == game->frame && departure.valid &&
        b->read16(game->room) == 2u && b->read16(game->room + 2u) == departure.entrance) {
        debug_retreat = false;
        restore_departure(b);
    }
    Hold record{};
    if (pc == game->frame && read_hold(b, record)) {
        for (const auto sound : sounds) call(game->sound, sound, 0);
        sounds.clear();
        const bool live = waiting && hold_sp == g_cpu.R[13] && world(b) &&
            b->read32(game->state_slot) == record[4];
        if (live && showing) {
            g_cpu.R[0] = 1u;
            g_cpu.R[14] = game->frame | 1u;
            return false;
        }
        if (record[5] == 2u && live && chosen >= 0 && chosen < static_cast<int>(destinations.size())) {
            const auto& d = destinations[chosen];
            const auto pp = pp_address(caster(record));
            // Before arrive(): The Lost Age's puts the party on foot.
            if (d.debug && visited(d)) remember_departure(b);
            if (visited(d) && b->read16(pp) >= game->cost && game->arrive(d)) {
                if (d.debug) {
                    // A cheat: no PP and no Retreat effect. The room request
                    // native Retreat writes, followed by its exit below.
                    b->write16(game->room, d.room);
                    b->write16(game->room + 2u, d.entrance);
                } else {
                    b->write16(pp, static_cast<std::uint16_t>(b->read16(pp) -
                        (game->cost - game->native_cost)));
                    if (game->native_arrival) {
                        // Persist the selected native row on the guest stack, so a
                        // state loaded during travel finishes without host state
                        // or a second PP charge. The native function owns the exit.
                        b->write32(g_cpu.R[13] + 5u * 4u, 0x80000000u | (d.route << 2));
                        showing = waiting = false; chosen = -1; hold_sp = 0;
                        g_cpu.R[14] = game->native_arrival | 1u;
                        gbarecomp::guest_return_now(pc, 1u);
                        return true;
                    }
                }
                // The very same exit request made by native dungeon Retreat.
                b->write16(record[4] + game->exit_offset, 999u);
            }
        }
        const auto sp = g_cpu.R[13];
        for (unsigned i = 0; i < record.size(); ++i) b->write32(sp + i * 4u, 0);
        g_cpu.R[13] = record[2];
        g_cpu.R[14] = record[3];
        showing = waiting = false; chosen = -1; hold_sp = 0;
        gbarecomp::guest_return_now(pc, 0);
        return true;
    }
    if (pc != game->dispatcher || waiting || g_cpu.R[14] != game->caller_return ||
        (g_cpu.R[0] & 0x3FFu) != 0x95u || !world(b)) return false;
    const unsigned caster = (g_cpu.R[0] >> 10) & 15u;
    const auto sp = g_cpu.R[13];
    if (caster >= 8u || sp < kHoldBytes || !stack_address(sp - kHoldBytes) ||
        b->read16(pp_address(caster)) < game->cost) return false;
    busy = true;
    destinations = game->destinations();
    busy = false;
    destinations.erase(std::remove_if(destinations.begin(), destinations.end(),
        [](const auto& d) { return !visited(d); }), destinations.end());
    if (gbarecomp::cheat_option_get(gbarecomp::kOptionDebugRooms, 0))
        for (const auto& r : game->debug_rooms)
            destinations.push_back({r.name, 0u, 0u, r.room, 0u, 0u, true});
    record = {kHoldTag, ~kHoldTag, sp, game->caller_return,
              b->read32(game->state_slot), 2u,
              caster | (b->read32(game->room) & 0xFFFF0000u), game->dispatcher};
    hold_sp = sp - kHoldBytes;
    for (unsigned i = 0; i < record.size(); ++i) b->write32(hold_sp + i * 4u, record[i]);
    cursor = 0; chosen = -1; held = 0x3FFu;
    showing = waiting = true;
    if (game->prepare_font) game->prepare_font();
    g_cpu.R[13] = hold_sp;
    g_cpu.R[14] = game->frame | 1u;
    gbarecomp::guest_return_now(pc, 1u);
    return true;
}

inline std::uint16_t keyinput(std::uint16_t value) {
    const auto pressed = static_cast<std::uint16_t>(~value & 0x3FFu);
    if (showing) {
        const unsigned down = pressed & ~held;
        held = pressed;
        if (down & (kB | kStart)) {
            showing = false; swallowing = true; sounds.push_back(kBack);
        } else if (down & kA) {
            chosen = destinations.empty() ? -1 : cursor;
            showing = false; swallowing = true;
            sounds.push_back(chosen < 0 ? kBack : kSelect);
        } else if (!destinations.empty()) {
            // Up/Down wrap round the list; a page jump stops at either end,
            // as in the Custom Battle list.
            const int count = static_cast<int>(destinations.size());
            const int before = cursor;
            if (down & kUp) cursor = (cursor + count - 1) % count;
            else if (down & kDown) cursor = (cursor + 1) % count;
            else if (down & (kLeft | kL)) cursor = std::max(0, cursor - kVisible);
            else if (down & (kRight | kR)) cursor = std::min(count - 1, cursor + kVisible);
            if (cursor != before) sounds.push_back(kMove);
        }
        return 0x3FFu;
    }
    if (swallowing) {
        if (pressed) return 0x3FFu;
        swallowing = false;
    }
    return value;
}

template<class Draw, class Width>
inline void paint(Draw& draw, Width& width, int x, int y) {
    constexpr int inner_width = (kTiles - 2) * 8;
    const auto fit = [&](std::string s, int available) {
        if (width(s) <= available) return s;
        while (!s.empty() && width(s + "...") > available) s.pop_back();
        return s + "...";
    };
    const auto title = gsr::menu_text("Travel to");
    draw(title, x + (inner_width - width(title)) / 2, y);
    if (destinations.empty()) {
        draw(fit(gsr::menu_text("No visited towns."), inner_width - 16), x + 8, y + 24);
    } else {
        const int first = (cursor / kVisible) * kVisible;
        for (int row = 0; row < kVisible && first + row < int(destinations.size()); ++row) {
            const auto& d = destinations[first + row];
            const auto name = gsr::tracker_text::name(d.name);
            const auto label = std::string(cursor == first + row ? "> " : "  ") +
                gsr::menu_text(name.c_str());
            draw(fit(label, inner_width - 16), x + 8, y + 14 + row * 12);
        }
        const auto page = std::to_string(first / kVisible + 1) + "/" +
            std::to_string((destinations.size() + kVisible - 1) / kVisible);
        draw(page, x + inner_width - width(page) - 8, y);
    }
    const auto footer = fit(gsr::menu_text("A: travel  B: cancel"), inner_width - 16);
    draw(footer, x + 8, y + (kRows - 2) * 8 - 8);
}
} // namespace gsr::retreat_travel
