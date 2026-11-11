// Game options and the Psynergy speed-up.
// Split out of runner_main.cpp. Names shared with the other runner_*.cpp files
// are declared in runner_shared.h.

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
#include "../../shared_game/quality_of_life.h"

extern "C" unsigned g_ws_active;
extern "C" unsigned long long runtime_current_frame();
extern "C" void runtime_iwram_code_write_dump(const char* why);
extern "C" void runtime_iwram_code_write_dump_range(const char* why,
                                                    std::uint32_t lo,
                                                    std::uint32_t hi);
extern "C" void runtime_unpacker_slot_dump(const char* why);

namespace {
using namespace gsr_runner;

}  // namespace
namespace gsr_runner {
SettingsPageFont g_settings_font;
std::atomic<bool> g_settings_font_loaded{false};
// True from the settings screen's setup to its closing: Hard Mode's label
// and help text are replaced only then, never in dialogue.
bool g_settings_screen_text = false;
// Set by the settings loop's first frame; cleared by the next screen setup
// and by a savestate load, which can leave the screen without closing it.
// Not a frame count: a loop frame that runs late must not hide the page.
std::atomic<bool> g_settings_open{false};
}  // namespace gsr_runner
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

}  // namespace
namespace gsr_runner {
// Emulation thread only: copies the menu font out of the ROM once.
void load_menu_font() {
    using namespace gsr::settings_page;
    if (g_settings_font_loaded.load(std::memory_order_acquire)) return;
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
    load_menu_font();
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

// The repeat-keys load: Left/Right on the Difficulty row.
int settings_page_repeat_keys(std::uint32_t*) {
    // The game's own five rows, Difficulty last (hard_mode.h); the old page
    // two now lives in the Quality of Life menu, so the loop moves as usual.
    // Up and Down are tested first and end the frame's input.
    using namespace gsr::settings_page;
    const std::uint32_t keys = bus_read_u32(kRepeatKeys);
    if (g_cpu.R[9] == kAutoSleepRow && !(keys & (kKeyUp | kKeyDown)))
        gsr::difficulty_row::keys((keys & kKeyLeft) != 0, (keys & kKeyRight) != 0);
    return 0;
}

// A word in the player's language (gbarecomp::ui_text), in the
// font's codes: the table is UTF-8, the font Latin-1 (á = 0xE1; FACTS.md
// "Spanish ROM: menu text").
std::string settings_text(const char* english) {
    return gsr::menu_text(english);
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

namespace gsr_runner {
std::vector<gsr::qol_menu::Row> quality_of_life_rows() {
    using namespace gsr::settings_page;
    using gsr::qol::step;
    std::vector<gsr::qol_menu::Row> rows;
    rows.push_back({kRowLabels[kWalkSpeedRow], kRowHelp[kWalkSpeedRow],
        [] { return std::string(kWalkingSpeedWords[walk_speed_index(runtime_get_mem_write_override_enabled())]); },
        [](int direction) {
            const int index = step(walk_speed_index(runtime_get_mem_write_override_enabled()), kWalkSpeedCount, direction);
            runtime_set_mem_write_override_enabled(kWalkSpeedHalves[index]);
        }});
    rows.push_back({kRowLabels[kEncounterRow], kRowHelp[kEncounterRow],
        [] { return std::string(kEncounterWords[g_encounter_rate.load()]); },
        [](int direction) { g_encounter_rate.store(gsr::settings_labels::step_encounter(g_encounter_rate.load(), direction)); save_game_options(); }});
    rows.push_back({kRowLabels[kScreenRow], kRowHelp[kScreenRow],
        [] { return std::string(gbarecomp::kViewModeLabels[std::clamp(runtime_get_view_mode(), 0, gbarecomp::kViewModeCount - 1)]); },
        [](int direction) { runtime_request_view_mode(step(runtime_get_view_mode(), gbarecomp::kViewModeCount, direction)); }});
    rows.push_back({kRowLabels[kNoSlowdownRow], kRowHelp[kNoSlowdownRow],
        [] { return runtime_get_no_slowdown() ? "On" : "Off"; },
        [](int) { runtime_request_no_slowdown(!runtime_get_no_slowdown()); }});
    rows.push_back({kRowLabels[kFieldPsynergyRow], kRowHelp[kFieldPsynergyRow],
        [] { return g_field_psynergy_fast.load() ? "On" : "Off"; },
        [](int) { g_field_psynergy_fast.store(!g_field_psynergy_fast.load()); save_game_options(); }});
    gsr::qol::append_switches(rows);
    return rows;
}
void install_quality_of_life() {
    static const gsr::qol::Game profile = {
        GS_ROM(0x0801776Cu), GS_ROM(0x08029531u), GS_ROM(0x08029506u),
        GS_ROM(0x080030F8u), GS_ROM(0x080F9010u),
        gs::text(0xC24u), gs::text(0xC28u), GS_ROM(0x080216E8u),
        42u, 23u, load_menu_font, quality_of_life_rows}; // Sleep 42 (pause icon table); 23 picked by Jimmy from the icon sheet, 11 November 2026.
    gsr::qol::install(profile);
}
} // namespace gsr_runner
