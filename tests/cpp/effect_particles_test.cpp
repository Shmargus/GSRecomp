// Does the spark stamper reproduce the rules measured off the game?
//
// The three that matter, all from FACTS.md, 2026-09-18:
//   * a spark is drawn from its CENTRE, at position - size/2;
//   * pixels ADD and saturate at 63, which is what makes a burst glow;
//   * a spark that falls partly outside is drawn for the part that fits.
//
// Plus the one that proves the widening is worth anything: sparks the game's
// 128-wide canvas would have thrown away must land in a wider one.

#include "effect_particles.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <vector>

namespace {

// Spark artwork shaped like the game's: index 0 is one pixel, index 3 is 4x4,
// laid out at the offsets the game's own table gives.
std::vector<std::uint8_t> make_art() {
    std::vector<std::uint8_t> art(78, 0u);
    art[0] = 20;                                  // index 0: 1x1
    for (int i = 1; i <= 4; ++i) art[i] = 10;     // index 1: 2x2
    for (int i = 5; i <= 13; ++i) art[i] = 8;     // index 2: 3x3
    for (int i = 14; i <= 29; ++i) art[i] = 30;   // index 3: 4x4
    return art;
}

void test_single_pixel_lands_on_its_centre() {
    gsr::EffectCanvas canvas;
    canvas.reset(16, 16);
    const std::vector<std::uint8_t> art = make_art();
    gsr::stamp_spark(&canvas, {5, 7, 0}, art.data(), art.size());
    assert(canvas.pixels[7 * 16 + 5] == 20);
    // and nothing else moved
    int lit = 0;
    for (std::uint8_t p : canvas.pixels) lit += (p != 0);
    assert(lit == 1);
}

void test_larger_spark_is_centred() {
    gsr::EffectCanvas canvas;
    canvas.reset(16, 16);
    const std::vector<std::uint8_t> art = make_art();
    // 4x4 drawn at (8,8) covers 6..9 on both axes: centre minus half the size.
    gsr::stamp_spark(&canvas, {8, 8, 3}, art.data(), art.size());
    assert(canvas.pixels[8 * 16 + 6] == 30);
    assert(canvas.pixels[9 * 16 + 9] == 30);
    assert(canvas.pixels[8 * 16 + 5] == 0);
    assert(canvas.pixels[8 * 16 + 10] == 0);
}

void test_pixels_add_and_saturate() {
    gsr::EffectCanvas canvas;
    canvas.reset(8, 8);
    const std::vector<std::uint8_t> art = make_art();
    // Two overlapping sparks add: 20 + 20 = 40, still under the ceiling.
    gsr::stamp_spark(&canvas, {4, 4, 0}, art.data(), art.size());
    gsr::stamp_spark(&canvas, {4, 4, 0}, art.data(), art.size());
    assert(canvas.pixels[4 * 8 + 4] == 40);
    // Piling on more must stop at 63 rather than wrapping to a dark value.
    for (int i = 0; i < 10; ++i)
        gsr::stamp_spark(&canvas, {4, 4, 0}, art.data(), art.size());
    assert(canvas.pixels[4 * 8 + 4] == gsr::kMaxIntensity);
}

void test_partly_outside_draws_the_part_that_fits() {
    gsr::EffectCanvas canvas;
    canvas.reset(16, 16);
    const std::vector<std::uint8_t> art = make_art();
    // A 4x4 centred at x=0 covers -2..1, so only columns 0 and 1 survive.
    gsr::stamp_spark(&canvas, {0, 8, 3}, art.data(), art.size());
    assert(canvas.pixels[8 * 16 + 0] == 30);
    assert(canvas.pixels[8 * 16 + 1] == 30);
    assert(canvas.pixels[8 * 16 + 2] == 0);
}

void test_a_wrong_art_pointer_draws_nothing() {
    gsr::EffectCanvas canvas;
    canvas.reset(8, 8);
    const std::vector<std::uint8_t> art = make_art();
    // Claiming a 4x4 sprite against a block too short to hold one must be
    // refused, not read past the end.
    gsr::stamp_spark(&canvas, {4, 4, 3}, art.data(), 8);
    for (std::uint8_t p : canvas.pixels) assert(p == 0);
}

void test_widening_recovers_what_the_game_threw_away() {
    const std::vector<std::uint8_t> art = make_art();
    // Sparks spread across a span wider than the console's canvas, the way a
    // burst does once it has been running for a while.
    std::vector<gsr::EffectSpark> sparks;
    for (int x = -40; x < 168; x += 8) sparks.push_back({x + 60, 64, 0});

    const int kept_by_game = gsr::sparks_inside(sparks.data(),
                                                static_cast<int>(sparks.size()),
                                                128, 128, 0, 0);
    gsr::EffectCanvas wide;
    wide.reset(224, 128);
    const int drawn_by_us = gsr::stamp_sparks(
        &wide, sparks.data(), static_cast<int>(sparks.size()), art.data(),
        art.size());

    // The game keeps only what fits its 128; we keep the lot.
    assert(kept_by_game < static_cast<int>(sparks.size()));
    assert(drawn_by_us == static_cast<int>(sparks.size()));
    std::printf("  the game would keep %d of %d sparks; we draw %d\n",
                kept_by_game, static_cast<int>(sparks.size()), drawn_by_us);
}

void test_captured_art_offset_is_used() {
    gsr::EffectCanvas canvas;
    canvas.reset(8, 8);
    const std::uint8_t art[] = {1, 2, 3, 41};
    const gsr::EffectSpark spark{4, 4, 0, 3};
    assert(gsr::stamp_sparks(&canvas, &spark, 1, art, sizeof art) == 1);
    assert(canvas.pixels[4 * 8 + 4] == 41);
}

void test_draw_count_requires_nontransparent_art() {
    gsr::EffectCanvas canvas;
    canvas.reset(8, 8);
    const std::uint8_t art[] = {0};
    gsr::EffectSpark spark{4, 4, 0};
    assert(gsr::stamp_sparks(&canvas, &spark, 1, art, sizeof art) == 0);
    spark.art_offset = 1; // outside the supplied artwork
    assert(gsr::stamp_sparks(&canvas, &spark, 1, art, sizeof art) == 0);
}

void test_large_stamp_crosses_the_original_canvas_edge() {
    gsr::EffectCanvas canvas;
    canvas.reset(160, 128);
    const std::vector<std::uint8_t> art(32 * 64, 20);
    gsr::EffectSpark stamp{128, 64, 0, 0};
    stamp.width = 32;
    stamp.height = 64;
    assert(gsr::stamp_sparks(&canvas, &stamp, 1, art.data(), art.size()) == 1);
    int lit = 0;
    for (auto p : canvas.pixels) lit += p != 0;
    assert(lit == 32 * 64);
    assert(canvas.pixels[64 * 160 + 143] == 20);
    assert(canvas.pixels[64 * 160 + 144] == 0);
}

void test_maximum_and_flipped_rectangular_stamps() {
    gsr::EffectCanvas canvas;
    canvas.reset(4, 4);
    std::fill(canvas.pixels.begin(), canvas.pixels.end(), 5);
    const std::uint8_t art[] = {1, 10, 20, 30, 40, 50};
    gsr::EffectSpark stamp{2, 2, 0, 0};
    stamp.width = 2;
    stamp.height = 3;
    stamp.flip_x = stamp.flip_y = true;
    stamp.blend = gsr::EffectBlend::Maximum;
    assert(gsr::stamp_spark(&canvas, stamp, art, sizeof art));
    assert(canvas.pixels[1 * 4 + 1] == 50);
    assert(canvas.pixels[1 * 4 + 2] == 40);
    assert(canvas.pixels[3 * 4 + 1] == 10);
    assert(canvas.pixels[3 * 4 + 2] == 5);
    gsr::stamp_spark(&canvas, stamp, art, sizeof art);
    assert(canvas.pixels[1 * 4 + 1] == 50);
}

void test_expanding_canvas_preserves_and_fades_history() {
    gsr::EffectCanvas canvas;
    const std::uint8_t art[] = {63};
    gsr::EffectSpark first{-2, -1, 0, 0};
    assert(gsr::stamp_spark_expanding(&canvas, first, art, sizeof art));
    canvas.fade();
    gsr::EffectSpark next{3, 2, 0, 0};
    assert(gsr::stamp_spark_expanding(&canvas, next, art, sizeof art));
    auto at = [&](int x, int y) {
        return canvas.pixels[(y + canvas.origin_y) * canvas.width + x + canvas.origin_x];
    };
    assert(at(-2, -1) == 47);
    assert(at(3, 2) == 63);
    canvas.fade();
    assert(at(-2, -1) == 35);
    assert(at(3, 2) == 47);
    canvas.fade(true);
    assert(at(-2, -1) == 17);
    assert(at(3, 2) == 23);
    canvas.clear();
    assert(at(-2, -1) == 0 && at(3, 2) == 0);
}

}  // namespace

int main() {
    test_single_pixel_lands_on_its_centre();
    test_larger_spark_is_centred();
    test_pixels_add_and_saturate();
    test_partly_outside_draws_the_part_that_fits();
    test_a_wrong_art_pointer_draws_nothing();
    test_widening_recovers_what_the_game_threw_away();
    test_captured_art_offset_is_used();
    test_draw_count_requires_nontransparent_art();
    test_large_stamp_crosses_the_original_canvas_edge();
    test_maximum_and_flipped_rectangular_stamps();
    test_expanding_canvas_preserves_and_fades_history();
    std::printf("effect_particles: all checks passed\n");
    return 0;
}
