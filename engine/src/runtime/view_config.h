#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gbarecomp {

enum ViewAspect {
    ViewAspectOriginal = 0,
    ViewAspectFourThree,
    ViewAspectSixteenTen,
    ViewAspectSixteenNine,
    ViewAspectWindow,
    ViewAspectMinimalSharpPixels,
};
inline constexpr int kViewAspectCount = 6;
inline constexpr const char* kViewAspectLabels[kViewAspectCount] = {
    "3:2 (original)", "4:3", "16:10", "16:9", "Window",
    "Minimal Sharp Pixels",
};
inline constexpr const char* kViewAspectKeys[kViewAspectCount] = {
    "3:2", "4:3", "16:10", "16:9", "window", "sharp-fill",
};
inline constexpr int kViewModeCount = 3;
inline constexpr const char* kViewModeLabels[kViewModeCount] = {
    "Original", "Wide", "Extra Wide",
};
inline constexpr std::uint16_t kViewModeWidths[kViewModeCount] = {240, 360, 480};
inline constexpr std::uint16_t kViewModeHeights[kViewModeCount] = {160, 240, 320};

inline bool view_aspect_dynamic(int aspect) {
    return aspect == ViewAspectWindow ||
           aspect == ViewAspectMinimalSharpPixels;
}

inline int native_integer_scale(int drawable_width, int drawable_height) {
    return std::max(0, std::min(drawable_width / 240, drawable_height / 160));
}

struct ViewCameraInsets {
    std::int64_t before = 0;
    std::int64_t after = 0;
};

// Allocate the available room span around the native camera. Units may be
// pixels or the game's fixed-point coordinates; preserve odd margin splits.
inline ViewCameraInsets resolve_view_camera_insets(
    std::int64_t spare, std::int64_t before, std::int64_t after) {
    if (spare <= 0) return {};
    const auto center = std::clamp((spare + before - after) / 2,
                                   std::int64_t{0}, spare);
    return {std::min(before, center), std::min(after, spare - center)};
}

struct ViewGeometry {
    std::uint32_t width = 240;
    std::uint32_t height = 160;
    std::uint32_t extra_left = 0;
    std::uint32_t extra_right = 0;
    std::uint32_t extra_top = 0;
    std::uint32_t extra_bottom = 0;
};

// The selected mode is an envelope, not a forced zoom. Preserve the complete
// native frame when an extreme aspect cannot fit inside that envelope.
inline ViewGeometry resolve_aspect_view_geometry(
    int bound_width, int bound_height, int aspect,
    int drawable_width, int drawable_height) {
    const int maximum_width = std::max(240, bound_width);
    const int maximum_height = std::max(160, bound_height);
    int width = maximum_width;
    int height = maximum_height;
    if (aspect == ViewAspectMinimalSharpPixels) {
        const int scale = native_integer_scale(drawable_width, drawable_height);
        if (scale < 1) {
            width = 240;
            height = 160;
        } else {
            width = static_cast<int>((static_cast<std::int64_t>(drawable_width) +
                                      scale - 1) / scale);
            height = static_cast<int>((static_cast<std::int64_t>(drawable_height) +
                                       scale - 1) / scale);
        }
    } else {
        int numerator = 0;
        int denominator = 0;
        switch (aspect) {
        case ViewAspectFourThree: numerator = 4; denominator = 3; break;
        case ViewAspectSixteenTen: numerator = 16; denominator = 10; break;
        case ViewAspectSixteenNine: numerator = 16; denominator = 9; break;
        case ViewAspectWindow:
            numerator = drawable_width;
            denominator = drawable_height;
            break;
        default: break;
        }
        if (numerator > 0 && denominator > 0) {
            const auto round_ratio = [](int value, int n, int d) {
                return (static_cast<std::int64_t>(value) * n + d / 2) / d;
            };
            const std::int64_t fitted_height =
                round_ratio(width, denominator, numerator);
            if (fitted_height <= maximum_height) {
                height = static_cast<int>(fitted_height);
            } else {
                width = static_cast<int>(round_ratio(height, numerator, denominator));
            }
        }
    }
    width = std::clamp(width, 240, maximum_width);
    height = std::clamp(height, 160, maximum_height);
    const auto extra_width = static_cast<std::uint32_t>(width - 240);
    const auto extra_height = static_cast<std::uint32_t>(height - 160);
    return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
            extra_width / 2, extra_width - extra_width / 2,
            extra_height / 2, extra_height - extra_height / 2};
}

inline bool legacy_extra_to_view_width(int extra_per_side, int* width) {
    if (!width || extra_per_side < 0 ||
        extra_per_side > (std::numeric_limits<int>::max() - 240) / 2) {
        return false;
    }
    *width = 240 + 2 * extra_per_side;
    return true;
}

// Pure policy resolver kept separate from the renderer so capability and
// faithful-default behavior can be unit-tested without launching a game.
// Width and height are resolved together: accepting a 360-wide request must
// not accidentally leave the old 160-line surface active.
inline ViewGeometry resolve_view_geometry(
    int requested_width, int requested_height,
    std::uint32_t game_max_width, std::uint32_t game_max_height,
    bool development_override,
    std::uint32_t engine_max_width, std::uint32_t engine_max_height) {
    constexpr std::uint32_t kNativeWidth = 240;
    constexpr std::uint32_t kNativeHeight = 160;
    const std::uint32_t engine_max = std::max(kNativeWidth, engine_max_width);
    const std::uint32_t engine_max_h =
        std::max(kNativeHeight, engine_max_height);
    const std::uint32_t opted_in_max =
        std::clamp(game_max_width, kNativeWidth, engine_max);
    const std::uint32_t opted_in_max_h =
        std::clamp(game_max_height, kNativeHeight, engine_max_h);
    const std::uint32_t allowed_max =
        development_override ? engine_max : opted_in_max;
    const std::uint32_t allowed_max_h =
        development_override ? engine_max_h : opted_in_max_h;
    const std::uint32_t requested = requested_width < 240
        ? kNativeWidth
        : static_cast<std::uint32_t>(requested_width);
    const std::uint32_t requested_h = requested_height < 160
        ? kNativeHeight
        : static_cast<std::uint32_t>(requested_height);
    const std::uint32_t width = std::min(requested, allowed_max);
    const std::uint32_t height = std::min(requested_h, allowed_max_h);
    const std::uint32_t extra = width - kNativeWidth;
    const std::uint32_t extra_h = height - kNativeHeight;
    return {width, height,
            extra / 2u, extra - extra / 2u,
            extra_h / 2u, extra_h - extra_h / 2u};
}

// Backward-compatible horizontal-only overload. Existing callers for games
// that have not opted into vertical expansion retain the native 160-line
// surface and the exact old width policy.
inline ViewGeometry resolve_view_geometry(int requested_width,
                                          std::uint32_t game_max_width,
                                          bool development_override,
                                          std::uint32_t engine_max_width) {
    return resolve_view_geometry(requested_width, 160,
                                 game_max_width, 160,
                                 development_override,
                                 engine_max_width, 160);
}

// Convert a host drawable aspect ratio into a logical GBA view width while
// keeping the authentic 160-line height. Narrower-than-native windows never
// crop the game; wider windows reveal more horizontal content up to game_max.
inline std::uint32_t resize_driven_view_width(int drawable_width,
                                              int drawable_height,
                                              std::uint32_t game_max_width,
                                              std::uint32_t engine_max_width) {
    constexpr std::uint32_t kNativeWidth = 240;
    constexpr std::uint32_t kNativeHeight = 160;
    const std::uint32_t maximum = std::clamp(
        game_max_width, kNativeWidth,
        std::max(kNativeWidth, engine_max_width));
    if (drawable_width <= 0 || drawable_height <= 0) return kNativeWidth;

    // Round to the nearest logical pixel. Use 64-bit arithmetic so very large
    // desktop dimensions cannot overflow before the clamp.
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(drawable_width) * kNativeHeight;
    const std::uint64_t rounded =
        (scaled + static_cast<std::uint64_t>(drawable_height) / 2u) /
        static_cast<std::uint64_t>(drawable_height);
    return static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(rounded, kNativeWidth, maximum));
}

}  // namespace gbarecomp
