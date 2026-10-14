// room_buffer.h — milestone 2, step 1: build a room's tilemap ahead of time
// and prove it is right.
//
// Nothing here changes what appears on screen. It builds, from the EWRAM map
// tables, the tile entries the game is about to put in VRAM, then compares
// them against what the game actually put there. A mismatch count of zero
// over a session is the evidence that a room can be reconstructed offline —
// which is the whole premise of milestone 2 (see ROADMAP.md).
//
// Doing it this way round was a deliberate choice: verifying against live
// VRAM is exact, needs no renderer change, and cannot be fooled the way a
// "looks right on screen" check can. The old widescreen work failed three
// times partly because its per-pixel answers were never checked against
// anything.
//
// Every address and rule below is measured, with the evidence in FACTS.md.
// None of it is inferred from how the code "ought" to work:
//
//   room rect   0x02030DC0  four u16: min_x, max_x, min_y, max_y, in pixels
//               ("The bounds field offsets are recovered")
//   camera      0x02030DB0  x and y, 16.16 fixed point, same pixel space
//   id grid     0x02010000  128x128 u32, low 12 bits are the metatile id
//   atlas       0x02020000  8 bytes per id: four u16 text-mode tile entries,
//                           top-left, top-right, bottom-left, bottom-right
//   per layer   the layer's own grid region is offset from the ground layer
//               by (BGxHOFS - camera_x)/16, (BGxVOFS - camera_y)/16, taking
//               the scroll registers UNMASKED — the game writes values wider
//               than the hardware's 9 bits and the excess is the offset
//               ("Per-layer sourcing, settled 2026-09-05")
#pragma once

#include <cstdint>

namespace gsr {

// Caches the GSR_ROOM_BUFFER env flag and, only when set, wires the checker's
// readout into host_config_ui.h's extra-draw slot. Call once at startup,
// after function_tracer_init() and map_recorder_init(), so this chains onto
// whatever they installed rather than replacing it.
void room_buffer_init();

// Hot path: called for every guest function entry, gated to once per guest
// frame internally. Rebuilds the visible window from the EWRAM tables and
// compares it against live VRAM.
void room_buffer_on_entry(std::uint32_t entry_pc);

// Observes the field tilemap writers' arguments so the checker knows where in
// the grid each layer reads. That is not derivable: the writer takes the grid
// cell as an argument and the offsets between layers are per-room, with even
// the axis changing between rooms (FACTS.md, "The per-layer regions, observed
// directly"). The game states them; this listens.
void room_buffer_on_function_args(std::uint32_t entry_pc, std::uint32_t r0,
                                  std::uint32_t r1, std::uint32_t r2);

// Screen coordinate -> field tile entry, for the PPU callback below. False
// means "not known"; the caller keeps its own answer.
bool room_buffer_supply(int bg, int screen_x, int screen_y,
                        std::uint16_t* out_entry);

// True when the field is being drawn from the room buffer. Runners use this to
// stand down margin heuristics that exist only because there was no real
// source for margin content.
bool room_buffer_rendering();

}  // namespace gsr

// Answers the PPU's g_ws_field_tilemap_source for the three field layers.
// Screen coordinates in, screen entry out; returns 0 whenever the answer is
// not known so the hardware fetch stands. Installed by room_buffer_init()
// only when GSR_ROOM_BUFFER_RENDER is set. Outside namespace gsr because the
// PPU takes it as a plain C function pointer.
extern "C" int gsr_field_tilemap_source(int bg, int screen_x, int screen_y,
                                        std::uint16_t* out_entry);
// Paired row-boundary hook. The PPU calls this once before invoking the source
// for a render row; it prepares the row's register state plus the active bus
// data and one-tile map cache used by the per-pixel callback.
extern "C" void gsr_field_tilemap_source_begin(int screen_y,
                                                 std::uint16_t dispcnt,
                                                 const std::uint8_t* io);
