// touch_controls.h: on-screen GBA buttons for touch screens (Android).
//
// Read SDL's finger state each frame. Draw the buttons in the menu's
// ImGui pass, using window coordinates.
//
// Tap the edge button to open the F1 menu or drag it to move it.
// When idle it slides partly offscreen and can still be tapped.
//
// Desktop builds compile the stubs: touch_controls_enabled() is false there
// and nothing changes.
#pragma once

#include <cstdint>

namespace gbarecomp {

bool touch_controls_enabled();

struct TouchControlsInput {
    std::uint16_t held = 0;     // GBA KEYINPUT bits held (1 = pressed)
    bool menu_pressed = false;  // the menu button was tapped this frame
};

// Sample all current touches in a `width` by `height` window. Skip input
// while the menu is open.
TouchControlsInput touch_controls_poll(int width, int height, bool menu_open);

// Show or hide the GBA buttons. The menu button stays visible during play.
void touch_controls_set_buttons_shown(bool shown);

// Draw the buttons in the current ImGui frame using window coordinates.
// Skip drawing while the menu is open.
void touch_controls_draw_overlay(float width, float height, bool menu_open);

}  // namespace gbarecomp
