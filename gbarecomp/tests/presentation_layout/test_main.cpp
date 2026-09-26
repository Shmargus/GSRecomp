#include "presentation_layout.h"

#include <cstdio>
#include <cstdlib>

namespace {

using gbarecomp::ScalingMode;

void expect_layout(int drawable_width, int drawable_height,
                   int logical_width, int logical_height,
                   ScalingMode mode,
                   int x, int y, int width, int height, int integer_scale,
                   const char* label) {
    const auto got = gbarecomp::compute_presentation_layout(
        drawable_width, drawable_height, logical_width, logical_height, mode);
    if (got.x != x || got.y != y || got.width != width ||
        got.height != height || got.integer_scale != integer_scale) {
        std::fprintf(stderr,
                     "%s: got {%d,%d %dx%d scale=%d}, expected "
                     "{%d,%d %dx%d scale=%d}\n",
                     label, got.x, got.y, got.width, got.height,
                     got.integer_scale, x, y, width, height, integer_scale);
        std::exit(1);
    }
    if (mode != ScalingMode::Stretch && got.width != 0 &&
        static_cast<long long>(got.width) * logical_height !=
            static_cast<long long>(got.height) * logical_width) {
        std::fprintf(stderr, "%s: destination aspect ratio changed\n", label);
        std::exit(1);
    }
}

// The property that actually matters for pixel art: under nearest-neighbour,
// every source pixel must occupy the SAME number of destination pixels. A
// non-integer scale gives some columns N and others N+1, which is the visible
// wobble this mode exists to prevent.
void expect_uniform_pixels(int drawable_width, int drawable_height,
                           int logical_width, int logical_height,
                           const char* label) {
    const auto got = gbarecomp::compute_presentation_layout(
        drawable_width, drawable_height, logical_width, logical_height,
        ScalingMode::IntegerLetterbox);
    if (got.width <= 0) return;  // window smaller than one logical frame
    long long first = -1;
    for (int i = 0; i < logical_width; ++i) {
        const long long span =
            static_cast<long long>(i + 1) * got.width / logical_width -
            static_cast<long long>(i) * got.width / logical_width;
        if (first < 0) first = span;
        if (span != first) {
            std::fprintf(stderr,
                         "%s: non-uniform pixel widths (%lld vs %lld) at "
                         "%dx%d -> %dx%d\n",
                         label, first, span, drawable_width, drawable_height,
                         got.width, got.height);
            std::exit(1);
        }
    }
    // The destination must also stay inside the drawable.
    if (got.x < 0 || got.y < 0 ||
        got.x + got.width > drawable_width ||
        got.y + got.height > drawable_height) {
        std::fprintf(stderr, "%s: destination escapes the drawable\n", label);
        std::exit(1);
    }
}

}  // namespace

int main() {
    // Integer letterbox (the default) — exact multiples are unchanged.
    expect_layout(720, 480, 240, 160, ScalingMode::IntegerLetterbox,
                  0, 0, 720, 480, 3, "integer exact 3x");
    // ...and non-multiples now snap DOWN to a whole scale and letterbox,
    // instead of the old 999x666 uneven fill.
    expect_layout(1000, 700, 240, 160, ScalingMode::IntegerLetterbox,
                  20, 30, 960, 640, 4, "integer letterboxed 4x");
    expect_layout(1920, 1080, 240, 160, ScalingMode::IntegerLetterbox,
                  240, 60, 1440, 960, 6, "integer 1080p 6x");
    expect_layout(3440, 1440, 240, 160, ScalingMode::IntegerLetterbox,
                  640, 0, 2160, 1440, 9, "integer ultrawide 1440p 9x");
    expect_layout(3440, 1440, 480, 320, ScalingMode::IntegerLetterbox,
                  760, 80, 1920, 1280, 4, "native 2x ultrawide layout");
    // 720p is height-limited: 5x would need 800 lines, so it settles at 4x.
    expect_layout(1280, 720, 240, 160, ScalingMode::IntegerLetterbox,
                  160, 40, 960, 640, 4, "integer 720p 4x height-limited");
    // A drawable smaller than one logical frame must not show nothing, and must
    // not crop: fall back to the exact-aspect fit.
    expect_layout(200, 150, 240, 160, ScalingMode::IntegerLetterbox,
                  1, 9, 198, 132, 0, "integer sub-native falls back to fit");

    // Uniform-pixel property across a sweep of real window sizes.
    expect_uniform_pixels(800, 600, 240, 160, "uniform 800x600");
    expect_uniform_pixels(1000, 700, 240, 160, "uniform 1000x700");
    expect_uniform_pixels(1280, 720, 240, 160, "uniform 720p");
    expect_uniform_pixels(1366, 768, 240, 160, "uniform 1366x768");
    expect_uniform_pixels(1600, 900, 240, 160, "uniform 1600x900");
    expect_uniform_pixels(1920, 1080, 240, 160, "uniform 1080p");
    expect_uniform_pixels(2560, 1440, 240, 160, "uniform 1440p");
    expect_uniform_pixels(3440, 1440, 240, 160, "uniform ultrawide 1440p");
    expect_uniform_pixels(3840, 2160, 240, 160, "uniform 4k");
    expect_uniform_pixels(1152, 640, 288, 160, "uniform widescreen 4x");
    expect_uniform_pixels(1280, 720, 288, 160, "uniform widescreen 720p");

    // Aspect fill keeps the previous behaviour exactly, for anyone who prefers
    // maximum window coverage over uniform pixels.
    expect_layout(720, 480, 240, 160, ScalingMode::AspectFill,
                  0, 0, 720, 480, 3, "fill exact 3x");
    expect_layout(1000, 700, 240, 160, ScalingMode::AspectFill,
                  0, 17, 999, 666, 0, "fill responsive");
    expect_layout(1152, 640, 288, 160, ScalingMode::AspectFill,
                  0, 0, 1152, 640, 4, "fill widescreen exact 4x");
    expect_layout(1280, 720, 288, 160, ScalingMode::AspectFill,
                  1, 5, 1278, 710, 0, "fill widescreen responsive");
    expect_layout(1000, 700, 288, 160, ScalingMode::AspectFill,
                  0, 72, 999, 555, 0, "fill widescreen non-integer");
    expect_layout(200, 150, 240, 160, ScalingMode::AspectFill,
                  1, 9, 198, 132, 0, "fill exact-ratio downscale");
    expect_layout(200, 120, 288, 160, ScalingMode::AspectFill,
                  1, 5, 198, 110, 0, "fill widescreen downscale");

    // Stretch ignores aspect entirely and covers the drawable.
    expect_layout(1000, 700, 240, 160, ScalingMode::Stretch,
                  0, 0, 1000, 700, 0, "stretch fills drawable");

    // Invalid drawables are still empty in every mode.
    expect_layout(0, 480, 240, 160, ScalingMode::IntegerLetterbox,
                  0, 0, 0, 0, 0, "invalid drawable integer");
    expect_layout(0, 480, 240, 160, ScalingMode::AspectFill,
                  0, 0, 0, 0, 0, "invalid drawable fill");
    expect_layout(640, 0, 240, 160, ScalingMode::Stretch,
                  0, 0, 0, 0, 0, "invalid drawable stretch");

    std::puts("presentation_layout_tests: PASS");
    return 0;
}
