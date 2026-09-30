#pragma once

#include <cstdint>

// The cheat menu: a window in Golden Sun's own style, opened with the F1
// menu's rebindable "Cheat Menu" hotkey (default F11) anywhere in the game.
// The game is paused while it is open and every key goes to the menu
// (RunOptions::game_menu_toggle / keyinput_filter / paused_overlay).
//
// Infinite HP and PP are the engine's F1 cheats (runtime_set_infinite_hp/pp),
// so both menus always show the same state. The three multipliers act on
// Golden Sun's battle reward tally, Func_c24f0, run once per defeated enemy
// (verified ROM, FACTS.md 2026-09-25). It adds the enemy's coins (enemy
// data +0x4C) to total +0 and its experience (+0x52) to total +4, at
// [0x03001E74] + 0x530, then rolls its drop. Func_c2724 pays the total out:
// +4 to every party member's experience (+0x124) with text 0x83A, +0 as
// coins (0x08079700, clamped to 999,999) with text 0x83B.
namespace gsr::cheat_menu {

// The tally's three accumulating stores. 0x080C25F6 is the coin store on
// the bonus path, 0x080C2650 on the plain path; both paths store experience
// at 0x080C265C. The runner scales what each store adds.
constexpr std::uint32_t kCoinBonusStorePc = 0x080C25F6u;
constexpr std::uint32_t kCoinStorePc = 0x080C2650u;
constexpr std::uint32_t kExpStorePc = 0x080C265Cu;

// The drop roll: threshold r5 = 0x20000 >> rate (enemy data +0x50, 2 less on
// the bonus path), roll r0 = random & 0xFFFF; `cmp r5, r0` then this BLE
// skips the drop when r5 <= r0. The runner compares r5 * multiplier instead.
constexpr std::uint32_t kDropBranchPc = 0x080C26BEu;

// Multiplier steps, in halves so 0.5x stays exact.
// 25x..100x apply to Experience and Coins only; Drop Chance stops at 10x
// (kDropStepCount), where a drop is already all but certain.
constexpr int kStepCount = 9;
constexpr int kDropStepCount = 6;
constexpr int kStepHalves[kStepCount] = {0, 1, 2, 4, 10, 20, 50, 100, 200};
constexpr const char* kStepWords[kStepCount] = {
    "0x", "0.5x", "1x", "2x", "5x", "10x", "25x", "50x", "100x"};
constexpr int kNormalStep = 2;

constexpr int kRowCount = 5;
constexpr int kInfiniteHpRow = 0;
constexpr int kInfinitePpRow = 1;
constexpr int kExpRow = 2;
constexpr int kCoinRow = 3;
constexpr int kDropRow = 4;
constexpr const char* kTitle = "Cheats";
constexpr const char* kRowLabels[kRowCount] = {
    "Infinite HP", "Infinite PP", "Experience", "Coins", "Drop Chance"};

// game_options.ini keys (step index 0..8; Drop 0..5).
constexpr const char* kExpKey = "ExpMultiplier=";
constexpr const char* kCoinKey = "CoinMultiplier=";
constexpr const char* kDropKey = "DropMultiplier=";

// The window, in the game's own pieces: menu font tiles 16..23 are its
// corners and edges and 32 its inside, drawn with background palette bank
// 15 (measured on gpu_rewind_0025's item menu, BG0 map 0x2000).
constexpr unsigned char kTopLeft = 16, kTop = 17, kTopRight = 18;
constexpr unsigned char kBottomLeft = 19, kBottom = 20, kBottomRight = 21;
constexpr unsigned char kLeft = 22, kRight = 23, kFill = 32;
constexpr int kWindowBank = 15;
// Bank 15 as that capture held it; used when the live bank is not the
// window palette (letter 0x7FFF at index 1 and shadow 0 at index 3).
constexpr std::uint16_t kDefaultWindowPalette[16] = {
    0x44E0, 0x7FFF, 0x318C, 0x0000, 0x4580, 0x3960, 0x3140, 0x2920,
    0x4DA1, 0x55C1, 0x5E01, 0x294A, 0x5294, 0x001F, 0x03FF, 0x7C00};

// Placement in native tiles: 20 x 14 tiles, near the middle of 240x160.
constexpr int kWindowTileX = 5;
constexpr int kWindowTileY = 3;
constexpr int kWindowTiles = 20;
constexpr int kWindowRows = 14;
// Inside the window: title on line 0, rows from line 16, 16 pixels apart;
// labels at x 8 as on the settings screen, values centred on x 112.
constexpr int kRowLineY0 = 16;
constexpr int kRowPitch = 16;
constexpr int kLabelX = 8;
constexpr int kValueCentreX = 112;

}  // namespace gsr::cheat_menu
