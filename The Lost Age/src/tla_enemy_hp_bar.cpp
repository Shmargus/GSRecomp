// Golden Sun: The Lost Age -- Enemy HP bar: while the player picks a target
// in battle, a small red bar with the HP number sits just above the enemy's
// name. The rules and measurements of both games are in
// ../../shared_game/enemy_hp_bar.h (one copy); this file installs
// The Lost Age's addresses and paints with this game's menu font. F1 Game
// switch (src/tla_walk_speed.cpp lists it), off by default.

#include "tla_rom_version.h"
#include <cstdint>
#include <string>

#include "../../shared_game/enemy_hp_bar.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

namespace hp = gsr::enemy_hp_bar;

// The drawn name's width in the menu font, from its u16 text.
int name_width(std::uint32_t name) {
    std::string text;
    for (std::uint32_t at = name; text.size() < 32; at += 2u) {
        const std::uint16_t code = bus_read_u16(at);
        if (code == 0) break;
        if (code >= 0x20u && code <= 0xFFu) text += static_cast<char>(code);
    }
    return static_cast<int>(tla_menu_text_width(text));
}

}  // namespace

void tla_install_enemy_hp_bar() {
    hp::install({.frame_routine_pc = TLA_ROM(0x08013560u),
                 .target_loop_return = TLA_ROM(0x0804B646u),
                 .name_draw_pc = TLA_ROM(0x0803AAE4u),
                 .name_draw_return = TLA_ROM(0x0804B3ACu),
                 .candidate_list_sp = 0x14u,
                 .enemy_base_slot = 0x030000D8u});
}

void tla_enemy_hp_bar_on_entry(std::uint32_t pc) { hp::on_entry(pc, name_width); }

bool tla_enemy_hp_bar_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    hp::Bar bar;
    if (!hp::bar(&bar)) return false;
    // In the expanded view the console's picture sits inside the margins.
    int ox = 0, oy = 0;
    if (const gba::GbaPpu* ppu = gbarecomp::active_ppu()) {
        ox = static_cast<int>(ppu->view_extra_left());
        oy = static_cast<int>(ppu->view_extra_top());
    }
    const auto put = [&](int x, int y, int r, int g, int b) {
        x += ox;
        y += oy;
        if (x < 0 || y < 0 || x >= int(width) || y >= int(height)) return;
        std::uint8_t* p = rgb + (std::size_t(y) * width + std::size_t(x)) * 3u;
        p[0] = std::uint8_t(r);
        p[1] = std::uint8_t(g);
        p[2] = std::uint8_t(b);
    };
    const auto draw_number = [&](const std::string& number, int bar_left, int top) {
        const int w = static_cast<int>(tla_menu_text_width(number));
        tla_menu_paint_text(rgb, width, height, number,
                            bar_left + (hp::kBarWidth - w) / 2, top);
    };
    // Red: the window palette's default red (0x001F), whatever colour the
    // player picked for windows.
    hp::paint(bar, tla_menu_window_palette(), 0x001Fu, put, draw_number);
    return true;
}
