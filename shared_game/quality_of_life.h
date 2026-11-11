#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include "quality_of_life_menu.h"
#include "gba_bus.h"
#include "game_options.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "view_config.h"

// The Pause menu's Sleep message is replaced before it runs. Keep the
// continuation on the guest stack, like the Retreat picker, so loading a
// state made inside this menu can safely return to the native Pause menu.
namespace gsr::qol {
struct Game {
    std::uint32_t message, sleep_return, pause_restart, frame, sound;
    std::uint32_t sleep_label, sleep_help, icon_loader;
    unsigned sleep_icon, status_icon;
    void (*font)();
    std::vector<qol_menu::Row> (*rows)();
};
inline const Game* game = nullptr;
inline const gbarecomp::GameOptionSwitch* switches = nullptr;
inline int switch_count = 0;
inline qol_menu::Widget menu;
inline std::uint32_t hold_sp = 0;
inline bool calling = false;
inline std::size_t text_pos = 0;
inline std::uint32_t text_id = 0;
using Hold = std::array<std::uint32_t, 6>;
constexpr std::uint32_t kTag = 0x514F4C4Du; // QOLM
constexpr unsigned kHoldBytes = sizeof(Hold);
inline bool stack_address(std::uint32_t sp) {
    return (sp & 3u) == 0 && sp >= 0x03000000u && sp + kHoldBytes <= 0x03008000u;
}
inline bool read_hold(gba::GbaBus& b, Hold& h) {
    const auto sp = g_cpu.R[13];
    if (!game || !stack_address(sp)) return false;
    for (unsigned i = 0; i < h.size(); ++i) h[i] = b.read32(sp + i * 4u);
    return h[0] == kTag && h[1] == ~kTag && h[2] == sp + kHoldBytes &&
        h[3] == (game->pause_restart | 1u) && h[4] == game->message && h[5] == game->sleep_return;
}
inline void reset() { menu.reset(); hold_sp = 0; text_pos = 0; text_id = 0; }
inline void install(const Game& profile) { game = &profile; reset(); }
inline bool text(std::uint32_t id, std::string& out) {
    if (!game) return false;
    if (id == game->sleep_label) out = "Quality of Life";
    else if (id == game->sleep_help) out = "Change Quality of Life settings.";
    else return false;
    return true;
}
inline bool character(std::uint32_t id, std::uint32_t* out) {
    std::string label;
    if (!text(id, label)) { text_pos = 0; text_id = 0; return false; }
    if (id != text_id) { text_id = id; text_pos = 0; }
    const auto native = menu_text(label.c_str());
    *out = text_pos < native.size() ? static_cast<unsigned char>(native[text_pos++]) : 0;
    if (!*out) text_pos = 0;
    return true;
}
inline bool on_entry(std::uint32_t pc) {
    if (!game || calling) return false;
    // Both original icon paths use this loader. Only the Sleep picture's
    // identifier changes; allocation, animation and the Status artwork stay native.
    if (pc == game->icon_loader && g_cpu.R[0] == game->sleep_icon)
        g_cpu.R[0] = game->status_icon;
    if (pc != game->frame && pc != game->message) return false;
    auto* b = gbarecomp::active_bus();
    if (!b) return false;
    Hold h{};
    if (pc == game->frame && read_hold(*b, h)) {
        calling = true;
        for (const auto sound : menu.sounds) {
            std::uint32_t ignored = 0;
            gbarecomp::guest_call(game->sound | 1u, sound, 0, 0, 0, &ignored);
        }
        calling = false;
        menu.sounds.clear();
        if (hold_sp == g_cpu.R[13] && menu.showing) {
            g_cpu.R[0] = 1;
            g_cpu.R[14] = game->frame | 1u;
            return false;
        }
        for (unsigned i = 0; i < h.size(); ++i) b->write32(g_cpu.R[13] + i * 4u, 0);
        g_cpu.R[13] = h[2];
        g_cpu.R[14] = h[3];
        hold_sp = 0;
        menu.showing = false;
        gbarecomp::guest_return_now(pc, 0);
        return true;
    }
    if (pc != game->message || g_cpu.R[14] != game->sleep_return) return false;
    const auto sp = g_cpu.R[13];
    if (sp < kHoldBytes || !stack_address(sp - kHoldBytes)) return false;
    h = {kTag, ~kTag, sp, game->pause_restart | 1u, game->message, game->sleep_return};
    hold_sp = sp - kHoldBytes;
    for (unsigned i = 0; i < h.size(); ++i) b->write32(hold_sp + i * 4u, h[i]);
    game->font();
    menu.open(game->rows());
    g_cpu.R[13] = hold_sp;
    g_cpu.R[14] = game->frame | 1u;
    gbarecomp::guest_return_now(pc, 1);
    return true;
}
inline qol_menu::Row option(const char* label, const char* help, const char* key, int fallback = 0) {
    return {label, help, [=] { return gbarecomp::game_option_get(key, fallback) ? "On" : "Off"; },
        [=](int) { gbarecomp::game_option_set(key, !gbarecomp::game_option_get(key, fallback)); }};
}
inline void append_switches(std::vector<qol_menu::Row>& rows) {
    for (int i = 0; i < switch_count; ++i) {
        const auto& s = switches[i];
        rows.push_back(option(s.label, s.caption, s.key));
    }
}
inline int step(int value, int count, int direction) { return (value + count + direction) % count; }
} // namespace gsr::qol
