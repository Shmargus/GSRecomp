// Triangle history publishes zero pixels as erasures, while preserving
// unchanged pixels inside the copied rectangle.
#include "effect_capture.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    std::vector<std::uint8_t> ewram(0x40000), iwram(0x8000);
    std::array<std::uint32_t, 16> registers{};
    constexpr std::uint32_t canvas = 0x03001684u;
    registers[0] = canvas;
    registers[1] = 0x06000000u;
    registers[2] = 128u * 128u;
    gsr::effect_capture_set_enabled(true);
    // The measured copy/fade wrapper establishes the canvas being followed.
    gsr::effect_capture_on_entry(0x08001C90u, registers.data(), ewram.data(),
                                 iwram.data(), nullptr, 0, 1);
    const std::uint8_t before[] = {42, 17, 33};
    const std::uint8_t after[] = {0, 17, 9};
    gsr::effect_capture_history_paint(canvas, -3, 0, 3, 1, before);
    gsr::effect_capture_history_paint(canvas, -3, 0, 3, 1, after);
    std::array<std::uint8_t, 128 * 128> history{};
    assert(gsr::effect_capture_history_read(canvas, -3, 0, history.data()));
    assert(history[0] == 0 && history[1] == 17 && history[2] == 9);
    // Also check the publication handed to the renderer, before fading.
    gsr::effect_capture_on_entry(0x08001C90u, registers.data(), ewram.data(),
                                 iwram.data(), nullptr, 0, 2);
    const auto published = gsr::effect_capture_take();
    assert(published.canvas == canvas && published.stamps.size() == 1);
    assert(published.artwork.size() == 3);
    assert(published.artwork[0] == 0 && published.artwork[1] == 17 &&
           published.artwork[2] == 9);
    gsr::effect_capture_set_enabled(false);
    std::puts("effect_capture: triangle erasure and publication checks passed");
}
