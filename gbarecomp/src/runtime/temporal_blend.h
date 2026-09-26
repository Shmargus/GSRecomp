// temporal_blend.h — VFX-FLICKER-02: optional whole-frame temporal blend
// ("LCD ghosting"), applied to the final presented RGB888 frame.
//
// Real GBA hardware's LCD has a slow pixel response, so two frames drawn in
// quick succession visibly blend together. Some Golden Sun effects (the
// Kolima blue barrier, the world map) exploit this deliberately, alternating
// their draw every other frame as a translucency trick; a renderer that
// switches pixels instantly instead of blending them makes that alternation
// visibly shimmer/flicker. This is an opt-in, default-OFF presentation-only
// fix: blend the current frame with the previous PRESENTED frame so an exact
// 2-frame alternation can be fully stabilized (Strong = true 50/50 average).
//
// Host-side and presentation-only: nothing here is read by the guest, and it
// runs after the PPU has produced the frame, so it never affects emulation,
// verify/frame-hash paths, or timing. Header-only and SDL-free so it can be
// unit-tested against synthetic buffers without a display or a ROM (see
// tests/temporal_blend/test_main.cpp).

#pragma once

#include <cstddef>
#include <cstdint>

namespace gbarecomp {

// Combo values, shared by host_config_ui.cpp (draws the "Temporal blend"
// combo) and host_window.cpp (applies the weight at present time).
// Index -> weight is the PREVIOUS presented frame's contribution to the
// blended output. 0 = Off (the caller must skip blending entirely — this
// module makes no attempt at a zero-weight fast path, see
// temporal_blend_apply below). 3 = Strong is an exact 50/50 average, which
// fully stabilizes content the game alternates on an exact 2-frame period.
// Light/Medium are lighter approximations of the same LCD-ghosting effect
// for content that isn't an exact 2-frame alternation and would look
// smeary at a full 50/50 blend.
inline constexpr int kTemporalBlendLevelCount = 4;  // Off/Light/Medium/Strong
inline constexpr float kTemporalBlendWeights[kTemporalBlendLevelCount] = {
    0.0f,    // Off — never actually applied; caller passes through instead.
    0.175f,  // Light  (~17.5% previous-frame contribution)
    0.325f,  // Medium (~32.5% previous-frame contribution)
    0.5f,    // Strong — exact 50/50, matches an exact 2-frame alternation.
};

// Blends `current` (an RGB888 buffer, `byte_count` bytes) with `prev` (the
// same length) into `out`, using `weight` as the previous frame's
// contribution: out[i] = round(current[i] * (1 - weight) + prev[i] * weight).
//
// `out` must not alias `current` or `prev` (the caller owns three distinct
// buffers — see host_window.cpp's Backend::temporal_blend_out /
// temporal_blend_prev). Callers are expected to skip calling this entirely
// when weight == 0 (Off) or when there is no previous frame yet; this
// function does not special-case either — it always performs the blend
// arithmetic on whatever it is given, so an all-zero `prev` from a caller
// that ignored the "no previous frame yet" rule would visibly darken frame 1
// instead of passing it through untouched.
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

// VFX-FLICKER-03: selective flicker filter. The whole-frame blend above
// smears ordinary motion along with the alternating content it was meant to
// fix. This variant only blends a pixel that is alternating on an exact
// 2-frame period — the same trick the Kolima barrier and world map
// translucency use — and passes every other pixel through untouched, so
// normal movement stays sharp.
//
// `current` (frame N), `prev` (frame N-1) and `prev2` (frame N-2) must all
// be the same length (`byte_count`, a multiple of 3 for RGB888). Comparison
// is PER PIXEL — all 3 channels of a pixel together, never per byte: a
// per-byte compare would treat a pixel where only one channel happens to
// match frame N-2 as flicker, even though the pixel as a whole is not part
// of any 2-frame alternation (ordinary motion can produce that by
// coincidence).
//
// Per pixel: if it matches frame N-2 exactly AND differs from frame N-1, it
// is alternating on an exact 2-frame period, so blend it with `prev` at
// `weight` (same meaning as temporal_blend_apply's weight). Otherwise
// (static pixels and moving pixels both) copy `current` through unchanged.
//
// `out` must not alias `current`, `prev`, or `prev2`. Callers must not call
// this until both `prev` and `prev2` hold real previous PRESENTED frames —
// see host_window.cpp's present(), which passes frames 1 and 2 through
// untouched instead (no prev2 yet to compare against).
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

// Combo item indices for host_config_ui.cpp's "Temporal blend" combo. It
// offers Off, three selective ("Flicker only") levels, and three whole-frame
// levels, all sharing the four weights above:
//   0     = Off
//   1..3  = Flicker only (selective) — Light/Medium/Strong
//   4..6  = Whole frame              — Light/Medium/Strong
// Flicker-only indexes kTemporalBlendWeights directly; whole-frame subtracts
// 3 first to land on the same four weights.
inline constexpr int kTemporalBlendItemCount = 7;

// Default is "Flicker only — Strong": an exact 50/50 blend is what fully
// stabilizes a true 2-frame alternation, and selective mode (unlike the
// whole-frame default this replaces) leaves ordinary motion untouched.
inline constexpr int kTemporalBlendDefaultIndex = 3;

inline constexpr bool temporal_blend_index_is_selective(int index) {
    return index >= 1 && index <= 3;
}

// Weight for a combo index; 0.0f (a no-op weight) for Off or any
// out-of-range index — callers are expected to skip blending on Off the
// same way they do for temporal_blend_apply, see kTemporalBlendWeights above.
inline constexpr float temporal_blend_weight_for_index(int index) {
    return (index >= 1 && index <= 3)   ? kTemporalBlendWeights[index]
           : (index >= 4 && index <= 6) ? kTemporalBlendWeights[index - 3]
                                         : 0.0f;
}

}  // namespace gbarecomp
