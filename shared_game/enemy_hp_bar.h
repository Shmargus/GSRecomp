#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

// Enemy HP bar, both games: while the player picks a target in battle, a
// small red bar with the HP number sits just above the targeted enemy's
// name. F1 switch, off by default. One copy (enemy_hp_bar.cpp); each runner
// installs its game's addresses and paints with its own menu font.
//
// The Broken Seal, measured, verified ROM (FACTS.md, 2026-10-29, Saves
// state 4, Creeper battle):
//
//   0x08026080  target-selection routine, shared by its six callers
//               (0x08027638, 0x08027896, 0x08027ADE, 0x08027C72,
//               0x08027D54, 0x08027F54). Prologue pushes 0x20 bytes and
//               reserves 0x144, so its sp stays fixed for the loop.
//   0x08026E18  the loop's per-frame call to the frame routine 0x080030F8
//               (return 0x08026E1C); it then branches back to the loop head
//               0x08026388 until a target is confirmed or B cancels.
//   [sp+0x1C]   candidate list, u16 unit IDs (party 0..3, enemies 0x80..)
//   [sp+0x3C]   number of candidates (s32; 0x08026552 bounds the list by it)
//   [sp+0x44]   cursor index into that list (s32)
//
// Bench: two candidates; with the cursor on Creeper 1 the index read 0 and
// list[0] 0x80, on Creeper 2 index 1 and 0x81; the cursor sprite's x read
// 73 and 121.
//
// Enemy unit record: [0x03001F28] + (unit - 0x80) * 0x14C. Name at +0
// ("Creeper1"), max HP u16 at +0x34, current HP u16 at +0x38 (89/89 before
// Isaac's attack, 28/89 after).
//
// Enemy name label (ROM, F12 gpu_rewind_0062, Storm Lizard):
//
//   0x08026354  opens the label window with 0x080162D4(0, 12, 30, 4): tile
//               (0, 12), so its inside starts at pixel (8, 104).
//   0x08026B88  draws the name with 0x08017AA4(text, window, x, 4) (return
//               0x08026B8C): r0 the u16 name, r2 its x inside the window,
//               centred on the enemy with the wider table at 0x08032224
//               and clamped to 0..224. The text itself is the menu font
//               (widths 0x080370D4): "Storm Lizard" measures 70 there and
//               70 pixels on screen (x 60..129, top row y 108, x arg 51).
//
// The Lost Age (ROM only, SHA-1 verified; its FACTS.md 2026-10-30): the same
// routine at 0x0804A85C (same frame, sub sp 0x144), frame call 0x08013560
// from 0x0804B642 (return 0x0804B646), candidate list pointer [sp+0x14]
// (0x0804A994, read at 0x0804B5D8), count [sp+0x3C], index [sp+0x44]; label
// window 0x08039260(0, 12, 30, 4) at 0x0804AB78; name 0x0803AAE4(text,
// window, x, 4) from 0x0804B3A8 (return 0x0804B3AC), x clamped to 0..0xE0;
// enemy units [0x030000D8] + (unit - 0x80) * 0x14C (0x08016CA4), max HP
// +0x34, HP +0x38 (src/tla_hard_mode.cpp).
namespace gsr::enemy_hp_bar {

struct Game {
    std::uint32_t frame_routine_pc, target_loop_return;
    std::uint32_t name_draw_pc, name_draw_return;
    std::uint32_t candidate_list_sp;
    std::uint32_t enemy_base_slot;
};

constexpr int kNameInsideX = 8;
constexpr int kNameTop = 108;
constexpr std::uint32_t kCandidateCountSp = 0x3Cu;
constexpr std::uint32_t kCursorIndexSp = 0x44u;

constexpr std::uint32_t kUnitStride = 0x14Cu;
constexpr std::uint16_t kFirstEnemy = 0x80u;
constexpr std::uint16_t kLastEnemy = 0x85u;
constexpr std::uint32_t kMaxHpOffset = 0x34u;
constexpr std::uint32_t kCurrentHpOffset = 0x38u;

// The bar, in native pixels, centred over the name. Its inside is 8 rows,
// one menu-font line, for the HP number; 36 wide fits four 7-pixel digits.
constexpr int kBarWidth = 40;
constexpr int kBarHeight = 12;
constexpr int kBarGap = 2;
constexpr int kScreenWidth = 240;

// F1's Game page.
constexpr const char* kOptionKey = "EnemyHpBar";
constexpr const char* kSwitchLabel = "Enemy HP bar";
constexpr const char* kSwitchCaption =
    "In battle, while you choose a target, show a small red bar with the "
    "enemy's HP above its name.";

void install(const Game& game);
// Function entries, emulation thread. name_width: the drawn name's width in
// pixels, from its u16 text at `name`.
void on_entry(std::uint32_t pc, int (*name_width)(std::uint32_t name));

// Where the bar goes this frame and what it shows, native pixels; false when
// none (switch off, not choosing an enemy).
struct Bar {
    int left = 0, top = 0;
    int hp = 0, max_hp = 0;
};
bool bar(Bar* out);

// Black outline, white frame and a red fill over a dark track, in the
// window palette (16 colours; red always the default red), with the HP
// number from draw_number(text, bar_left, y), which centres it at
// bar_left + (kBarWidth - width) / 2. put(x, y, r, g, b).
template <class Put, class DrawNumber>
void paint(const Bar& b, const std::uint16_t* palette, std::uint16_t red_colour,
           Put put, DrawNumber draw_number) {
    constexpr int kInner = kBarWidth - 4;
    int filled = b.hp * kInner / b.max_hp;
    if (b.hp > 0 && filled == 0) filled = 1;
    struct Rgb { int r, g, b; };
    const auto colour = [](std::uint16_t c) {
        const int r = c & 31, g = (c >> 5) & 31, bl = (c >> 10) & 31;
        return Rgb{(r << 3) | (r >> 2), (g << 3) | (g >> 2), (bl << 3) | (bl >> 2)};
    };
    const Rgb outline = colour(palette[3]), frame = colour(palette[1]);
    const Rgb track = colour(palette[11]);
    const Rgb red = colour(red_colour);
    const Rgb red_top{(red.r + 255) / 2, red.g + (255 - red.g) / 3,
                      red.b + (255 - red.b) / 3};
    const Rgb red_bottom{red.r * 2 / 3, red.g * 2 / 3, red.b * 2 / 3};
    for (int y = 0; y < kBarHeight; ++y) {
        for (int x = 0; x < kBarWidth; ++x) {
            const bool edge_x = x == 0 || x == kBarWidth - 1;
            const bool edge_y = y == 0 || y == kBarHeight - 1;
            if (edge_x && edge_y) continue;  // rounded corners
            const Rgb* c;
            if (edge_x || edge_y) c = &outline;
            else if (x == 1 || x == kBarWidth - 2 || y == 1 ||
                     y == kBarHeight - 2) c = &frame;
            else if (x - 2 >= filled) c = &track;
            else if (y == 2) c = &red_top;
            else if (y == kBarHeight - 3) c = &red_bottom;
            else c = &red;
            put(b.left + x, b.top + y, c->r, c->g, c->b);
        }
    }
    // The HP number, centred on the bar's inside.
    draw_number(std::to_string(b.hp), b.left, b.top + 2);
}

}  // namespace gsr::enemy_hp_bar
