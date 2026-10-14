#pragma once

#include <algorithm>
#include <numeric>
#include "view_config.h"

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
    // Adaptive logical sizes can be relatively prime. Fit continuously,
    // rounding the destination to physical pixels rather than whole ratios.
    RoundedAspectFit = 3,
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

    if (mode == ScalingMode::RoundedAspectFit) {
        int width = drawable_width;
        int height = drawable_height;
        if (static_cast<std::int64_t>(drawable_width) * logical_height <=
            static_cast<std::int64_t>(drawable_height) * logical_width) {
            height = static_cast<int>((static_cast<std::int64_t>(width) *
                logical_height + logical_width / 2) / logical_width);
        } else {
            width = static_cast<int>((static_cast<std::int64_t>(height) *
                logical_width + logical_height / 2) / logical_height);
        }
        width = std::clamp(width, 1, drawable_width);
        height = std::clamp(height, 1, drawable_height);
        const int scale = width % logical_width == 0 &&
            height % logical_height == 0 && width / logical_width == height / logical_height
            ? width / logical_width : 0;
        return {(drawable_width - width) / 2, (drawable_height - height) / 2,
                width, height, scale};
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

// Ceil-sized adaptive surfaces may exceed the drawable by less than one
// enlarged pixel. Keep the native scale; clip only those added outer margins.
inline PresentationLayout compute_minimal_sharp_layout(
    int drawable_width, int drawable_height, int logical_width, int logical_height) {
    if (drawable_width <= 0 || drawable_height <= 0 ||
        logical_width <= 0 || logical_height <= 0) return {};
    const int scale = native_integer_scale(drawable_width, drawable_height);
    if (scale < 1)
        return compute_presentation_layout(drawable_width, drawable_height,
                                           logical_width, logical_height);
    const int width = logical_width * scale;
    const int height = logical_height * scale;
    return {(drawable_width - width) / 2, (drawable_height - height) / 2,
            width, height, scale};
}

}  // namespace gbarecomp
