#pragma once

#include <algorithm>
#include <numeric>

namespace gbarecomp {

// How a fixed logical framebuffer is fitted into a resizable drawable.
enum class ScalingMode {
    // Whole-pixel multiples only, centred, remainder letterboxed. Every source
    // pixel gets exactly the same number of destination pixels, which is the
    // only way pixel art survives an arbitrary window size. Default.
    IntegerLetterbox = 0,
    // Maximise the destination at the exact logical aspect ratio. Fills more of
    // the window, at the cost of uneven pixel duplication at most sizes.
    AspectFill = 1,
    // Fill the drawable completely, ignoring the logical aspect ratio.
    Stretch = 2,
};

// Destination rectangle for presenting a fixed logical framebuffer inside a
// resizable drawable. The logical pixels themselves are never resized here;
// this describes only the SDL presentation copy.
struct PresentationLayout {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int integer_scale = 0;  // >0 when width/height are whole-pixel multiples.
};

inline PresentationLayout compute_aspect_fill_layout(int drawable_width,
                                                      int drawable_height,
                                                      int logical_width,
                                                      int logical_height) {
    // Work in whole reduced-aspect units to avoid floating-point drift and
    // one-axis stretch, while still making every ordinary drag-resize visibly
    // change the presentation. Exact logical multiples retain integer scaling.
    const int divisor = std::gcd(logical_width, logical_height);
    const int aspect_width = logical_width / divisor;
    const int aspect_height = logical_height / divisor;
    const int units = std::min(drawable_width / aspect_width,
                               drawable_height / aspect_height);
    if (units < 1) return {};
    const int width = aspect_width * units;
    const int height = aspect_height * units;
    const int integer_scale = units % divisor == 0 ? units / divisor : 0;

    return {
        (drawable_width - width) / 2,
        (drawable_height - height) / 2,
        width,
        height,
        integer_scale,
    };
}

inline PresentationLayout compute_presentation_layout(
    int drawable_width,
    int drawable_height,
    int logical_width,
    int logical_height,
    ScalingMode mode = ScalingMode::IntegerLetterbox) {
    if (drawable_width <= 0 || drawable_height <= 0 ||
        logical_width <= 0 || logical_height <= 0) {
        return {};
    }

    if (mode == ScalingMode::Stretch) {
        return {0, 0, drawable_width, drawable_height, 0};
    }

    if (mode == ScalingMode::IntegerLetterbox) {
        const int scale = std::min(drawable_width / logical_width,
                                   drawable_height / logical_height);
        // A drawable smaller than one whole logical frame cannot be shown at an
        // integer scale at all. Cropping the guest image is never acceptable,
        // so fall back to the exact-aspect fit rather than showing nothing.
        if (scale >= 1) {
            const int width = logical_width * scale;
            const int height = logical_height * scale;
            return {
                (drawable_width - width) / 2,
                (drawable_height - height) / 2,
                width,
                height,
                scale,
            };
        }
    }

    return compute_aspect_fill_layout(drawable_width, drawable_height,
                                      logical_width, logical_height);
}

}  // namespace gbarecomp
