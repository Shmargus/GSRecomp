#pragma once

#include <cstdint>

// Walk through walls (developer F1 cheat, engine/src/runtime/
// developer_session.h). Measured in the SHA-1-verified ROM (FACTS.md,
// 2026-10-23). The player's field walking routine 0x0800EBEC tests each step
// with 0x080120DC (2: wall tile, 1: ground over 8 px higher, -1: over 12 px
// lower) and then tests the other objects; any failure stops or slides the
// player. Before those tests the developers left a path that skips them all:
// when the debug byte [0x03001F54] is set and L is held ([0x03001AE8] &
// 0x200), `b 0x0800F0C6` moves the player straight to the point ahead with
// no wall or people tests. The cheat takes that path by deciding its two
// `beq`s as not taken; the debug byte itself is never written.
namespace gsr::walk_through_walls {

constexpr std::uint32_t kDebugByteBranchPc = 0x0800ED22u;  // beq: byte 0
constexpr std::uint32_t kDebugKeyBranchPc = 0x0800ED34u;   // beq: L up

constexpr bool is_branch(std::uint32_t pc) {
    return pc == kDebugByteBranchPc || pc == kDebugKeyBranchPc;
}

}  // namespace gsr::walk_through_walls
