// Settings page two and the Psynergy speed-up.
// Split out of runner_main.cpp; the code is moved unchanged. Names shared with the
// other runner_*.cpp files are declared in runner_shared.h.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <tuple>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "crc32.h"
#include "env_flag.h"
#include "game_exit.h"
#include <memory>
#include "frame_capture.h"
#include "frame_history.h"
#include "texture_readback_queue.h"
#include "crash_handler.h"
#include "game_ram_layout.h"
#include "map_recorder.h"
#include "obj_recorder.h"
#include "battle_view.h"
#include "room_buffer.h"
#include "field_scene.h"
#include "obj_y_continuity.h"
#include "effect_capture.h"
#include "unpacker_catch.h"
#include "../../shared_game/unpacker_guard.h"
#include "field_scene_renderer.h"
#include "intro_music.h"
#include "world_map_source.h"
#include "rain_margins.h"
#include "tile_ring_redraw.h"
#include "cheat_menu.h"
#include "trackers_tbs.h"
#include "hard_mode.h"
#include "../../shared_game/custom_battle.h"
#include "../../shared_game/custom_battle_panel.h"
#include "custom_battle_tbs.h"
#include "retreat_travel_tbs.h"
#include "radial_psynergy_tbs.h"
#include "random_psynergy_tbs.h"
#include "random_weapons_tbs.h"
#include "../../shared_game/held_palette_effect.h"
#include "host_config_ui.h"
#include "seamless_mode.h"
#include "save_files_tbs.h"
#include "gpu_surface.h"
#include "view_config.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "gba_vram_trace.h"
#include "recompiled.h"
#include "relocatable_identity.h"
#include "relocatable_writer_policy.h"
#include "player_speed_cheat.h"
#include "walk_through_walls.h"
#include "developer_session.h"
#include "text_speed_cheat.h"
#include "settings_page.h"
#include "../../shared_game/settings_help.h"
#include "battle_speed.h"
#include "../../shared_game/enemy_hp_bar.h"
#include "../../shared_game/field_character.h"
#include "../../shared_game/challenge_mode.h"
#include "../../shared_game/spanish_menu.h"
#include "../../shared_game/menu_text.h"
#include "../../shared_game/italian_menu.h"
#include "../../shared_game/french_menu.h"
#include "../../shared_game/german_menu.h"
#include "obj_placements.h"
#include "runtime.h"
#include "memory_snapshot.h"
#include "runtime_bus_bridge.h"
#include "runtime_arm.h"
#include "arm_cpu_bridge.h"
#include "arm_decode.h"
#include "interpreter.h"
#include "thumb_decode.h"
#include "overlay_loader.h"
#include "self_heal.h"
#include "sha1.h"
#include "widescreen_policy.h"
#include "field_game_profile.h"
#include "rom_version.h"
#include "runner_shared.h"

extern "C" unsigned g_ws_active;
extern "C" unsigned long long runtime_current_frame();
extern "C" void runtime_iwram_code_write_dump(const char* why);
extern "C" void runtime_iwram_code_write_dump_range(const char* why,
                                                    std::uint32_t lo,
                                                    std::uint32_t hi);
extern "C" void runtime_unpacker_slot_dump(const char* why);

namespace {
using namespace gsr_runner;

// ---- Settings page two (src/settings_page.h) ----------------------------
//
}  // namespace
namespace gsr_runner {
SettingsPageFont g_settings_font;
gsr::settings_help::Line g_settings_help;
std::atomic<bool> g_settings_font_loaded{false};
std::atomic<int> g_settings_page{1};
// Set when A/B/Start closes the screen; cleared by the next screen setup.
std::atomic<bool> g_settings_closing{false};
// True from the settings screen's setup to its closing: Hard Mode's label
// and help text are replaced only then, never in dialogue.
bool g_settings_screen_text = false;
// Set by the settings loop's first frame; cleared by the next screen setup
// and by a savestate load, which can leave the screen without closing it.
// Not a frame count: a loop frame that runs late must not hide the page.
std::atomic<bool> g_settings_open{false};
}  // namespace gsr_runner
namespace {
// Window x | y << 8 | width << 16 | height << 24, in tiles.
std::atomic<std::uint32_t> g_settings_window{0};
std::atomic<std::uint32_t> g_settings_help_window{0};
std::atomic<int> g_settings_speed{2};
std::atomic<int> g_settings_view_mode{0};
std::atomic<int> g_settings_no_slowdown{0};
}  // namespace
namespace gsr_runner {
// Encounters: Normal / Half / Off, loaded from kOptionsFile at startup and
// saved there on each change.
std::atomic<int> g_encounter_rate{0};
bool g_encounter_skip_next = false;
}  // namespace gsr_runner
namespace {
// Battle speed-up (src/battle_speed.h), toggled with Select in battle; saved
// with the other options.
std::atomic<bool> g_battle_speed_on{false};
}  // namespace
namespace gsr_runner {
// Better Field Psy: Off / On (src/battle_speed.h), saved with the other
// options.
std::atomic<int> g_field_psynergy_fast{0};
}  // namespace gsr_runner
namespace {
std::string g_options_path = gsr::settings_page::kOptionsFile;

}  // namespace
namespace gsr_runner {
void load_game_options(const char* argv0) {
    using namespace gsr::settings_page;
    if (argv0 && *argv0) {
        const std::filesystem::path exe(argv0);
        if (exe.has_parent_path())
            g_options_path = (exe.parent_path() / kOptionsFile).string();
    }
    gbarecomp::game_options_load(g_options_path);
    const int encounters = gbarecomp::game_option_get(kEncounterKey, kEncounterNormal);
    if (encounters >= kEncounterNormal && encounters < kEncounterRateCount)
        g_encounter_rate.store(encounters);
    g_battle_speed_on.store(
        gbarecomp::game_option_get(gsr::battle_speed::kOptionsKey, 0) != 0);
    g_field_psynergy_fast.store(
        gbarecomp::game_option_get(kFieldPsynergyKey, 0) != 0 ? 1 : 0);
}
}  // namespace gsr_runner
namespace {

void save_game_options() {
    gbarecomp::game_option_set(gsr::settings_page::kEncounterKey,
                               g_encounter_rate.load());
    gbarecomp::game_option_set(gsr::battle_speed::kOptionsKey,
                               g_battle_speed_on.load() ? 1 : 0);
    gbarecomp::game_option_set(gsr::settings_page::kFieldPsynergyKey,
                               g_field_psynergy_fast.load() ? 1 : 0);
}
// The game's cursor row (r9), which is also the page-two row.
std::atomic<int> g_settings_row{0};

}  // namespace
namespace gsr_runner {
// Emulation thread only: copies the menu font out of the ROM once.
void load_menu_font() {
    using namespace gsr::settings_page;
    if (g_settings_font_loaded.load(std::memory_order_acquire)) return;
    g_settings_help.load(bus_read_u8, gs::rom(0x08032224u));
    const std::uint32_t font = bus_read_u32(kAssetTable + kFontAsset * 4u);
    for (std::uint32_t i = 0; i < sizeof g_settings_font.tiles; ++i)
        g_settings_font.tiles[i] = bus_read_u8(font + i);
    for (std::uint32_t i = 0; i < sizeof g_settings_font.widths; ++i)
        g_settings_font.widths[i] = bus_read_u8(kGlyphWidthTable + i);
    gbarecomp::config_ui_set_menu_art(g_settings_font.tiles, g_settings_font.widths,
        gsr::cheat_menu::kDefaultWindowPalette);
    g_settings_font_loaded.store(true, std::memory_order_release);
}

void note_settings_loop_frame() {
    using namespace gsr::settings_page;
    load_menu_font();
    g_settings_help.colours(bus_read_u16(0x050001E2u), bus_read_u16(0x050001E6u));
    auto rect = [](std::uint32_t window) {
        return (bus_read_u16(window + kWindowX) & 0xFFu) |
               ((bus_read_u16(window + kWindowY) & 0xFFu) << 8) |
               ((bus_read_u16(window + kWindowWidth) & 0xFFu) << 16) |
               ((bus_read_u16(window + kWindowHeight) & 0xFFu) << 24);
    };
    g_settings_window.store(rect(g_cpu.R[8]));
    g_settings_help_window.store(
        rect(bus_read_u32(g_cpu.R[13] + kHelpWindowStackOffset)));
    g_settings_speed.store(runtime_get_mem_write_override_enabled());
    g_settings_view_mode.store(runtime_get_view_mode());
    g_settings_no_slowdown.store(runtime_get_no_slowdown());
    g_settings_row.store(static_cast<int>(g_cpu.R[9]));
    g_settings_open.store(true);
}

// Sound effects our menus play with the game's own call (settings_page.h
// kSoundPc), queued where the menu acts and played at the next entry to the
// frame routine, where a guest call is safe. Emulation thread.
std::vector<std::uint32_t> g_menu_sounds;

void play_menu_sounds() {
    for (const std::uint32_t sound : g_menu_sounds) {
        std::uint32_t ignored = 0;
        gbarecomp::guest_call(gsr::settings_page::kSoundPc | 1u, sound, 0, 0, 0, &ignored);
    }
    g_menu_sounds.clear();
}

// The repeat-keys load: decides the page and what the game is allowed to see.
int settings_page_repeat_keys(std::uint32_t* out_value) {
    using namespace gsr::settings_page;
    const std::uint32_t keys = bus_read_u32(kRepeatKeys);
    if (g_settings_page.load() != 2) {
        // Down on Auto-Sleep: the game wraps its cursor to row 0 as usual,
        // which is where page two's row sits.
        if (g_cpu.R[9] == kAutoSleepRow && (keys & kKeyDown))
            g_settings_page.store(2);
        return 0;
    }
    const int row = static_cast<int>(g_cpu.R[9]);
    // The game tests Up and Down before Left/Right and ends the frame's
    // input on either, so passing the real word moves only its hand.
    if (keys & kKeyUp) {
        // Row 0 wraps back to Auto-Sleep: page one again.
        if (row == 0) g_settings_page.store(1);
        return 0;
    }
    if (keys & kKeyDown) {
        if (row < kRowCount - 1) return 0;
        // Last row: withheld, the hand stays.
    } else if ((keys & (kKeyLeft | kKeyRight)) && row == kWalkSpeedRow) {
        int index = walk_speed_index(runtime_get_mem_write_override_enabled());
        index = (keys & kKeyRight) ? (index + 1) % kWalkSpeedCount
                                   : (index + kWalkSpeedCount - 1) % kWalkSpeedCount;
        const int speed = kWalkSpeedHalves[index];
        runtime_set_mem_write_override_enabled(speed);
        g_settings_speed.store(speed);
    } else if ((keys & (kKeyLeft | kKeyRight)) && row == kEncounterRow) {
        int rate = g_encounter_rate.load();
        rate = (keys & kKeyRight) ? (rate + 1) % kEncounterRateCount
                                  : (rate + kEncounterRateCount - 1) %
                                        kEncounterRateCount;
        g_encounter_rate.store(rate);
        save_game_options();
    } else if ((keys & (kKeyLeft | kKeyRight)) && row == kScreenRow) {
        // Use the same view choices as the launcher and F1 menu.
        const int step = (keys & kKeyRight) ? 1 : gbarecomp::kViewModeCount - 1;
        const int mode = (runtime_get_view_mode() + step) % gbarecomp::kViewModeCount;
        runtime_request_view_mode(mode);
        g_settings_view_mode.store(mode);
    } else if ((keys & (kKeyLeft | kKeyRight)) && row == kNoSlowdownRow) {
        const int on = runtime_get_no_slowdown() ? 0 : 1;
        runtime_request_no_slowdown(on);
        g_settings_no_slowdown.store(on);
    } else if ((keys & (kKeyLeft | kKeyRight)) && row == kFieldPsynergyRow) {
        // Two values: either direction toggles.
        g_field_psynergy_fast.store(g_field_psynergy_fast.load() ? 0 : 1);
        save_game_options();
    }
    // A value changed: the sound page one plays for Left/Right.
    if ((keys & (kKeyLeft | kKeyRight)) && !(keys & kKeyDown))
        g_menu_sounds.push_back(gsr::custom_battle::kMoveSound);
    *out_value = kZeroWord;
    return 1;
}

// True while the settings loop is running and not closing: the page
// marker is drawn on either page, page two's contents only on page two.
bool settings_overlay_visible() {
    return g_settings_open.load() && !g_settings_closing.load() &&
           g_settings_font_loaded.load(std::memory_order_acquire);
}
}  // namespace gsr_runner
namespace {

void paint_settings_window(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height, std::uint32_t window,
                           bool page_two);

}  // namespace
namespace gsr_runner {
// Paints the page marker below the settings window, in the game's own font.
// Page two's rows are written into the game's own window instead
// (native_page_frame_end); its shaded description is clipped and scrolled
// at presentation time below.
void paint_settings_page(std::uint8_t* rgb, std::uint32_t width,
                         std::uint32_t height) {
    const bool page_two = g_settings_page.load() == 2;
    paint_settings_window(rgb, width, height, g_settings_window.load(),
                          page_two);
    const int row = g_settings_row.load();
    if (page_two && row >= 0 && row < gsr::settings_page::kRowCount) {
        g_settings_help.paint(rgb, width, height,
            static_cast<int>(g_golden_sun_wide_extra_left),
            static_cast<int>(g_golden_sun_wide_extra_top),
            gsr::menu_text(gsr::settings_page::kRowHelp[row]), row,
            g_settings_help_window.load());
    } else g_settings_help.reset();
}
}  // namespace gsr_runner
namespace {

// The settings window's page marker.
void paint_settings_window(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height, std::uint32_t window,
                           bool page_two) {
    using namespace gsr::settings_page;
    std::uint8_t letter[3] = {0xFF, 0xFF, 0xFF};
    std::uint8_t shadow[3] = {0x00, 0x00, 0x00};
    const int wx = static_cast<int>(window & 0xFFu);
    const int wy = static_cast<int>((window >> 8) & 0xFFu);
    const int ww = static_cast<int>((window >> 16) & 0xFFu);
    const int wh = static_cast<int>(window >> 24);
    auto pixel = native_pixel_at(rgb, width, height);
    const int left = (wx + 1) * 8;
    const int top = (wy + 1) * 8;
    const int right = (wx + ww - 1) * 8;
    const int bottom = (wy + wh - 1) * 8;
    // The settings window keeps its first column, where the cursor hand is.
    const int fill_left = left + 8;
    if (right <= fill_left || bottom <= top) return;

    auto glyph = [&](unsigned char code, int gx, int gy) {
        const std::uint8_t b =
            g_settings_font.tiles[code * 32u + gy * 4 + gx / 2];
        return static_cast<std::uint8_t>((b >> (4 * (gx & 1))) & 0xFu);
    };
    // Letter and shadow colours from page one's first label letter, read
    // before anything is painted over it.
    bool have_letter = false, have_shadow = false;
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            const std::uint8_t v = glyph(kColourSampleLetter, gx, gy);
            std::uint8_t* p = pixel(left + kLabelX + gx, top + gy);
            if (!p) continue;
            if (v == kGlyphLetter && !have_letter) {
                std::copy(p, p + 3, letter);
                have_letter = true;
            } else if (v == kGlyphShadow && !have_shadow) {
                std::copy(p, p + 3, shadow);
                have_shadow = true;
            }
        }
    }

    auto text_width = [&](const std::string& text) {
        int total = 0;
        for (const char c : text) {
            const auto code = static_cast<unsigned char>(c);
            if (code >= 0x20u) total += g_settings_font.widths[code - 0x20u];
        }
        return total;
    };
    auto draw = [&](const std::string& text, int x, int y) {
        for (const char c : text) {
            const auto code = static_cast<unsigned char>(c);
            if (code < 0x20u) continue;
            for (int gy = 0; gy < 8; ++gy) {
                for (int gx = 0; gx < 8; ++gx) {
                    const std::uint8_t v = glyph(code, gx, gy);
                    const std::uint8_t* colour =
                        v == kGlyphLetter ? letter
                        : v == kGlyphShadow ? shadow : nullptr;
                    if (!colour) continue;
                    if (std::uint8_t* p = pixel(left + x + gx, top + y + gy))
                        std::copy(colour, colour + 3, p);
                }
            }
            x += g_settings_font.widths[code - 0x20u];
        }
    };
    // Page two itself is in the game's window (native_page_frame_end); only
    // the page marker below the window is painted.

    // Page marker below the window's bottom-right corner.
    {
        auto put = [&](int x, int y, const std::uint8_t* colour) {
            if (std::uint8_t* p = pixel(left + x, top + y))
                std::copy(colour, colour + 3, p);
        };
        auto letter_at = [&](int gx, int gy) {
            if (gx < 0 || gx >= 8 || gy < 0 || gy >= kChevronRows)
                return false;
            const int row = page_two ? gy : kChevronRows - 1 - gy;
            return glyph(kChevronGlyph, gx, row) == kGlyphLetter;
        };
        const std::string marker = page_two ? kPageTwoMarker : kPageOneMarker;
        const int chevron_width = g_settings_font.widths[kChevronGlyph - 0x20u];
        const int marker_x = (wx + ww) * 8 - (left + text_width(marker) +
                                                chevron_width);
        const int marker_y = (wy + wh) * 8 - top;
        draw(marker, marker_x, marker_y);
        const int cx = marker_x + text_width(marker);
        // Vertically centred on the 8-pixel line.
        const int cy = marker_y + (8 - kChevronRows) / 2;
        for (int gy = 0; gy <= kChevronRows; ++gy) {
            for (int gx = 0; gx <= 8; ++gx) {
                if (letter_at(gx, gy)) put(cx + gx, cy + gy, letter);
                else if (letter_at(gx - 1, gy - 1)) put(cx + gx, cy + gy, shadow);
            }
        }
    }

}

// ---- Settings page two in the game's own window (settings_page.h) ------
// Emulation thread only.
struct NativeSettingsPage {
    bool active = false;
    // Game text tiles whose pixels page two replaced: the game's pixels and
    // the ones written, so a tile the game repainted meanwhile is left alone.
    struct Saved {
        std::array<std::uint8_t, 32> original{};
        std::array<std::uint8_t, 32> written{};
    };
    std::unordered_map<std::uint16_t, Saved> saved;
    // Blank cells pointed at a reserved tile: cell address -> the game's
    // cell value, and the value written.
    struct Cell {
        std::uint16_t original = 0;
        std::uint16_t written = 0;
    };
    std::unordered_map<std::uint32_t, Cell> cells;
    std::vector<std::uint16_t> reserved;
    std::size_t reserved_used = 0;
    // OAM shadow entries hidden: index -> the game's attribute 0.
    std::unordered_map<int, std::uint16_t> hidden;
};
NativeSettingsPage g_native_page;

bool native_page_text_tile(std::uint32_t block, std::uint16_t tile) {
    using namespace gsr::settings_page;
    const bool high_bank = bus_read_u8(block + kTileBankFlagOffset) != 0u;
    return high_bank ? tile >= 0x200u && tile <= 0x27Fu
                     : tile >= kTextBankFirst && tile <= kTextBankLast;
}

std::uint32_t native_page_usage(std::uint32_t block, std::uint16_t tile) {
    using namespace gsr::settings_page;
    return block + kTileUsageOffset + ((tile & 0xFFu) ^ 0x80u);
}

std::array<std::uint8_t, 32> native_page_read_tile(std::uint16_t tile) {
    std::array<std::uint8_t, 32> bytes{};
    const std::uint32_t base = gsr::settings_page::kBg0CharBase + tile * 32u;
    for (std::uint32_t i = 0; i < 32u; i += 4u) {
        const std::uint32_t v = bus_read_u32(base + i);
        for (std::uint32_t k = 0; k < 4u; ++k)
            bytes[i + k] = static_cast<std::uint8_t>(v >> (8u * k));
    }
    return bytes;
}

void native_page_write_tile(std::uint16_t tile,
                            const std::array<std::uint8_t, 32>& bytes) {
    const std::uint32_t base = gsr::settings_page::kBg0CharBase + tile * 32u;
    for (std::uint32_t i = 0; i < 32u; i += 4u) {
        bus_write_u32(base + i, static_cast<std::uint32_t>(bytes[i]) |
                                    (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                    (static_cast<std::uint32_t>(bytes[i + 2]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[i + 3]) << 24));
    }
}

// Writes a cell to the shadow and to VRAM, so it shows whether or not the
// game flushes the shadow this frame.
void native_page_write_cell(std::uint32_t block, std::uint32_t cell,
                            std::uint16_t value) {
    bus_write_u16(block + cell * 2u, value);
    bus_write_u16(gsr::settings_page::kBg0ScreenBase + cell * 2u, value);
}

}  // namespace
namespace gsr_runner {
// Back to page one: the game's pixels and cells where page two's are still
// in place, reserved tiles released, icons shown. `restore` is false when
// the window is closing or memory was replaced, and only the host state is
// dropped.
void native_page_end(bool restore) {
    NativeSettingsPage& page = g_native_page;
    if (!page.active) return;
    const std::uint32_t block = bus_read_u32(gsr::settings_page::kWindowBlockSlot);
    const std::uint32_t oam = bus_read_u32(gsr::settings_page::kOamShadowSlot);
    if (restore) {
        for (const auto& [tile, saved] : page.saved) {
            if (native_page_read_tile(tile) == saved.written)
                native_page_write_tile(tile, saved.original);
        }
        // A reserved tile is released only while its cell is still ours;
        // one the game drew over may since have been handed out again.
        for (const auto& [cell, value] : page.cells) {
            if (bus_read_u16(block + cell * 2u) != value.written) continue;
            native_page_write_cell(block, cell, value.original);
            bus_write_u8(native_page_usage(block, value.written & 0x3FFu), 0u);
        }
        for (const auto& [index, attr0] : page.hidden) {
            const std::uint32_t entry = oam + static_cast<std::uint32_t>(index) * 8u;
            if ((bus_read_u16(entry) & 0x0300u) == 0x0200u)
                bus_write_u16(entry, attr0);
        }
    }
    // Closing: the game frees the window's cells itself (Func_1e260).
    page = NativeSettingsPage{};
}

// A page-two word in the player's language (gbarecomp::ui_text), in the
// font's codes: the table is UTF-8, the font Latin-1 (á = 0xE1; FACTS.md
// "Spanish ROM: menu text").
std::string settings_text(const char* english) {
    return gsr::menu_text(english);
}
}  // namespace gsr_runner
namespace {

// Page two's text as 4bpp pixels over the window interior, in the menu
// font: letter 1, shadow 3, background 4, like the game's own text tiles.
std::vector<std::uint8_t> native_page_canvas(int width, int height) {
    using namespace gsr::settings_page;
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(width) * height,
                                     kGlyphBackground);
    auto glyph = [](unsigned char code, int gx, int gy) {
        const std::uint8_t b = g_settings_font.tiles[code * 32u + gy * 4 + gx / 2];
        return static_cast<std::uint8_t>((b >> (4 * (gx & 1))) & 0xFu);
    };
    auto text_width = [](const std::string& text) {
        int total = 0;
        for (const char c : text) {
            const auto code = static_cast<unsigned char>(c);
            if (code >= 0x20u) total += g_settings_font.widths[code - 0x20u];
        }
        return total;
    };
    auto draw = [&](const std::string& text, int x, int y) {
        for (const char c : text) {
            const auto code = static_cast<unsigned char>(c);
            if (code < 0x20u) continue;
            for (int gy = 0; gy < 8; ++gy) {
                for (int gx = 0; gx < 8; ++gx) {
                    const std::uint8_t v = glyph(code, gx, gy);
                    if (v != kGlyphLetter && v != kGlyphShadow) continue;
                    const int px = x + gx, py = y + gy;
                    if (px < 0 || py < 0 || px >= width || py >= height) continue;
                    canvas[static_cast<std::size_t>(py) * width + px] = v;
                }
            }
            x += g_settings_font.widths[code - 0x20u];
        }
    };
    const int view_mode = std::clamp(g_settings_view_mode.load(), 0, gbarecomp::kViewModeCount - 1);
    int encounters = g_encounter_rate.load();
    if (encounters < kEncounterNormal || encounters >= kEncounterRateCount)
        encounters = kEncounterNormal;
    const int no_slowdown = g_settings_no_slowdown.load() ? 1 : 0;
    const int field_psynergy = g_field_psynergy_fast.load() ? 1 : 0;
    const int speed = walk_speed_index(g_settings_speed.load());
    const std::string words[kRowCount] = {settings_text(kWalkingSpeedWords[speed]),
                                          settings_text(kEncounterWords[encounters]),
                                          settings_text(kScreenWords[view_mode]),
                                          settings_text(kOnOffWords[no_slowdown]),
                                          settings_text(kFieldPsynergyWords[field_psynergy])};
    const int cursor_row = g_settings_row.load();
    for (int row = 0; row < kRowCount; ++row) {
        const int y = kRowLineY[row];
        draw(settings_text(kRowLabels[row]), kLabelX, y);
        const std::string value = row == cursor_row
            ? "< " + words[row] + " >" : words[row];
        draw(value, kValueCentreX - text_width(value) / 2, y);
    }
    return canvas;
}

// Log budget for frame captures where the game had drawn page one's text
// over page two.
unsigned g_native_page_capture_logs = 0;

}  // namespace
namespace gsr_runner {
// End of a game frame (the frame routine's wait): keep page two in the
// settings window while it shows, or put page one back once it does not.
//
// `at_capture` is the second caller, the frame capture at the start of
// VBlank (gpu_field_capture_hook): on Up/Down the game redraws page one's
// values into fresh text tiles after the wait had already run, and they were
// on screen for two frames (gpu_rewind_0059, frames 70-71 and 110-111;
// ROADMAP.md). There it runs from inside guest code, so it only puts page
// two's pixels back into text tiles: no tile is handed out, no cell or
// sprite is touched, and nothing starts or ends.
void native_page_frame_end(bool at_capture) {
    using namespace gsr::settings_page;
    const bool open = g_settings_open.load() && !g_settings_closing.load() &&
                      g_settings_font_loaded.load(std::memory_order_acquire);
    // Logs (bounded) why page two was not kept while it should show: a
    // player build still showed page one's text untouched despite the
    // capture-time repaint (bug_report_20260930_212742, rewind 2 frames 22-23).
    auto note_skip = [&](const char* where, const char* why) {
        static unsigned logged = 0;
        if (logged >= 60u) return;
        ++logged;
        std::fprintf(stderr,
                     "[settings] page two not kept at %s: %s (frame %llu, "
                     "open=%d closing=%d page=%d active=%d line %u)\n",
                     where, why, runtime_current_frame(),
                     g_settings_open.load() ? 1 : 0,
                     g_settings_closing.load() ? 1 : 0, g_settings_page.load(),
                     g_native_page.active ? 1 : 0,
                     static_cast<unsigned>(bus_read_u16(0x04000006u)));
    };
    if (at_capture &&
        (!open || g_settings_page.load() != 2 || !g_native_page.active)) {
        if (g_settings_page.load() == 2 || g_native_page.active)
            note_skip("the frame capture", !open ? "not open"
                      : g_settings_page.load() != 2 ? "page one"
                                                    : "not started");
        return;
    }
    if (!open) {
        if (g_native_page.active) note_skip("the frame wait", "not open");
        native_page_end(false);
        return;
    }
    if (g_settings_page.load() != 2) {
        if (g_native_page.active) note_skip("the frame wait", "page one");
        native_page_end(true);
        return;
    }
    const std::uint32_t block = bus_read_u32(kWindowBlockSlot);
    const std::uint32_t oam = bus_read_u32(kOamShadowSlot);
    if ((block >> 24) != 0x02u) return;
    NativeSettingsPage& page = g_native_page;
    int repainted = 0;  // text tiles the game had drawn over
    if (!page.active) {
        page.active = true;
        // Free text tiles, in the game's own usage table.
        const bool high_bank = bus_read_u8(block + kTileBankFlagOffset) != 0u;
        const std::uint16_t first = high_bank ? 0x200u : kTextBankFirst;
        const std::uint16_t last = high_bank ? 0x27Fu : kTextBankLast;
        for (std::uint16_t t = first; t <= last; ++t) {
            if (bus_read_u8(native_page_usage(block, t)) == 0u)
                page.reserved.push_back(t);
        }
    }

    const std::uint32_t window = g_settings_window.load();
    const int wx = static_cast<int>(window & 0xFFu);
    const int wy = static_cast<int>((window >> 8) & 0xFFu);
    const int ww = static_cast<int>((window >> 16) & 0xFFu);
    const int wh = static_cast<int>(window >> 24);
    const int cols = ww - 2, rows = wh - 2;
    if (cols <= 0 || rows <= 0 || wx + ww > 32 || wy + wh > 32) return;
    const int width = cols * 8, height = rows * 8;
    const std::vector<std::uint8_t> canvas = native_page_canvas(width, height);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            std::array<std::uint8_t, 32> wanted{};
            bool blank = true;
            for (int py = 0; py < 8; ++py) {
                for (int px = 0; px < 8; px += 2) {
                    const std::size_t at =
                        static_cast<std::size_t>(r * 8 + py) * width + c * 8 + px;
                    const std::uint8_t lo = canvas[at], hi = canvas[at + 1];
                    wanted[py * 4 + px / 2] =
                        static_cast<std::uint8_t>(lo | (hi << 4));
                    if (lo != kGlyphBackground || hi != kGlyphBackground)
                        blank = false;
                }
            }
            const std::uint32_t cell =
                static_cast<std::uint32_t>((wy + 1 + r) * 32 + (wx + 1 + c));
            const std::uint16_t value = bus_read_u16(block + cell * 2u);
            const std::uint16_t tile = value & 0x3FFu;
            const auto own = page.cells.find(cell);
            const bool ours = own != page.cells.end() && own->second.written == value;
            if (ours) {
                if (native_page_read_tile(tile) != wanted)
                    native_page_write_tile(tile, wanted);
                bus_write_u8(native_page_usage(block, tile), 1u);
                continue;
            }
            if (native_page_text_tile(block, tile)) {
                // One of the game's text tiles: page two's pixels in it.
                const std::array<std::uint8_t, 32> now = native_page_read_tile(tile);
                auto& saved = page.saved[tile];
                if (now != saved.written) saved.original = now;
                if (now != wanted) {
                    ++repainted;
                    native_page_write_tile(tile, wanted);
                }
                saved.written = wanted;
                continue;
            }
            // Blank or frame: only a cell that needs text takes a tile.
            if (at_capture || blank || tile != kBlankTile) continue;
            // A candidate the game took since page two opened is skipped.
            while (page.reserved_used < page.reserved.size() &&
                   bus_read_u8(native_page_usage(
                       block, page.reserved[page.reserved_used])) != 0u)
                ++page.reserved_used;
            if (page.reserved_used >= page.reserved.size()) continue;
            const std::uint16_t spare = page.reserved[page.reserved_used++];
            bus_write_u8(native_page_usage(block, spare), 1u);
            native_page_write_tile(spare, wanted);
            const std::uint16_t written =
                static_cast<std::uint16_t>((value & 0xFC00u) | spare);
            native_page_write_cell(block, cell, written);
            page.cells[cell] = {value, written};
        }
    }

    if (at_capture) {
        if (repainted > 0 && g_native_page_capture_logs < 40u) {
            ++g_native_page_capture_logs;
            std::fprintf(stderr,
                         "[settings] page two: %d text tiles of page one put "
                         "back at the frame capture (frame %llu, line %u)\n",
                         repainted, runtime_current_frame(),
                         static_cast<unsigned>(bus_read_u16(0x04000006u)));
        }
        return;
    }
    // The game clears its sprite-list pointer while it redraws the menu
    // (bug_report_20261001_100121, frames 1033/1072/1129). That must not skip
    // the text repaint above, so only the sprites wait for the next frame.
    if ((oam >> 24) != 0x03u) return;

    // Page one's icons inside the window: every sprite whose top-left lies
    // right of the first interior column (the cursor hand's) and within the
    // window's rows.
    const int left = (wx + 2) * 8, right = (wx + ww - 1) * 8;
    const int top = wy * 8, bottom = (wy + wh - 1) * 8;
    for (int i = 0; i < 128; ++i) {
        const std::uint32_t entry = oam + static_cast<std::uint32_t>(i) * 8u;
        const std::uint16_t attr0 = bus_read_u16(entry);
        if ((attr0 & 0x0300u) == 0x0200u) continue;  // already hidden
        int y = attr0 & 0xFF;
        if (y >= 160) y -= 256;
        int x = bus_read_u16(entry + 2u) & 0x1FF;
        if (x >= 256) x -= 512;
        if (x < left || x >= right || y < top || y >= bottom) continue;
        page.hidden[i] = attr0;
        bus_write_u16(entry, static_cast<std::uint16_t>((attr0 & ~0x0300u) | 0x0200u));
    }
}
}  // namespace gsr_runner
namespace {

// Page two in the graphics card's copy of the frame (gpu_field_capture_hook,
// after VRAM and OAM are copied). The repaint above may not hand out tiles
// inside guest code, so two cases still showed page one for a frame or two
// (bug_report_20261001_114130): a cell the game blanked mid-redraw where
// page two needs text (rewind frame 94, the tips of "< Wide >"), and the
// captures before page two has started (`not started` in the log, two after
// each Down from Auto-Sleep). This writes only the host copies, never guest
// memory: page two's pixels into the text tiles the picture uses, a spare
// tile for a blank cell that needs text, and page one's icons hidden, with
// the same rules as the frame wait.
unsigned g_native_page_patch_logs = 0;

}  // namespace
namespace gsr_runner {
void native_page_patch_capture(std::vector<std::uint8_t>& vram,
                               std::vector<std::uint8_t>& oam,
                               gsr::FieldScene& scene) {
    using namespace gsr::settings_page;
    const bool open = g_settings_open.load() && !g_settings_closing.load() &&
                      g_settings_font_loaded.load(std::memory_order_acquire);
    if (!open || g_settings_page.load() != 2) return;
    if (vram.size() < 96u * 1024u || oam.size() < 1024u) return;
    const std::uint32_t block = bus_read_u32(kWindowBlockSlot);
    if ((block >> 24) != 0x02u) return;

    const std::uint32_t window = g_settings_window.load();
    const int wx = static_cast<int>(window & 0xFFu);
    const int wy = static_cast<int>((window >> 8) & 0xFFu);
    const int ww = static_cast<int>((window >> 16) & 0xFFu);
    const int wh = static_cast<int>(window >> 24);
    const int cols = ww - 2, rows = wh - 2;
    if (cols <= 0 || rows <= 0 || wx + ww > 32 || wy + wh > 32) return;
    const int width = cols * 8, height = rows * 8;
    const std::vector<std::uint8_t> canvas = native_page_canvas(width, height);

    const std::size_t char_base = kBg0CharBase - 0x06000000u;
    const std::size_t screen_base = kBg0ScreenBase - 0x06000000u;
    auto cell_value = [&](std::uint32_t cell) {
        const std::size_t at = screen_base + cell * 2u;
        return static_cast<std::uint16_t>(vram[at] | (vram[at + 1] << 8));
    };
    // Spare tiles: free in the game's usage table and used by no cell of
    // this picture's BG0.
    std::array<bool, 0x400> used{};
    for (std::uint32_t cell = 0; cell < 1024u; ++cell)
        used[cell_value(cell) & 0x3FFu] = true;
    const bool high_bank = bus_read_u8(block + kTileBankFlagOffset) != 0u;
    std::uint16_t next_spare = high_bank ? 0x200u : kTextBankFirst;
    const std::uint16_t last_spare = high_bank ? 0x27Fu : kTextBankLast;
    auto take_spare = [&]() -> int {
        while (next_spare <= last_spare) {
            const std::uint16_t t = next_spare++;
            if (!used[t] && bus_read_u8(native_page_usage(block, t)) == 0u)
                return t;
        }
        return -1;
    };

    int text_tiles = 0, blank_cells = 0, sprites = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            std::array<std::uint8_t, 32> wanted{};
            bool blank = true;
            for (int py = 0; py < 8; ++py) {
                for (int px = 0; px < 8; px += 2) {
                    const std::size_t at =
                        static_cast<std::size_t>(r * 8 + py) * width + c * 8 + px;
                    const std::uint8_t lo = canvas[at], hi = canvas[at + 1];
                    wanted[py * 4 + px / 2] =
                        static_cast<std::uint8_t>(lo | (hi << 4));
                    if (lo != kGlyphBackground || hi != kGlyphBackground)
                        blank = false;
                }
            }
            const std::uint32_t cell =
                static_cast<std::uint32_t>((wy + 1 + r) * 32 + (wx + 1 + c));
            const std::uint16_t value = cell_value(cell);
            const std::uint16_t tile = value & 0x3FFu;
            if (native_page_text_tile(block, tile)) {
                std::uint8_t* pixels = vram.data() + char_base + tile * 32u;
                if (!std::equal(wanted.begin(), wanted.end(), pixels)) {
                    std::copy(wanted.begin(), wanted.end(), pixels);
                    ++text_tiles;
                }
                continue;
            }
            if (blank || tile != kBlankTile) continue;
            const int spare = take_spare();
            if (spare < 0) continue;
            std::copy(wanted.begin(), wanted.end(),
                      vram.data() + char_base + static_cast<std::size_t>(spare) * 32u);
            const std::uint16_t written =
                static_cast<std::uint16_t>((value & 0xFC00u) | spare);
            vram[screen_base + cell * 2u] = static_cast<std::uint8_t>(written);
            vram[screen_base + cell * 2u + 1] = static_cast<std::uint8_t>(written >> 8);
            ++blank_cells;
        }
    }

    const int left = (wx + 2) * 8, right = (wx + ww - 1) * 8;
    const int top = wy * 8, bottom = (wy + wh - 1) * 8;
    for (int i = 0; i < 128; ++i) {
        std::uint8_t* entry = oam.data() + i * 8;
        const std::uint16_t attr0 = static_cast<std::uint16_t>(entry[0] | (entry[1] << 8));
        if ((attr0 & 0x0300u) == 0x0200u) continue;  // already hidden
        int y = attr0 & 0xFF;
        if (y >= 160) y -= 256;
        int x = (entry[2] | (entry[3] << 8)) & 0x1FF;
        if (x >= 256) x -= 512;
        if (x < left || x >= right || y < top || y >= bottom) continue;
        const std::uint16_t hidden = static_cast<std::uint16_t>((attr0 & ~0x0300u) | 0x0200u);
        entry[0] = static_cast<std::uint8_t>(hidden);
        entry[1] = static_cast<std::uint8_t>(hidden >> 8);
        // The card draws the scene's decoded objects, not the table.
        scene.objects[i].present = false;
        ++sprites;
    }

    if ((text_tiles || blank_cells || sprites) && g_native_page_patch_logs < 40u) {
        ++g_native_page_patch_logs;
        std::fprintf(stderr,
                     "[settings] page two in the captured picture: %d text "
                     "tiles, %d blank cells, %d icons (frame %llu, started=%d)\n",
                     text_tiles, blank_cells, sprites, runtime_current_frame(),
                     g_native_page.active ? 1 : 0);
    }
}
}  // namespace gsr_runner
namespace {

// ---- Battle speed-up (src/battle_speed.h) ------------------------------
bool g_battle_toggle_held = false;
bool g_battle_skip_this_frame = false;
bool g_battle_halt_drop_armed = false;
// The battle background table index the last real VBlank used, or -1.
int g_battle_bg_front_index = -1;
std::int64_t g_battle_arena_front_index = -1;
// When the dropped wait put an index back, the game's own value it replaced
// and the value written, or -1. The game only fills and flips a table when
// the picture changes: if the pass after the drop does not flip again, its
// VBlank must scan out the table the dropped pass filled, not the restored
// one (a settled world-map battle camera otherwise kept the stale table for
// good: gpu_rewind_0033 at 2x vs 0034 at 1x, FACTS.md 2026-09-24).
int g_battle_bg_pending_index = -1;
int g_battle_bg_restored_index = -1;
std::int64_t g_battle_arena_pending_index = -1;
std::int64_t g_battle_arena_restored_index = -1;

// ---- Field Psynergy speed-up (src/battle_speed.h) ----------------------
// A cast is in progress while the cast object is set and a field Psynergy
// dispatcher's return sits in a live stack frame, read at the frame wait.
// Emulation thread only.
bool g_field_psy_running = false;  // the field path is holding 2x (for the log)

bool field_psynergy_casting() {
    using namespace gsr::battle_speed;
    if (bus_read_u32(kFieldCastSlot) == 0u) return false;
    const std::uint32_t sp = g_cpu.R[13] & ~3u;
    if (sp < kStackBottom || sp >= kStackTop) return false;
    for (std::uint32_t a = sp; a < kStackTop; a += 4u)
        for (const std::uint32_t r : kFieldDispatcherReturns)
            if (bus_read_u32(a) == r) return true;
    return false;
}

std::uint32_t battle_bg_index_address() {
    using namespace gsr::battle_speed;
    const std::uint32_t block = bus_read_u32(kBattleBgTablesSlot);
    const std::uint32_t region = block >> 24;
    if (region != 0x02u && region != 0x03u) return 0u;
    return block + kBattleBgIndexOffset;
}
std::uint32_t battle_arena_index_address() {
    using namespace gsr::battle_speed;
    const std::uint32_t block = bus_read_u32(kBattleArenaTablesSlot);
    const std::uint32_t region = block >> 24;
    if (region != 0x02u && region != 0x03u) return 0u;
    return block + kBattleArenaIndexOffset;
}
// Last frame the speed-up ran in battle; the ">>" shows while it is recent.
std::atomic<unsigned long long> g_battle_speed_seen_frame{0};
// The overclock (q8) in effect before the speed-up raised it, or 0 when the
// speed-up has not raised it.
unsigned g_battle_speed_saved_overclock_q8 = 0;

// Two battle frames must finish inside one VBlank period, or the VBlank
// handler commits a half-built second frame (seen as flicker with the
// overclock at 1x, session 20260923_101919). While the speed-up runs, the
// guest CPU gets at least kMinOverclock; the player's setting comes back
// when the battle ends or 2x is turned off, unless they changed it meanwhile.
void battle_speed_hold_overclock(bool hold) {
    constexpr unsigned kQ8 = 256u;
    const unsigned minimum = gsr::battle_speed::kMinOverclock * kQ8;
    const unsigned current = runtime_get_overclock_factor_q8();
    if (hold) {
        if (g_battle_speed_saved_overclock_q8 == 0u && current < minimum) {
            g_battle_speed_saved_overclock_q8 = current;
            runtime_set_overclock_factor_q8(minimum);
        }
    } else if (g_battle_speed_saved_overclock_q8 != 0u) {
        if (current == minimum)
            runtime_set_overclock_factor_q8(g_battle_speed_saved_overclock_q8);
        g_battle_speed_saved_overclock_q8 = 0u;
    }
}

// True while battle work paced by the display is running (the entry); 2x
// then runs one pass per VBlank (src/battle_speed.h). Hit fades run at 2x,
// as in The Lost Age.
bool battle_speed_display_paced() {
    using namespace gsr::battle_speed;
    return bus_read_u32(kVCountHandlerSlot) == kEntryVCountHandler;
}

// True while the game's VBlank register queue holds a DISPCNT write; the
// pass's wait then stays, so the layer switch lands before the game's next
// step (src/battle_speed.h).
bool battle_speed_layer_switch_queued() {
    using namespace gsr::battle_speed;
    std::uint32_t count = bus_read_u16(kVBlankQueueCount);
    if (count > kVBlankQueueMax) count = kVBlankQueueMax;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = kVBlankQueueEntries + i * kVBlankQueueEntrySize;
        if (bus_read_u32(entry + 4u) == kDispcnt) return true;
    }
    return false;
}

}  // namespace
namespace gsr_runner {
// The wait's mask load, once per frame routine pass: reads Select, and on
// every other battle frame keeps the VBlank flag and arms the Halt drop.
int battle_speed_wait_mask(std::uint32_t* out_value) {
    using namespace gsr::battle_speed;
    const bool in_battle = bus_read_u32(kBattleHeapSlot) != 0u;
    const bool toggle = (~bus_read_u16(kKeyInput) & kKeySelect) != 0u;
    if (in_battle && toggle && !g_battle_toggle_held) {
        g_battle_speed_on.store(!g_battle_speed_on.load());
        save_game_options();
    }
    g_battle_toggle_held = toggle;
    const bool battle_2x = in_battle && g_battle_speed_on.load();
    // A field Psynergy cast at Fast: the same 2x, outside battle only.
    const bool field_2x = !in_battle && g_field_psynergy_fast.load() != 0 &&
                          field_psynergy_casting();
    if (field_2x != g_field_psy_running) {
        g_field_psy_running = field_2x;
        std::fprintf(stderr, field_2x ? "[field-psy-fast] on frame=%llu\n"
                                      : "[field-psy-fast] off frame=%llu\n",
                     static_cast<unsigned long long>(runtime_current_frame()));
    }
    if (!battle_2x) {
        // Battle-only state: the per-line table restores and the HBlank DMA
        // latch belong to the battle's own drawing, never the field's.
        g_battle_bg_front_index = -1;
        g_battle_arena_front_index = -1;
        g_battle_bg_pending_index = -1;
        g_battle_arena_pending_index = -1;
        gba::g_hblank_dma_latch.store(false, std::memory_order_relaxed);
        gba::g_hblank_dma_cpu_write_wins.store(false, std::memory_order_relaxed);
    }
    if (!battle_2x && !field_2x) {
        g_battle_skip_this_frame = false;
        battle_speed_hold_overclock(false);
        return 0;
    }
    battle_speed_hold_overclock(true);
    bool display_paced = false;
    if (battle_2x) {
        // Per-line tables are read as they stood at the start of each frame.
        gba::g_hblank_dma_latch.store(true, std::memory_order_relaxed);
        // A spell that sets its own layer register while the arena's line
        // table still runs keeps it once the table stops: at 2x the frame
        // wait between the two can be the dropped one, and Mist, Aqua Sock
        // and Heat Flash were left showing the table's last value
        // (gpu_rewind_0122/0123/0125, FACTS.md 2026-10-02).
        gba::g_hblank_dma_cpu_write_wins.store(true, std::memory_order_relaxed);
        load_menu_font();
        g_battle_speed_seen_frame.store(runtime_current_frame());
        display_paced = battle_speed_display_paced();
    }
    if (battle_speed_layer_switch_queued()) display_paced = true;
    g_battle_skip_this_frame = !display_paced && !g_battle_skip_this_frame;
    // Both index tables are battle blocks; 0 leaves them alone in the field.
    const std::uint32_t bg_index = battle_2x ? battle_bg_index_address() : 0u;
    const std::uint32_t arena_index =
        battle_2x ? battle_arena_index_address() : 0u;
    if (!g_battle_skip_this_frame) {
        // This wait halts. If the dropped pass's flip was put back and this
        // pass did not flip again, hand the game its own choice back first:
        // that table is the complete one.
        if (bg_index && g_battle_bg_pending_index >= 0 &&
            bus_read_u8(bg_index) == g_battle_bg_restored_index) {
            bus_write_u8(bg_index,
                         static_cast<std::uint8_t>(g_battle_bg_pending_index));
        }
        if (arena_index && g_battle_arena_pending_index >= 0 &&
            bus_read_u32(arena_index) ==
                static_cast<std::uint32_t>(g_battle_arena_restored_index)) {
            bus_write_u32(arena_index, static_cast<std::uint32_t>(
                                           g_battle_arena_pending_index));
        }
        g_battle_bg_pending_index = -1;
        g_battle_arena_pending_index = -1;
        // The VBlank that follows scans out this index.
        g_battle_bg_front_index =
            bg_index ? static_cast<int>(bus_read_u8(bg_index)) : -1;
        g_battle_arena_front_index =
            arena_index ? static_cast<std::int64_t>(bus_read_u32(arena_index))
                        : -1;
        return 0;
    }
    if (bg_index && g_battle_bg_front_index >= 0 &&
        bus_read_u8(bg_index) != g_battle_bg_front_index) {
        g_battle_bg_pending_index = static_cast<int>(bus_read_u8(bg_index));
        g_battle_bg_restored_index = g_battle_bg_front_index;
        bus_write_u8(bg_index,
                     static_cast<std::uint8_t>(g_battle_bg_front_index));
    }
    if (arena_index && g_battle_arena_front_index >= 0 &&
        bus_read_u32(arena_index) !=
            static_cast<std::uint32_t>(g_battle_arena_front_index)) {
        g_battle_arena_pending_index =
            static_cast<std::int64_t>(bus_read_u32(arena_index));
        g_battle_arena_restored_index = g_battle_arena_front_index;
        bus_write_u32(arena_index,
                      static_cast<std::uint32_t>(g_battle_arena_front_index));
    }
    g_battle_halt_drop_armed = true;
    *out_value = kKeepFlagMask;
    return 1;
}

int golden_sun_swi_override(std::uint32_t return_pc, std::uint32_t swi_num) {
    using namespace gsr::battle_speed;
    if (return_pc != kWaitHaltReturnPc || swi_num != kSwiHalt) return 0;
    // One drop per armed pass: if the flag was clear after all, the wait's
    // loop comes back here and halts normally.
    const bool drop = g_battle_halt_drop_armed;
    g_battle_halt_drop_armed = false;
    return drop ? 1 : 0;
}

bool battle_speed_indicator_visible() {
    if (!g_settings_font_loaded.load(std::memory_order_acquire)) return false;
    return g_battle_speed_on.load() &&
           runtime_current_frame() <= g_battle_speed_seen_frame.load() + 1u;
}

// ">>" in the menu font, white with a dark shadow, at the native top left.
void paint_battle_speed_indicator(std::uint8_t* rgb, std::uint32_t width,
                                  std::uint32_t height) {
    using namespace gsr::battle_speed;
    const char* const text = kIndicator;
    constexpr std::uint8_t kLetter[3] = {0xF8, 0xF8, 0xF8};
    constexpr std::uint8_t kShadow[3] = {0x28, 0x28, 0x28};
    int x = kIndicatorX + static_cast<int>(g_golden_sun_wide_extra_left);
    const int y = kIndicatorY + static_cast<int>(g_golden_sun_wide_extra_top);
    for (const char* c = text; *c; ++c) {
        const auto code = static_cast<unsigned char>(*c);
        for (int gy = 0; gy < 8; ++gy) {
            for (int gx = 0; gx < 8; ++gx) {
                const std::uint8_t b =
                    g_settings_font.tiles[code * 32u + gy * 4 + gx / 2];
                const std::uint8_t v = (b >> (4 * (gx & 1))) & 0xFu;
                const std::uint8_t* colour =
                    v == gsr::settings_page::kGlyphLetter ? kLetter
                    : v == gsr::settings_page::kGlyphShadow ? kShadow
                                                            : nullptr;
                const int px = x + gx, py = y + gy;
                if (!colour || px < 0 || py < 0 ||
                    px >= static_cast<int>(width) ||
                    py >= static_cast<int>(height)) continue;
                std::copy(colour, colour + 3,
                          rgb + (static_cast<std::size_t>(py) * width + px) *
                                    3u);
            }
        }
        x += g_settings_font.widths[code - 0x20u];
    }
}
}  // namespace gsr_runner
namespace {

}  // namespace
