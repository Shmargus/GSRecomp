// screen_filter.h -- test-only screen filters (LCD3x, xBR, CRT Lottes, ScaleFX,
// bilinear, sharp bilinear) for the final present on SDL's OpenGL renderer.
//
// Strictly host-side and strictly opt-in: host_window only calls this when the
// developer launcher's "Screen filters" test box is on and the user picks a
// filter in F1 > Video. With the filter Off nothing here runs.

#pragma once

#include <array>
#include <iterator>

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Rect;

namespace gbarecomp {

// Values are saved in config.ini ([Video] ScreenFilter): append only.
enum class ScreenFilter { Off, Lcd3x, Xbr, CrtLottes, ScaleFx, Bilinear, SharpBilinear };
constexpr int kScreenFilterLast = static_cast<int>(ScreenFilter::SharpBilinear);

// One adjustable filter value: the single table the F1 sliders, config.ini
// ([Video] `key`) and the shaders all use. Values are in the slider's units;
// each shader converts them (see the Params[] #defines in screen_filter.cpp).
// A filter's entries reach its shader as Params[0], Params[1], ... in table
// order. `def` is the upstream shader's default.
struct ScreenFilterParam {
    ScreenFilter filter;
    const char*  key;
    const char*  label;
    float        lo, hi, def, step;
    const char*  format;
};
inline constexpr ScreenFilterParam kScreenFilterParams[] = {
    // LCD3x: depth of the dark rows / columns. Upstream brighten_scanlines 16
    // and brighten_lcd 4 are depths 2/17 and 2/5.
    {ScreenFilter::Lcd3x, "LcdScanlines", "Scanlines", 0.0f, 100.0f, 200.0f / 17.0f, 1.0f, "%.0f%%"},
    {ScreenFilter::Lcd3x, "LcdGrid", "Pixel grid", 0.0f, 100.0f, 40.0f, 1.0f, "%.0f%%"},
    // xBR: XBR_EQ_THRESHOLD.
    {ScreenFilter::Xbr, "XbrTolerance", "Edge tolerance", 1.0f, 50.0f, 15.0f, 1.0f, "%.0f"},
    // CRT Lottes: -hardScan, warp scale, mask depth (maskDark 1-m,
    // maskLight 1+m), brightBoost, bloomAmount.
    {ScreenFilter::CrtLottes, "CrtScanlines", "Scanlines", 0.0f, 20.0f, 8.0f, 0.5f, "%.1f"},
    {ScreenFilter::CrtLottes, "CrtCurvature", "Curvature", 0.0f, 200.0f, 100.0f, 5.0f, "%.0f%%"},
    {ScreenFilter::CrtLottes, "CrtMask", "Shadow mask", 0.0f, 100.0f, 50.0f, 5.0f, "%.0f%%"},
    {ScreenFilter::CrtLottes, "CrtBrightness", "Brightness", 50.0f, 200.0f, 100.0f, 5.0f, "%.0f%%"},
    {ScreenFilter::CrtLottes, "CrtGlow", "Glow", 0.0f, 100.0f, 15.0f, 1.0f, "%.0f%%"},
    // ScaleFX: SFX_CLR.
    {ScreenFilter::ScaleFx, "ScaleFxThreshold", "Edge threshold", 1.0f, 100.0f, 50.0f, 1.0f, "%.0f%%"},
};
constexpr int kScreenFilterParamCount =
    static_cast<int>(std::size(kScreenFilterParams));
constexpr int kScreenFilterMaxParams = 8;  // per filter; the shaders' Params[]

using ScreenFilterValues = std::array<float, kScreenFilterParamCount>;
constexpr ScreenFilterValues screen_filter_defaults() {
    ScreenFilterValues v{};
    for (int i = 0; i < kScreenFilterParamCount; ++i) v[i] = kScreenFilterParams[i].def;
    return v;
}

// Draw `source` (texture pixels) of `texture` into `dest` (renderer-output
// pixels, top-left origin) through `filter`, on SDL's OpenGL renderer.
// `pixel_w` x `pixel_h` is the 1x size of that region. `params` holds
// kScreenFilterParamCount values in kScreenFilterParams order (nullptr: the
// defaults). False when it cannot (not the OpenGL renderer, GL functions or
// shader unavailable); the caller then copies as usual.
bool screen_filter_draw(SDL_Renderer* renderer, SDL_Texture* texture,
                        ScreenFilter filter, const float* params,
                        const SDL_Rect& source, int pixel_w, int pixel_h,
                        const SDL_Rect& dest);

// Frees the GL objects. Safe to call at any time, including never-used.
void screen_filter_shutdown();

}  // namespace gbarecomp
