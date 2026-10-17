#pragma once

// Where each object record was last placed, by record address, for the
// expanded view's sprite positions. Shared by both games; each game records
// at its own character builders and asks from its OAM attribute providers.
//
// The console's object Y is 8 bits and X 9: once objects are placed outside
// its screen any raw value can mean either side. So each builder pass
// records the object's real X and Y by its sprite record (attr0/1/2 at
// +4/+6/+8), and the providers get them back from the record that holds
// that raw position, or placed it there in the last few frames.
//
// Measured in The Lost Age (F12 0015, 2026-10-05: a creature at row ~170
// shown on one frame and gone for the next 50): a lookup that expires after
// two frames or also needs the tile word fails whenever the game leaves an
// object's record alone for a while or animates it after placing it, and the
// object then falls back to the hardware's reading, above the screen.
//
// Each record keeps its last few placements, not only the newest: the scene
// is captured at VBlank start, before the game's VBlank copies this frame's
// objects to OAM, so OAM can still hold the previous placement of an object
// that has since moved (TLA F12 0023-0025: walking NPCs at the bottom edge
// flickered and jumped).
//
// Each placement also keeps the record's sprite words as they were then: a
// world-map object the game unloads has its record zeroed while OAM still
// shows its last placement for one frame (TLA F12 tla_rewind_0009,
// 2026-10-14: a town blipping in at the bottom).

#include <array>
#include <cstdint>

#include "gba_bus.h"

namespace gbarecomp {

class ObjPlacements {
public:
    struct Placement {
        unsigned long long frame = 0;
        std::uint16_t raw_y = 0, raw_x = 0;
        std::int16_t x = 0, y = 0;
        std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
        std::uint32_t record = 0;  // the sprite record that was placed
        // The game's full tile number for the record when it was placed, or
        // -1: OAM shows a placement a frame after the game may have
        // released its tiles (TLA F12 tla_rewind_0047 frame 105).
        int tile = -1;
    };

    // Forgets every placement when the frame goes back (a loaded state) or
    // the scene changes; the game names its scene with three words.
    void sync(unsigned long long now, std::uint32_t a, std::uint32_t b,
              std::uint32_t c) {
        if (now < frame_ || a != key_[0] || b != key_[1] || c != key_[2])
            placed_ = {};
        frame_ = now;
        key_ = {a, b, c};
    }

    // A hook before the game's attribute store can supply its outgoing attr0;
    // otherwise snapshot the record as it stands.
    void note(gba::GbaBus& bus, unsigned long long now, std::uint32_t record,
              int x, int y, int tile = -1, int placed_attr0 = -1) {
        if ((record >> 24) != 0x02u && (record >> 24) != 0x03u) return;
        Placed* slot = nullptr;
        for (Placed& p : placed_) {
            if (p.record == record) { slot = &p; break; }
            if (!slot || p.frame < slot->frame) slot = &p;  // else the oldest
        }
        if (slot->record != record) *slot = Placed{};
        slot->record = record;
        slot->frame = now;
        Placement& p = slot->recent[slot->next];
        slot->next = (slot->next + 1) % kHistory;
        p.frame = now;
        p.raw_y = static_cast<std::uint16_t>(y & 0xFF);
        p.raw_x = static_cast<std::uint16_t>(x & 0x1FF);
        p.x = static_cast<std::int16_t>(x);
        p.y = static_cast<std::int16_t>(y);
        p.attr0 = placed_attr0 >= 0 ? static_cast<std::uint16_t>(placed_attr0)
                                   : bus.read16(record + 4u);
        p.attr1 = bus.read16(record + 6u);
        p.attr2 = bus.read16(record + 8u);
        p.record = record;
        p.tile = tile;
    }

    // The placement an OAM entry came from, or null. A record answers if it
    // still holds this raw position (an object the game has not moved
    // since), or if it was placed there in the last few frames (OAM lags the
    // records by one). An older placement of a record that has since moved
    // on is no answer: a long-gone bottom-margin placement of another object
    // answered for an NPC just above the view and drew it in the bottom
    // margin (TLA, Jimmy, 2026-10-05).
    const Placement* find(gba::GbaBus& bus, unsigned long long now,
                          std::uint16_t attr0, std::uint16_t attr1,
                          std::uint16_t attr2) const {
        const std::uint16_t raw_y = attr0 & 0xFFu, raw_x = attr1 & 0x1FFu;
        constexpr unsigned long long kRecent = 3;
        // Shape/size, rendering mode and palette/priority distinguish bodies,
        // shadows and unrelated records sharing the same wrapped
        // coordinates. The animated tile and OAM-assigned affine matrix
        // index may change.
        const auto same_kind = [&](std::uint16_t a0, std::uint16_t a1,
                                   std::uint16_t a2) {
            return ((a0 ^ attr0) & 0xFD00u) == 0u &&
                   ((a1 ^ attr1) & 0xC000u) == 0u &&
                   ((a2 ^ attr2) & 0xFC00u) == 0u;
        };
        // Two records can share one raw position and kind: a sand burst
        // stacks two puffs on the same spot (TLA F12 tla_rewind_0055 frame
        // 31, OAM 7 and 10), and puffs rise from one spawn point, so a new
        // puff sits now where OAM still shows last frame's older one
        // (tla_rewind_0056 frame 93: OAM 20 at raw (0x67, 0x70), tile 0x30,
        // record 0x03001648 placed there at frame 92). The record whose tile
        // is the entry's wins first, then a record still there, then the
        // newest placement, so each entry gets its own record back.
        const auto same_tile = [&](std::uint16_t a2) {
            return ((a2 ^ attr2) & 0x03FFu) == 0u;
        };
        const Placement* best = nullptr;
        bool best_live = false, best_tile = false;
        for (const Placed& r : placed_) {
            if (r.record == 0u || r.frame + kLifetime < now) continue;
            const bool record_kind = same_kind(bus.read16(r.record + 4u),
                                               bus.read16(r.record + 6u),
                                               bus.read16(r.record + 8u));
            const bool live = record_kind && bus.read8(r.record + 4u) == raw_y &&
                              (bus.read16(r.record + 6u) & 0x1FFu) == raw_x;
            for (const Placement& p : r.recent) {
                if (p.frame == 0u || p.raw_y != raw_y || p.raw_x != raw_x)
                    continue;
                if (!live && p.frame + kRecent < now) continue;
                // A record cleared since (an unloaded object) answers only
                // for placements whose own sprite words were this kind.
                if (!record_kind && !same_kind(p.attr0, p.attr1, p.attr2))
                    continue;
                const bool tile = same_tile(live ? bus.read16(r.record + 8u) : p.attr2);
                const bool better = !best || (tile && !best_tile) ||
                                    (tile == best_tile && live && !best_live) ||
                                    (tile == best_tile && live == best_live &&
                                     p.frame > best->frame);
                if (better) {
                    best = &p;
                    best_live = live;
                    best_tile = tile;
                }
            }
        }
        return best;
    }

private:
    static constexpr int kHistory = 4;
    static constexpr unsigned long long kLifetime = 600;  // frames
    struct Placed {
        std::uint32_t record = 0;
        unsigned long long frame = 0;  // newest placement
        std::array<Placement, kHistory> recent{};
        int next = 0;
    };
    std::array<Placed, 512> placed_{};
    unsigned long long frame_ = 0;
    std::array<std::uint32_t, 3> key_{};
};

}  // namespace gbarecomp
