// Golden Sun: The Lost Age -- Walk Speed (1x, 1.5x, 2x, 3x).
//
// Same design as Golden Sun 1's, re-found on this ROM (FACTS.md,
// 2026-10-04). Each locomotion routine reads the run button (setting
// 0x02000240 + 0x23C against the held keys 0x03001150) and stores the
// player's speed limit, a 16.16 pixels-per-frame word, at object + 0x30 every
// frame before moving it. The runner scales what those stores write:
//   0x08027064  field:     run 0x18000 at 0x080270AC, walk 0x10000 at 0x080270C0
//   0x080279B0  world map: run 0x10000 at 0x080279E2, walk 0x08000 at 0x080279F4
//   0x08029570  rope:      0x08000 at 0x08029592 (the player's action
//               0x1D on a stretched rope, F12 0077/0078; FACTS 2026-10-08)
//   0x080290F4  climbing:  0x08000 at 0x08029118 (action 0x1B; FACTS
//               2026-10-15)
//   0x08028534  sailing:   0x14000 at 0x080285AC (action 0x20;
//               F12 0071, FACTS 2026-10-17)
// (Golden Sun 1's field and world-map routines store the same numbers.) Two
// other routines with fixed speeds, 0x08026300 (0x8000 both) and 0x080267BC
// (0x10000 both), keep the game's own.
//
// Climbing ends where a once-per-frame test finds the ground ahead too close
// (Up: margin below 16 px, Down: above -8 px); a faster step could pass
// that point, which in Golden Sun 1 fired a ship-mast event early. So at
// both tests, while one faster step (plus half a pixel of slack) could reach
// the stop, the speed goes back to the game's 0x8000 and the velocity is cut
// to it: the last stretch, the stop and the climb off run as in the original.
// Getting on and off is a separate scripted action (0x11) and is not scaled.
//
// The multiplier is the engine's (F1 menu Cheats -> Player -> Walking speed,
// saved in config.ini), in halves: 2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x.

#include <cstdint>
#include <cstdio>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kSpeedLimitOffset = 0x30u;
constexpr uint32_t kWriterPcs[] = {
    0x080270ACu,  // field run
    0x080270C0u,  // field walk
    0x080279E2u,  // world map run
    0x080279F4u,  // world map walk
    0x08029592u,  // rope walk
    0x08029118u,  // climb
    0x080285ACu,  // sailing
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
constexpr uint32_t kClimbUpTestPc = 0x08029236u;
constexpr uint32_t kClimbDownTestPc = 0x0802925Au;
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
    if (room >= fast + kGameClimbSpeed) return;
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

int climb_down_test(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc == kClimbDownTestPc)
        climb_near_end(kClimbDownStop - static_cast<int32_t>(g_cpu.R[3]));
    return g_next_literal ? g_next_literal(pc, original, out_value) : 0;
}

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
