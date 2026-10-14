#pragma once

#include <cstdint>

// Golden Sun USA/Europe metadata belongs to the project runner, not the
// reusable ARMv4T runtime. These identities were measured from the generated
// image and the hash-verified ROM (FACTS.md, 2026-09-10).
namespace gsr::text_speed_cheat {

// The dialogue text processor 0x080168F4 keeps a per-call token allowance in
// the stack slot [sp+0x20]. Four sites write it:
//
//   0x08016920  seed from the table at 0x0807380B, indexed by the Message
//               speed byte at 0x0200044C -- {1, 1, 10} for Slow/Normal/Fast.
//   0x08016942  replacement with 5*clamp(halfword(0x03001CD0), 0, 2)+3, taken
//               only when the byte at r8+0xEA5 is nonzero (the path battle
//               text uses; measured at 8 there).
//   0x08016EDC  forced stop on a space token (r7 == 0x20), when that same
//               r8+0xEA5 byte is zero.
//   0x08016A34, 0x08016D72  forced stops on the box/line-break paths.
//
// Session 20260910_225128 measured that the Fast allowance is never spent:
// all 156 invocations ended at 0x08016EDC, the deepest countdown being
// 10 -> 5. Fast therefore draws one WORD per call, not ten tokens, and
// raising the table value alone changes nothing.
//
// Instant text overrides exactly two of those writes: the seed, and the
// space stop. The line-break and box stops at 0x08016A34 / 0x08016D72 are
// deliberately left alone, so page waits and "press A to continue" keep
// their original behaviour.
constexpr std::uint32_t kBudgetSeedPc = 0x08016920u;
constexpr std::uint32_t kWordStopPc = 0x08016EDCu;

// One page of dialogue is far below this. The counter is decremented once per
// parsed token and the slot is a full word, so a large value simply means the
// parser reaches its own exit before the allowance runs out.
constexpr std::uint32_t kInstantBudget = 255u;

// Message speed lives at 0x02000240 + 0x20C; 0 = Slow, 1 = Normal, 2 = Fast.
// Instant text is a fourth value, 3, so the in-game option selects
// Slow / Normal / Fast / Instant.
constexpr std::uint32_t kMessageSpeedAddress = 0x0200044Cu;
constexpr std::uint32_t kMessageSpeedInstant = 3u;

constexpr bool matches(std::uint32_t pc, std::uint32_t width) {
    return width == 4u && (pc == kBudgetSeedPc || pc == kWordStopPc);
}

// The settings screen's setup 0x0801D014 stores each option's choice count;
// the screen's input loop 0x0801D4CC wraps left/right by it. 0x0801D072 is
// `movs r3,#3`, Message speed's count. Four makes value 3 reachable.
constexpr std::uint32_t kChoiceCountPc = 0x0801D072u;
constexpr std::uint32_t kChoiceCountWithInstant = 4u;

// The settings screen's per-frame routine 0x0801CF48 bounces the selected
// icon of the Message-speed row through slot (row * 3 + value) at +0x5D4 of
// the screen struct (r5); the row has three slots, so value 3 lands on the
// next row's first icon. 0x0801CFC6 is `ble` on the row index (r6) that skips
// the bounce; forcing it taken leaves no icon bouncing for Instant.
constexpr std::uint32_t kIconBounceBranchPc = 0x0801CFC6u;
constexpr std::uint32_t kMessageSpeedRow = 2u;
constexpr std::uint32_t kScreenMessageSpeedOffset = 0x596u;

// The Message-speed caption is text ID 0xC0A + value, loaded at 0x0801D6AE
// (`ldr r3,=0xC0A`; the screen's value is in r2, the cursor row in r9).
// Value 3 reaches 0xC0D, "Message" -- the row's own label, which setup also
// draws. The runner writes the whole caption itself: the text decoder
// 0x08018038 takes each decoded character at 0x080180D0 (`adds r7,r0,#0`,
// ASCII; the text ID is at [sp+0x30]) and ends the string on 0 (0x080182A6
// -> 0x0801831A). The caption is the word, with "< " and " >" around it
// while the cursor is on this row.
constexpr std::uint32_t kCaptionBaseLiteralPc = 0x0801D6AEu;
constexpr std::uint32_t kCaptionBase = 0x00000C0Au;
constexpr std::uint32_t kDecodedCharPc = 0x080180D0u;
constexpr std::uint32_t kDecoderTextIdStackOffset = 0x30u;
constexpr const char* kSpeedWords[4] = {"Slow", "Normal", "Fast", "Instant"};
constexpr char kArrowLeft[] = "< ";
constexpr char kArrowRight[] = " >";

// Menu text advances by the per-character width table 0x080370D4, indexed by
// code - 0x20 (it matches every advance in the 2026-09-14 glyph trace).
// Menu text and menu icons share one window-relative x: the Auto-Sleep and
// Speech icons sit at 0x64 and 0x7C with a 24-pixel pitch, spanning
// 0x64..0x94, so the caption is centred on 0x7C.
constexpr std::uint32_t kGlyphWidthTable = 0x080370D4u;
constexpr std::uint32_t kCaptionCentreX = 0x7Cu;

// The Message-speed row is text only: its three icons are uploaded blank and
// the caption word sits where they began. Setup reads the row's icon table
// 0x080367C9 {34, 35, 36} at 0x0801D39C and creates three icons; the redraw
// loop re-reads it at 0x0801D5B6 once per icon. Both go through the icon
// loader 0x080216E8 (icon ID in r6), whose `adds r2,r5,#0` at 0x08021732
// hands the unpacked picture to the OBJ upload 0x08003FA4. A source of
// 0xFFFFFFFF makes that upload clear the slot instead (0x08003FFA ->
// 0x03000164, a zero fill). Caption clear and draw take x = 0xA0 at
// 0x0801D6B8 / 0x0801D6C6; the first icon's x is 0x54.
constexpr std::uint32_t kSetupIconTableLiteralPc = 0x0801D39Cu;
constexpr std::uint32_t kRedrawIconTableLiteralPc = 0x0801D5B6u;
constexpr std::uint32_t kMessageSpeedIconTable = 0x080367C9u;
constexpr std::uint32_t kSetupIconCount = 3u;
constexpr std::uint32_t kIconSourcePc = 0x08021732u;
constexpr std::uint32_t kFirstMessageSpeedIcon = 34u;
constexpr std::uint32_t kLastMessageSpeedIcon = 36u;
constexpr std::uint32_t kBlankIconSource = 0xFFFFFFFFu;
constexpr std::uint32_t kCaptionClearXPc = 0x0801D6B8u;
constexpr std::uint32_t kCaptionDrawXPc = 0x0801D6C6u;
constexpr std::uint32_t kCaptionClearX = 0x54u;

// Two ROM tables are indexed by the Message-speed byte and hold only three
// entries, so index 3 reads the next bytes: the dialogue delay table
// 0x08073808 {4, 0, 0} (index 3 = 1) and the battle text table 0x0809FC28
// {1, 3, 10} (index 3 = 0). Re-basing the table pointer one byte lower makes
// index 3 read Fast's entry. Applied whenever the byte is 3, launcher option
// or not, so a save holding 3 never reads the out-of-range bytes.
constexpr std::uint32_t kDelayTableLiteralPc = 0x08016E56u;
constexpr std::uint32_t kDelayTable = 0x08073808u;
constexpr std::uint32_t kBattleTableLiteralPc = 0x08093332u;
constexpr std::uint32_t kBattleTable = 0x0809FC28u;

}  // namespace gsr::text_speed_cheat
