// screen_filter.h -- test-only screen filters (LCD3x, xBR, CRT Lottes, ScaleFX) for the final
// present on SDL's OpenGL renderer.
//
// Strictly host-side and strictly opt-in: host_window only calls this when the
// developer launcher's "Screen filters" test box is on and the user picks a
// filter in F1 > Video. With the filter Off nothing here runs.

#pragma once

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Rect;

namespace gbarecomp {

enum class ScreenFilter { Off, Lcd3x, Xbr, CrtLottes, ScaleFx };

// Draw `source` (texture pixels) of `texture` into `dest` (renderer-output
// pixels, top-left origin) through `filter`, on SDL's OpenGL renderer.
// `pixel_w` x `pixel_h` is the 1x size of that region. False when it cannot
// (not the OpenGL renderer, GL functions or shader unavailable); the caller
// then copies as usual.
bool screen_filter_draw(SDL_Renderer* renderer, SDL_Texture* texture,
                        ScreenFilter filter, const SDL_Rect& source,
                        int pixel_w, int pixel_h, const SDL_Rect& dest);

// Frees the GL objects. Safe to call at any time, including never-used.
void screen_filter_shutdown();

}  // namespace gbarecomp
