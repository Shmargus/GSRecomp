// Golden Sun: The Lost Age -- reward cheats, controlled by F1 > Cheats.

#include <algorithm>
#include <cstdint>
#include <string>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kPartyHpBase = 0x02000554u;  // record 0 + 0x34
constexpr uint32_t kPartyStride = 0x14Cu;
constexpr unsigned kPartySlots = 8u;

// The tally's accumulating stores in 0x0812824C (32-bit `str`): coins on
// the bonus path and the plain path, EXP on both.
constexpr uint32_t kCoinBonusStorePc = 0x0812839Eu;
constexpr uint32_t kCoinStorePc = 0x081283F8u;
constexpr uint32_t kExpStorePc = 0x08128404u;
constexpr uint32_t kDropBranchPc = 0x0812846Eu;

// Multiplier steps, in halves so 0.5x stays exact. 25x..100x apply to
// Experience and Coins only; Drop Chance stops at 10x, where a drop is
// already all but certain.
constexpr int kStepCount = 9;
constexpr int kDropStepCount = 6;
constexpr int kStepHalves[kStepCount] = {0, 1, 2, 4, 10, 20, 50, 100, 200};
constexpr int kNormalStep = 2;

int (*g_next_branch)(uint32_t, uint32_t, uint32_t*) = nullptr;
RuntimeMemWriteOverrideHook g_next_write_override = nullptr;

// What one tally store adds, times the step's multiplier.
int reward_store(uint32_t pc, uint32_t addr, uint32_t requested, uint32_t width,
                 uint32_t* out_value) {
    if (pc != kCoinBonusStorePc && pc != kCoinStorePc && pc != kExpStorePc)
        return g_next_write_override
                   ? g_next_write_override(pc, addr, requested, width, out_value)
                   : 0;
    int step = gbarecomp::game_option_get(
        pc == kExpStorePc ? gbarecomp::kOptionExpMultiplier : gbarecomp::kOptionCoinMultiplier,
        kNormalStep);
    if (step < 0 || step >= kStepCount) step = kNormalStep;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !out_value || width != 4u || step == kNormalStep) return 0;
    const uint32_t before = b->read32(addr);
    const uint64_t added = static_cast<uint32_t>(requested - before);
    const uint64_t total =
        before + added * static_cast<uint64_t>(kStepHalves[step]) / 2u;
    *out_value = static_cast<uint32_t>(std::min<uint64_t>(total, 0x7FFFFFFFu));
    return *out_value != requested;
}

// The drop roll's BLE (taken = no drop), decided on the scaled threshold.
int drop_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    if (pc != kDropBranchPc)
        return g_next_branch ? g_next_branch(pc, original, out_decision) : 0;
    int step = gbarecomp::game_option_get(gbarecomp::kOptionDropMultiplier, kNormalStep);
    if (step < 0 || step >= kDropStepCount) step = kNormalStep;
    if (step == kNormalStep) return 0;
    const int64_t threshold =
        static_cast<int64_t>(static_cast<int32_t>(g_cpu.R[5])) * kStepHalves[step] / 2;
    const int64_t roll = static_cast<int32_t>(g_cpu.R[0]);
    *out_decision = threshold <= roll ? 1u : 0u;
    return *out_decision != original ? 1 : 0;
}

}  // namespace

// After tla_options_load. Chains in front of the branch and write hooks already
// installed.
void tla_install_cheat_menu(gbarecomp::RunOptions& options) {
    runtime_set_party_layout(kPartyHpBase, kPartyStride, kPartySlots);
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = drop_branch;
    g_next_write_override = g_runtime_mem_write_override;
    g_runtime_mem_write_override = reward_store;
}
