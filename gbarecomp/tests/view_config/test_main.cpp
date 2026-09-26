#include "view_config.h"

#include <cassert>

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

    return 0;
}
