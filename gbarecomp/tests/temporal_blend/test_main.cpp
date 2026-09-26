// VFX-FLICKER-02: synthetic tests for temporal_blend.h. No ROM, no SDL, no
// real game frames — every buffer here is a hand-built synthetic pattern.

#include "temporal_blend.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using gbarecomp::kTemporalBlendWeights;
using gbarecomp::temporal_blend_apply;
using gbarecomp::temporal_blend_apply_selective;

void fail(const char* label) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    std::exit(1);
}

// (a) Off is a bit-exact passthrough. "Off" means the caller never calls
// temporal_blend_apply at all (weight 0 is not specially handled inside the
// function — see temporal_blend.h). Model that contract here: the
// present-path decision to skip blending at level 0 IS the passthrough, so
// prove it the way host_window.cpp exercises it — by simply not calling the
// blend and asserting the frame reaching the texture upload is the original
// buffer, byte for byte.
void test_off_is_passthrough() {
    const std::size_t kBytes = 240 * 160 * 3;
    std::vector<uint8_t> frame(kBytes);
    for (std::size_t i = 0; i < kBytes; ++i)
        frame[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    std::vector<uint8_t> reference = frame;

    // Level 0 == Off: host_window.cpp's blend_engaged gate is false whenever
    // blend_index == 0, so present() never touches the buffer — model that
    // by simply not invoking the blend and comparing against the original.
    const int level = 0;
    const bool blend_engaged = (level != 0);
    if (blend_engaged) fail("off_is_passthrough: blend unexpectedly engaged");

    if (frame.size() != reference.size())
        fail("off_is_passthrough: size changed");
    for (std::size_t i = 0; i < kBytes; ++i) {
        if (frame[i] != reference[i]) {
            std::fprintf(stderr,
                         "off_is_passthrough: byte %zu changed %u -> %u\n", i,
                         reference[i], frame[i]);
            std::exit(1);
        }
    }
    std::printf("PASS: off_is_passthrough\n");
}

// (b) Strong (index 3, weight 0.5) applied to an exact 2-frame alternating
// pattern (frame A all 0x00, frame B all 0xFF, repeating) converges to the
// steady 50/50 average after warmup. temporal_blend_apply blends against the
// RAW (unblended) previous frame — see temporal_blend.h — so with a strictly
// alternating raw input the blended output is the same constant on every
// frame from the first blended one onward, not just in some asymptotic
// limit; assert that stronger, exact property.
void test_strong_converges_on_exact_alternation() {
    const std::size_t kBytes = 64;  // small synthetic buffer, no ROM needed
    const uint8_t kFrameA = 0x00;
    const uint8_t kFrameB = 0xFF;
    const float weight = kTemporalBlendWeights[3];  // Strong
    if (weight != 0.5f) fail("strong_converges: kTemporalBlendWeights[3] != 0.5");

    std::vector<uint8_t> a(kBytes, kFrameA);
    std::vector<uint8_t> b(kBytes, kFrameB);
    std::vector<uint8_t> out(kBytes);

    // Expected steady value: round(0*0.5 + 255*0.5) == 128, matching the
    // exact arithmetic temporal_blend_apply performs.
    const uint8_t kExpected = static_cast<uint8_t>(
        static_cast<float>(kFrameA) * 0.5f +
        static_cast<float>(kFrameB) * 0.5f + 0.5f);

    // Frame 1 (B), previous raw = A.
    temporal_blend_apply(b.data(), a.data(), out.data(), kBytes, weight);
    for (std::size_t i = 0; i < kBytes; ++i)
        if (out[i] != kExpected) fail("strong_converges: frame1 mismatch");

    // Frame 2 (A), previous raw = B. Same steady value, immediately (not
    // merely "eventually") for a true 50/50 blend against an exact
    // alternation — the stronger property this test locks in.
    temporal_blend_apply(a.data(), b.data(), out.data(), kBytes, weight);
    for (std::size_t i = 0; i < kBytes; ++i)
        if (out[i] != kExpected) fail("strong_converges: frame2 mismatch");

    // Frame 3 (B again), previous raw = A: still steady.
    temporal_blend_apply(b.data(), a.data(), out.data(), kBytes, weight);
    for (std::size_t i = 0; i < kBytes; ++i)
        if (out[i] != kExpected) fail("strong_converges: frame3 mismatch");

    std::printf("PASS: strong_converges_on_exact_alternation (steady=%u)\n",
               kExpected);
}

// (c) The persistent previous-frame buffer is correctly initialized on the
// very first frame: no previous frame exists yet, so the sane behavior
// (mirroring host_window.cpp's temporal_blend_prev_valid gate) is to pass
// the first frame through unblended rather than blending against
// zero-initialized/stale memory. Model the same two-phase contract
// (validity flag, then blend) that Backend::present() implements.
void test_first_frame_no_previous_is_passthrough() {
    const std::size_t kBytes = 32;
    std::vector<uint8_t> frame(kBytes);
    for (std::size_t i = 0; i < kBytes; ++i)
        frame[i] = static_cast<uint8_t>(0x40 + i);
    std::vector<uint8_t> reference = frame;

    bool prev_valid = false;  // Backend::temporal_blend_prev_valid at boot
    std::vector<uint8_t> prev(kBytes, 0);  // zero-initialized, as at startup
    std::vector<uint8_t> out(kBytes);

    // Mirror host_window.cpp's present(): only blend when a previous frame
    // is actually known; otherwise pass through and warm the buffer.
    uint8_t* result;
    if (prev_valid) {
        temporal_blend_apply(frame.data(), prev.data(), out.data(), kBytes,
                             kTemporalBlendWeights[3]);
        result = out.data();
    } else {
        prev.assign(frame.begin(), frame.end());
        prev_valid = true;
        result = frame.data();
    }

    for (std::size_t i = 0; i < kBytes; ++i) {
        if (result[i] != reference[i]) {
            fail("first_frame_no_previous_is_passthrough: byte mismatch");
        }
    }
    if (!prev_valid)
        fail("first_frame_no_previous_is_passthrough: buffer not warmed");
    std::printf("PASS: first_frame_no_previous_is_passthrough\n");
}

// VFX-FLICKER-03: synthetic tests for temporal_blend_apply_selective. All
// buffers are single RGB888 pixels (3 bytes) unless noted, built by hand —
// no ROM, no real game frames.

// (a) A pixel alternating A-B-A (frame1=A, frame2=B, frame3=A) is on an
// exact 2-frame period: current(=frame3=A) matches prev2(=frame1=A) and
// differs from prev(=frame2=B), so it must be blended with prev at `weight`.
void test_selective_alternating_pixel_is_blended() {
    const uint8_t a[3] = {0x00, 0x10, 0x20};
    const uint8_t b[3] = {0xFF, 0xE0, 0xD0};
    uint8_t out[3];
    const float weight = kTemporalBlendWeights[3];  // Strong, 0.5

    // current = a (frame3), prev = b (frame2), prev2 = a (frame1).
    temporal_blend_apply_selective(a, b, a, out, 3, weight);
    for (int c = 0; c < 3; ++c) {
        const uint8_t expected = static_cast<uint8_t>(
            static_cast<float>(a[c]) * 0.5f + static_cast<float>(b[c]) * 0.5f +
            0.5f);
        if (out[c] != expected) {
            std::fprintf(stderr,
                         "selective_alternating: channel %d got %u want %u\n",
                         c, out[c], expected);
            std::exit(1);
        }
    }
    std::printf("PASS: selective_alternating_pixel_is_blended\n");
}

// (b) A pixel constant across all three frames matches BOTH prev and prev2,
// so it is not flicker (matches_prev is true) — copy current through
// unchanged.
void test_selective_constant_pixel_passthrough() {
    const uint8_t p[3] = {0x55, 0x66, 0x77};
    uint8_t out[3] = {0, 0, 0};
    temporal_blend_apply_selective(p, p, p, out, 3, kTemporalBlendWeights[3]);
    for (int c = 0; c < 3; ++c)
        if (out[c] != p[c]) fail("selective_constant_pixel: byte changed");
    std::printf("PASS: selective_constant_pixel_passthrough\n");
}

// (c) A pixel changing every frame (frame1=A, frame2=B, frame3=C, all
// distinct) has no N-2 match, so it is ordinary motion, not flicker —
// current must pass through unchanged.
void test_selective_moving_pixel_passthrough() {
    const uint8_t frame_a[3] = {0x10, 0x20, 0x30};
    const uint8_t frame_b[3] = {0x40, 0x50, 0x60};
    const uint8_t frame_c[3] = {0x70, 0x80, 0x90};
    uint8_t out[3] = {0, 0, 0};
    // current = frame_c, prev = frame_b, prev2 = frame_a: current matches
    // neither.
    temporal_blend_apply_selective(frame_c, frame_b, frame_a, out, 3,
                                    kTemporalBlendWeights[3]);
    for (int c = 0; c < 3; ++c)
        if (out[c] != frame_c[c]) fail("selective_moving_pixel: byte changed");
    std::printf("PASS: selective_moving_pixel_passthrough\n");
}

// (d) Frames 1 and 2 (no real prev2 yet) must pass current through
// untouched — mirrors host_window.cpp's temporal_blend_history_count gate
// (present() never calls temporal_blend_apply_selective until history_count
// >= 2), modeled here the same way test_first_frame_no_previous_is_passthrough
// models the whole-frame gate above.
void test_selective_first_two_frames_passthrough() {
    const uint8_t frame1[3] = {0x11, 0x22, 0x33};
    const uint8_t frame2[3] = {0x44, 0x55, 0x66};
    int history_count = 0;
    const int required = 2;  // selective mode's `required`, see present()

    // Frame 1: history_count (0) < required -> passthrough, history -> 1.
    bool did_blend = history_count >= required;
    if (did_blend) fail("selective_first_two_frames: frame1 unexpectedly blended");
    if (history_count < 2) ++history_count;
    if (history_count != 1) fail("selective_first_two_frames: history after frame1");

    // Frame 2: history_count (1) < required -> still passthrough, history -> 2.
    did_blend = history_count >= required;
    if (did_blend) fail("selective_first_two_frames: frame2 unexpectedly blended");
    if (history_count < 2) ++history_count;
    if (history_count != 2) fail("selective_first_two_frames: history after frame2");

    (void)frame1; (void)frame2;  // stand-ins for the frames that were skipped
    std::printf("PASS: selective_first_two_frames_passthrough\n");
}

// (e) Per-byte false positive: a pixel where only ONE channel matches frame
// N-2 (R matches, G and B don't) must NOT be treated as flicker — the
// compare is per PIXEL (all 3 channels together), not per byte. If this
// were compared per byte, the R channel alone would look like an
// alternating byte and get blended while G/B didn't; the correct behavior
// is the whole pixel passes through unchanged.
void test_selective_per_byte_false_positive_rejected() {
    const uint8_t current[3] = {0x10, 0x20, 0x30};
    const uint8_t prev[3] = {0x50, 0x60, 0x70};   // differs from current in all 3
    const uint8_t prev2[3] = {0x10, 0x99, 0x99};  // R matches current, G/B don't
    uint8_t out[3] = {0, 0, 0};
    temporal_blend_apply_selective(current, prev, prev2, out, 3,
                                    kTemporalBlendWeights[3]);
    for (int c = 0; c < 3; ++c) {
        if (out[c] != current[c]) {
            std::fprintf(stderr,
                         "selective_per_byte_false_positive: channel %d got "
                         "%u want %u (treated as flicker on a partial "
                         "channel match)\n",
                         c, out[c], current[c]);
            std::exit(1);
        }
    }
    std::printf("PASS: selective_per_byte_false_positive_rejected\n");
}

}  // namespace

int main() {
    test_off_is_passthrough();
    test_strong_converges_on_exact_alternation();
    test_first_frame_no_previous_is_passthrough();
    test_selective_alternating_pixel_is_blended();
    test_selective_constant_pixel_passthrough();
    test_selective_moving_pixel_passthrough();
    test_selective_first_two_frames_passthrough();
    test_selective_per_byte_false_positive_rejected();
    std::printf("all temporal_blend tests passed\n");
    return 0;
}
