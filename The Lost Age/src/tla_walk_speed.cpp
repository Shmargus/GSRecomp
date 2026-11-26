// Golden Sun: The Lost Age -- Walk Speed (1x, 1.5x, 2x, 3x).
//
// Same design as the Broken Seal's, re-found on this ROM (FACTS.md
// "Walk Speed and settings page two").
//
// Each locomotion routine reads the run button (setting 0x02000240 + 0x23C
// against the held keys 0x03001150). Every frame, before moving the player, it
// stores the player's speed limit at object + 0x30, a 16.16 pixels-per-frame
// word. The runner scales what those stores write:
//   - 0x08027064 field: run 0x18000 at 0x080270AC, walk 0x10000 at 0x080270C0.
//   - 0x080279B0 world map: run 0x10000 at 0x080279E2, walk 0x08000 at
//     0x080279F4.
//   - 0x08029570 rope: 0x08000 at 0x08029592 (the player's action 0x1D on a
//     stretched rope).
//   - 0x080290F4 climbing: 0x08000 at 0x08029118 (action 0x1B).
//   - 0x08028534 sailing: 0x14000 at 0x080285AC (action 0x20).
//   - 0x08027E20 boat: 0x10000 at 0x08027E7A, and 0x40000 at 0x08027E9A with B
//     held (action 0x1F, the Sea of Time boat).
//   - 0x08028AAC flying: 0x14000 + height / 16 at 0x08028B3C (action 0x21, the
//     ship with wings).
// The Broken Seal's field and world-map routines store the same numbers.
// Two other routines have fixed speeds and keep the game's own:
// 0x08026300 (0x8000 both) and 0x080267BC (0x10000 both).
//
// Climbing ends in one of two ways, found by a once-per-frame test:
//   - the ground ahead is too close (Up: margin below 16 px, Down: above
//     -8 px), or
//   - going sideways, the point 8 px ahead leaves the climbable tiles (flag
//     0x40).
// A faster step could pass that point, which in the Broken Seal fired a
// ship-mast event early. So at all three tests, while two faster steps (plus
// half a pixel of slack) could reach the stop, the speed goes back to the
// game's 0x8000 and the velocity is cut to it. The last stretch, the stop and
// the climb off then run as in the original. (One step is the exact reach; the
// second is a safety margin.)
// Getting on and off is a separate scripted action (0x11) and is not scaled.
//
// The multiplier is the engine's (F1 menu Cheats -> Player -> Walking speed,
// saved in config.ini), in halves: 2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x.
//
// Walk through walls (developer F1 cheat, engine/src/runtime/
// developer_session.h). The field routine 0x08027064 tests each step with
// 0x0802D87C (2: wall tile, 1: ground over 8 px higher, -1: over 12 px lower)
// and the other objects. Before those tests the developers left a path that
// skips them all: when the debug byte [0x03001238] is set and L is held
// ([0x03001150] & 0x200), `b 0x08027664` moves the player straight to the
// point ahead with no wall or people tests (the same path as the Broken
// Seal's). The cheat takes it by deciding its two `beq`s as not taken. The
// debug byte itself is never written.

#include "tla_rom_version.h"
#include <cstdint>
#include <cstdio>

#include "developer_session.h"
#include "host_config_ui.h"
#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "../../shared_game/auto_field_psynergy_labels.h"
#include "../../shared_game/autosave.h"
#include "../../shared_game/enemy_hp_bar.h"
#include "../../shared_game/trackers.h"
#include "tla.h"
#include "../../shared_game/quality_of_life.h"

namespace {

constexpr uint32_t kSpeedLimitOffset = 0x30u;
const uint32_t kWriterPcs[] = {
    TLA_ROM(0x080270ACu),  // field run
    TLA_ROM(0x080270C0u),  // field walk
    TLA_ROM(0x080279E2u),  // world map run
    TLA_ROM(0x080279F4u),  // world map walk
    TLA_ROM(0x08029592u),  // rope walk
    TLA_ROM(0x08029118u),  // climb
    TLA_ROM(0x080285ACu),  // sailing
    TLA_ROM(0x08027E7Au),  // boat
    TLA_ROM(0x08027E9Au),  // boat, B held
    TLA_ROM(0x08028B3Cu),  // flying ship
};
constexpr unsigned kWriterCount = sizeof(kWriterPcs) / sizeof(kWriterPcs[0]);

RuntimeMemWriteOverrideHook g_next_write_override = nullptr;
bool g_logged[kWriterCount] = {};

int walk_speed_store(uint32_t pc, uint32_t addr, uint32_t requested,
                     uint32_t width, uint32_t* out_value) {
    if (width == 4u && out_value) {
        for (unsigned i = 0; i < kWriterCount; ++i) {
            if (pc != kWriterPcs[i]) continue;
            const uint32_t halves =
                static_cast<uint32_t>(runtime_get_mem_write_override_enabled());
            if (!g_logged[i]) {
                g_logged[i] = true;
                std::fprintf(stderr,
                             "[walk-speed] pc=0x%08X object=0x%08X speed=0x%05X x%u/2\n",
                             pc, addr - kSpeedLimitOffset, requested, halves);
            }
            *out_value = requested * halves / 2u;
            return *out_value != requested;
        }
    }
    return g_next_write_override
               ? g_next_write_override(pc, addr, requested, width, out_value)
               : 0;
}

// Climb end tests (0x080290F4): r3 = ground height ahead minus the player's
// height, r7 = the climbing object. The game keeps climbing while
// Up: r3 >= 0x100000, Down: r3 <= -0x80000.
// Sideways: 0x08029218 runs only once both the player's tile and the tile
// 8 px ahead (r5, a 4-byte map tile entry; the row is 128 entries) carry
// flag 0x40 in byte +3; [sp] = that point's x (16.16), r6 = sp.
const uint32_t kClimbUpTestPc = TLA_ROM(0x08029236u);
const uint32_t kClimbDownTestPc = TLA_ROM(0x0802925Au);
const uint32_t kClimbSideTestPc = TLA_ROM(0x08029218u);
constexpr int32_t kTileSize = 0x100000;  // 16 px
constexpr int32_t kClimbUpStop = 0x100000, kClimbDownStop = -0x80000;
constexpr int32_t kGameClimbSpeed = 0x8000;
constexpr uint32_t kVelocityOffset = 0x24u;  // +0x24/+0x28/+0x2C

int (*g_next_alu)(uint32_t, uint32_t, uint32_t*) = nullptr;
int (*g_next_literal)(uint32_t, uint32_t, uint32_t*) = nullptr;
bool g_climb_logged = false;

void climb_near_end(int32_t room) {
    const uint32_t halves =
        static_cast<uint32_t>(runtime_get_mem_write_override_enabled());
    gba::GbaBus* b = gbarecomp::active_bus();
    if (halves <= 2u || !b) return;
    const int32_t fast = kGameClimbSpeed * static_cast<int32_t>(halves) / 2;
    if (!g_climb_logged) {
        g_climb_logged = true;
        std::fprintf(stderr, "[walk-speed] climb test object=0x%08X room=0x%X\n",
                     g_cpu.R[7], static_cast<uint32_t>(room));
    }
    if (room >= 2 * fast + kGameClimbSpeed) return;
    const uint32_t obj = g_cpu.R[7];
    b->write32(obj + kSpeedLimitOffset, kGameClimbSpeed);
    int32_t v[3], peak = 0;
    for (int i = 0; i < 3; ++i) {
        v[i] = static_cast<int32_t>(b->read32(obj + kVelocityOffset + 4u * i));
        const int32_t mag = v[i] < 0 ? -v[i] : v[i];
        if (mag > peak) peak = mag;
    }
    if (peak <= kGameClimbSpeed) return;
    for (int i = 0; i < 3; ++i)
        b->write32(obj + kVelocityOffset + 4u * i,
                   static_cast<uint32_t>(static_cast<int64_t>(v[i]) *
                                         kGameClimbSpeed / peak));
}

int climb_up_test(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc == kClimbUpTestPc)
        climb_near_end(static_cast<int32_t>(g_cpu.R[3]) - kClimbUpStop);
    return g_next_alu ? g_next_alu(pc, original, out_value) : 0;
}

// Room left before the point 8 px ahead leaves the climbable column: to the
// edge of its tile, or a whole tile more when the next one is climbable too
// (far enough that climb_near_end leaves the speed alone).
void climb_side_test() {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return;
    const uint32_t obj = g_cpu.R[7];
    const int32_t ahead = static_cast<int32_t>(b->read32(g_cpu.R[6]));
    const int32_t dx = ahead - static_cast<int32_t>(b->read32(obj + 8u));
    if (dx == 0) return;
    const int32_t tile_left = ahead & ~(kTileSize - 1);
    const int32_t room = dx > 0 ? tile_left + kTileSize - 1 - ahead : ahead - tile_left;
    const uint32_t next = g_cpu.R[5] + (dx > 0 ? 4u : static_cast<uint32_t>(-4));
    if (b->read8(next + 3u) & 0x40u) return;
    climb_near_end(room);
}

int climb_down_test(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc == kClimbDownTestPc)
        climb_near_end(kClimbDownStop - static_cast<int32_t>(g_cpu.R[3]));
    else if (pc == kClimbSideTestPc)
        climb_side_test();
    return g_next_literal ? g_next_literal(pc, original, out_value) : 0;
}

const uint32_t kDebugByteBranchPc = TLA_ROM(0x08027274u);  // beq: byte 0
const uint32_t kDebugKeyBranchPc = TLA_ROM(0x08027282u);   // beq: L up

int (*g_next_branch)(uint32_t, uint32_t, uint32_t*) = nullptr;

int walk_through_walls_branch(uint32_t pc, uint32_t original,
                              uint32_t* out_decision) {
    if (pc == kDebugByteBranchPc || pc == kDebugKeyBranchPc)
        return gbarecomp::walk_through_walls::decide_branch(original, out_decision);
    return g_next_branch ? g_next_branch(pc, original, out_decision) : 0;
}

// F1's Game page: the Enemy HP bar (tla_enemy_hp_bar.cpp) and the
// trackers (tla_trackers.cpp); developer sessions also list walking through
// walls.
const gbarecomp::GameOptionSwitch kGameSwitches[] = {
    {gsr::auto_field_psynergy::kSwitchLabel, gsr::auto_field_psynergy::kSwitchCaption,
     gsr::auto_field_psynergy::kOptionKey},
    {gsr::autosave::kSwitchLabel, gsr::autosave::kSwitchCaption,
     gsr::autosave::kOptionKey},
    {gsr::enemy_hp_bar::kSwitchLabel, gsr::enemy_hp_bar::kSwitchCaption,
     gsr::enemy_hp_bar::kOptionKey},
#ifndef __ANDROID__  // phones: no tracker windows (gsr::trackers)
    {gsr::djinn_tracker::kSwitchLabel, gsr::djinn_tracker::kSwitchCaption,
     gsr::djinn_tracker::kOptionKey},
    {gsr::djinn_tracker::kShowAllLabel, gsr::djinn_tracker::kShowAllCaption,
     gsr::djinn_tracker::kShowAllKey},
    {gsr::treasure_tracker::kSwitchLabel, gsr::treasure_tracker::kSwitchCaption,
     gsr::treasure_tracker::kOptionKey},
#endif
    {gsr::select_menu::kSwitchLabel, gsr::select_menu::kSwitchCaption,
     gsr::select_menu::kOptionKey},
    {gbarecomp::walk_through_walls::kSwitchLabel,
     gbarecomp::walk_through_walls::kSwitchCaption,
     gbarecomp::walk_through_walls::kOptionKey}};

}  // namespace

// Chains in front of whatever write hook is installed (Instant text's) and
// the immediate and literal hooks already in RunOptions.
void tla_install_walk_speed(gbarecomp::RunOptions& options) {
    g_next_write_override = g_runtime_mem_write_override;
    g_runtime_mem_write_override = walk_speed_store;
    g_next_alu = options.thumb_alu_immediate_override;
    options.thumb_alu_immediate_override = climb_up_test;
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = climb_down_test;
}

// The F1 Game page lists the switch in developer sessions only.
void tla_install_walk_through_walls(gbarecomp::RunOptions& options) {
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = walk_through_walls_branch;
    gsr::qol::set_switches(kGameSwitches, static_cast<int>(std::size(kGameSwitches)));
}
