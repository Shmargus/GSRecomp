// Golden Sun battle presentation for the expanded view.
//
// The battle remains an authentic 240x160 composition in the centre of the
// expanded canvas. Enabled arena layers continue into the left and right
// margins only while the game's live WIN0V scene band is active. They keep
// their native coordinates, clipping and scale; sprites, menus and command
// icons remain native.
//
// Full vertical remapping was tested and deferred: the game's detailed rows
// can carry different BGxCNT map geometry even when the sampled frame summary
// shows no scroll/scale/reference animation. A source-row-complete renderer
// would be a larger change than this fallback.
#pragma once

#include <cstdint>

namespace gsr::battle {

// The regular layer carrying the arena while the camera sits back.
inline constexpr unsigned kBackdropLayer = 1u;

// The affine layer that can carry the arena while the camera pushes in. Its
// effect ownership varies by state and is not assumed by the margin fallback.
inline constexpr unsigned kArenaAffineLayer = 2u;

// The menu layer: HP windows, command windows, battle messages.
inline constexpr unsigned kMenuLayer = 0u;

// Mode 1 with at least one battle BG layer on. BG2's affine map geometry and
// visual role vary by battle state.
inline bool is_battle_frame(std::uint16_t dispcnt) {
    const unsigned arena_layers = (0x0100u << kBackdropLayer) |
                                  (0x0100u << kArenaAffineLayer);
    return (dispcnt & 0x07u) == 1u && (dispcnt & arena_layers) != 0u;
}

// The battle band's live top and bottom rows, as the game declares them in
// WIN0V. Refuse an empty, reversed or out-of-screen band rather than extending
// from a cleared or half-written register.
inline bool band_bounds(std::uint16_t win0v, int native_height,
                        int* out_top, int* out_bottom) {
    const int y1 = static_cast<int>((win0v >> 8) & 0x00FFu);
    const int y2 = static_cast<int>(win0v & 0x00FFu);
    if (y1 >= native_height || y1 >= y2 ||
        y2 < native_height / 2 || y2 > native_height) return false;
    if (out_top) *out_top = y1;
    if (out_bottom) *out_bottom = y2;
    return true;
}

// The middle of that band, retained for the diagnostic rule log.
inline int band_centre(int band_top_row, int band_bottom_row) {
    return band_top_row + (band_bottom_row - band_top_row) / 2;
}

}  // namespace gsr::battle
