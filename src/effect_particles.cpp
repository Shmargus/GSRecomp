// effect_particles.cpp -- see effect_particles.h for why this exists.

#include "effect_particles.h"

#include <algorithm>
#include <utility>

namespace gsr {

void EffectCanvas::reset(int w, int h) {
    width = w < 0 ? 0 : w;
    height = h < 0 ? 0 : h;
    origin_x = width / 2;
    origin_y = height / 2;
    pixels.assign(static_cast<std::size_t>(width) * height, 0u);
}

void EffectCanvas::clear() {
    std::fill(pixels.begin(), pixels.end(), static_cast<std::uint8_t>(0));
}

void EffectCanvas::fade(bool half) {
    for (auto& pixel : pixels)
        pixel = static_cast<std::uint8_t>(
            (half ? unsigned(pixel) / 2u : unsigned(pixel) * 3u / 4u) & 63u);
}

bool stamp_spark_expanding(EffectCanvas* canvas, const EffectSpark& spark,
                           const std::uint8_t* art, std::size_t art_bytes) {
    if (!canvas) return false;
    const std::int64_t x = std::int64_t(spark.x) - effect_stamp_width(spark) / 2;
    const std::int64_t y = std::int64_t(spark.y) - effect_stamp_height(spark) / 2;
    // Growth stops kMaxReach pixels from the origin; the stamp is clipped there.
    // A stray far-off spark (Heat Wave, 2026-09-27) otherwise asked for a
    // canvas gigabytes large and the allocation threw. That far is off-screen.
    constexpr std::int64_t kMaxReach = 512;
    const auto left = std::max(std::min(x, -std::int64_t(canvas->origin_x)), -kMaxReach);
    const auto top = std::max(std::min(y, -std::int64_t(canvas->origin_y)), -kMaxReach);
    const auto right = std::min(std::max(x + effect_stamp_width(spark),
        std::int64_t(canvas->width) - canvas->origin_x), kMaxReach);
    const auto bottom = std::min(std::max(y + effect_stamp_height(spark),
        std::int64_t(canvas->height) - canvas->origin_y), kMaxReach);
    const auto limit = std::numeric_limits<int>::max();
    if (right - left > limit || bottom - top > limit ||
        -left > limit || -top > limit) return false;
    const int width = static_cast<int>(right - left);
    const int height = static_cast<int>(bottom - top);
    const int origin_x = static_cast<int>(-left);
    const int origin_y = static_cast<int>(-top);
    if (width != canvas->width || height != canvas->height ||
        origin_x != canvas->origin_x || origin_y != canvas->origin_y) {
        EffectCanvas expanded;
        expanded.reset(width, height);
        expanded.origin_x = origin_x;
        expanded.origin_y = origin_y;
        for (int row = 0; row < canvas->height; ++row) {
            std::copy_n(canvas->pixels.data() + std::size_t(row) * canvas->width,
                canvas->width, expanded.pixels.data() +
                std::size_t(row + origin_y - canvas->origin_y) * width +
                origin_x - canvas->origin_x);
        }
        *canvas = std::move(expanded);
    }
    EffectSpark placed = spark;
    placed.x += origin_x;
    placed.y += origin_y;
    return stamp_spark(canvas, placed, art, art_bytes);
}

bool stamp_spark(EffectCanvas* canvas, const EffectSpark& spark,
                 const std::uint8_t* art, std::size_t art_bytes) {
    if (!canvas || canvas->width <= 0 || canvas->height <= 0 || !art) return false;

    // The game clamps the index into its own table rather than rejecting it
    // (IWRAM 0x0300635A..0x03006366), so a spark whose life has run past the
    // end of the table draws as the largest spark rather than vanishing.
    int index = spark.size_index;
    if (index < 0) index = 0;
    if (index >= kSpriteCount) index = kSpriteCount - 1;

    const int width = effect_stamp_width(spark);
    const int height = effect_stamp_height(spark);
    const std::size_t offset =
        spark.art_offset == std::numeric_limits<std::size_t>::max()
            ? static_cast<std::size_t>(kSpriteOffsets[index]) : spark.art_offset;
    const std::size_t needed = static_cast<std::size_t>(width) * height;
    if (offset > art_bytes || needed > art_bytes - offset) return false;

    // Drawn from the centre, the same half-size step the game takes.
    const int left = spark.x - width / 2;
    const int top = spark.y - height / 2;

    bool drawn = false;
    for (int row = 0; row < height; ++row) {
        const int y = top + row;
        if (y < 0 || y >= canvas->height) continue;
        for (int column = 0; column < width; ++column) {
            const int x = left + column;
            if (x < 0 || x >= canvas->width) continue;
            const int source_x = spark.flip_x ? width - 1 - column : column;
            const int source_y = spark.flip_y ? height - 1 - row : row;
            const std::uint8_t source =
                art[offset + static_cast<std::size_t>(source_y) * width + source_x];
            if (source == 0u) continue;  // transparent
            drawn = true;
            std::uint8_t& destination =
                canvas->pixels[static_cast<std::size_t>(y) * canvas->width + x];
            if (spark.blend == EffectBlend::Maximum) {
                destination = std::max(destination, source);
            } else if (spark.blend == EffectBlend::Replace) {
                destination = source;
            } else {
                const unsigned sum = static_cast<unsigned>(destination) + source;
                destination = static_cast<std::uint8_t>(
                    sum > kMaxIntensity ? kMaxIntensity : sum);
            }
        }
    }
    return drawn;
}

int stamp_sparks(EffectCanvas* canvas, const EffectSpark* sparks, int count,
                 const std::uint8_t* art, std::size_t art_bytes) {
    if (!canvas || !sparks || count <= 0) return 0;
    int drawn = 0;
    for (int i = 0; i < count; ++i) {
        if (stamp_spark(canvas, sparks[i], art, art_bytes)) ++drawn;
    }
    return drawn;
}

int sparks_inside(const EffectSpark* sparks, int count, int canvas_width,
                  int canvas_height, int origin_x, int origin_y) {
    if (!sparks || count <= 0) return 0;
    int inside = 0;
    for (int i = 0; i < count; ++i) {
        const int x = sparks[i].x - origin_x;
        const int y = sparks[i].y - origin_y;
        if (x >= 0 && y >= 0 && x < canvas_width && y < canvas_height) ++inside;
    }
    return inside;
}

}  // namespace gsr
