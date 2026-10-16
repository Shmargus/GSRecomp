// gba_irq.h — IRQ timing shared by both execution engines. IE / IF / IME
// live directly in GbaIo's register array; there is no separate IRQ device.
//
// Reference: GBATEK § "GBA Interrupt Control" and ARM ARM ARMv4T A2.6.

#pragma once

#include <cstdint>

namespace gba {

// Wake-from-HALT IRQ latency, in CPU cycles. On hardware the ARM7TDMI does
// NOT vector the instant an IRQ pends out of HALT — there is a fixed wake +
// pipeline-refill latency before exception entry. Both execution engines must
// model the SAME latency or their per-frame VBlank phase drifts: the game
// VBlankIntrWaits (SWI Halt) every frame, so this delay applies once per
// frame. The recomp omitting it (taking the IRQ immediately) vectored ~7
// PPU-cycles early each frame, shifting m4a sequencer phase enough to
// double-tick a sound channel once a fade became active → MC-HP-002 hang.
// Single source of truth, shared by the interpreter oracle (bios_smoke) and
// the recompiled runtime (runtime_bus_bridge runtime_tick).
constexpr uint32_t kIrqWakeDelayCycles = 7;

}  // namespace gba
