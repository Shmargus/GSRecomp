// sprite_flicker.h -- sprites a game shows only on every other frame.
//
// Some effects draw two sets of sprites on alternate frames and leave the
// GBA's slow LCD to blend them into one steady, half see-through picture:
// Golden Sun's title lens flare swaps two halo sets every frame, keeping
// both sets' tiles loaded and changing only the object table (measured
// 2026-11-12, The Broken Seal FACTS.md). A sharp modern display shows the
// swap as flicker.
//
// This finds such sprites by comparing the object table with the last three
// frames', and builds the object table of the OTHER phase: this frame's
// sprites with the alternating ones taken out and last frame's alternating
// ones put back. The renderer draws the frame once with each table and mixes
// the two pictures (FieldSceneRenderer::set_flicker_smoothing), so only the
// pixels those sprites touch change. Image-based detection was measured and
// rejected: scrolling dithered grass and floor tiles look like flicker to it.
//
// Header-only and GPU-free so it can be tested on its own.

#pragma once

#include <array>
#include <cstdlib>

#include "field_scene.h"

namespace gsr {

using SceneObjectTable = std::array<SceneObject, FieldScene::kObjects>;

// How far a sprite may move between two frames of the same phase (two
// frames apart) and still count as the same sprite. The title flare's
// halos move at most 1 pixel in two frames (OAM at frames 441-444); 4
// allows twice that per frame.
inline constexpr int kSpriteFlickerReach = 4;
// How far from it the sprite must be missing in the frames between. A
// shaking screen (gpu_rewind_0038, a ladder jumping 5-10 pixels) or a
// battler running in to attack (gpu_rewind_0089) is present nearby, only
// moved; a flickering sprite is gone. The flare's other halo set reuses one
// tile, but that copy sits about 200 pixels away (parked below the screen).
inline constexpr int kSpriteFlickerGone = 32;

inline bool sprite_flicker_candidate(const SceneObject& o) {
    return o.present && !o.window;
}

// Same artwork and drawing attributes; position is compared separately.
inline bool sprite_flicker_same_art(const SceneObject& a, const SceneObject& b) {
    return a.tile == b.tile && a.width == b.width && a.height == b.height &&
           a.palette == b.palette && a.priority == b.priority &&
           a.hflip == b.hflip && a.vflip == b.vflip &&
           a.blended == b.blended && a.mosaic == b.mosaic &&
           a.affine == b.affine && a.double_size == b.double_size;
}

// Whether `table` holds a sprite with o's artwork within `reach` pixels.
inline bool sprite_flicker_in(const SceneObject& o, const SceneObjectTable& table,
                              int reach = kSpriteFlickerReach) {
    for (const SceneObject& t : table) {
        if (!sprite_flicker_candidate(t) || !sprite_flicker_same_art(o, t))
            continue;
        if (std::abs(t.x - o.x) <= reach && std::abs(t.y - o.y) <= reach)
            return true;
    }
    return false;
}

// The last three frames' object tables, newest first.
class SpriteFlickerHistory {
public:
    void reset() { count_ = 0; }
    void push(const SceneObjectTable& now) {
        frames_[2] = frames_[1];
        frames_[1] = frames_[0];
        frames_[0] = now;
        if (count_ < 3) ++count_;
    }
    bool ready() const { return count_ >= 3; }
    // 1 = last frame, 2 = the frame before, 3 = the one before that.
    const SceneObjectTable& back(int n) const { return frames_[n - 1]; }

private:
    std::array<SceneObjectTable, 3> frames_{};
    int count_ = 0;
};

// Builds the other phase's object table into `out`. Returns false (out
// untouched) when nothing alternates. A sprite alternates when it is gone
// from the previous frame and the one three back but present two back --
// four frames of a steady every-other-frame pattern, so a sprite that
// appears, disappears or blinks once is left alone.
inline bool sprite_flicker_other_phase(const SceneObjectTable& now,
                                       const SpriteFlickerHistory& history,
                                       SceneObjectTable* out) {
    if (!history.ready()) return false;
    const SceneObjectTable& prev1 = history.back(1);
    const SceneObjectTable& prev2 = history.back(2);
    const SceneObjectTable& prev3 = history.back(3);

    std::array<bool, FieldScene::kObjects> drop{};
    bool any = false;
    for (int s = 0; s < FieldScene::kObjects; ++s) {
        const SceneObject& o = now[s];
        if (!sprite_flicker_candidate(o)) continue;
        if (!sprite_flicker_in(o, prev1, kSpriteFlickerGone) &&
            sprite_flicker_in(o, prev2) &&
            !sprite_flicker_in(o, prev3, kSpriteFlickerGone)) {
            drop[s] = true;
            any = true;
        }
    }
    std::array<bool, FieldScene::kObjects> add{};
    for (int s = 0; s < FieldScene::kObjects; ++s) {
        const SceneObject& o = prev1[s];
        if (!sprite_flicker_candidate(o)) continue;
        if (!sprite_flicker_in(o, now, kSpriteFlickerGone) &&
            sprite_flicker_in(o, prev3) &&
            !sprite_flicker_in(o, prev2, kSpriteFlickerGone)) {
            add[s] = true;
            any = true;
        }
    }
    if (!any) return false;

    *out = now;
    for (int s = 0; s < FieldScene::kObjects; ++s)
        if (drop[s]) (*out)[s].present = false;
    // Last frame's sprites go back in their own slots (slot order is draw
    // order); one whose slot is taken goes in the next free slot.
    for (int s = 0; s < FieldScene::kObjects; ++s) {
        if (!add[s]) continue;
        int slot = -1;
        for (int k = 0; k < FieldScene::kObjects && slot < 0; ++k) {
            const int t = (s + k) % FieldScene::kObjects;
            if (!(*out)[t].present) slot = t;
        }
        if (slot < 0) break;
        (*out)[slot] = prev1[s];
    }
    return true;
}

}  // namespace gsr
