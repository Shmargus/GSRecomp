#pragma once

#include <cstdint>

// Hard Mode: enemies get 1.5x HP and 1.25x Attack and Defence. The switch is
// the settings screen's fourth row, the game's own Auto-Sleep row, kept with
// its storage: the screen's byte is saved in save block +0x22A and copied to
// the live flag, so the choice is per save file and can change any time
// (verified ROM, FACTS.md 2026-10-04, "Settings row 4 (Auto-Sleep) and the
// auto-sleep flag").
//
// Hard Mode is stored as 2 (kHardModeValue); 0 and 1 both mean Off. Value 1
// is the old Auto-Sleep On, which an existing save (or the title menu, at
// 0x020084BA) may hold and which must read as Off. The settings screen only
// knows 0/1, so the runner translates around it: on entry to setup it writes
// 2 -> 1, else 0 to save +0x22A; on entry to teardown, after A/Start stored
// the screen's byte (r5 = 0), it turns 1 back into 2 in both +0x22A and the
// live flag; after B (r5 = -1) it restores the original save byte. A new
// game asks "Hard Mode?" after naming (entry to Func_77f40) and stores 2 or
// 0 to both places. Auto-Sleep's only effect, the 3-minute idle sleep in the
// frame wait, is switched off by forcing the branch at kIdleBranchPc.
//
// Enemy stats: Func_79460 copies an enemy's table data into the unit, then
// calls Func_77428 (`bl` at 0x080795BC) which rebuilds every stat from the
// base fields (FACTS.md 2026-10-04, "Enemy battle stats are set up by
// Func_79460"). The runner scales those base fields on entry to Func_77428
// when it was called from there (r6 = unit).
namespace gsr::hard_mode {

// Live flag, kHardModeValue = Hard Mode on.
constexpr std::uint32_t kAutoSleepFlag = 0x03001D08u;
// Save block +0x22A, the flag's saved copy.
constexpr std::uint32_t kSavedFlag = 0x0200046Au;
constexpr std::uint8_t kHardModeValue = 2;
// New game: Func_77f40 follows the naming loop; the prompt opens on entry.
constexpr std::uint32_t kNewGameEntryPc = 0x08077F40u;
// Frame wait Func_30f8: `beq 0x080031E4` skips the idle counter.
constexpr std::uint32_t kIdleBranchPc = 0x080031BCu;

constexpr std::uint32_t kStatRecalcPc = 0x08077428u;
// LR after the `bl` at 0x080795BC in Func_79460 (compare with R[14] & ~1).
constexpr std::uint32_t kEnemySetupReturn = 0x080795C0u;

// Unit fields, all 16-bit. Max and current HP are set from the same table
// value as the base HP; Attack and Defence are base values.
constexpr std::uint32_t kBaseHpOffset = 0x10u;
constexpr std::uint32_t kMaxHpOffset = 0x34u;
constexpr std::uint32_t kCurrentHpOffset = 0x38u;
constexpr std::uint32_t kBaseAttackOffset = 0x18u;
constexpr std::uint32_t kBaseDefenceOffset = 0x1Au;
constexpr std::uint32_t kStatMax = 0x7FFFu;
// Integer, rounded down.
constexpr std::uint32_t kHpNumerator = 3u;
constexpr std::uint32_t kHpDenominator = 2u;
constexpr std::uint32_t kStatNumerator = 5u;
constexpr std::uint32_t kStatDenominator = 4u;

// The settings screen's function entry (setup 0x0801D014); the label is drawn
// during setup, before the screen's loop runs.
constexpr std::uint32_t kSettingsSetupEntryPc = 0x0801D014u;
// Teardown: r5 = 0 after A/Start's stores, -1 after B (no stores).
constexpr std::uint32_t kSettingsTeardownEntryPc = 0x0801D0F0u;

// Text: label 0xC12 ("Auto-Sleep"), help 0xC19 ("Turn auto-sleep on or off.")
// and the caption 0xC13 + value are replaced character by character. The
// caption is written whole, to add the arrows around the cursor's row.
constexpr std::uint32_t kLabelTextId = 0x00000C12u;
constexpr std::uint32_t kHelpTextId = 0x00000C19u;
constexpr std::uint32_t kCaptionBase = 0x00000C13u;
constexpr const char* kLabelText = "Hard Mode";
constexpr const char* kHelpText = "Make enemies tougher.";
constexpr const char* kPromptTitle = "Hard Mode?";
constexpr const char* kPromptLines[3] = {
    "Enemies get more HP,", "Attack and Defence.", "Change it in Settings."};
constexpr const char* kPromptRows[2] = {"No", "Yes"};
constexpr const char* kWords[2] = {"Off", "On"};

// The row is text only, like Message speed. Icon table 0x080367CE = {0x27,
// 0x28}: loaded at setup (two icons) and once per icon in the redraw loop.
constexpr std::uint32_t kSetupIconTableLiteralPc = 0x0801D412u;
constexpr std::uint32_t kRedrawIconTableLiteralPc = 0x0801D624u;
constexpr std::uint32_t kIconTable = 0x080367CEu;
constexpr std::uint32_t kSetupIconCount = 2u;
constexpr std::uint32_t kFirstIcon = 0x27u;
constexpr std::uint32_t kLastIcon = 0x28u;

// Caption: base literal (value in r2, cursor row in r9), clear x and draw x.
constexpr std::uint32_t kCaptionBaseLiteralPc = 0x0801D6FCu;
constexpr std::uint32_t kCaptionClearXPc = 0x0801D706u;
constexpr std::uint32_t kCaptionDrawXPc = 0x0801D716u;

}  // namespace gsr::hard_mode
