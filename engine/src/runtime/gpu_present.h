// Present the field renderer's texture directly on Android.
//
// This skips the GPU-to-CPU readback and blends flicker on the GPU, using the
// same rule as temporal_blend.h. Measured on a Tensor G4 phone, the readback
// path cost this share of the game thread:
//   - graphics driver: 19%
//   - frame copies: 7%
//   - CPU blend: 6.6%
//
// A GPU texture is offered when the frame needs no CPU painting and the window
// accepts direct presentation. Colour profiles are applied on the card here
// too (a profile used to force the copy back, which made a Galaxy Tab A9+
// slow in the wider views), and screen filters read the finished picture on
// the card (gpu_present_finish), smooth scaling as the Bilinear filter. Only
// 2x interpolation (Original view) still uses the readback path.
//
// Desktop builds compile stubs: nothing is ever offered and nothing changes.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

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
// Window: the same picture (blend, layer, colours) whole into a texture of
// the frame's size, RGBA, row 0 the top row as an SDL texture, for a screen
// filter to read (screen_filter_draw_gl). Use instead of gpu_present_draw,
// once per frame. 0: not drawn.
unsigned gpu_present_finish(SDL_Renderer* renderer, const GpuPresentFrame& frame,
                            int blend_index);
// Window: forget the blend history (a frame went the copied way).
void gpu_present_reset_history();

// Window, before each draw: the colour profile's table (color_lut.h
// table_rgb(): 32768 RGB triples by BGR555), or nullptr for Raw. `id`
// (ColorLut::id) says when it changed; it is uploaded once per change and
// applied after the flicker blend, as the copied path does.
void gpu_present_set_color_table(const unsigned char* table, unsigned id);

// Optional RGBA8 overlay (battle HP bar or speed icon), top row first.
// Alpha 0 is transparent. Upload only the dirty rectangle (w = 0 skips
// upload). Keep the buffer alive until the frame has been drawn.
void gpu_present_set_overlay(const unsigned char* rgba, int width, int height,
                             int dirty_x, int dirty_y, int dirty_w, int dirty_h,
                             bool visible);

// Painters that only write pixels (the battle HP bar, the ">>" speed sign)
// paint into begin()'s RGB scratch instead of the picture when the frame is
// shown straight from the card; finish() sends what they painted as the
// overlay. The scratch holds kSentinel where nothing is painted (a colour no
// painter uses), and only the box that changed is sent on. Shared by both
// games' runners.
class GpuOverlayLayer {
public:
    static constexpr std::uint8_t kSentinel[3] = {1, 2, 3};

    std::uint8_t* begin(std::uint32_t w, std::uint32_t h) {
        if (w != width_ || h != height_) {
            width_ = w;
            height_ = h;
            scratch_.resize(std::size_t{w} * h * 3u);
            for (std::size_t i = 0; i < scratch_.size(); i += 3)
                std::memcpy(&scratch_[i], kSentinel, 3);
            rgba_.assign(std::size_t{w} * h * 4u, 0);
            x0_ = y0_ = x1_ = y1_ = 0;
        }
        return scratch_.data();
    }

    void finish() {
        // This frame's painted box.
        int nx0 = static_cast<int>(width_), ny0 = static_cast<int>(height_), nx1 = 0, ny1 = 0;
        for (std::uint32_t y = 0; y < height_; ++y) {
            const std::uint8_t* row = &scratch_[std::size_t{y} * width_ * 3u];
            for (std::uint32_t x = 0; x < width_; ++x) {
                if (std::memcmp(row + x * 3u, kSentinel, 3) == 0) continue;
                nx0 = std::min(nx0, static_cast<int>(x));
                nx1 = std::max(nx1, static_cast<int>(x) + 1);
                ny0 = std::min(ny0, static_cast<int>(y));
                ny1 = std::max(ny1, static_cast<int>(y) + 1);
            }
        }
        const bool painted = nx1 > nx0;
        // Clear the last box, fill this one and put the sentinel back.
        for (int y = y0_; y < y1_; ++y)
            std::memset(&rgba_[(std::size_t(y) * width_ + x0_) * 4u], 0, std::size_t(x1_ - x0_) * 4u);
        if (painted) {
            for (int y = ny0; y < ny1; ++y) {
                for (int x = nx0; x < nx1; ++x) {
                    std::uint8_t* s = &scratch_[(std::size_t(y) * width_ + x) * 3u];
                    std::uint8_t* d = &rgba_[(std::size_t(y) * width_ + x) * 4u];
                    if (std::memcmp(s, kSentinel, 3) == 0) continue;
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                    std::memcpy(s, kSentinel, 3);
                }
            }
        }
        // Sent: the union of the old and new boxes.
        int dx0 = painted ? nx0 : x0_, dy0 = painted ? ny0 : y0_;
        int dx1 = painted ? nx1 : x1_, dy1 = painted ? ny1 : y1_;
        if (x1_ > x0_ && painted) {
            dx0 = std::min(dx0, x0_); dy0 = std::min(dy0, y0_);
            dx1 = std::max(dx1, x1_); dy1 = std::max(dy1, y1_);
        }
        x0_ = painted ? nx0 : 0; y0_ = painted ? ny0 : 0;
        x1_ = painted ? nx1 : 0; y1_ = painted ? ny1 : 0;
        gpu_present_set_overlay(rgba_.data(), static_cast<int>(width_),
                                static_cast<int>(height_), dx0, dy0,
                                std::max(0, dx1 - dx0), std::max(0, dy1 - dy0), painted);
    }

private:
    std::vector<std::uint8_t> scratch_, rgba_;
    std::uint32_t width_ = 0, height_ = 0;
    int x0_ = 0, y0_ = 0, x1_ = 0, y1_ = 0;  // last painted box; x1 == x0: none
};

}  // namespace gbarecomp
