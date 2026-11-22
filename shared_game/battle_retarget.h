#pragma once

#include <cstdint>
#include <string>

#include "quality_of_life_menu.h"
#include "game_options.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

// Retarget, both games (Quality of Life menu, off by default): when every
// fighter a move was aimed at has fallen before it runs, the move goes to a
// random living fighter on that side instead of the turn being lost.
//
// The game's rule (ROM, both SHA-1 verified; FACTS.md "Battle retarget"):
// one target-list builder per game, GS1 0x080BE18C, TLA 0x08123648, called
// only from that game's move resolver (GS1 0x080BE378, TLA 0x0812381C; six
// calls each) with r0 = the ability and sb = the resolver's frame. It reads
// the command and the battle state through that frame (GS1: command
// [sb-0x10], state [sb-0xC]; TLA swapped). Command +0 s16 actor (party 0..7,
// enemies 0x80..), +0xA u16 selection (low 4 bits the chosen position, 0x80
// the enemy list), +0xC s16 reach. Lists: state +0x58 party, +0x66 enemies,
// s16 ids, 0xFF ends, 0xFE an empty place. Positions centre-reach+1 ..
// centre+reach-1 are kept unless empty or at 0 HP (unit +0x38; revive moves
// skip the HP test). None kept: message, the actor's turn ends (-1).
//
// The hook only ever moves the chosen position, and only for moves aimed
// at the actor's opposing side, so heals and revives aimed at allies stay
// native; the builder then runs unchanged on the new position.
namespace gsr::battle_retarget {

struct Game {
    std::uint32_t builder_pc;
    std::int32_t command_in_frame, state_in_frame;
    std::uint32_t party_records;      // party unit n: party_records + n * 0x14C
    std::uint32_t enemy_records_slot; // enemy unit: [slot] + (id - 0x80) * 0x14C
};

constexpr const char* kOptionKey = "BattleRetarget";
enum Mode { kOff = 0, kParty = 1, kEveryone = 2, kModeCount = 3 };
constexpr const char* kModeWords[kModeCount] = {"Off", "Party", "Everyone"};

constexpr std::uint32_t kUnitStride = 0x14Cu, kHp = 0x38u;
constexpr std::uint32_t kPartyList = 0x58u, kEnemyList = 0x66u;
constexpr std::uint32_t kSelection = 0xAu, kReach = 0xCu;
constexpr int kListMax = 8;

inline const Game* game = nullptr;
inline void install(const Game& profile) { game = &profile; }

inline qol_menu::Row qol_row() {
    return {"Retarget", "If a move's target has already fallen, hit another at random.",
        [] {
            const int v = gbarecomp::game_option_get(kOptionKey, kOff);
            return std::string(kModeWords[v >= 0 && v < kModeCount ? v : 0]);
        },
        [](int direction) {
            int v = gbarecomp::game_option_get(kOptionKey, kOff);
            v = ((v >= 0 && v < kModeCount ? v : 0) + kModeCount + direction) % kModeCount;
            gbarecomp::game_option_set(kOptionKey, v);
        }};
}

// Function entries, emulation thread.
inline void on_entry(std::uint32_t pc) {
    if (!game || pc != game->builder_pc) return;
    const int mode = gbarecomp::game_option_get(kOptionKey, kOff);
    if (mode != kParty && mode != kEveryone) return;
    auto* b = gbarecomp::active_bus();
    if (!b) return;
    const std::uint32_t frame = g_cpu.R[9];
    const std::uint32_t command = b->read32(frame + game->command_in_frame);
    const std::uint32_t state = b->read32(frame + game->state_in_frame);
    if ((command >> 24) != 0x02u && (command >> 24) != 0x03u) return;
    if ((state >> 24) != 0x02u && (state >> 24) != 0x03u) return;

    const auto actor = static_cast<std::int16_t>(b->read16(command));
    const bool party_actor = actor >= 0 && actor <= 7;
    if (!party_actor && mode != kEveryone) return;
    const std::uint16_t selection = b->read16(command + kSelection);
    const bool enemy_side = (selection & 0x80u) != 0;
    if (enemy_side != party_actor) return;  // aimed at the actor's own side

    const auto alive = [&](std::int16_t id) {
        std::uint32_t unit = 0;
        if (id >= 0 && id <= 7) unit = game->party_records + std::uint32_t(id) * kUnitStride;
        else if (id >= 0x80 && id <= 0x85) {
            const std::uint32_t base = b->read32(game->enemy_records_slot);
            if (!base) return false;
            unit = base + std::uint32_t(id - 0x80) * kUnitStride;
        } else return false;
        return static_cast<std::int16_t>(b->read16(unit + kHp)) != 0;
    };
    const std::uint32_t list = state + (enemy_side ? kEnemyList : kPartyList);
    int living[kListMax], count = 0, size = 0;
    for (; size < kListMax; ++size) {
        const auto id = static_cast<std::int16_t>(b->read16(list + std::uint32_t(size) * 2u));
        if (id == 0xFF) break;
        if (id != 0xFE && alive(id)) living[count++] = size;
    }
    if (count == 0) return;  // nobody left: the battle is ending

    const int centre = selection & 0xF;
    const int reach = static_cast<std::int16_t>(b->read16(command + kReach));
    for (int i = 0; i < count; ++i)
        if (living[i] > centre - reach && living[i] < centre + reach) return;  // native hits someone

    // Repeatable for a given battle state (replays and rewinds see the same
    // pick), varied by every fighter's HP so it changes from turn to turn.
    std::uint32_t h = 0x9E3779B9u ^ std::uint32_t(actor) ^ (g_cpu.R[0] << 16);
    for (int i = 0; i < size; ++i) {
        const auto id = static_cast<std::int16_t>(b->read16(list + std::uint32_t(i) * 2u));
        if (id != 0xFE && id >= 0x80 && id <= 0x85) {
            const std::uint32_t base = b->read32(game->enemy_records_slot);
            if (base) h = (h ^ b->read16(base + std::uint32_t(id - 0x80) * kUnitStride + kHp)) * 0x01000193u;
        }
    }
    for (int n = 0; n <= 7; ++n)
        h = (h ^ b->read16(game->party_records + std::uint32_t(n) * kUnitStride + kHp)) * 0x01000193u;
    h ^= h >> 15;
    const int pick = living[h % std::uint32_t(count)];
    b->write16(command + kSelection, static_cast<std::uint16_t>((selection & ~0xFu) | std::uint16_t(pick)));
}

}  // namespace gsr::battle_retarget
