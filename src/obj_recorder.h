// obj_recorder.h — GSR_OBJ_RECORD sprite-placement census.
//
// The question this exists to answer, and nothing else: WHICH committed
// sprites can be given a full-precision position, and which cannot?
//
// Background. The PPU currently asks OAM where each sprite is. OAM stores X
// in 9 bits and Y in 8. A 360x240 view spans rows -104..199 -- 304 rows --
// and a byte names 256, so rows below -56 share a byte with rows in the
// bottom margin (measured 2026-09-06: slot 23 sits at logical y=-101 and
// writes byte 155, which also means row 155). No arithmetic on the byte can
// separate them, at any view size. The escape is to stop reading the byte:
// the guest computes each sprite's position at full precision before
// truncating it into OAM, that value is already captured
// (record_golden_sun_obj_staging in runner_main.cpp), and OAM can be demoted
// to an identity tag.
//
// That capture exists but its coverage has never been measured -- it is
// handed off on writer route D4, which fires only on "a successful, gated
// commit", while route F0 is the one every committed sprite passes through,
// body and shadow alike. Before rewiring the renderer to trust the table,
// measure how much of the table is actually there.
//
// Central design constraint, same as map_recorder.h: this MEASURES, it does
// not fix. It never changes a guest write, a coordinate, or a rendered
// pixel. The experimental off-screen culler shares the same lookup
// (golden_sun_obj_resolve_placement) but is a separate toggle; with only
// this recorder on, the run is byte-for-byte an ordinary run plus file I/O.
//
// Golden Sun-specific tooling: lives entirely in src/, matches
// map_recorder.h's shape and toggle style, and is analysed offline by
// tools/decode_obj.py.
#pragma once

#include <cstdint>

#include "widescreen_policy.h"

namespace gsr {

// One committed OAM-shadow write, after the placement lookup has run.
//
// Every field is observed, never derived: `outcome` and `resolved_*` come
// from golden_sun_obj_resolve_placement, the rest are the bytes and call
// context at the F0 commit seam. `record_base`/`shadow` are meaningful only
// when the sprite was traced to a guest actor record; `resolved_*` only when
// golden_sun_obj_placement_is_exact(outcome).
struct ObjPlacementSample {
    std::uint64_t frame = 0;
    int slot = -1;
    std::uint32_t slot_address = 0;
    // Which of the game's sprite tables this commit went into. The game
    // builds its list in two places and uploads whichever is current, so a
    // per-table count is how we confirm both are actually being observed.
    std::uint32_t table_base = 0;
    // Call context at the commit, so an unsourced sprite can be attributed
    // to the guest code path that emitted it. This is the actionable half of
    // the census: it names the next doorway to hook.
    std::uint32_t writer_pc = 0;
    std::uint32_t return_pc = 0;
    std::uint32_t depth = 0;
    // Guest actor record the sprite was traced to, when it was traced at all
    // (body at record+0x00, shadow at record+0x0C).
    std::uint32_t staging = 0;
    std::uint32_t record_base = 0;
    bool shadow = false;
    bool record_identified = false;
    // Rotation/scaling sprite. Its position resolves like any other's; only
    // its bounds need the transform. Counted separately because Golden Sun
    // uses affine sprites heavily in the field, so "how well are affine
    // sprites covered?" is its own question.
    bool affine = false;
    // The committed OAM bytes -- the truncated position we are trying to
    // stop depending on.
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    std::uint32_t raw_x = 0;
    std::uint32_t raw_y = 0;
    // How far a shadow sits from the body it was placed from. Recorded so
    // the paired recovery's assumption -- that a pair is closer together
    // than half the truncated field, 128 rows or 256 columns -- is measured
    // rather than assumed. Zero for a body.
    int paired_offset_x = 0;
    int paired_offset_y = 0;
    // Full-precision position, valid only for the two exact outcomes.
    int resolved_x = 0;
    int resolved_y = 0;
    int width = 0;
    int height = 0;
    widescreen::GoldenSunObjPlacementOutcome outcome =
        widescreen::GoldenSunObjPlacementOutcome::SourceUnavailable;
};

// Caches the GSR_OBJ_RECORD env flag and, only when it is set, creates the
// session directory and registers the exit summary. Call once at startup
// next to map_recorder_init(). Safe to call regardless of the flag.
void obj_recorder_init();

// Hot-path gate. runner_main.cpp's F0 observer runs its placement lookup
// when this OR the experimental-fixes toggle is on, so the recorder can
// measure without the culler being armed.
bool obj_recorder_enabled();

// Ordered diagnostic events; attributes are identity metadata, no asset data.
// One-shot EC-entry observation joined to the F0 destination. Unlike the
// placement context, it retains sources outside the actor array. It is never
// consulted by rendering. `state` distinguishes no entry, a stale/different
// invocation, unreadable RAM, changed attributes, and an exact source handoff.
struct ObjSourceCapture {
    const char* state = "not-sampled";
    std::uint64_t frame = UINT64_MAX, epoch = 0;
    std::uint32_t pc = 0, source = 0, target = 0, depth = 0, return_pc = 0;
    std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
    // Flags: 1=RAM attributes read; 2=known actor identity; 4=staging found;
    // 8=staged X valid; 16=staged Y valid. These are observations, not trust.
    std::uint32_t flags = 0;
    std::uint32_t stage_source = 0;
    std::uint64_t stage_frame = UINT64_MAX, stage_epoch = 0;
    int stage_x = 0, stage_y = 0;
};

struct ObjLifetimeSample {
    const char* event = "commit";
    const char* reason = "ok";
    std::uint64_t frame = 0, epoch = 0, dma = 0;
    int slot = -1;
    std::uint32_t source = 0, staging = 0;
    std::uint64_t context_frame = UINT64_MAX, body_frame = UINT64_MAX;
    std::uint64_t body_epoch = 0;
    std::uint32_t checks = 0;
    std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
    std::uint64_t provenance_frame = UINT64_MAX, provenance_epoch = 0;
    std::uint32_t target = 0;
    std::uint16_t expected0 = 0, expected1 = 0, expected2 = 0;
    ObjSourceCapture entry{};
};
// For a `commit` row with reason `record-identity`, `staging` is the raw F0
// entry pointer retained for diagnosis; it is not trusted actor identity.
// The row's ATTR fields are the committed OAM words; the diagnostic context
// is selected only when all three words matched at the F0 entry.
void obj_recorder_note_lifetime(const ObjLifetimeSample& sample);

// Tracked writer observation for OAM-shadow table slots touched by the
// current capture. Only addresses, PCs, transfer metadata and OAM identity
// attributes are recorded; no guest payload is retained.
struct ObjShadowWriteSample {
    const char* event = "cpu";
    std::uint64_t frame = 0, epoch = 0, dma = 0;
    int slot = -1;
    std::uint32_t writer_pc = 0;
    std::uint32_t address = 0, size = 0;
    std::uint32_t source = 0, destination = 0;
    std::uint16_t control = 0;
    int start_mode = 0;
    // Bits 0..2 say which ATTR0/1/2 words were part of this write. Missing
    // words in a partial DMA are left zero in the row.
    std::uint8_t attr_mask = 0;
    std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
    // The generated call-return stack's top two entries at the write (0 when
    // shallower): where the writer returns to, and where its caller does.
    // Names the code that decided to draw this sprite, which on the world
    // map is not one of the field routes the placement census follows.
    std::uint32_t return_pc = 0, caller_return_pc = 0;
};
void obj_recorder_note_shadow_write(const ObjShadowWriteSample& sample);

// One CPU store to one of the two measured EWRAM object-source regions.
// This is a writer census only: the callback runs before the store, so it
// records the writer and field location without retaining guest payload.
struct ObjEwramWriteSample {
    std::uint64_t frame = 0, epoch = 0;
    const char* region = "unknown";
    std::uint32_t writer_pc = 0;
    std::uint32_t address = 0, offset = 0, size = 0;
};
void obj_recorder_note_ewram_write(const ObjEwramWriteSample& sample);

// Narrow Region B boulder trace. These are scalar handoff observations only:
// the two measured source fields, the register state at their writers and at
// the source/commit seams, and the committed OAM identity. It does not retain
// the surrounding EWRAM record or any asset payload.
struct ObjBoulderTraceSample {
    const char* event = "unknown";
    const char* source_state = "unknown";
    const char* outcome = "unknown";
    std::uint64_t frame = 0, epoch = 0;
    int slot = -1;
    std::uint32_t source = 0, source_offset = 0;
    std::uint32_t target = 0, writer_pc = 0, return_pc = 0, depth = 0;
    std::uint32_t entry_pc = 0, entry_depth = 0, entry_return_pc = 0;
    // Guest register snapshots at the measured writer/calculation handoffs.
    // The register names are intentionally retained; their field meaning is
    // what this trace is meant to establish.
    std::uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r5 = 0, r6 = 0;
    std::uint32_t r7 = 0, r9 = 0, r10 = 0, r11 = 0;
    std::uint32_t store_value = 0;
    std::uint32_t field_a_value = 0, field_b_value = 0;
    bool candidate_fields_valid = false;
    std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
    std::uint32_t raw_x = 0, raw_y = 0;
    int resolved_x = 0, resolved_y = 0;
};
void obj_recorder_note_boulder_trace(const ObjBoulderTraceSample& sample);

// One placement event joined to the measured source fields and live GBA
// display scroll state. Candidate fields are named by offset because their
// position meaning is still an open measurement question. Only the two
// offsets observed in Region B writer traffic are read.
struct ObjCameraSample {
    const char* event = "f0";
    const char* reason = "unknown";
    const char* source_state = "unknown";
    const char* source_region = "unknown";
    std::uint64_t frame = 0, epoch = 0;
    int slot = -1;
    std::uint32_t writer_pc = 0, target = 0, source = 0;
    std::uint64_t entry_frame = UINT64_MAX, entry_epoch = 0;
    std::uint32_t entry_pc = 0, entry_depth = 0, entry_return_pc = 0;
    std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
    std::uint16_t dispcnt = 0;
    std::uint16_t bg0_hofs = 0, bg0_vofs = 0;
    std::uint16_t bg1_hofs = 0, bg1_vofs = 0;
    std::uint16_t bg2_hofs = 0, bg2_vofs = 0;
    std::uint16_t bg3_hofs = 0, bg3_vofs = 0;
    std::uint32_t field_a_offset = 0, field_a_value = 0;
    std::uint32_t field_b_offset = 0, field_b_value = 0;
    bool candidate_fields_valid = false;
};
void obj_recorder_note_camera_sample(const ObjCameraSample& sample);

// Hot path: one committed sprite, already resolved. Tallies it, rolls the
// per-frame row over when the frame changes, and files unsourced sprites by
// call site. No-ops in one branch when the recorder is off.
void obj_recorder_note_placement(const ObjPlacementSample& sample);

}  // namespace gsr
