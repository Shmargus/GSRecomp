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
// (Golden Sun 1's field and world-map routines store the same numbers.) Two
// other routines with fixed speeds, 0x08026300 (0x8000 both, a climb) and
// 0x080267BC (0x10000 both), keep the game's own: Golden Sun 1 left ladders
// and pushing alone after a ladder's top check was overshot (2026-10-01).
//
// The multiplier is the engine's (F1 menu Cheats -> Player -> Walking speed,
// saved in config.ini), in halves: 2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x.

#include <cstdint>
#include <cstdio>

#include "runtime_arm.h"
#include "tla.h"

namespace {

constexpr uint32_t kSpeedLimitOffset = 0x30u;
constexpr uint32_t kWriterPcs[] = {
    0x080270ACu,  // field run
    0x080270C0u,  // field walk
    0x080279E2u,  // world map run
    0x080279F4u,  // world map walk
    0x08029592u,  // rope walk
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

}  // namespace

// Chains in front of whatever write hook is installed (Instant text's).
void tla_install_walk_speed() {
    g_next_write_override = g_runtime_mem_write_override;
    g_runtime_mem_write_override = walk_speed_store;
}
