#pragma once

#include <cstdint>

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
// its steps (FACTS.md, 2026-09-24). 2x pauses, one pass per VBlank, while
// either marker below holds.
//
// Battle entry: Func_c02a4 installs 0x080C0298 (resets BG0VOFS) as the
// VCount interrupt handler for exactly the entry sequence; its stripe task
// Func_c0228 skipped every other step at 2x and left a black-lined tile.
// 0x0800307C stores handlers in a word table at 0x030000E0, one per
// interrupt bit; slot 2 is VCount. An empty slot holds 0x08003009.
constexpr std::uint32_t kVCountHandlerSlot = 0x030000E0u + 2u * 4u;
constexpr std::uint32_t kEntryVCountHandler = 0x080C0299u;

// Hit fades: the effect canvas task Func_cd260 keeps its block at
// [0x03001EEC]; the word at +0x7780 selects the copy, and mode 2 is the fading
// one (Func_54e4 "copy and fade", or Func_5534 "copy and halve" when +0x7784
// is 0x32). Every regular attack runs mode 2, param 75 then 0x32; cut to
// half its steps, the halving phase left the canvas lit when the game
// cleared it, and it flashed over the arena. Only that phase pauses: the
// whole of mode 2 also slowed spells (Jimmy, 2026-09-24). Each mode/param
// change in a 2x battle is logged as [battle-speed] so spells' own values
// can be measured.
constexpr std::uint32_t kEffectCanvasBlockSlot = 0x03001EECu;
constexpr std::uint32_t kEffectCanvasModeOffset = 0x7780u;
constexpr std::uint32_t kEffectCanvasParamOffset = 0x7784u;
constexpr std::uint32_t kEffectCanvasFadeMode = 2u;
constexpr std::uint32_t kEffectCanvasHalveParam = 0x32u;
// The halving phase is the same for spells, so it is told apart by length:
// regular attacks' ran 35, 41, 35 and 35 displayed frames at normal speed,
// Flare's and other spells' 91-141 (session 20260924_104009). Only its first
// kMaxPacedFadeFrames pause, which covers an attack whole and hands a long
// spell fade back to 2x (Jimmy: Flare stayed slow).
constexpr unsigned long long kMaxPacedFadeFrames = 48u;
// Spells are also told apart directly: their canvas runs drawing mode 1 or 3
// before the fade (Flare: mode 3 for 23 frames, then the halving phase), and
// no regular attack's does, across sessions 20260924_104009 and _142353.
// A canvas that has been in either mode since its block appeared never
// pauses (Jimmy: Flare still ran at normal speed with the 48-frame cap).
constexpr std::uint32_t kEffectCanvasSpellModeA = 1u;
constexpr std::uint32_t kEffectCanvasSpellModeB = 3u;

// Guest CPU overclock held while 2x runs in battle: the F1 menu's On value.
constexpr unsigned kMinOverclock = 10u;

// KEYINPUT, active low. Jimmy moved the toggle from L+R to Select
// (2026-09-23).
constexpr std::uint32_t kKeyInput = 0x04000130u;
constexpr std::uint16_t kKeySelect = 0x0004u;

// Outside battle, L+R+Select toggles the saved setting, so a battle can be
// replayed from a savestate at either speed (Jimmy, 2026-09-24). The
// indicator then shows the new state for kNoticeFrames: ">>" on, "1x" off.
constexpr std::uint16_t kKeyR = 0x0100u;
constexpr std::uint16_t kKeyL = 0x0200u;
constexpr std::uint16_t kFieldToggleKeys = kKeySelect | kKeyL | kKeyR;
constexpr const char* kIndicatorOff = "1x";
constexpr unsigned kNoticeFrames = 90u;

// ">>" in the game's menu font, in native screen pixels: the top-left
// corner, just below the party window (y 20 overlapped its Djinn counts,
// Jimmy's screenshot 2026-09-23).
constexpr const char* kIndicator = ">>";
constexpr int kIndicatorX = 4;
constexpr int kIndicatorY = 32;

constexpr const char* kOptionsKey = "BattleSpeed=";

// Field Psynergy speed-up (FACTS.md 2026-09-29, session 20260929_193801): a
// field Psynergy cast is in progress on exactly the frames in which the game
// entered Func_96f50 (Whirlwind, every frame) or Func_96c80 (Move, every
// frame); neither ran outside casts. With the setting on Fast those frames
// run at the same 2x as battle.
constexpr std::uint32_t kFieldPsynergyEntryA = 0x08096F50u;
constexpr std::uint32_t kFieldPsynergyEntryB = 0x08096C80u;

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
