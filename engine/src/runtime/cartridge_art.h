#pragma once

// The collection launcher's selected cartridge: its soft glow and its idle
// bounce (pulse, drifting light, rising motes). One copy for the launcher's
// selector and the engine's black cover during the Seamless hand-over
// (host_window.cpp), so both show the same animation. No drawing library
// here: each side turns the pieces into its own draw calls.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gbarecomp::cartridge_art {

// The soft light behind a cartridge, from its alpha silhouette: a white
// RGBA image `padding` pixels larger than the scaled cartridge on each side.
struct Glow {
    std::vector<unsigned char> rgba;
    int width = 0, height = 0;     // the scaled cartridge, without padding
    int padding = 0;
};

inline Glow make_glow(const std::vector<unsigned char>& rgba, int width, int height) {
    // Expand and soften the alpha silhouette, leaving the sprite pixels crisp.
    // Only the soft glow needs a small mask; avoid processing large exports.
    Glow glow;
    glow.width = std::min(width, 256);
    glow.height = std::max(1, int(std::round(float(height) * glow.width / width)));
    const int dilation = std::max(1, std::min(glow.width, glow.height) / 70);
    const int blur = std::max(2, std::min(glow.width, glow.height) / 30);
    glow.padding = dilation + 2 * blur + 1;
    const int w = glow.width + 2 * glow.padding, h = glow.height + 2 * glow.padding;
    std::vector<float> mask(std::size_t(w) * h, 0), scratch(mask.size());
    for (int y = 0; y < glow.height; ++y) for (int x = 0; x < glow.width; ++x) {
        const int sx = std::min(width - 1, int((x + 0.5f) * width / glow.width));
        const int sy = std::min(height - 1, int((y + 0.5f) * height / glow.height));
        mask[std::size_t(y + glow.padding) * w + x + glow.padding] =
            rgba[(std::size_t(sy) * width + sx) * 4 + 3];
    }
    for (int axis = 0; axis < 2; ++axis) {
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            float alpha = 0;
            for (int d = -dilation; d <= dilation; ++d) {
                const int sx = x + (axis == 0 ? d : 0), sy = y + (axis == 1 ? d : 0);
                if (sx >= 0 && sx < w && sy >= 0 && sy < h)
                    alpha = std::max(alpha, mask[std::size_t(sy) * w + sx]);
            }
            scratch[std::size_t(y) * w + x] = alpha;
        }
        mask.swap(scratch);
    }
    for (int pass = 0; pass < 4; ++pass) {
        const bool horizontal = pass % 2 == 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            float alpha = 0;
            for (int d = -blur; d <= blur; ++d) {
                const int sx = x + (horizontal ? d : 0), sy = y + (horizontal ? 0 : d);
                if (sx >= 0 && sx < w && sy >= 0 && sy < h)
                    alpha += mask[std::size_t(sy) * w + sx];
            }
            scratch[std::size_t(y) * w + x] = alpha / (2 * blur + 1);
        }
        mask.swap(scratch);
    }
    glow.rgba.resize(mask.size() * 4);
    for (std::size_t i = 0; i < mask.size(); ++i) {
        glow.rgba[i * 4] = 255; glow.rgba[i * 4 + 1] = 255; glow.rgba[i * 4 + 2] = 255;
        glow.rgba[i * 4 + 3] = static_cast<unsigned char>(std::clamp(mask[i], 0.0f, 255.0f));
    }
    return glow;
}

// The selected cartridge's size pulse at `seconds` (artistic icon cadence,
// rather than an exact reproduction of game timing).
inline float pulse(double seconds) {
    return 1 + 0.035f * float(0.5 - 0.5 * std::cos(seconds * 4.8));
}

// One draw call: the cartridge or glow image stretched over a rectangle and
// multiplied by a colour, or a filled circle (dots).
struct Piece {
    enum Kind { Image, GlowImage, Dot } kind;
    float x0, y0, x1, y1;     // Dot: centre x0,y0 and radius x1
    std::uint8_t r, g, b, a;
    int segments;             // Dot only
};

// The selected cartridge (`game` 0 The Broken Seal, 1 The Lost Age) centred at
// x, y with the given drawn size, back to front. `image_height` and
// `image_width` already include pulse(). `scale` is the layout scale (the
// launcher's 820x600 design).
inline std::vector<Piece> selected(int game, float x, float y, float image_width,
                                   float image_height, const Glow& glow, float scale,
                                   double seconds, float opacity) {
    std::vector<Piece> pieces;
    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::clamp(int(value), 0, 255));
    };
    const float glow_opacity = 0.14f + 0.07f * float(0.5 - 0.5 * std::cos(seconds * 1.8));
    if (glow.width > 0) {
        const float padding_x = glow.padding * image_width / glow.width;
        const float padding_y = glow.padding * image_height / glow.height;
        const float glow_width = image_width / 2 + padding_x;
        const float glow_height = image_height / 2 + padding_y;
        const float drift = 3 * scale * std::sin(float(seconds * 1.3));
        // Cool, drifting outer light with a warm Psynergy-like core.
        const int outer = int(180 * glow_opacity * opacity);
        pieces.push_back({Piece::GlowImage,
                          x - glow_width * 1.10f + drift, y - glow_height * 1.12f,
                          x + glow_width * 1.10f + drift, y + glow_height * 1.12f,
                          std::uint8_t(game ? 119 : 104), std::uint8_t(game ? 132 : 225),
                          std::uint8_t(game ? 255 : 182), byte(outer), 0});
        pieces.push_back({Piece::GlowImage, x - glow_width, y - glow_height,
                          x + glow_width, y + glow_height, 255, 219, 136,
                          byte(int(255 * glow_opacity * opacity)), 0});
        for (int mote = 0; mote < 16; ++mote) {
            const float angle = mote * 2.39996f + float(seconds * 0.09);
            const float rising = std::fmod(float(seconds * 0.10) + mote * 0.137f, 1.0f);
            const float mx = x + std::cos(angle) * image_width * (0.51f + 0.06f * std::sin(angle * 3));
            const float my = y + std::sin(angle) * image_height * 0.68f - rising * 10 * scale;
            const float shimmer = 0.4f + 0.6f * float(0.5 + 0.5 * std::sin(seconds * 2.1 + mote * 1.7));
            const int alpha = int(105 * shimmer * opacity);
            pieces.push_back({Piece::Dot, mx, my, 3 * scale, 0, std::uint8_t(game ? 167 : 159),
                              std::uint8_t(game ? 168 : 239), std::uint8_t(game ? 255 : 204),
                              byte(alpha / 5), 8});
            pieces.push_back({Piece::Dot, mx, my, (mote % 3 ? 0.8f : 1.25f) * scale, 0,
                              255, 237, 182, byte(alpha), 6});
        }
    }
    pieces.push_back({Piece::Image, x - image_width / 2, y - image_height / 2,
                      x + image_width / 2, y + image_height / 2, 255, 255, 255,
                      byte(int(255 * opacity)), 0});
    return pieces;
}

// The environment variables the launcher sets for a game that should show
// this cartridge on its black cover (the Seamless hand-over): a 32-bit BMP of
// the cut-out cartridge, and which game it is (0 or 1, for the glow colour).
inline constexpr const char* kCoverArtEnv = "GSR_COVER_CARTRIDGE";
inline constexpr const char* kCoverGameEnv = "GSR_COVER_CARTRIDGE_GAME";

}  // namespace gbarecomp::cartridge_art
