#pragma once

#include <cstdint>

// Golden Sun USA/Europe metadata belongs to the project runner, not the
// reusable ARMv4T runtime. These identities were measured from the generated
// image used by this project.
namespace gsr::player_speed_cheat {

// The overworld and field-map locomotion paths write the player's speed limit
// before collision and integration. Exact addresses select only the measured
// GS011 player objects; scripted gap-jump writers are deliberately excluded.
constexpr std::uint32_t kWriterXPc = 0x0800F32Au;
constexpr std::uint32_t kAccumulatorX = 0x02030DD4u;
constexpr std::uint32_t kWriterYPc = 0x0800F33Cu;
constexpr std::uint32_t kAccumulatorY = 0x02030DD4u;
constexpr std::uint32_t kFieldRunWriterPc = 0x0800EC8Eu;
constexpr std::uint32_t kFieldWalkWriterPc = 0x0800ECA0u;
constexpr std::uint32_t kFieldAccumulator = 0x02030EECu;
// Ladders and pushing keep the game's own speed (Jimmy, 2026-10-01: "too
// risky"). The climbing handler (0x0800F7F4) stores 0.5 px/frame at
// 0x0800F80A and stops the player at a ladder's top by a height check made
// each frame; at 1.5 px/frame the player passed that point in one frame, and
// on the Tolbi-bound ship's mast an event sent the player back to the deck
// instead of the climb off (gpu_rewind_0083), so the Anchor Charm at the top
// could not be reached. Pushes (set-object-speed 0x08092064, speed 0x3333)
// were scaled the same way and are left alone with them.
// Retain the original names for diagnostic counters.
constexpr std::uint32_t kWriterPc = kWriterYPc;
constexpr std::uint32_t kAccumulator = kAccumulatorY;

constexpr bool matches(std::uint32_t pc, std::uint32_t addr,
                       std::uint32_t width) {
    return width == 4u &&
        ((pc == kWriterXPc && addr == kAccumulatorX) ||
         (pc == kWriterYPc && addr == kAccumulatorY) ||
         (pc == kFieldRunWriterPc && addr == kFieldAccumulator) ||
         (pc == kFieldWalkWriterPc && addr == kFieldAccumulator));
}

// The guest stores the absolute fixed-point speed limit each frame.
// The multiplier is in halves (2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x).
constexpr std::uint32_t transform(std::uint32_t before,
                                  std::uint32_t requested,
                                  std::uint32_t multiplier) {
    (void)before;
    return requested * multiplier / 2u;
}

}  // namespace gsr::player_speed_cheat
