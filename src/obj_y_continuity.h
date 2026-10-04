// Sprite edge continuity: which of a tall field sprite's two Y readings to
// draw in the expanded view. Test option ("Sprite edge continuity" in the
// launcher's test variables, GSR_OBJ_Y_CONTINUITY); off by default.
//
// OAM keeps Y in 8 bits, so every raw value has two readings 256 apart.
// The 360x240 view is 240 rows tall, so for a sprite box taller than 56
// rows BOTH readings can reach the view, and the providers' answer can put
// the sprite on the wrong edge. Measured 2026-10-04 over every saved
// capture (nine such moments): the Vale boulder falling in from the top
// was drawn in the bottom margin for 2 frames and, flying out of the top,
// drawn rising from the bottom for 6 (Steam Deck gpu_rewind_0007/0008); a
// sprite leaving the bottom was drawn at the top for one frame
// (gpu_rewind_0065, frame 103). In all nine the reading nearer the same
// sprite's previous-frame position is the right one.
//
// With no previous position (the sprite's first frame) a guessed bottom
// reading that starts on the console's screen is held inside the console's
// window, which is exactly what the GBA shows; the margins stay empty until
// the sprite has a position to continue from.
//
// Only GUESSED answers change: the providers' band rule, or the hardware
// sign rule when no provider answered. A position taken from the game's own
// record (provenance) or our position table stands. Not on the world map,
// whose actor table already says which side a sprite is on.
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>

#include "field_scene.h"
#include "widescreen_policy.h"

namespace gsr {

struct ObjYContinuity {
    struct Seen {
        bool present = false;
        bool confined = false;  // held inside the console's window
        int x = 0, y = 0;
        int width = 0, height = 0;
        bool affine = false, double_size = false;
        int palette = 0;
    };
    std::array<Seen, FieldScene::kObjects> previous{};
    std::uint64_t previous_frame = UINT64_MAX;
};

struct ObjYContinuityChange {
    int slot = -1;
    int raw_y = 0;
    int from_y = 0;
    int to_y = 0;      // meaningless when confined
    bool confined = false;
    int previous_y = 0;
};

// Rewrites scene->objects[i].y (or sets .parked to hold a sprite inside the
// console's window) for ambiguous tall sprites. `field` is false outside a
// field frame in the expanded view (battle, world map, native view): nothing
// changes and the history is dropped. `oam` is the 1 KiB OAM the scene was
// captured from; `guessed[i]` is true when slot i's Y was a guess. Calls
// `report` for every change.
template <typename Report>
void apply_obj_y_continuity(FieldScene* scene, const std::uint8_t* oam,
                            const bool* guessed, bool field,
                            ObjYContinuity* state, Report&& report) {
    using Seen = ObjYContinuity::Seen;
    constexpr int kTop = -widescreen::kExpandedExtraY;
    constexpr int kBottom =
        static_cast<int>(widescreen::kNativeHeight) + widescreen::kExpandedExtraY;
    constexpr int kNativeBottom = static_cast<int>(widescreen::kNativeHeight);
    // A sprite moves a few pixels a frame (the boulder 6, the fastest seen
    // 8); the two readings are 256 apart, so this cannot pick the wrong one.
    constexpr int kMatchRadius = 24;
    // Rewind and fast-forward skip frames: gpu_rewind_0065 holds every third.
    constexpr std::uint64_t kMaxFrameGap = 4;

    const std::uint64_t frame = scene->frame;
    if (!field) {
        state->previous = {};
        state->previous_frame = UINT64_MAX;
        return;
    }
    const bool history = state->previous_frame != UINT64_MAX &&
                         frame > state->previous_frame &&
                         frame - state->previous_frame <= kMaxFrameGap;

    std::array<Seen, FieldScene::kObjects> now{};
    for (int i = 0; i < FieldScene::kObjects; ++i) {
        SceneObject& O = scene->objects[i];
        if (!O.present) continue;
        Seen& s = now[i];
        s.present = true;
        s.x = O.x;
        s.y = O.y;
        s.width = O.width;
        s.height = O.height;
        s.affine = O.affine;
        s.double_size = O.double_size;
        s.palette = O.palette;
        if (O.parked || !guessed[i]) continue;

        const int box_h = (O.affine && O.double_size) ? 2 * O.height : O.height;
        const int raw = oam[static_cast<std::size_t>(i) * 8u];
        const int low = raw;          // the reading below
        const int high = raw - 256;   // the reading above
        auto in_view = [&](int y) { return y + box_h > kTop && y < kBottom; };
        if (!in_view(low) || !in_view(high)) continue;  // not ambiguous

        int best_distance = kMatchRadius + 1;
        int best_y = 0;
        int best_previous = 0;
        bool disagree = false;
        for (int j = 0; history && j < FieldScene::kObjects; ++j) {
            const Seen& p = state->previous[static_cast<std::size_t>(j)];
            if (!p.present || p.confined || p.width != O.width ||
                p.height != O.height || p.affine != O.affine ||
                p.double_size != O.double_size || p.palette != O.palette ||
                std::abs(p.x - O.x) > kMatchRadius)
                continue;
            const int d_low = std::abs(low - p.y);
            const int d_high = std::abs(high - p.y);
            const int d = d_low <= d_high ? d_low : d_high;
            const int y = d_low <= d_high ? low : high;
            if (d < best_distance) {
                best_distance = d;
                best_y = y;
                best_previous = p.y;
                disagree = false;
            } else if (d == best_distance && y != best_y) {
                disagree = true;
            }
        }
        if (best_distance <= kMatchRadius && !disagree) {
            if (O.y != best_y)
                report(ObjYContinuityChange{i, raw, O.y, best_y, false,
                                            best_previous});
            O.y = best_y;
            s.y = best_y;
            continue;
        }
        // No position to continue from. Only the bottom reading that
        // starts on the console's own screen is at risk (the providers'
        // band rule calls 0..159 unambiguous); hold it inside the window.
        if (O.y == low && low < kNativeBottom) {
            report(ObjYContinuityChange{i, raw, O.y, O.y, true, 0});
            O.parked = true;
            s.confined = true;
        }
    }
    state->previous = now;
    state->previous_frame = frame;
}

}  // namespace gsr
