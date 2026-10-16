// Golden Sun: The Lost Age -- the cheat menu, as Golden Sun 1's.
//
// A window in the game's own style, opened with the F1 menu's rebindable
// "Cheat Menu" hotkey (default F11) anywhere in the game. The game is paused
// while it is open and every key goes to the menu (RunOptions::
// game_menu_toggle, keyinput_filter, pause_request_poll, paused_overlay).
//
// Infinite HP and PP are the engine's F1 cheats, so both menus always show
// the same state; here they fill the eight characters' records 0x02000520 +
// id * 0x14C (max HP, max PP, HP, PP at +0x34..+0x3A; F12 0048). The three
// multipliers act on the battle reward tally T = [0x03000024] + 0x550
// (FACTS.md, 2026-10-08): each defeated enemy's coins (T+0) and EXP (T+4)
// are scaled as 0x0812824C adds them, as in Golden Sun 1, so 0.5x halves
// each reward and "You got ..." shows the scaled total
// (the game caps coins at 999,999 itself, 0x080AFEB0). Drop Chance scales
// the drop roll's threshold r5 at its `ble` 0x0812846E.

#include <algorithm>
#include <cstdint>
#include <string>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kPartyHpBase = 0x02000554u;  // record 0 + 0x34
constexpr uint32_t kPartyStride = 0x14Cu;
constexpr unsigned kPartySlots = 8u;

// The tally's accumulating stores in 0x0812824C (32-bit `str`): coins on
// the bonus path and the plain path, EXP on both.
constexpr uint32_t kCoinBonusStorePc = 0x0812839Eu;
constexpr uint32_t kCoinStorePc = 0x081283F8u;
constexpr uint32_t kExpStorePc = 0x08128404u;
constexpr uint32_t kDropBranchPc = 0x0812846Eu;

// Multiplier steps, in halves so 0.5x stays exact. 25x..100x apply to
// Experience and Coins only; Drop Chance stops at 10x, where a drop is
// already all but certain.
constexpr int kStepCount = 9;
constexpr int kDropStepCount = 6;
constexpr int kStepHalves[kStepCount] = {0, 1, 2, 4, 10, 20, 50, 100, 200};
constexpr const char* kStepWords[kStepCount] = {
    "0x", "0.5x", "1x", "2x", "5x", "10x", "25x", "50x", "100x"};
constexpr int kNormalStep = 2;

constexpr int kRowCount = 5;
constexpr int kInfiniteHpRow = 0, kInfinitePpRow = 1, kExpRow = 2,
              kCoinRow = 3, kDropRow = 4;
constexpr const char* kTitle = "Cheats";
constexpr const char* kRowLabels[kRowCount] = {
    "Infinite HP", "Infinite PP", "Experience", "Coins", "Drop Chance"};
// game_options.ini keys of the multiplier rows (step index).
constexpr const char* kStepKeys[kRowCount] = {
    nullptr, nullptr, gbarecomp::kOptionExpMultiplier, gbarecomp::kOptionCoinMultiplier,
    gbarecomp::kOptionDropMultiplier};

constexpr uint16_t kKeyA = 0x01u, kKeyB = 0x02u, kKeyStart = 0x08u;
constexpr uint16_t kKeyRight = 0x10u, kKeyLeft = 0x20u, kKeyUp = 0x40u,
                   kKeyDown = 0x80u;
constexpr uint16_t kAllKeys = 0x3FFu;

// Placement in native tiles: 20 x 14 tiles, near the middle of 240x160.
// Inside: title on line 0, rows from line 16, 16 pixels apart; labels at x
// 8 as on the settings screen, values centred on x 112.
constexpr int kWindowTileX = 5, kWindowTileY = 3;
constexpr int kWindowTiles = 20, kWindowRows = 14;
constexpr int kRowLineY0 = 16, kRowPitch = 16;
constexpr int kLabelX = 8, kValueCentreX = 112;

int g_steps[kRowCount] = {0, 0, kNormalStep, kNormalStep, kNormalStep};
bool g_open = false;
bool g_requested_pause = false;
int g_row = 0;
uint16_t g_held = 0;
bool g_swallow = false;

int (*g_next_branch)(uint32_t, uint32_t, uint32_t*) = nullptr;
RuntimeMemWriteOverrideHook g_next_write_override = nullptr;

int step_count(int row) { return row == kDropRow ? kDropStepCount : kStepCount; }

void toggle() {
    g_open = !g_open;
    // Keys already held stay with whoever had them: nothing reaches the
    // menu until it is pressed again, and nothing reaches the game until
    // every key is let go after closing.
    g_held = kAllKeys;
    if (g_open) {
        g_row = 0;
    } else {
        g_swallow = true;
        for (int row = kExpRow; row < kRowCount; ++row)
            gbarecomp::game_option_set(kStepKeys[row], g_steps[row]);
    }
}

bool pause_poll(bool* out_paused) {
    if (g_open == g_requested_pause) return false;
    g_requested_pause = g_open;
    *out_paused = g_open;
    return true;
}

void change(int row, int direction) {
    if (row == kInfiniteHpRow)
        runtime_set_infinite_hp(runtime_get_infinite_hp() ? 0 : 1);
    else if (row == kInfinitePpRow)
        runtime_set_infinite_pp(runtime_get_infinite_pp() ? 0 : 1);
    else
        g_steps[row] = std::clamp(g_steps[row] + direction, 0, step_count(row) - 1);
}

uint16_t keyinput_filter(uint16_t keyinput) {
    const uint16_t pressed = static_cast<uint16_t>(~keyinput & kAllKeys);
    if (!g_open) {
        if (g_swallow) {
            if (pressed != 0) return kAllKeys;
            g_swallow = false;
        }
        return keyinput;
    }
    const uint16_t down = static_cast<uint16_t>(pressed & ~g_held);
    g_held = pressed;
    if (down & kKeyUp) g_row = (g_row + kRowCount - 1) % kRowCount;
    if (down & kKeyDown) g_row = (g_row + 1) % kRowCount;
    if (down & kKeyLeft) change(g_row, -1);
    if (down & kKeyRight) change(g_row, +1);
    if ((down & kKeyA) && g_row <= kInfinitePpRow) change(g_row, +1);
    if (down & (kKeyB | kKeyStart)) toggle();
    return kAllKeys;
}

std::string row_value(int row) {
    if (row == kInfiniteHpRow) return runtime_get_infinite_hp() ? "On" : "Off";
    if (row == kInfinitePpRow) return runtime_get_infinite_pp() ? "On" : "Off";
    return kStepWords[g_steps[row]];
}

void paint(uint8_t* rgb, uint32_t width, uint32_t height) {
    if (!g_open) return;
    tla_menu_paint_window(rgb, width, height, kWindowTileX, kWindowTileY,
                          kWindowTiles, kWindowRows);
    const int inner_x = (kWindowTileX + 1) * 8;
    const int inner_y = (kWindowTileY + 1) * 8;
    auto text_width = [](const std::string& text) {
        return static_cast<int>(tla_menu_text_width(text));
    };
    tla_menu_paint_text(rgb, width, height, kTitle,
                       inner_x + (kWindowTiles - 2) * 4 - text_width(kTitle) / 2,
                       inner_y);
    for (int row = 0; row < kRowCount; ++row) {
        const int y = inner_y + kRowLineY0 + row * kRowPitch;
        tla_menu_paint_text(rgb, width, height, kRowLabels[row], inner_x + kLabelX, y);
        std::string value = row_value(row);
        if (row == g_row) value = "< " + value + " >";
        tla_menu_paint_text(rgb, width, height, value,
                            inner_x + kValueCentreX - text_width(value) / 2, y);
    }
}

// What one tally store adds, times the step's multiplier.
int reward_store(uint32_t pc, uint32_t addr, uint32_t requested, uint32_t width,
                 uint32_t* out_value) {
    if (pc != kCoinBonusStorePc && pc != kCoinStorePc && pc != kExpStorePc)
        return g_next_write_override
                   ? g_next_write_override(pc, addr, requested, width, out_value)
                   : 0;
    const int step = g_steps[pc == kExpStorePc ? kExpRow : kCoinRow];
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !out_value || width != 4u || step == kNormalStep) return 0;
    const uint32_t before = b->read32(addr);
    const uint64_t added = static_cast<uint32_t>(requested - before);
    const uint64_t total =
        before + added * static_cast<uint64_t>(kStepHalves[step]) / 2u;
    *out_value = static_cast<uint32_t>(std::min<uint64_t>(total, 0x7FFFFFFFu));
    return *out_value != requested;
}

// The drop roll's BLE (taken = no drop), decided on the scaled threshold.
int drop_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    if (pc != kDropBranchPc)
        return g_next_branch ? g_next_branch(pc, original, out_decision) : 0;
    const int step = g_steps[kDropRow];
    if (step == kNormalStep) return 0;
    const int64_t threshold =
        static_cast<int64_t>(static_cast<int32_t>(g_cpu.R[5])) * kStepHalves[step] / 2;
    const int64_t roll = static_cast<int32_t>(g_cpu.R[0]);
    *out_decision = threshold <= roll ? 1u : 0u;
    return *out_decision != original ? 1 : 0;
}

}  // namespace

// After tla_options_load. Chains in front of the branch and write hooks already
// installed.
void tla_install_cheat_menu(gbarecomp::RunOptions& options) {
    runtime_set_party_layout(kPartyHpBase, kPartyStride, kPartySlots);
    for (int row = kExpRow; row < kRowCount; ++row) {
        const int saved = gbarecomp::game_option_get(kStepKeys[row], kNormalStep);
        g_steps[row] = saved >= 0 && saved < step_count(row) ? saved : kNormalStep;
    }
    options.game_menu_toggle = toggle;
    options.keyinput_filter = keyinput_filter;
    options.pause_request_poll = pause_poll;
    options.paused_overlay = paint;
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = drop_branch;
    g_next_write_override = g_runtime_mem_write_override;
    g_runtime_mem_write_override = reward_store;
}
