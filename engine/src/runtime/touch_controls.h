// touch_controls.h: on-screen GBA buttons for touch screens (Android).
//
// Read SDL's finger state each frame. Draw the buttons in the menu's
// ImGui pass, using window coordinates.
//
// Two round edge buttons: the menu (opens the F1 menu) and fast forward
// (>>, switches the Fast Forward (toggle) latch on and off; gold while on).
// Tap one, or drag it to move it. Menu > Controls > Place on-screen buttons
// lets the player move the GBA buttons too (saved in config.ini). Idle, each slides partly offscreen and can
// still be tapped; fast forward stays out while it runs.
//
// Desktop builds compile the stubs: touch_controls_enabled() is false there
// and nothing changes.
#pragma once

#include <cstdint>
#include <string>

namespace gbarecomp {

bool touch_controls_enabled();

struct TouchControlsInput {
    std::uint16_t held = 0;     // GBA KEYINPUT bits held (1 = pressed)
    bool menu_pressed = false;  // the menu button was tapped this frame
    bool fast_forward_pressed = false;  // the >> button was tapped this frame
};

// Sample all current touches in a `width` by `height` window. Skip input
// while the menu is open.
TouchControlsInput touch_controls_poll(int width, int height, bool menu_open);

// Show or hide the GBA buttons. The menu button stays visible during play.
void touch_controls_set_buttons_shown(bool shown);

// Whether fast forward is running, for the >> button's look.
void touch_controls_set_fast_forward(bool on);
// Show or hide the >> button ([Video] TouchFastForward; shown by default).
void touch_controls_set_fast_forward_shown(bool shown);

// Placing: the player drags the GBA buttons, with Done and Reset at the
// top; the game gets no input. Smaller and Bigger resize the last button
// touched (the menu and >> buttons too), or every button while "All buttons"
// is ticked. Done (or opening the menu) ends it.
void touch_controls_begin_placing();
bool touch_controls_placing();
// True once after placing ends: save touch_controls_layout() then.
bool touch_controls_take_layout_changed();
// The saved places and sizes, for config.ini [Video] TouchLayout ("": the
// defaults). One item per button, ';' between: "x,y" or "-", then "*size".
std::string touch_controls_layout();
void touch_controls_set_layout(const char* text);

// Draw the buttons in the current ImGui frame using window coordinates.
// Skip drawing while the menu is open.
void touch_controls_draw_overlay(float width, float height, bool menu_open);

}  // namespace gbarecomp
