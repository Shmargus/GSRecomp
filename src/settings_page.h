#pragma once

#include <cstdint>

// A second page on Golden Sun's settings screen: Walk Speed, Encounters,
// Screen and No Slowdown.
//
// The native screen is fixed at five rows filling the native height, so the
// page is drawn by the runner over the settings window at present time; the
// game's screen underneath is never changed. Measured addresses, verified
// ROM (FACTS.md, 2026-09-22):
//
//   0x0801D4CC  settings input loop. After its per-frame wait it reads the
//               pressed-keys word 0x03001C94 (literal at 0x0801D772; Select,
//               A|Start, B) and then the repeat-keys word 0x03001B04 (literal
//               at 0x0801D7CA; up 0x40, down 0x80, left 0x20, right 0x10).
//               r8 is the settings window, r9 the cursor row (0..4, wrapped
//               by (row + 5) mod 5). Up (0x0801D7CC) and Down (0x0801D7E8)
//               are tested before Left/Right and each ends the frame's input.
//               The cursor hand is centred on window y row * 24 - 4, plus 8
//               on row 0 (0x0801D72C..0x0801D744): lines 0, 0x10 and 0x28
//               for rows 0..2, where page one's labels sit (0x0801D16C).
//   0x080162D4  window record: +0x8 width, +0xA height, +0xC x, +0xE y, in
//               tiles. Text at window (tx, ty) lands at ((x + 1) * 8 + tx,
//               (y + 1) * 8 + ty) (0x0801E7C0).
//   asset 0x13  the menu font: 8x8 4bpp tiles, tile = character code; pixel
//               1 is the letter, 3 its shadow, 4 the window background.
//               Asset table 0x08320000 (4-byte pointers).
//   0x080370D4  menu advance per character, indexed by code - 0x20.
//
// The page is entered by pressing Down on Auto-Sleep (row 4), which the game
// also takes, so its cursor hand moves to row 0, the page's first row. On
// the page the game still moves its hand for Up and Down between the page's
// rows; Up on the first row returns to page one on Auto-Sleep, and Down on
// the last row is withheld. Left/Right go to the page and the game is handed
// a zero word, so it never changes a page-one setting. A/Start/B still reach
// the game and close the screen as usual. Select is withheld (it cycles the
// window colour preset, invisible from here).
namespace gsr::settings_page {

constexpr std::uint32_t kPressedKeysLiteralPc = 0x0801D772u;
constexpr std::uint32_t kPressedKeys = 0x03001C94u;
constexpr std::uint32_t kRepeatKeysLiteralPc = 0x0801D7CAu;
constexpr std::uint32_t kRepeatKeys = 0x03001B04u;
// ROM header bytes 0xB8..0xBB are reserved and zero.
constexpr std::uint32_t kZeroWord = 0x080000B8u;

constexpr std::uint32_t kKeySelect = 0x04u;
constexpr std::uint32_t kKeyRight = 0x10u;
constexpr std::uint32_t kKeyLeft = 0x20u;
constexpr std::uint32_t kKeyUp = 0x40u;
constexpr std::uint32_t kKeyDown = 0x80u;

constexpr std::uint32_t kKeyA = 0x01u;
constexpr std::uint32_t kKeyB = 0x02u;
constexpr std::uint32_t kKeyStart = 0x08u;

constexpr std::uint32_t kAutoSleepRow = 4u;

// The help window above (text ID 0xC15 + row at (0, 0), 0x0801D758) is kept
// by the loop at [sp+0x10].
constexpr std::uint32_t kHelpWindowStackOffset = 0x10u;
constexpr int kRowCount = 4;
constexpr int kWalkSpeedRow = 0;
constexpr int kEncounterRow = 1;
constexpr int kScreenRow = 2;
constexpr int kNoSlowdownRow = 3;
constexpr const char* kRowLabels[kRowCount] = {"Walk Speed", "Encounters",
                                               "Screen", "No Slowdown"};
constexpr const char* kRowHelp[kRowCount] = {
    "Set walking speed.", "Set how often monsters appear.",
    "Set the screen size.", "Remove slowdown."};
// Text line of each row, matching the cursor hand (above).
constexpr int kRowLineY[kRowCount] = {0x00, 0x10, 0x28, 0x40};
// Page number and a chevron (down on page one, up on page two),
// right-aligned just below the settings window on both pages. The chevron
// is the font's '^', flipped for down; its shadow is redrawn one pixel down
// and right of the letter pixels, which is how the font's own shadow sits.
constexpr const char* kPageOneMarker = "1/2 ";
constexpr const char* kPageTwoMarker = "2/2 ";
constexpr unsigned char kChevronGlyph = '^';
constexpr int kChevronRows = 4;  // '^' uses glyph rows 0..3

// The window's inside reaches 4 pixel rows into its top and bottom border
// tiles (font tiles 0x11 rows 4..7 and 0x14 rows 0..3: a shaded edge).
constexpr int kFrameInnerRows = 4;

constexpr std::uint32_t kWindowWidth = 0x8u;
constexpr std::uint32_t kWindowHeight = 0xAu;
constexpr std::uint32_t kWindowX = 0xCu;
constexpr std::uint32_t kWindowY = 0xEu;

constexpr std::uint32_t kAssetTable = 0x08320000u;
constexpr std::uint32_t kFontAsset = 0x13u;
constexpr std::uint32_t kGlyphWidthTable = 0x080370D4u;
constexpr std::uint8_t kGlyphLetter = 1u;
constexpr std::uint8_t kGlyphShadow = 3u;

// Page-two layout, in window text coordinates: labels at x 8 like page
// one's, values centred on 0x7C like Message speed's, with "< " / " >" on
// the cursor's row.
constexpr int kLabelX = 8;
constexpr int kValueCentreX = 0x7C;
// Page one's "Window color" label starts at (8, 0); its first letter
// supplies the exact letter and shadow colours.
constexpr char kColourSampleLetter = 'W';

constexpr const char* kWalkingSpeedWords[3] = {"Normal", "2x", "3x"};
constexpr const char* kEncounterWords[3] = {"Normal", "Half", "Off"};
// Fixed view mode 0 is Native 240x160, 1 the expanded view (runner options).
constexpr const char* kScreenWords[2] = {"Normal", "Wide"};
// Enhanced Timing (60 Hz) and CPU Overclock (10x) together, through the
// engine's F1 settings; On only while both are on.
constexpr const char* kOnOffWords[2] = {"Off", "On"};

// Random encounters (FACTS.md, 2026-09-23). The field's encounter step
// 0x0808AEE0.. loads the base 0x02000240 at 0x0808AF84, adds the step's
// amount to the word at base + 0x238 (0x02000478) and starts a battle once
// it reaches the threshold the step keeps at [fp + 0x1AC]; the battle start
// clears it (0x0808CA5A). Redirecting that one base load to
// kZeroWord - 0x238 makes the step read 0 and store into ROM (dropped), so
// the step's amount is lost and the real counter does not move. Off does
// that on every step, Half on every other step.
constexpr std::uint32_t kEncounterBaseLiteralPc = 0x0808AF84u;
constexpr std::uint32_t kEncounterBase = 0x02000240u;
constexpr std::uint32_t kEncounterCounterOffset = 0x238u;
constexpr int kEncounterNormal = 0;
constexpr int kEncounterHalf = 1;
constexpr int kEncounterOff = 2;
// Page two is drawn in the game's own window graphics, not over the picture
// (Jimmy, 2026-09-23). Verified ROM + gpu_rewind_0005/0006 (FACTS.md,
// 2026-09-23):
//
//   [0x03001E8C]  the window system block B (EWRAM). B + 0x000 is the BG0
//                 tilemap shadow (32 cells a row, u16) that DMA3 copies to
//                 VRAM 0x06002000 (Func_160fc); B + 0xDA0 is a usage byte
//                 per text tile, index (tile & 0xFF) ^ 0x80, 1 = in use
//                 (Func_1e260 frees with &= 0xFC); B + 0xEA2 non-zero selects
//                 the 0x200..0x27F bank instead of 0x80..0xFF.
//   BG0           char base 0x06000000, 4bpp, palette bank 15; a text line
//                 has its own tiles in the bank; an empty interior cell is
//                 tile 0x20, shared and all colour 4, never written.
//   [0x03001F20]  the OAM shadow the VBlank handler copies to OAM.
//   0x0801E74C    draws the help line (id 0xC15 + row) through the decoder
//                 0x08018038, so its characters pass 0x080180D0 with the id
//                 at [sp+0x30], as the Message-speed caption does.
//
// While page two shows, each interior cell gets page two's pixels: a cell
// already on one of the game's text tiles has that tile's pixels replaced
// (the original kept and put back on returning to page one); a blank cell
// that needs text is pointed at a free text tile marked in use for as long
// as page two shows. Page one's icon sprites inside the window are hidden.
// This is redone at the end of every game frame (the frame routine's wait,
// 0x08003318), after the game's own drawing for that frame.
constexpr std::uint32_t kWindowBlockSlot = 0x03001E8Cu;
constexpr std::uint32_t kTileUsageOffset = 0xDA0u;
constexpr std::uint32_t kTileBankFlagOffset = 0xEA2u;
constexpr std::uint32_t kBg0CharBase = 0x06000000u;
constexpr std::uint32_t kBg0ScreenBase = 0x06002000u;
constexpr std::uint16_t kBlankTile = 0x20u;
constexpr std::uint16_t kTextBankFirst = 0x80u;
constexpr std::uint16_t kTextBankLast = 0xFFu;
constexpr std::uint8_t kGlyphBackground = 4u;
constexpr std::uint32_t kOamShadowSlot = 0x03001F20u;
constexpr std::uint32_t kHelpTextBase = 0xC15u;

// Kept next to the executable; the engine's config.ini is its own.
constexpr const char* kOptionsFile = "game_options.ini";
constexpr const char* kEncounterKey = "EncounterRate=";

}  // namespace gsr::settings_page
