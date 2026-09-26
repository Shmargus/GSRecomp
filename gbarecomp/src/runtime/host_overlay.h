// host_overlay.h — minimal RGB888 overlay primitives for in-window UI.
//
// The host window presents a packed RGB888 framebuffer and, until now, drew
// no text at all (the FPS readout goes in the title bar). The rebind menu
// needs to draw over the guest frame, so this is the smallest thing that can:
// a 5x7 bitmap font in an 8x8 cell plus a rectangle fill.
//
// Deliberately NOT a general UI toolkit and deliberately not SDL_ttf: no new
// dependency, no font file to ship, and it composites into the same buffer
// the framedump path records, so what a capture shows is what was on screen.
//
// Guest-state-neutral: everything here runs at present time on a copy of the
// frame. It cannot perturb emulation, and verify/frame-hash paths that read
// the PPU output directly never see it.

#pragma once

#include <cstdint>

namespace gbarecomp {

// Solid rectangle, clipped to the buffer. `shade` blends toward the existing
// pixel: 255 = opaque, 128 = half, 0 = no-op. Used for the menu backdrop so
// the game stays faintly visible behind it.
void overlay_fill(uint8_t* rgb, int w, int h, int x, int y, int rw, int rh,
                  uint8_t r, uint8_t g, uint8_t bl, uint8_t shade);

// Draw `s` at (x, y), 8 pixels per character cell. Characters are upper-cased
// and anything outside ASCII 32..95 renders as a space, which keeps the font
// table to 64 glyphs — enough for button names and SDL scancode names.
void overlay_text(uint8_t* rgb, int w, int h, int x, int y, const char* s,
                  uint8_t r, uint8_t g, uint8_t bl);

// Width in pixels the string will occupy (8 px per character).
int overlay_text_width(const char* s);

}  // namespace gbarecomp
