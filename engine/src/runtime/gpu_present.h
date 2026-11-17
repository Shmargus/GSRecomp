// Present the field renderer's texture directly on Android.
//
// Skip the GPU-to-CPU readback and blend flicker on the GPU using the
// same rule as temporal_blend.h. Tensor G4 profile (14 November 2026):
// graphics driver 19% of the game thread, frame copies 7%, CPU blend 6.6%.
//
// Offer a GPU texture when the frame needs no CPU painting and the
// window accepts direct presentation. Colour grading, screen filters,
// 2x interpolation, native-scale textures and smooth scaling use the
// readback path.
//
// Desktop builds compile stubs: nothing is ever offered and nothing changes.
#pragma once

struct SDL_Renderer;
struct SDL_Rect;

namespace gbarecomp {

struct GpuPresentFrame {
    unsigned texture = 0;  // RGBA8, GL rows bottom-up
    int width = 0;
    int height = 0;
};

// Window, once per present: whether the next frame may come as a texture.
void gpu_present_set_accepting(bool accepting);
bool gpu_present_accepting();

// Offer or withdraw a texture for this frame, including repeated
// presents under enhanced timing.
void gpu_present_offer(const GpuPresentFrame& frame);
void gpu_present_withdraw();
// The frame on offer, if any.
const GpuPresentFrame* gpu_present_current();

// Window: draw `frame`'s `source` (frame pixels, top-down) into `dest`
// (renderer output pixels, top-down), blended by the flicker setting
// (temporal_blend.h index; 0 off). Flushes SDL's queued drawing first and
// leaves GL as SDL had it. False: not drawn (the caller shows nothing new).
bool gpu_present_draw(SDL_Renderer* renderer, const GpuPresentFrame& frame,
                      const SDL_Rect& source, const SDL_Rect& dest,
                      int blend_index);
// Window: forget the blend history (a frame went the copied way).
void gpu_present_reset_history();

// Optional RGBA8 overlay (battle HP bar or speed icon), top row first.
// Alpha 0 is transparent. Upload only the dirty rectangle (w = 0 skips
// upload). Keep the buffer alive until the frame has been drawn.
void gpu_present_set_overlay(const unsigned char* rgba, int width, int height,
                             int dirty_x, int dirty_y, int dirty_w, int dirty_h,
                             bool visible);

}  // namespace gbarecomp
