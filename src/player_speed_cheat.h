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
constexpr std::uint32_t transform(std::uint32_t before,
                                  std::uint32_t requested,
                                  std::uint32_t multiplier) {
    (void)before;
    return requested * multiplier;
}

}  // namespace gsr::player_speed_cheat
