// temporal_blend.h: VFX-FLICKER-02/03: optional temporal blend ("LCD
// ghosting") of the final presented RGB888 frame, whole-frame or flicker-only.
//
// Real GBA hardware's LCD has a slow pixel response, so two frames drawn in
// quick succession visibly blend together. Some Golden Sun effects (the
// Kolima blue barrier, the world map) use this deliberately. They alternate
// their draw every other frame as a translucency trick. A renderer that
// switches pixels instantly instead of blending them makes that alternation
// visibly shimmer or flicker.
//
// This is a presentation-only fix. It blends the current frame with the
// previous PRESENTED frame, so an exact 2-frame alternation can be fully
// stabilized (Strong = a true 50/50 average). The default is "Flicker only -
// Strong" (kTemporalBlendDefaultIndex).
//
// It is host-side and presentation-only. The guest never reads anything here,
// and it runs after the PPU has produced the frame, so it never affects
// emulation, the verify/frame-hash paths or timing. It is header-only and
// SDL-free, so it can be unit-tested against synthetic buffers without a
// display or a ROM.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gbarecomp {

// The Flicker reduction setting selects the previous frame's weight.
// Off bypasses the blend. Strong averages the two frames equally, which
// stabilizes a two-frame flicker. Light and Medium leave more of the
// current frame so ordinary movement smears less.
inline constexpr int kTemporalBlendLevelCount = 4;  // Off/Light/Medium/Strong
inline constexpr float kTemporalBlendWeights[kTemporalBlendLevelCount] = {
    0.0f,    // Off: never actually applied; caller passes through instead.
    0.175f,  // Light  (~17.5% previous-frame contribution)
    0.325f,  // Medium (~32.5% previous-frame contribution)
    0.5f,    // Strong: exact 50/50, matches an exact 2-frame alternation.
};

// Blends `current` (an RGB888 buffer, `byte_count` bytes) with `prev` (the
// same length) into `out`, using `weight` as the previous frame's
// contribution: out[i] = round(current[i] * (1 - weight) + prev[i] * weight).
//
// `out`, `current` and `prev` must be separate buffers. Skip blending when
// Off or before the first real previous frame. A zeroed `prev` darkens
// the first frame.
inline void temporal_blend_apply(const uint8_t* current, const uint8_t* prev,
                                  uint8_t* out, std::size_t byte_count,
                                  float weight) {
    const float cur_w = 1.0f - weight;
    for (std::size_t i = 0; i < byte_count; ++i) {
        const float v = static_cast<float>(current[i]) * cur_w +
                         static_cast<float>(prev[i]) * weight;
        out[i] = static_cast<uint8_t>(v + 0.5f);
    }
}

// Blend pixels that alternate on a two-frame cycle, as in the Kolima
// barrier and world-map translucency. Everything else stays sharp.
//
// All three frames are RGB888 and have the same `byte_count`, a multiple
// of 3. Compare whole pixels. A match in one channel can be ordinary
// motion.
//
// Per pixel:
//   - If it matches frame N-2 exactly AND differs from frame N-1, it is
//     alternating on an exact 2-frame period. Blend it with `prev` at
//     `weight` (same meaning as temporal_blend_apply's weight).
//   - Otherwise, copy `current` through unchanged. This covers both static
//     pixels and moving pixels.
//
// `out` must be separate from all inputs. Pass the first two presented
// frames through because there is no N-2 frame yet.
inline void temporal_blend_apply_selective(const uint8_t* current,
                                            const uint8_t* prev,
                                            const uint8_t* prev2, uint8_t* out,
                                            std::size_t byte_count,
                                            float weight) {
    const float cur_w = 1.0f - weight;
    std::size_t i = 0;
    for (; i + 3 <= byte_count; i += 3) {
        const bool matches_prev2 = current[i] == prev2[i] &&
                                    current[i + 1] == prev2[i + 1] &&
                                    current[i + 2] == prev2[i + 2];
        const bool matches_prev = current[i] == prev[i] &&
                                   current[i + 1] == prev[i + 1] &&
                                   current[i + 2] == prev[i + 2];
        if (matches_prev2 && !matches_prev) {
            for (int c = 0; c < 3; ++c) {
                const float v = static_cast<float>(current[i + c]) * cur_w +
                                 static_cast<float>(prev[i + c]) * weight;
                out[i + c] = static_cast<uint8_t>(v + 0.5f);
            }
        } else {
            out[i] = current[i];
            out[i + 1] = current[i + 1];
            out[i + 2] = current[i + 2];
        }
    }
    // Trailing bytes that don't form a full RGB888 pixel (shouldn't happen
    // for a real frame buffer, but keep this safe for any byte_count) pass
    // through unchanged rather than being read out of bounds.
    for (; i < byte_count; ++i) out[i] = current[i];
}

// Item indices for the TemporalBlend setting, all sharing the four weights above:
//   0     = Off
//   1..3  = Flicker only (selective), Light/Medium/Strong
//   4..6  = Whole frame, Light/Medium/Strong
// Flicker-only indexes kTemporalBlendWeights directly; whole-frame subtracts
// 3 first to land on the same four weights. The F1 combo offers only 0..3;
// the whole-frame values are still accepted from an older settings file.
inline constexpr int kTemporalBlendItemCount = 7;

// Default is "Flicker only, Strong": an exact 50/50 blend is what fully
// stabilizes a true 2-frame alternation, and selective mode (unlike a
// whole-frame blend) leaves ordinary motion untouched.
inline constexpr int kTemporalBlendDefaultIndex = 3;

inline constexpr bool temporal_blend_index_is_selective(int index) {
    return index >= 1 && index <= 3;
}

// Weight for a combo index; 0.0f (a no-op weight) for Off or any
// out-of-range index: callers are expected to skip blending on Off the
// same way they do for temporal_blend_apply, see kTemporalBlendWeights above.
inline constexpr float temporal_blend_weight_for_index(int index) {
    return (index >= 1 && index <= 3)   ? kTemporalBlendWeights[index]
           : (index >= 4 && index <= 6) ? kTemporalBlendWeights[index - 3]
                                         : 0.0f;
}

}  // namespace gbarecomp
