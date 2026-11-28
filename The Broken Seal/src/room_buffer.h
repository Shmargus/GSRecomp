// room_buffer.h: measured EWRAM room-table addresses used by the GPU field
// renderer (see field_game_profile.cpp).
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
//               the scroll registers UNMASKED: the game writes values wider
//               than the hardware's 9 bits and the excess is the offset
//               (FACTS.md "Per-layer sourcing")
#pragma once
