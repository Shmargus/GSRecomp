#pragma once

#include <cstdint>

#include "game_options.h"

// Battle speed-up: Select in battle toggles 2x, shown as ">>" at the top
// left.
//
// Measured addresses, verified ROM (FACTS.md, 2026-09-23):
//
//   0x080030F8  frame routine: runs the frame's work, then waits for the
//               next VBlank. The wait clears bit 0 of the game's VBlank
//               flag 0x03001D28 with the mask 0xFFFE (literal load at
//               0x08003318), then loops `svc #2` (Halt, returns to
//               0x08003324) until the VBlank handler 0x08003650 sets it.
//   0x08003650  VBlank handler; it calls the sound engine (0x080F9018 ->
//               0x080F93A8), so music keeps VBlank time.
//   0x03001F00  heap slot that is non-zero only during battles (all 1,397
//               battle snapshots in 17 recordings; the 45 other non-zero
//               snapshots are the blank battle fade frames).
//
// On every other battle frame the runner hands the wait the mask 0xFFFF,
// so the flag the last VBlank set stays set, and drops that frame's Halt:
// the wait falls straight through and the next frame's work starts at
// once. Two frames of battle then run per VBlank. If the flag was somehow
// clear, the wait's own loop halts again as normal (the drop is one-shot).
namespace gsr::battle_speed {

constexpr std::uint32_t kWaitMaskLiteralPc = 0x08003318u;
constexpr std::uint32_t kWaitMask = 0x0000FFFEu;
constexpr std::uint32_t kKeepFlagMask = 0x0000FFFFu;
constexpr std::uint32_t kWaitHaltReturnPc = 0x08003324u;
constexpr std::uint32_t kSwiHalt = 2u;

constexpr std::uint32_t kBattleHeapSlot = 0x03001F00u;

// The battle background's per-line tables: two 0x780-byte buffers in the
// block at [0x03001ED8]; the byte at +0xF00 selects the one DMA0 scans out
// (set up each VBlank by 0x080944EC). 0x08094544 fills the other buffer and
// flips the byte (0x080946FA) once per frame of guest work. With two per
// VBlank the second would fill the buffer on screen, so on every dropped
// wait the byte is put back to what the last VBlank used (FACTS.md,
// 2026-09-23).
constexpr std::uint32_t kBattleBgTablesSlot = 0x03001ED8u;
constexpr std::uint32_t kBattleBgIndexOffset = 0xF00u;

// The arena's BG2CNT line table, the same scheme (verified ROM, 2026-09-23):
// block at [0x03001E78] ([0x03001F00] - 0x88), index word at +0. The
// VBlank-time setup 0x080C0130 points DMA0 (HBlank, dest BG2CNT) at
// block + 0x20 + index * 0x140 and copies block + 0x10 to BG2PA..BG2Y;
// 0x080C0A24 fills the other table and flips the word at 0x080C0BD0. Left
// unprotected, the second frame of a pair rewrote the table on screen and
// the bank switch landed 2-8 lines late (dark lines, gpu_rewind_0003).
constexpr std::uint32_t kBattleArenaTablesSlot = 0x03001E78u;
constexpr std::uint32_t kBattleArenaIndexOffset = 0x0u;

// Display-paced work: some battle drawing advances once per displayed frame
// while the battle script advances once per game pass, so at 2x it gets half
// its steps (FACTS.md, 2026-09-24). 2x pauses, one pass per VBlank, during
// battle entry only, as in The Lost Age (hit fades, effect canvas mode 2,
// param 0x32, do not pause).
//
// Battle entry: Func_c02a4 installs 0x080C0298 (resets BG0VOFS) as the
// VCount interrupt handler for exactly the entry sequence; its stripe task
// Func_c0228 skipped every other step at 2x and left a black-lined tile.
// 0x0800307C stores handlers in a word table at 0x030000E0, one per
// interrupt bit; slot 2 is VCount. An empty slot holds 0x08003009.
constexpr std::uint32_t kVCountHandlerSlot = 0x030000E0u + 2u * 4u;
constexpr std::uint32_t kEntryVCountHandler = 0x080C0299u;

// Layer switches wait for a real VBlank (FACTS.md 2026-10-15,
// gpu_rewind_0045 frames 4-5 and 103-104). The game queues register writes
// for its VBlank handler: count u16 at 0x02002090 (at most 32), then 12-byte
// entries from 0x02002094 {value, address, 0x20000} (helper 0x0800387C).
// An attack's teardown queues DISPCNT 0x1541 (effect layer BG1 off), waits
// one frame, then writes the arena's map back into BG1's map (0x080CDD14);
// when that wait was the dropped one, the map showed on a BG1 still lit and
// blended: a one-frame flash after every regular attack. A pass that leaves
// a DISPCNT write queued therefore keeps its wait.
constexpr std::uint32_t kVBlankQueueCount = 0x02002090u;
constexpr std::uint32_t kVBlankQueueEntries = 0x02002094u;
constexpr std::uint32_t kVBlankQueueEntrySize = 12u;
constexpr std::uint32_t kVBlankQueueMax = 32u;
constexpr std::uint32_t kDispcnt = 0x04000000u;

// Guest CPU overclock held while 2x runs in battle: the F1 menu's On value.
constexpr unsigned kMinOverclock = 10u;

// KEYINPUT, active low. Select toggles in battle only, as in The Lost Age.
constexpr std::uint32_t kKeyInput = 0x04000130u;
constexpr std::uint16_t kKeySelect = 0x0004u;

// ">>" in the game's menu font, in native screen pixels: the top-left
// corner, just below the party window (y 20 overlapped its Djinn counts).
constexpr const char* kIndicator = ">>";
constexpr int kIndicatorX = 4;
constexpr int kIndicatorY = 32;

constexpr const char* kOptionsKey = gbarecomp::kOptionBattleSpeed;

// Field Psynergy speed-up, The Lost Age's rule (FACTS.md 2026-10-15). The field Psynergy dispatcher 0x0808E680
// (The Lost Age's 0x080CE61C, same layout) runs the cast inside one of two
// cast blocks: 0x08096960 (bl at 0x0808E8CA) or 0x08096810 (bl at
// 0x0808E8D0), chosen by flags 0x140/0x141; both read the cast object in
// [0x03001F30] and switch on its kind at +0x1E. So the return into the
// dispatcher sits in a live stack frame for as long as the cast runs, and
// is gone for the target's reaction and for scenes that only run a cast's
// routines (Mars Star, FACTS.md 2026-10-05). With the setting on Fast those
// frames run at the same 2x as battle.
constexpr std::uint32_t kFieldCastSlot = 0x03001F30u;
constexpr std::uint32_t kFieldDispatcherReturns[] = {0x0808E8CFu, 0x0808E8D5u};
constexpr std::uint32_t kStackBottom = 0x03007000u;
constexpr std::uint32_t kStackTop = 0x03007F00u;

// Reveal's range (ROM 0x080982DC, run every frame while Reveal is up; FACTS
// 2026-09-30). With S = [0x03001EBC]: the cast spot is S+0xCBC (x) and
// S+0xCBE (y), a countdown S+0xCBA. It forms dx*0xD105>>16 and dy from the
// leader, compares dx*dx + dy*dy (r2) with 0xE10 (60 px squared) and, when
// that is reached or the countdown is 0, loads 0x2090 at 0x08098370 and
// stores it to S+0x17E, which ends Reveal. Better Field Psy doubles the
// distance: the load then returns S+0x17E's own value (the store changes
// nothing) while r2 is under 4 * 0xE10 and the countdown is not 0.
constexpr std::uint32_t kRevealEndLiteralPc = 0x08098370u;
constexpr std::uint32_t kRevealEndCommand = 0x00002090u;
constexpr std::uint32_t kRevealStatePointer = 0x03001EBCu;
constexpr std::uint32_t kRevealCountdownOffset = 0xCBAu;
constexpr std::uint32_t kRevealCommandOffset = 0x17Eu;
constexpr std::uint32_t kRevealRangeSquared = 0xE10u;
constexpr std::uint32_t kRevealBetterRangeSquared = kRevealRangeSquared * 4u;

}  // namespace gsr::battle_speed
