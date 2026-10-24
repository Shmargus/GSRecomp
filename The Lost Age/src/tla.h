// Golden Sun: The Lost Age -- the runner's features, one declaration each.
// Every src/tla_*.cpp includes this, so a definition that drifts from its
// declaration fails to compile.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "game_options.h"

namespace gbarecomp { struct RunOptions; struct HistoryFrame; }

void tla_install_gs_save_import(gbarecomp::RunOptions& options, bool headless);
bool tla_gs_save_import_text(std::uint32_t text_id, std::string& out);
bool tla_gs_save_import_visible();
bool tla_gs_save_import_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height);
void tla_gs_save_import_on_entry(std::uint32_t pc);

// game_options.ini, the runner's own options: engine game_options.h.

// src/tla_ram_images.cpp: routines the game copies into RAM buffers.
void tla_install_ram_images();

// src/tla_text_speed.cpp: Instant text, a fourth Message-speed choice.
void tla_install_text_speed(gbarecomp::RunOptions& options);
bool tla_text_speed_text(std::uint32_t text_id, std::string& out);

// src/tla_walk_speed.cpp: Walk Speed (the engine's walking-speed setting).
void tla_install_walk_speed(gbarecomp::RunOptions& options);
// Walk through walls (developer F1 cheat). Chains in front of the branch
// hook, so it is installed after the hooks that do not chain.
void tla_install_walk_through_walls(gbarecomp::RunOptions& options);

// Allocation slot table 0x03000000 + id (allocator 0x08014CC0): slot 0x24
// is non-zero exactly during battles, entry fade included (session
// 20261003_201931 replayed, frames 2542-3295 of 3424 with title, menus,
// field and cutscenes; the save-state battle bench, frames 10021-11771).
constexpr std::uint32_t kTlaBattleSlot = 0x03000024u;

// The window system's block, [slot 0x3C]: + 8 is its 32x32 map of BG0,
// written during the frame and flushed to BG0 at VBlank (settings page,
// src/tla_gpu_field.cpp's room copy).
constexpr std::uint32_t kTlaWindowBlockSlot = 0x0300003Cu;
constexpr std::uint32_t kTlaWindowMapOffset = 0x8u;

// The current map number: 0 between rooms, 1 on the title screen, else the
// room (44 in Madra, F12 0106).
constexpr std::uint32_t kTlaMapIdAddr = 0x02000428u;

// src/tla_battle_speed.cpp: Select in battle toggles 2x; field Psynergy casts
// at 2x (Better Field Psy).
void tla_install_battle_speed(gbarecomp::RunOptions& options);
bool tla_battle_speed_active();
bool tla_battle_spell_setup_frame();
bool tla_battle_speed_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height);
bool tla_field_psy_fast();
void tla_set_field_psy_fast(bool on);

// src/tla_encounters.cpp: Encounters (settings page two).
void tla_install_encounters(gbarecomp::RunOptions& options);
int tla_encounter_rate();
int tla_encounter_rate_count();
void tla_set_encounter_rate(int rate);

// src/tla_settings_page.cpp: settings page two.
void tla_install_settings_page(gbarecomp::RunOptions& options);
bool tla_settings_page_two_showing();
bool tla_settings_page_overlay_visible();
bool tla_settings_page_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height);
bool tla_settings_page_text(std::uint32_t text_id, std::string& out);

// src/tla_hard_mode.cpp: Hard Mode (tougher enemies) on every new game and in
// place of Auto-Sleep.
void tla_install_hard_mode(gbarecomp::RunOptions& options);
void tla_hard_mode_on_entry(std::uint32_t pc);
bool tla_hard_mode_text(std::uint32_t text_id, std::string& out);

// src/tla_local_music.cpp: Local music (album recordings in place of songs).
void tla_install_local_music(gbarecomp::RunOptions& options);
void tla_local_music_on_entry(std::uint32_t pc);
void tla_local_music_on_savestate_load();

// src/tla_cheat_menu.cpp: the cheat menu (F11): Infinite HP/PP and the
// Experience, Coins and Drop Chance multipliers.
void tla_install_cheat_menu(gbarecomp::RunOptions& options);

// src/tla_spell_sparks.cpp: spell sparks the game skips past its canvas,
// handed to the spell-effect capture for the margins.
void tla_install_spell_sparks(gbarecomp::RunOptions& options);

// src/tla_spell_mesh.cpp: spell triangles past the canvas, at the
// rasterizer's entry (called from tla_gpu_field_on_entry).
void tla_spell_mesh_on_entry(std::uint32_t pc);

// src/tla_text.cpp: text drawn in place of the game's own lines, from the
// tla_*_text answers; the menu font for the runner's own windows (pixel 1
// letter, 3 shadow, 4 background).
void tla_install_text(gbarecomp::RunOptions& options);
constexpr std::uint8_t kTlaGlyphLetter = 1u, kTlaGlyphShadow = 3u,
                       kTlaGlyphBackground = 4u;
std::uint8_t tla_menu_glyph(unsigned char code, int gx, int gy);
std::uint32_t tla_menu_text_width(const std::string& text);
void tla_menu_draw_text(const std::string& text, int x, int y,
                        const std::function<void(int, int, std::uint8_t)>& put);
void tla_menu_paint_window(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
                           int tile_x, int tile_y, int tiles_w, int tiles_h);
void tla_menu_paint_text(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
                         const std::string& text, int x, int y);

// Settings page one: the cursor row (halfword at screen + 0x574), and the value
// column the word rows use (Message speed, Hard Mode), cleared from x 0x54 and
// centred on 0x7C where the rows' icons were. The Auto-Sleep row's value clear
// `movs r1,#0xa0` is at 0x08040350.
constexpr std::uint32_t kTlaSettingsCursorRowOffset = 0x574u;
constexpr std::uint32_t kTlaValueClearX = 0x54u, kTlaValueCentreX = 0x7Cu;
constexpr std::uint32_t kTlaAutoSleepClearXPc = 0x08040350u;
// A word row's value: armed when the game loads the row's value text, then
// drawn once in its place (src/tla_text.cpp).
struct TlaValueCaption {
    std::string text;
    std::uint32_t id = 0;
    bool armed = false;
    // The word, with "< " / " >" while the cursor is on the row.
    void arm(std::uint32_t text_id, const char* word, bool selected);
    // Answers the text decoder for `text_id`, once.
    bool take(std::uint32_t text_id, std::string& out);
    // The x that centres it on kTlaValueCentreX.
    std::uint32_t draw_x() const;
};

// src/tla_widescreen.cpp: the expanded view's camera and characters.
void tla_install_widescreen(gbarecomp::RunOptions& options);
// window_open: a game window (menu) is showing this frame.
void tla_widescreen_frame(bool window_open);
// Function entries the expanded view watches (the camera follow).
void tla_widescreen_on_entry(std::uint32_t pc);
// Rows the expanded view's camera rule holds the camera from where the game
// put it (+ below), 0 when the game's own camera is in effect.
int tla_camera_hold_y();
// Room bounds (pixels; x then y) the camera rule keeps after the game
// narrowed them mid-map; the margins show that wider area too.
bool tla_camera_kept_bounds(bool kept[2], int lo[2], int edge[2]);

// src/tla_sprite_memory.cpp: a second sprite page for field people in the
// expanded view. The branch hook answers -1 for any other pc. The frame hook
// says whether the page is mapped; tile gives a sprite
// record's full tile number, or -1.
int tla_sprite_memory_branch(std::uint32_t pc, std::uint32_t original,
                             std::uint32_t* out_decision);
bool tla_sprite_memory_frame(bool expanded);
int tla_sprite_memory_tile(std::uint32_t record, std::uint16_t attr2);

// src/tla_gpu_field.cpp: draw the field with the graphics card.
void tla_install_gpu_field();
void tla_gpu_field_on_entry(std::uint32_t pc);
bool tla_gpu_field_present(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
                           gbarecomp::HistoryFrame& frame, bool synchronous);
void tla_rewind_reset();
void tla_gpu_field_dump(const std::string& path);
std::string tla_gpu_field_snapshot(std::uint32_t width, std::uint32_t height,
                                   std::uint64_t* scene_frame = nullptr);

// src/tla_rewind.cpp: F12 saves the last 2 seconds of frames.
void tla_install_rewind();
void tla_paint_text(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
                    int x, int y, const char* text, int advance = 6);
