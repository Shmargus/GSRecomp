#include "view_config.h"

#include <cassert>
#include <utility>

int main() {
    using gbarecomp::resolve_view_geometry;

    const auto native = resolve_view_geometry(240, 160, 360, 240, false,
                                               480, 240);
    assert(native.width == 240 && native.height == 160);
    assert(native.extra_left == 0 && native.extra_right == 0);
    assert(native.extra_top == 0 && native.extra_bottom == 0);

    const auto wide = resolve_view_geometry(288, 160, 360, 240, false,
                                             480, 240);
    assert(wide.width == 288 && wide.height == 160);
    assert(wide.extra_left == 24 && wide.extra_right == 24);
    assert(wide.extra_top == 0 && wide.extra_bottom == 0);

    const auto expanded = resolve_view_geometry(360, 240, 360, 240, false,
                                                 480, 240);
    assert(expanded.width == 360 && expanded.height == 240);
    assert(expanded.extra_left == 60 && expanded.extra_right == 60);
    assert(expanded.extra_top == 40 && expanded.extra_bottom == 40);

    const auto clamped = resolve_view_geometry(480, 320, 360, 240, false,
                                                480, 240);
    assert(clamped.width == 360 && clamped.height == 240);
    assert(clamped.extra_left == 60 && clamped.extra_right == 60);
    assert(clamped.extra_top == 40 && clamped.extra_bottom == 40);

    // Development mode may exercise the engine envelope, but still clamps to
    // the renderer's own maximum when the game cap is lower.
    const auto development = resolve_view_geometry(480, 240, 360, 200, true,
                                                    480, 240);
    assert(development.width == 480 && development.height == 240);
    assert(development.extra_left == 120 && development.extra_right == 120);
    assert(development.extra_top == 40 && development.extra_bottom == 40);

    using namespace gbarecomp;
    const auto four_three = resolve_aspect_view_geometry(360, 240, ViewAspectFourThree, 0, 0);
    assert(four_three.width == 320 && four_three.height == 240);
    const auto sixteen_ten = resolve_aspect_view_geometry(360, 240, ViewAspectSixteenTen, 0, 0);
    assert(sixteen_ten.width == 360 && sixteen_ten.height == 225);
    const auto sixteen_nine = resolve_aspect_view_geometry(360, 240, ViewAspectSixteenNine, 0, 0);
    assert(sixteen_nine.width == 360 && sixteen_nine.height == 203);
    assert(sixteen_nine.extra_top == 21 && sixteen_nine.extra_bottom == 22);
    const auto extra_wide = resolve_aspect_view_geometry(480, 320, ViewAspectSixteenNine, 0, 0);
    assert(extra_wide.width == 480 && extra_wide.height == 270);
    const auto original = resolve_aspect_view_geometry(360, 240, ViewAspectOriginal, 1920, 1080);
    assert(original.width == 360 && original.height == 240);
    const auto window = resolve_aspect_view_geometry(360, 240, ViewAspectWindow, 1920, 1080);
    assert(window.width == sixteen_nine.width && window.height == sixteen_nine.height);
    const auto sharp = resolve_aspect_view_geometry(360, 240, ViewAspectMinimalSharpPixels, 1920, 1080);
    assert(sharp.width == 320 && sharp.height == 180);
    assert(native_integer_scale(1920, 1080) == 6);
    const auto sharp_odd = resolve_aspect_view_geometry(360, 240, ViewAspectMinimalSharpPixels, 1366, 768);
    assert(sharp_odd.width == 342 && sharp_odd.height == 192);
    const auto limited = resolve_aspect_view_geometry(360, 240, ViewAspectMinimalSharpPixels, 3440, 1440);
    assert(limited.width == 360 && limited.height == 160);
    const auto larger = resolve_aspect_view_geometry(480, 320, ViewAspectMinimalSharpPixels, 3440, 1440);
    assert(larger.width == 383 && larger.height == 160);
    const auto tiny = resolve_aspect_view_geometry(480, 320, ViewAspectMinimalSharpPixels, 200, 100);
    assert(tiny.width == 240 && tiny.height == 160);
    assert(native_integer_scale(200, 100) == 0);
    assert(view_aspect_dynamic(ViewAspectWindow) && view_aspect_dynamic(ViewAspectMinimalSharpPixels));
    assert(!view_aspect_dynamic(ViewAspectSixteenNine));

    // Caps and native visibility apply even to portrait/ultrawide windows.
    for (int aspect = 0; aspect < kViewAspectCount; ++aspect) {
        for (const auto& drawable : {std::pair{1, 10000}, std::pair{10000, 1},
                                     std::pair{0, 0}, std::pair{1920, 1080}}) {
            const auto view = resolve_aspect_view_geometry(480, 320, aspect, drawable.first, drawable.second);
            assert(view.width >= 240 && view.width <= 480);
            assert(view.height >= 160 && view.height <= 320);
            assert(view.extra_left + view.extra_right + 240 == view.width);
            assert(view.extra_top + view.extra_bottom + 160 == view.height);
        }
        const auto faithful = resolve_aspect_view_geometry(240, 160, aspect, 1920, 1080);
        assert(faithful.width == 240 && faithful.height == 160);
    }

    const auto odd_camera = resolve_view_camera_insets(43, 21, 22);
    assert(odd_camera.before == 21 && odd_camera.after == 22);
    const auto small_camera = resolve_view_camera_insets(100, 120, 120);
    assert(small_camera.before == 50 && small_camera.after == 50);
    const auto native_camera = resolve_view_camera_insets(100, 0, 0);
    assert(native_camera.before == 0 && native_camera.after == 0);
    const auto no_spare = resolve_view_camera_insets(0, 120, 80);
    assert(no_spare.before == 0 && no_spare.after == 0);

    return 0;
}
