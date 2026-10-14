// effect_sources.cpp -- see effect_sources.h.

#include "effect_sources.h"

#include <cmath>
#include <cstring>

namespace gsr {
namespace {

std::int32_t read_s32(const std::uint8_t* at) {
    std::uint32_t v = static_cast<std::uint32_t>(at[0]) |
                      (static_cast<std::uint32_t>(at[1]) << 8) |
                      (static_cast<std::uint32_t>(at[2]) << 16) |
                      (static_cast<std::uint32_t>(at[3]) << 24);
    return static_cast<std::int32_t>(v);
}

// Where a system's records start, and how many it keeps. Both are the loop
// bounds read out of each system's own init, never a guess.
struct Layout {
    std::uint32_t offset = 0;  // from the structure pointer
    int count = 0;
    int shift = 0;             // how many bits of fraction a position carries
    int centre = 0;            // added after the shift
};

bool layout_for(EffectSource source, Layout* out) {
    switch (source) {
        case EffectSource::Offset16:
            // Func_c11ec, 0x080C1348..0x080C1358: asr #10, then + 0x40.
            *out = Layout{kOffset16Array, 16, 10, 64};
            return true;
        case EffectSource::Cartesian:
            // Func_cbc0c seeds `byte << 16`, so the position is already in
            // pixels once the fraction is dropped, with no centre added. The
            // 64-record Func_cc5d8 shares the layout; reading the larger count
            // covers both, and the surplus records fail validation harmlessly
            // when the shorter system is the live one.
            *out = Layout{kCartesianArray, 64, 16, 0};
            return true;
        case EffectSource::None:
        default:
            return false;
    }
}

}  // namespace

bool GuestMemory::read_u32(std::uint32_t address, std::uint32_t* out) const {
    const std::uint8_t* base = nullptr;
    std::size_t size = 0;
    std::uint32_t offset = 0;
    if (address >= kIwramBase && iwram) {
        base = iwram;
        size = iwram_bytes;
        offset = address - kIwramBase;
    } else if (address >= kEwramBase && ewram) {
        base = ewram;
        size = ewram_bytes;
        offset = address - kEwramBase;
    }
    if (!base || offset + 4u > size) return false;
    *out = static_cast<std::uint32_t>(read_s32(base + offset));
    return true;
}

EffectRead read_sparks(const GuestMemory& memory, EffectSource source,
                       int canvas_width, int canvas_height) {
    EffectRead result;
    Layout layout;
    if (!layout_for(source, &layout)) return result;

    std::uint32_t structure = 0;
    if (!memory.read_u32(kStructurePointer, &structure)) return result;
    // The pointer must land in EWRAM at all. A zero or a stale IWRAM value
    // means no effect has set this up, and reading on would be reading noise.
    if (structure < kEwramBase || structure >= kEwramBase + memory.ewram_bytes)
        return result;

    const std::uint32_t array = structure + layout.offset;
    const std::size_t begin = array - kEwramBase;
    const std::size_t span =
        static_cast<std::size_t>(layout.count) * kRecordBytes;
    if (begin + span > memory.ewram_bytes) return result;

    for (int i = 0; i < layout.count; ++i) {
        const std::uint8_t* record =
            memory.ewram + begin + static_cast<std::size_t>(i) * kRecordBytes;
        const std::int32_t raw_x = read_s32(record + kRecordX);
        const std::int32_t raw_y = read_s32(record + kRecordY);
        const std::int32_t life = read_s32(record + kRecordLife);

        // A record with no position at all is a free slot, not a bad read.
        if (raw_x == 0 && raw_y == 0) continue;

        const int x = static_cast<int>(raw_x >> layout.shift) + layout.centre;
        const int y = static_cast<int>(raw_y >> layout.shift) + layout.centre;

        // The test that stops a staging image being drawn as sparks: a real
        // particle sits within reach of the canvas, and its life field indexes
        // a sprite rather than holding an arbitrary number.
        const bool placed = x >= -kPositionSlack &&
                            y >= -kPositionSlack &&
                            x < canvas_width + kPositionSlack &&
                            y < canvas_height + kPositionSlack;
        const bool sized = life >= -1 && life < 4096;
        if (!placed || !sized) {
            ++result.rejected;
            continue;
        }

        EffectSpark spark;
        spark.x = x;
        spark.y = y;
        // The game clamps this index into its sprite table rather than
        // rejecting it (IWRAM 0x0300635A); a spent particle carries -1.
        spark.size_index = life < 0 ? 0 : static_cast<int>(life);
        result.sparks.push_back(spark);
        ++result.live;
    }

    // Believe the source only if enough of it held together. A handful of
    // plausible values inside unrelated memory is luck; a dozen is a system.
    if (result.live < kMinimumLive || result.rejected > result.live) {
        return EffectRead{};
    }
    result.source = source;
    return result;
}

EffectRead read_polar_sparks(const GuestMemory& memory, int canvas_width,
                             int canvas_height, int distance_shift) {
    EffectRead result;
    if (!memory.ewram) return result;
    const std::size_t begin = kPolarArray - kEwramBase;
    const std::size_t span =
        static_cast<std::size_t>(kPolarCount) * kRecordBytes;
    if (begin + span > memory.ewram_bytes) return result;

    const int centre_x = canvas_width / 2;
    const int centre_y = canvas_height / 2;
    for (int i = 0; i < kPolarCount; ++i) {
        const std::uint8_t* record =
            memory.ewram + begin + static_cast<std::size_t>(i) * kRecordBytes;
        const std::uint32_t angle =
            static_cast<std::uint32_t>(read_s32(record + kRecordX)) & 0xFFFFu;
        const std::int32_t distance = read_s32(record + kRecordDistance);
        const std::int32_t life = read_s32(record + kRecordLife);
        if (distance == 0) continue;

        const double turns = static_cast<double>(angle) /
                             static_cast<double>(kFullTurn);
        const double radians = turns * 2.0 * 3.14159265358979323846;
        const double radius =
            static_cast<double>(distance >> distance_shift);
        const int x = centre_x + static_cast<int>(radius * std::cos(radians));
        const int y = centre_y + static_cast<int>(radius * std::sin(radians));

        const bool placed = x >= -kPositionSlack && y >= -kPositionSlack &&
                            x < canvas_width + kPositionSlack &&
                            y < canvas_height + kPositionSlack;
        if (!placed || life < -1 || life >= 4096) {
            ++result.rejected;
            continue;
        }
        EffectSpark spark;
        spark.x = x;
        spark.y = y;
        spark.size_index = life < 0 ? 0 : static_cast<int>(life);
        result.sparks.push_back(spark);
        ++result.live;
    }
    if (result.live < kMinimumLive || result.rejected > result.live)
        return EffectRead{};
    result.source = EffectSource::Polar;
    return result;
}

CanvasAgreement agree_with_canvas(const EffectRead& read,
                                  const std::uint8_t* canvas,
                                  std::size_t canvas_bytes, int width,
                                  int height) {
    CanvasAgreement out;
    if (!canvas || width <= 0 || height <= 0 || read.sparks.empty()) return out;
    const int tiles_across = width / 8;
    const std::size_t needed =
        static_cast<std::size_t>(width) * height;
    if (tiles_across <= 0 || canvas_bytes < needed) return out;

    auto lit = [&](int x, int y) {
        // The game's canvas is tile-ordered: tile (x>>3, y>>3), then the pixel
        // within it. Same walk its own stamp routine does.
        const std::size_t tile =
            static_cast<std::size_t>(y >> 3) * tiles_across + (x >> 3);
        const std::size_t at =
            tile * 64u + static_cast<std::size_t>(y & 7) * 8u + (x & 7);
        return at < canvas_bytes && canvas[at] != 0u;
    };

    std::size_t drawn = 0;
    for (std::size_t i = 0; i < needed; ++i) drawn += (canvas[i] != 0u);
    out.baseline = static_cast<double>(drawn) / static_cast<double>(needed);

    int hits = 0;
    double sum = 0.0;
    double sum_sq = 0.0;
    for (const EffectSpark& spark : read.sparks) {
        // Score only the sparks the GAME could have drawn. The ones outside
        // its canvas are the point of the exercise, but they cannot agree or
        // disagree with a picture that has no pixel for them.
        if (spark.x < 0 || spark.y < 0 || spark.x >= width || spark.y >= height)
            continue;
        ++out.sampled;
        if (lit(spark.x, spark.y)) ++hits;
        for (double v : {static_cast<double>(spark.x),
                         static_cast<double>(spark.y)}) {
            sum += v;
            sum_sq += v * v;
        }
    }
    if (out.sampled < kMinimumLive) return out;

    out.hit_rate = static_cast<double>(hits) / out.sampled;
    const double n = out.sampled * 2.0;
    const double mean = sum / n;
    const double variance = sum_sq / n - mean * mean;
    out.spread = variance > 0.0 ? std::sqrt(variance) : 0.0;

    // A canvas with nothing on it cannot confirm anything, and one that is
    // almost entirely lit cannot either -- there is no signal left to find.
    if (out.baseline <= 0.0 || out.baseline >= 0.9) return out;
    out.agrees = out.hit_rate >= out.baseline * kMinimumAgreementRatio &&
                 out.spread >= kMinimumSpread;
    return out;
}

EffectRead detect_verified(const GuestMemory& memory,
                           const std::uint8_t* canvas,
                           std::size_t canvas_bytes, int width, int height) {
    const EffectSource candidates[] = {EffectSource::Offset16,
                                       EffectSource::Cartesian};
    for (EffectSource candidate : candidates) {
        EffectRead read = read_sparks(memory, candidate, width, height);
        if (read.source == EffectSource::None) continue;
        const CanvasAgreement score =
            agree_with_canvas(read, canvas, canvas_bytes, width, height);
        if (score.agrees) return read;
    }
    // The polar system last, and across its candidate scales. The gate decides
    // which scale is right, if any -- a wrong one either collapses every spark
    // onto the centre or scatters them, and both fail.
    for (int shift : kPolarShiftCandidates) {
        EffectRead read = read_polar_sparks(memory, width, height, shift);
        if (read.source == EffectSource::None) continue;
        const CanvasAgreement score =
            agree_with_canvas(read, canvas, canvas_bytes, width, height);
        if (score.agrees) return read;
    }
    return EffectRead{};
}

EffectRead detect_and_read(const GuestMemory& memory, int canvas_width,
                           int canvas_height) {
    const EffectSource candidates[] = {EffectSource::Offset16,
                                       EffectSource::Cartesian};
    for (EffectSource candidate : candidates) {
        EffectRead read =
            read_sparks(memory, candidate, canvas_width, canvas_height);
        if (read.source != EffectSource::None) return read;
    }
    return EffectRead{};
}

}  // namespace gsr
