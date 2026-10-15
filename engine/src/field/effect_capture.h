// effect_capture.h -- the spell-trail capture shared by both games.
//
// Follows the game's 128x128 effect canvas (stamps, clears, the fading copy
// to video memory) and keeps a history that can grow past the canvas, so the
// expanded view can draw trails the console would have cut off. ONE copy,
// compiled by The Broken Seal and The Lost Age; every address is in the
// game's effect_capture_profile() (each game's src/field_game_profile.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "effect_particles.h"

namespace gsr {

// Where one game keeps its effect machinery. A zero address means "this game
// has no such routine/slot"; the rule that would use it is skipped.
struct EffectCaptureProfile {
    // Allocation slot holding the canvas pointer. 0: the canvas is learned
    // from the game's 16 KB copy of it to video memory instead.
    std::uint32_t canvas_slot = 0;
    // First of the two allocation slots holding the live stamp routines. 0:
    // a stamp routine is recognised by its own header bytes instead.
    std::uint32_t stamp_slot = 0;
    // Bytes of the Thumb `bx pc` stub in front of resident ARM routines and
    // generated stamps (either address can be entered). 0: no stub.
    std::uint32_t thumb_stub = 0;
    // ROM range the game copies to IWRAM 0x03000000; an entry inside it is
    // taken as its IWRAM copy's. Empty: no such copy is followed.
    std::uint32_t iwram_image_rom = 0, iwram_image_rom_end = 0;

    // Canvas copy-to-VRAM routines, up to two entries each (0 = unused):
    // copy then fade to 3/4, copy then halve, copy then subtract the
    // parameter (floor 0), copy then add it (cap 63).
    std::uint32_t copy_fade[2] = {};
    std::uint32_t copy_halve[2] = {};
    std::uint32_t copy_subtract[2] = {};
    std::uint32_t copy_add[2] = {};
    // Plain copy (r0 destination, r1 canvas) and the ROM bytes it must match.
    std::uint32_t plain_copy = 0, plain_copy_source = 0;
    // Clear and fill of the whole canvas, each checked against its ROM bytes.
    std::uint32_t clear_canvas = 0, clear_source = 0, clear_bytes = 0;
    std::uint32_t fill_canvas = 0, fill_source = 0, fill_bytes = 0;
    // Slot of the canvas-mode block (mode +0x7780, active +0x7824).
    std::uint32_t canvas_block_slot = 0;

    // The stamp generator's templates in ROM (bytes stay in the player's ROM).
    std::uint32_t header_template = 0, saturation_template = 0;
    std::uint32_t flip_x_template = 0, flip_y_template = 0;
    std::uint32_t flip_xy_template = 0, no_flip_template = 0;
    std::uint32_t max_template = 0, max_reverse_template = 0;
    std::uint32_t add_template = 0, add_reverse_template = 0;
    std::uint32_t replace_template = 0, replace_reverse_template = 0;
};

// Defined by the game (link-time hook, like field_game_profile()).
const EffectCaptureProfile& effect_capture_profile();

// The completed persistent canvas is packed as one rectangular stamp, so
// both presentation and F12 replay include the game's fading trails.
struct CapturedEffectFrame {
    std::uint32_t canvas = 0;
    std::vector<EffectSpark> stamps;
    std::vector<std::uint8_t> artwork;
};

void effect_capture_set_enabled(bool enabled);
bool effect_capture_enabled();
bool effect_capture_observes(std::uint32_t pc);

// Observe the canvas clear/display-copy lifecycle and live stamp entries,
// with their actual stack width/height. The generated code is checked
// against the user's ROM templates to identify maximum/additive blending and
// source flips. No guest state changes.
void effect_capture_on_entry(std::uint32_t pc, const std::uint32_t* registers,
                             const std::uint8_t* ewram,
                             const std::uint8_t* iwram,
                             const std::uint8_t* rom, std::size_t rom_bytes,
                             std::uint64_t frame);
// A spark the game moved but did not draw because it left its canvas (the
// caller's own bounds test). Stamped into the same expanding history with
// the routine, art and box the game would have passed, so it fades with the
// game's own trails. Ignored unless it targets the canvas being followed.
// No guest state changes.
void effect_capture_skipped_spark(std::uint32_t routine, std::uint32_t canvas,
                                  std::uint32_t art, std::int32_t x,
                                  std::int32_t y, std::uint32_t width,
                                  std::uint32_t height,
                                  const std::uint8_t* ewram,
                                  const std::uint8_t* iwram,
                                  const std::uint8_t* rom,
                                  std::size_t rom_bytes, std::uint64_t frame);
// The history over the 128x128 square at canvas (left, top), row-major, 0
// where it holds nothing. False unless `canvas` is the one being followed
// (TLARecomp's triangle capture, src/tla_spell_mesh.cpp).
bool effect_capture_history_read(std::uint32_t canvas, int left, int top,
                                 std::uint8_t* out);
// Copies all `pixels`, including zero (row-major, width x height), into the history
// at canvas (left, top), growing it as a stamp would.
void effect_capture_history_paint(std::uint32_t canvas, int left, int top,
                                  int width, int height,
                                  const std::uint8_t* pixels);
// The guest frame of the last canvas copy to video memory.
std::uint64_t effect_capture_last_copy_frame();
CapturedEffectFrame effect_capture_take();
void effect_capture_report(char* out, std::size_t out_bytes);

}  // namespace gsr
