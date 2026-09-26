// map_recorder.h — GSR_MAP_RECORD scene recorder.
//
// Rebuilds the diagnostic deleted 2026-09-04 (see FACTS.md, "The bounds
// field offsets are currently lost") as a general SCENE recorder covering
// every scene class GBA hardware actually presents: Mode 0 field
// (towns/dungeons/indoor rooms -- hardware cannot tell these apart, see
// FACTS.md "Scene classes"), Mode 2 overworld, Mode 1 battle.
//
// Central design constraint, do not "optimise" away: this dumps COMPLETE
// memory images (full EWRAM/IWRAM/VRAM/PAL/OAM/IO) rather than selected
// addresses. The room-bounds struct layout and the overworld's map source
// are currently unknown, and guessing either would violate the no-guessed-
// offsets rule in AGENTS.md. Dumping everything lets that analysis happen
// offline (tools/decode_snap.py) instead of guessing up front what to log.
//
// Golden Sun-specific tooling: lives entirely in src/, matches
// function_tracer.h's style and shares its "Snapshot now" / "Mark window"
// pattern, but is otherwise fully independent -- see map_recorder.cpp for
// how the two share host_config_ui.h's single extra-draw extension point
// without either depending on the other being enabled.
#pragma once

#include <cstdint>

namespace gsr {

// Caches the GSR_MAP_RECORD env flag, and -- only when it is set -- wires
// the recorder's "Snapshot now" + label control into host_config_ui.cpp.
// Call once at startup, before gbarecomp::run_game(), after
// function_tracer_init() (so this can chain onto whatever draw/keyboard
// callback the tracer already installed rather than clobbering it -- see
// the chaining comment in map_recorder.cpp). Safe to call regardless of
// whether the tracer is enabled.
void map_recorder_init();

// Hot path: called from golden_sun_function_entry_observer for EVERY guest
// function entry, alongside (not through) gsr::function_tracer_on_entry.
// Each has its own cached flag and its own once-per-guest-frame gate, so
// either diagnostic can be enabled independently of the other. When
// GSR_MAP_RECORD is unset this is a single cached-flag branch and nothing
// else.
void map_recorder_on_entry(std::uint32_t entry_pc);

// Hot path: called for every EWRAM CPU store -- from the recorder's own
// observer, which map_recorder_on_entry installs (see map_recorder.cpp on
// why it cannot be installed from main()), or chained from
// golden_sun_wide_ewram_write_observer when widescreen owns the slot -- so
// the recorder
// can build a per-frame bitmap of which 32-byte EWRAM lines were written.
// Used to recover the room-bounds struct offsets: a room load is detected
// from the existing per-frame page-CRC diff (the metatile grid at
// 0x02010000 being wholesale rewritten), and the last few frames' bitmaps
// plus a few after are flushed to roomload_<frame>.bin in the recorder's
// session directory. No-ops in one branch when GSR_MAP_RECORD is unset.
void map_recorder_on_ewram_write(std::uint32_t address, std::uint32_t size);

// Argument capture for the six field tilemap writers.
//
// Those functions take the source and destination grid regions as arguments
// (see FACTS.md, "the region is an ARGUMENT, not a layout"), so where each BG
// layer's ids live cannot be derived from a static rule -- it has to be
// observed. The function tracer already records entry arguments, but only the
// most recent set per capture window, which yielded 13 samples across a whole
// session. This records every distinct (writer, room, arguments) combination
// with a count, to writer_args.csv in the recorder's session directory.
//
// Called for every guest function entry, so the disabled path is one flag
// test and the enabled path is a short compare against six addresses.
void map_recorder_on_function_args(std::uint32_t entry_pc, std::uint32_t r0,
                                   std::uint32_t r1, std::uint32_t r2,
                                   std::uint32_t r3);

}  // namespace gsr
