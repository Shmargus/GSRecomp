#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "effect_particles.h"

namespace gsr {

// The completed persistent canvas is packed as one rectangular stamp, so
// both presentation and F12 replay include the game's fading trails.
struct CapturedEffectFrame {
    std::uint32_t canvas = 0;
    std::vector<EffectSpark> stamps;
    std::vector<std::uint8_t> artwork;
};

void effect_capture_set_enabled(bool enabled);
bool effect_capture_enabled();
bool effect_capture_observes(std::uint32_t pc);

// Observe the canvas clear/display-copy lifecycle and live slot-46/47 entries,
// with their actual stack
// width/height. The generated code is checked against the user's ROM templates
// to identify maximum/additive blending and source flips. No guest state changes.
void effect_capture_on_entry(std::uint32_t pc, const std::uint32_t* registers,
                             const std::uint8_t* ewram,
                             const std::uint8_t* iwram,
                             const std::uint8_t* rom, std::size_t rom_bytes,
                             std::uint64_t frame);
CapturedEffectFrame effect_capture_take();
void effect_capture_report(char* out, std::size_t out_bytes);

}  // namespace gsr
