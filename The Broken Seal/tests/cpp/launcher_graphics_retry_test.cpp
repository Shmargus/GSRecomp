#include "../../../launcher/launcher_graphics_retry.h"
#include <cassert>

int main() {
    using gsr::should_offer_direct3d_retry;
    constexpr auto stack = "ChoosePixelFormat\nwglChoosePixelFormat\nSDL2.dll";
    constexpr auto startup = "[ERR] [host-window] graphics-startup=opengl\n";
    assert(should_offer_direct3d_retry(false, 1, true, "", stack));
    assert(should_offer_direct3d_retry(false, 1, true, startup, ""));
    assert(!should_offer_direct3d_retry(false, 0, true, startup, stack));
    assert(should_offer_direct3d_retry(false, 1, false, startup, "")); // driver termination
    assert(!should_offer_direct3d_retry(false, 1, false, "", stack)); // stale legacy report
    assert(!should_offer_direct3d_retry(true, 1, true, startup, stack));
    assert(!should_offer_direct3d_retry(false, 1, true,
        "[host-window] graphics-startup=opengl\n[host-window] graphics-ready", stack));
    assert(!should_offer_direct3d_retry(false, 1, true,
        "host_window: renderer=opengl", stack)); // gameplay crash
    assert(!should_offer_direct3d_retry(false, 1, true, "", "SDL2.dll unrelated crash"));
    assert(!should_offer_direct3d_retry(false, 1, true, "", "ChoosePixelFormat SDL2.dll"));
    assert(!should_offer_direct3d_retry(false, 1, true, "", "ChoosePixelFormat wgl.dll"));
    assert(!should_offer_direct3d_retry(false, 0, false,
        "[host-window] graphics-ready", "")); // healthy exit
}
