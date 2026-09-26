// effect_sources.h -- read the game's spell particles out of guest memory.
//
// `effect_particles.h` draws sparks; this decides WHICH sparks. They are
// separate because the drawing is shared and the reading is not: the effect
// systems measured so far do not agree on where their particles live, how many
// there are, or even what a position means (FACTS.md, 2026-09-18).
//
//   | system        | array                   | count | position         |
//   |---------------|-------------------------|-------|------------------|
//   | Func_c11ec    | [0x03001EEC] + 0x11C0   | 16    | (v >> 10) + 64   |
//   | Func_cbc0c    | [0x03001EEC] + 0x7080   | 33    | 16.16 pixels     |
//   | Func_cc5d8    | [0x03001EEC] + 0x7080   | 64    | 16.16 pixels     |
//   | Func_cb7f8    | 0x02010000              | 128   | POLAR, not read  |
//
// The polar one stores `rand & 0xFFFF` as an angle and a separate distance,
// and runs both through the game's sine and cosine. Until that is read it is
// deliberately absent here rather than guessed at -- three separate attempts
// to fit a position out of those records failed, because an angle is not a
// coordinate however you shift it.
//
// EVERY READ IS VALIDATED. A source is accepted only when its records actually
// look like live particles; otherwise the caller must fall back to the game's
// own canvas, unchanged. That matters because 0x02010000 is shared scratch --
// the same bytes are a particle array while one system runs and a staging
// image while another does -- so "there is memory at this address" proves
// nothing. Guessing wrong here would draw an image buffer as a spray of sparks.
#pragma once

#include <cstdint>
#include <vector>

#include "effect_particles.h"

namespace gsr {

// Which system's particles we believe we are looking at.
enum class EffectSource {
    None,        // nothing recognised; use the game's canvas
    Offset16,    // Func_c11ec: 16 records, (v >> 10) + centre
    Cartesian,   // Func_cbc0c / Func_cc5d8: 16.16 pixel coordinates
    Polar,       // Func_cb7f8 / Func_cc5d8: an angle and a distance
};

// The polar system stores a full turn as 0..0xFFFF. Confirmed from the game's
// own cosine, `Func_231c` (0x0800231C), which is the sine with a quarter turn
// added -- and that quarter turn is the literal 0x4000.
inline constexpr std::uint32_t kFullTurn = 0x10000u;
inline constexpr std::uint32_t kPolarArray = 0x02010000u;
inline constexpr int kPolarCount = 128;
inline constexpr int kRecordDistance = 0x08;

// How far to shift the stored distance before treating it as pixels. The game
// scales it through its own fixed-point helpers and the factor has not been
// read out of the update loop, so the reader TRIES these and keeps whichever
// agrees with the canvas the game drew. That is only legitimate because the
// agreement test is a real gate: a wrong scale collapses or scatters, and both
// are refused.
inline constexpr int kPolarShiftCandidates[] = {6, 7, 8, 9, 10};

// Guest addresses, named rather than spelled inline so the evidence trail in
// FACTS.md can be followed from the code.
inline constexpr std::uint32_t kEwramBase = 0x02000000u;
inline constexpr std::uint32_t kIwramBase = 0x03000000u;
inline constexpr std::uint32_t kStructurePointer = 0x03001EECu;
inline constexpr std::uint32_t kOffset16Array = 0x11C0u;
inline constexpr std::uint32_t kCartesianArray = 0x7080u;
inline constexpr int kRecordBytes = 28;

// Field offsets within a record, common to the systems read so far.
inline constexpr int kRecordX = 0x00;
inline constexpr int kRecordY = 0x04;
inline constexpr int kRecordLife = 0x18;

struct EffectRead {
    EffectSource source = EffectSource::None;
    std::vector<EffectSpark> sparks;
    int live = 0;       // records that carried a real position
    int rejected = 0;   // records refused by the validation below
};

// A window onto guest memory. The renderer already keeps EWRAM for the room
// buffer and IWRAM for the frame dump, so both are in hand; passing them as
// spans keeps this testable without a running game.
struct GuestMemory {
    const std::uint8_t* ewram = nullptr;
    std::size_t ewram_bytes = 0;
    const std::uint8_t* iwram = nullptr;
    std::size_t iwram_bytes = 0;

    bool read_u32(std::uint32_t address, std::uint32_t* out) const;
};

// Read one system's particles. `canvas_width`/`canvas_height` are the game's
// own canvas, which is what a position is validated against -- a particle
// outside it is exactly the one being clipped, so a generous margin is allowed
// either side, but a record pointing a thousand pixels away is not a particle.
EffectRead read_sparks(const GuestMemory& memory, EffectSource source,
                       int canvas_width, int canvas_height);

// The polar reader needs a scale, so it is separate from the table-driven ones.
EffectRead read_polar_sparks(const GuestMemory& memory, int canvas_width,
                             int canvas_height, int distance_shift);

// Try the known systems and return the first that validates. Returns a read
// with `source == EffectSource::None` when nothing does, which is the signal
// to leave the frame to the game.
EffectRead detect_and_read(const GuestMemory& memory, int canvas_width,
                           int canvas_height);

// How far outside the game's canvas a particle may sit and still be believed.
// Sparks beyond the canvas are the whole point, so this cannot be zero; but it
// bounds how wrong a misidentified source can look.
inline constexpr int kPositionSlack = 192;

// A source must produce at least this many live records to be believed. One
// stray value inside a staging image could pass the position test by luck; a
// dozen of them in one array could not.
inline constexpr int kMinimumLive = 6;

// AGREEING WITH THE GAME IS THE REAL TEST.
//
// The position and count rules above are not enough, and that was measured
// rather than assumed: run against three real captures they accepted a source
// on all three, and none of those captures is running a system this file can
// read. Plausible-looking numbers are everywhere in shared scratch.
//
// So the reader has to prove itself against something only the right answer
// can match: the canvas the game itself drew this frame. If our sparks are the
// game's sparks, they land where the game lit pixels. If we are reading an
// image buffer or the wrong system, they do not.
//
// The scoring has to be done carefully, because a bright canvas flatters any
// guess -- one of these canvases is 55% lit, so "landed on a drawn pixel" is
// nearly free, and a degenerate answer that piles every spark on the middle
// scores 100%. Two guards, both learned from fits that fooled this project
// earlier the same day (FACTS.md, 2026-09-18):
//
//   * beat the canvas's OWN lit fraction by a clear margin, not 50%;
//   * keep a real spread, so a collapsed answer cannot win.
inline constexpr double kMinimumAgreementRatio = 2.5;  // times the baseline
inline constexpr double kMinimumSpread = 8.0;          // pixels, population sd

struct CanvasAgreement {
    double baseline = 0.0;   // fraction of the game's canvas that is lit
    double hit_rate = 0.0;   // fraction of our sparks landing on lit pixels
    double spread = 0.0;     // spread of our spark positions
    int sampled = 0;
    bool agrees = false;
};

// Score a read against the game's own 8-bit canvas. `canvas` is tile-ordered,
// as the game keeps it: 8x8 tiles, `width / 8` of them per row.
CanvasAgreement agree_with_canvas(const EffectRead& read,
                                  const std::uint8_t* canvas,
                                  std::size_t canvas_bytes, int width,
                                  int height);

// The detector, with the agreement test applied. This is what the renderer
// calls: it returns a source only when that source reproduces what the game
// drew, and `EffectSource::None` otherwise, which means leave the frame alone.
EffectRead detect_verified(const GuestMemory& memory,
                           const std::uint8_t* canvas,
                           std::size_t canvas_bytes, int width, int height);

}  // namespace gsr
