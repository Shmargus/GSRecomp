// effect_particles.h -- draw Golden Sun's spell sparks ourselves, at any width.
//
// WHY THIS EXISTS. The game paints every spell effect into a canvas exactly as
// wide as the console's own screen, and clips each spark to it. In a widened
// view that clip is visible: seven of nine captured effects run at full
// opacity into canvas column 0 and stop dead (FACTS.md, 2026-09-18). The
// canvas cannot be widened in place -- the game generates its stamping routine
// at runtime, that generator takes ONE size for both axes, above 128 it emits
// instructions whose destination register becomes `pc`, and the next size up
// is 65,536 bytes against a 65,536-byte heap. All three walls are structural.
//
// The capture observes the game's stamp calls before their canvas clipping.
// The requested positions and artwork are handed to this stamper directly.
//
// WHAT THIS FILE OWNS. Only the stamping, which is the part every effect
// system shares and the part that is fully measured:
//
//   * captured width/height include the large effect artwork, not just the
//     small-spark table (32x64 and 48x48 callers, FACTS.md 2026-09-19);
//   * maximum and saturating-add blend operations follow the live generated
//     routine, as do source flips;
//   * positions are centres here; capture converts the game's top-left once.
//
// Deliberately NOT here: where the particles live. Each effect system keeps
// them somewhere different -- one on the game's heap, another at a fixed
// address it shares with two hundred other routines -- so the source is the
// caller's problem and this file just draws what it is handed.
//
// The output buffer is LINEAR, one byte per pixel, unlike the game's
// tile-ordered canvas. Nothing downstream of us needs the tile order, and a
// linear buffer is what a texture upload wants.
#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>
#include <vector>

namespace gsr {

// Replace: a non-zero source pixel overwrites the canvas (Func_ed408 mode 1).
enum class EffectBlend { Add, Maximum, Replace };

// One spark in canvas coordinates. The GPU applies the original layer's
// per-row transform to the completed canvas, including the spark footprints.
struct EffectSpark {
    int x = 0;            // centre, in canvas pixels
    int y = 0;
    int size_index = 0;   // 0..6; clamped here, as the game clamps it
    // Captured artwork may live anywhere in the supplied block. Legacy
    // table-based callers leave this unset.
    std::size_t art_offset = std::numeric_limits<std::size_t>::max();
    // Zero dimensions preserve the legacy small-spark table interface.
    int width = 0;
    int height = 0;
    EffectBlend blend = EffectBlend::Add;
    bool flip_x = false;
    bool flip_y = false;
};

// The game's own spark geometry, read from ROM tables at 0x080C3620 and
// 0x080C3604 (FACTS.md, 2026-09-18). Index 0 is a single pixel; 4, 5 and 6 are
// all 4x4 and differ only in which artwork they point at.
inline constexpr int kSpriteCount = 7;
inline constexpr int kSpriteSizes[kSpriteCount] = {1, 2, 3, 4, 4, 4, 4};
inline constexpr int kSpriteOffsets[kSpriteCount] = {0, 1, 5, 14, 30, 46, 62};

inline int effect_stamp_width(const EffectSpark& spark) {
    if (spark.width > 0) return spark.width;
    const int i = spark.size_index < 0 ? 0 :
                  spark.size_index >= kSpriteCount ? kSpriteCount - 1 : spark.size_index;
    return kSpriteSizes[i];
}
inline int effect_stamp_height(const EffectSpark& spark) {
    return spark.height > 0 ? spark.height : effect_stamp_width(spark);
}

// The canvas is 6-bit: the game's inner loop saturates at 0x3F rather than
// wrapping, which is why a dense burst reads as a solid glow instead of
// tearing into noise.
inline constexpr std::uint8_t kMaxIntensity = 0x3F;

// A buffer we own, at whatever size the view needs. `origin_x`/`origin_y` are
// where the game's canvas centre sits in it, so a caller can place a 128-wide
// effect in the middle of a 360-wide buffer and have the sparks that used to
// be clipped simply land.
struct EffectCanvas {
    int width = 0;
    int height = 0;
    int origin_x = 0;
    int origin_y = 0;
    std::vector<std::uint8_t> pixels;

    void reset(int w, int h);
    void clear();
    // The display-copy helper retains floor(3 * intensity / 4) for the next
    // update (ROM 0x08001EA8, called by 0x080054E4).
    void fade(bool half = false);
    bool in_bounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height;
    }
};

// Stamp one spark, returning true only if a nontransparent pixel lands.
// `art_offset` selects captured artwork; unset uses `kSpriteOffsets[index]`.
// Out-of-range reads are refused. Geometry still comes from `kSpriteSizes`.
//
// Unlike the game, a spark that falls partly outside the buffer is drawn for
// the part that fits and the rest is dropped -- the same rule, just against a
// boundary that is now far enough out to be off-screen.
bool stamp_spark(EffectCanvas* canvas, const EffectSpark& spark,
                 const std::uint8_t* art, std::size_t art_bytes);

// Preserve earlier pixels while growing to fit a stamp in game coordinates.
bool stamp_spark_expanding(EffectCanvas* canvas, const EffectSpark& spark,
                           const std::uint8_t* art, std::size_t art_bytes);

// Stamp a whole frame's worth. Returns how many sparks landed at least one
// pixel, which is what a caller checks to know the source was right: a source
// pointing at the wrong memory draws nothing at all.
int stamp_sparks(EffectCanvas* canvas, const EffectSpark* sparks, int count,
                 const std::uint8_t* art, std::size_t art_bytes);

// How many of `count` sparks would the GAME have kept, given its canvas width?
// This exists to measure what the widening actually buys on a real capture:
// run it over the same sparks with the game's 128 and with our width, and the
// difference is the artwork the player was losing.
int sparks_inside(const EffectSpark* sparks, int count, int canvas_width,
                  int canvas_height, int origin_x, int origin_y);

}  // namespace gsr
