// gba_irq.h: IRQ timing shared by both execution engines. IE / IF / IME
// live directly in GbaIo's register array; there is no separate IRQ device.
//
// Reference: GBATEK § "GBA Interrupt Control" and ARM ARM ARMv4T A2.6.

#pragma once

#include <cstdint>

namespace gba {

// Wake-from-HALT IRQ latency, in CPU cycles.
// On hardware the ARM7TDMI does not vector the instant an IRQ pends out of
// HALT. A fixed wake and pipeline-refill latency passes before exception
// entry. Both execution engines must model the same latency, or their
// per-frame VBlank phase drifts.
// The game calls VBlankIntrWait (SWI Halt) every frame, so this delay applies
// once per frame. When the recomp omitted it (taking the IRQ immediately), the
// IRQ vectored ~7 PPU cycles early each frame. That shifted the m4a sequencer
// phase enough to double-tick a sound channel once a fade became active, which
// hung the game.
// This is the one definition, shared by the interpreter oracle (bios_smoke)
// and the recompiled runtime (runtime_bus_bridge runtime_tick).
constexpr uint32_t kIrqWakeDelayCycles = 7;

}  // namespace gba
