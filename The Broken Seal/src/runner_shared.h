// Names shared between runner_main.cpp and the files split out of it
// (runner_ram_code.cpp, runner_settings.cpp, runner_cheats.cpp). Declarations
// only, except the types and small inline helpers the callers need to see in
// full. Each name's definition lives in exactly one of those four files.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "field_scene.h"
#include "tile_ring_redraw.h"
#include "cheat_menu.h"
#include "settings_page.h"
#include "host_config_ui.h"
#include "obj_recorder.h"
#include "relocatable_identity.h"
#include "relocatable_writer_policy.h"
#include "../../shared_game/settings_help.h"
#include "../../shared_game/custom_battle_panel.h"


struct DispatchEntry {
    std::uint32_t addr;
    std::uint8_t thumb;
    void (*fn)(void);
};

extern "C" const DispatchEntry gsr_func2d5c_kDispatchTable[];
extern "C" const unsigned gsr_func2d5c_kDispatchTableLen;
extern "C" const DispatchEntry gsr_funca37c_kDispatchTable[];
extern "C" const unsigned gsr_funca37c_kDispatchTableLen;
extern "C" const DispatchEntry gsr_func2808_03006000_kDispatchTable[];
extern "C" const unsigned gsr_func2808_03006000_kDispatchTableLen;
extern "C" const DispatchEntry gsr_func15e10_03003a84_kDispatchTable[];
extern "C" const unsigned gsr_func15e10_03003a84_kDispatchTableLen;
extern "C" const DispatchEntry gsr_funca418_03003400_kDispatchTable[];
extern "C" const unsigned gsr_funca418_03003400_kDispatchTableLen;
extern "C" const DispatchEntry gsr_func15afc_03003a84_kDispatchTable[];
extern "C" const unsigned gsr_func15afc_03003a84_kDispatchTableLen;
// The flash driver's relocatable working-area routines. Each is ONE
// position-independent corpus serving every base the allocator hands out; see
// kRelocatableCodeImages below.
extern "C" const DispatchEntry gsr_func9bb8_pic_kDispatchTable[];
extern "C" const unsigned gsr_func9bb8_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func9bb8_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func9bb8_pic_kImageSize;
extern "C" const DispatchEntry gsr_func15430_pic_kDispatchTable[];
extern "C" const unsigned gsr_func15430_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func15430_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func15430_pic_kImageSize;
extern "C" const DispatchEntry gsr_func15570_pic_kDispatchTable[];
extern "C" const unsigned gsr_func15570_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func15570_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func15570_pic_kImageSize;
extern "C" const DispatchEntry gsr_func158e8_pic_kDispatchTable[];
extern "C" const unsigned gsr_func158e8_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func158e8_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func158e8_pic_kImageSize;
extern "C" const DispatchEntry gsr_func6b84_pic_kDispatchTable[];
extern "C" const unsigned gsr_func6b84_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func6b84_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func6b84_pic_kImageSize;
extern "C" const DispatchEntry gsr_func7994_pic_kDispatchTable[];
extern "C" const unsigned gsr_func7994_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func7994_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func7994_pic_kImageSize;
extern "C" const DispatchEntry gsr_func9e7c_pic_kDispatchTable[];
extern "C" const unsigned gsr_func9e7c_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func9e7c_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func9e7c_pic_kImageSize;
extern "C" const DispatchEntry gsr_funca0f8_pic_kDispatchTable[];
extern "C" const unsigned gsr_funca0f8_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_funca0f8_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_funca0f8_pic_kImageSize;
extern "C" const DispatchEntry gsr_func1b70_pic_kDispatchTable[];
extern "C" const unsigned gsr_func1b70_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func1b70_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func1b70_pic_kImageSize;
extern "C" const DispatchEntry gsr_func2544_pic_kDispatchTable[];
extern "C" const unsigned gsr_func2544_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func2544_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func2544_pic_kImageSize;
// Func_6abc, the four-byte THUMB stack thunk, position-independent.
extern "C" const DispatchEntry gsr_func6abc_pic_kDispatchTable[];
extern "C" const unsigned gsr_func6abc_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func6abc_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func6abc_pic_kImageSize;
// Position-independent: ONE corpus for Func_2cf4 at every stack depth its
// caller reaches. The set of bases is bounded only by the call graph, so
// per-base corpora (0x03007ba4, 0x03007dbc, 0x03007dc4, 0x03007dc8 and any
// later one) are not used.
extern "C" const DispatchEntry gsr_func2cf4_pic_kDispatchTable[];
extern "C" const unsigned gsr_func2cf4_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func2cf4_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func2cf4_pic_kImageSize;
// Func_1dc8, the flash driver's relocatable working-area routine.
extern "C" const DispatchEntry gsr_func1dc8_pic_kDispatchTable[];
extern "C" const unsigned gsr_func1dc8_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func1dc8_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func1dc8_pic_kImageSize;
// Func_15afc and Func_15e10 share the 0x03003a84 staging slot, but a
// 10,800-frame campaign run copied Func_15afc to 0x030044f4 instead, so a fixed
// base is not enough; see kRelocatableCodeImages below.
extern "C" const DispatchEntry gsr_func15afc_pic_kDispatchTable[];
extern "C" const unsigned gsr_func15afc_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func15afc_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func15afc_pic_kImageSize;
extern "C" const DispatchEntry gsr_func15e10_pic_kDispatchTable[];
extern "C" const unsigned gsr_func15e10_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func15e10_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func15e10_pic_kImageSize;
// The two remaining ARM routines of the same flash-driver pool family.
extern "C" const DispatchEntry gsr_func15d74_pic_kDispatchTable[];
extern "C" const unsigned gsr_func15d74_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func15d74_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func15d74_pic_kImageSize;
extern "C" const DispatchEntry gsr_func155d0_pic_kDispatchTable[];
extern "C" const unsigned gsr_func155d0_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func155d0_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func155d0_pic_kImageSize;
// Func_9bb8's SHORT (0x1e4) copy variant - a second identity for the SAME
// routine, not a second routine. See kRelocatableCodeImages.
extern "C" const DispatchEntry gsr_func9bb8short_pic_kDispatchTable[];
extern "C" const unsigned gsr_func9bb8short_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func9bb8short_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func9bb8short_pic_kImageSize;
// Code-only Func_9bb8: the identity that survives the game patching its own
// literal pool after the copy. See kRelocatableCodeImages.
extern "C" const DispatchEntry gsr_func9bb8code_pic_kDispatchTable[];
extern "C" const unsigned gsr_func9bb8code_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func9bb8code_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func9bb8code_pic_kImageSize;
// Func_b5138, a transient allocator block DMA-copied into IWRAM at whatever
// base the allocator hands out. Observed at 0x0300207c (first scripted
// fight); base is allocator-determined, so this is registered
// position-independent from the start rather than fixed. GS-011 follow-up.
extern "C" const DispatchEntry gsr_funcb5138_pic_kDispatchTable[];
extern "C" const unsigned gsr_funcb5138_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_funcb5138_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_funcb5138_pic_kImageSize;
// Func_f0024, the ending credits' decoder: the same self-relocating decoder
// as Func_b5138 from another module, copied into an IWRAM pool block.
// Observed at 0x0300347c.
extern "C" const DispatchEntry gsr_funcf0024_pic_kDispatchTable[];
extern "C" const unsigned gsr_funcf0024_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_funcf0024_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_funcf0024_pic_kImageSize;
// Func_a418, the same ROM routine already registered fixed at 0x03003400
// (see kTransientCodeImages below), also observed at 0x03002000. Routine
// and base vary independently (D-005), so it is registered
// position-independent.
extern "C" const DispatchEntry gsr_funca418_pic_kDispatchTable[];
extern "C" const unsigned gsr_funca418_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_funca418_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_funca418_pic_kImageSize;
// The spell canvas's six DMA3-copied stack blobs, each a single ARM
// STT_FUNC-sized routine copied by its own wrapper (Func_54e4, Func_5534,
// 0x08005490, 0x0800543C, 0x0800562C, 0x0800567C — all the same shape):
// "copy and fade" (ROM 0x08001ea8, 0x50 bytes), "copy and halve" (ROM
// 0x08001ef8, 0x40 bytes), Func_1f38 (0x80 bytes), Func_1fb8 (0x84 bytes),
// Func_203c (0x5c bytes) and Func_2098 (0x5c bytes). Each wrapper's
// literal-pool word is a byte count (the wrapper right-shifts it by 2 to get
// the DMA3 word count), not a word count. See
// config/usa/transient-func_1ea8-relocatable.toml,
// transient-func_1ef8-relocatable.toml and
// transient-func_1f38-/1fb8-/2098-relocatable.toml for the derivation.
extern "C" const DispatchEntry gsr_func1ea8_pic_kDispatchTable[];
extern "C" const unsigned gsr_func1ea8_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func1ea8_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func1ea8_pic_kImageSize;
extern "C" const DispatchEntry gsr_func1ef8_pic_kDispatchTable[];
extern "C" const unsigned gsr_func1ef8_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func1ef8_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func1ef8_pic_kImageSize;
extern "C" const DispatchEntry gsr_func1f38_pic_kDispatchTable[];
extern "C" const unsigned gsr_func1f38_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func1f38_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func1f38_pic_kImageSize;
extern "C" const DispatchEntry gsr_func1fb8_pic_kDispatchTable[];
extern "C" const unsigned gsr_func1fb8_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func1fb8_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func1fb8_pic_kImageSize;
extern "C" const DispatchEntry gsr_func2098_pic_kDispatchTable[];
extern "C" const unsigned gsr_func2098_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func2098_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func2098_pic_kImageSize;
// Func_6c24 (ROM 0x08006c24, 0x44 bytes: THUMB code plus its own inline
// literal pool), the whole unit Func_6c68 copies onto the stack before
// jumping into it. Confirmed at offset 0 in session 20260923_202001 (not a
// 16-byte interior slice). See config/usa/transient-func_6c24-relocatable.toml.
extern "C" const DispatchEntry gsr_func6c24_pic_kDispatchTable[];
extern "C" const unsigned gsr_func6c24_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func6c24_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func6c24_pic_kImageSize;
// Func_203c (ROM 0x0800203c, 0x5c bytes ARM, no interior $d), DMA3-copied
// onto the stack by the wrapper at 0x0800562c (the same shape as
// 0x080054e4/0x08005534). See config/usa/transient-func_203c-relocatable.toml.
extern "C" const DispatchEntry gsr_func203c_pic_kDispatchTable[];
extern "C" const unsigned gsr_func203c_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func203c_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func203c_pic_kImageSize;
extern "C" const DispatchEntry gsr_synth_dc8_030008d4_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8_030008d4_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8_03000904_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8_03000904_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8_030008ec_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8_030008ec_kDispatchTableLen;
// Every particle stamp routine Func_ed408 can build, pre-compiled as one
// position-independent image (player_builder/builder_main.cpp).
struct GsrStampVariant {
    std::uint32_t offset;
    std::uint32_t length;
    const char* sha1;
};
extern "C" const DispatchEntry gsr_stamps_kDispatchTable[];
extern "C" const unsigned gsr_stamps_kDispatchTableLen;
extern "C" const std::uint32_t gsr_stamps_kImageOrigin;
extern "C" const GsrStampVariant gsr_stamps_kVariants[];
extern "C" const unsigned gsr_stamps_kVariantCount;
extern "C" const DispatchEntry gsr_synth_dc8c_0_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8c_0_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8c_1_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8c_1_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8c_2_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8c_2_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8c_3_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8c_3_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8c_4_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8c_4_kDispatchTableLen;
extern "C" const DispatchEntry kDispatchTable[];
extern "C" const unsigned kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8b_0_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8b_0_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8b_1_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8b_1_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8b_2_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8b_2_kDispatchTableLen;
extern "C" const DispatchEntry gsr_synth_dc8b_3_kDispatchTable[];
extern "C" const unsigned gsr_synth_dc8b_3_kDispatchTableLen;
extern "C" const DispatchEntry gsr_overlay_rom_779188_kDispatchTable[];
extern "C" const unsigned gsr_overlay_rom_779188_kDispatchTableLen;
extern "C" const DispatchEntry gsr_overlay_rom_787e04_kDispatchTable[];
extern "C" const unsigned gsr_overlay_rom_787e04_kDispatchTableLen;
extern "C" const DispatchEntry gsr_overlay_rom_77dd1c_kDispatchTable[];
extern "C" const unsigned gsr_overlay_rom_77dd1c_kDispatchTableLen;
extern "C" const DispatchEntry gsr_overlay_rom_784360_kDispatchTable[];
extern "C" const unsigned gsr_overlay_rom_784360_kDispatchTableLen;
extern "C" const DispatchEntry gsr_overlay_rom_78603c_kDispatchTable[];
extern "C" const unsigned gsr_overlay_rom_78603c_kDispatchTableLen;
extern "C" const DispatchEntry gsr_overlay_rom_7795e8_kDispatchTable[];
extern "C" const unsigned gsr_overlay_rom_7795e8_kDispatchTableLen;
extern "C" const DispatchEntry gsr_overlay_rom_780898_kDispatchTable[];
extern "C" const unsigned gsr_overlay_rom_780898_kDispatchTableLen;
#define GSR_OVERLAY(id, start, end, sha1)                                  \
    extern "C" const DispatchEntry gsr_overlay_##id##_kDispatchTable[];   \
    extern "C" const unsigned gsr_overlay_##id##_kDispatchTableLen;
#include "overlay-registry.inc"
#undef GSR_OVERLAY

namespace gbarecomp {
extern "C" RuntimeGuestFn overlay_resolve(std::uint32_t pc, int thumb);
}


namespace gsr_runner {

// ---- Types ----

// A verified transient image is safe to reuse until one of its RAM pages
// changes. The global generation is retained for diagnostics, but using it as
// the dispatch-cache key made an unrelated overlay write flush every hot PC.
enum class VerifiedRamAction : std::uint8_t { Static, Native };

inline constexpr std::size_t kNoVerifiedIdentity =
    static_cast<std::size_t>(-1);

struct VerifiedRamCacheEntry {
    std::size_t identity_index = kNoVerifiedIdentity;
    VerifiedRamAction action = VerifiedRamAction::Static;
    void (*fn)(void) = nullptr;
};

struct TransientCodeImage {
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t rom_start;
    int thumb;
    const char* sha1;
    const char* name;
    const DispatchEntry* dispatch_table;
    const unsigned* dispatch_table_len;
    // Leading bytes the game may overwrite after entering the image. Identity
    // hashes the ROM bytes in their place, and an entry inside them is served
    // only while they still equal ROM.
    std::uint32_t volatile_prefix = 0;
};

// A RAM code image whose translation is position-independent: guest addresses
// inside it are computed from g_runtime_image_base, so one corpus dispatches
// at every base the game copies the routine to. `origin` is the address the
// corpus was generated at — the dispatch table is expressed in it, and a live
// PC is translated by subtracting the verified base.
//
// This is what turns the relocatable pool from an unbounded enumeration into a
// finite one: a routine is registered once, not once per (routine, base).
struct RelocatableCodeImage {
    std::uint32_t rom_start;   // immutable ROM backing; 0 = no linear source
    int thumb;
    const char* sha1;          // identity over the extent, minus excluded_ranges
    const char* name;
    const std::uint32_t* origin;
    const std::uint32_t* size;
    const DispatchEntry* dispatch_table;
    const unsigned* dispatch_table_len;
    // Declared [[data_range]] spans (offsets from origin/base) the image
    // writes to itself at runtime. Skipped when computing the identity hash
    // so self-modified data doesn't fail a code-only identity. Empty for
    // every image that has no interior data range.
    const gsr::ByteRange* excluded_ranges = nullptr;
    unsigned excluded_ranges_len = 0;
};

// Written on the emulation thread by the settings loop's key-word loads and
// read at present time, so everything the painter needs is copied here first:
// it never reads guest memory itself.
struct SettingsPageFont {
    std::uint8_t tiles[256 * 32] = {};
    std::uint8_t widths[256 - 0x20] = {};
};

// The questions are asked with the game still running, so its music and
// the questions' sounds play, held in the game's own frame routine: on entry
// Func_77f40 (Thumb) returns at once into the frame routine 0x080030F8(1)
// (Thumb), whose return comes back to its own entry once a frame until both
// are answered; then it returns into Func_77f40 with its saved arguments and
// return address, and the new game goes on. The engine runs, draws and reads
// keys frame by frame as usual (a guest_call loop would not: the launcher
// turns present-in-place off, so nothing is shown or read inside a call).
// The keys go to the questions only (cheat_menu_keyinput_filter).
enum class QuestionHold { Off, Waiting, Resuming };

// Seamless mode at the end of the game (seamless_mode.h). Emulation thread.
enum class SeamlessPhase { Off, Choosing, Leaving };

// Challenge Mode's sizes in The Broken Seal (index = heroes): Solo, Duo,
// Triple; its heroes Isaac, Garet, Ivan, Mia (ids 0..3).
inline constexpr int kChallengeHeroes = 4;


// ---- Variables defined in runner_main.cpp ----
extern std::uint32_t g_golden_sun_wide_extra_left;
extern std::uint32_t g_golden_sun_wide_extra_top;
extern unsigned long long g_relocatable_resident_hashes;
extern unsigned long long g_relocatable_resident_hash_bytes;
extern unsigned long long g_func1dc8_writer_hashes;
extern std::uint64_t g_golden_sun_func1dc8_writer_recognized;
extern std::uint64_t g_golden_sun_func1dc8_writer_identity_mismatch;
extern std::uint64_t g_golden_sun_func1dc8_writer_unknown_variant;
extern unsigned g_golden_sun_func1dc8_writer_logs;
extern std::uint32_t g_golden_sun_func1dc8_last_base;
extern std::uint64_t g_golden_sun_func1dc8_last_generation;
extern std::uint32_t g_decoder_text_id;
extern gsr::TileRingRedraw g_tile_ring_redraw;

// ---- Variables defined in runner_settings.cpp ----
extern SettingsPageFont g_settings_font;
extern std::atomic<bool> g_settings_font_loaded;
extern bool g_settings_screen_text;
extern std::atomic<bool> g_settings_open;
extern std::atomic<int> g_encounter_rate;
extern bool g_encounter_skip_next;
extern std::atomic<int> g_field_psynergy_fast;
extern std::vector<std::uint32_t> g_menu_sounds;

// ---- Variables defined in runner_cheats.cpp ----
extern std::uint16_t g_cheat_window_palette[16];
extern bool g_cheat_window_palette_known;
extern bool g_hard_prompt_open;
extern QuestionHold g_question_hold;
extern SeamlessPhase g_seamless_phase;
extern const gbarecomp::GameOptionSwitch kGameSwitches[];
extern const int kPlayerGameSwitchCount;
extern const int kGameSwitchCount;

// ---- Variables defined in runner_ram_code.cpp ----
extern unsigned long long g_relocatable_identity_hashes;
extern unsigned long long g_relocatable_identity_cache_hits;
extern unsigned long long g_relocatable_identity_snapshot_hits;
extern std::unordered_map<std::uint64_t, VerifiedRamCacheEntry> g_verified_ram_cache;
extern unsigned long long g_verified_identity_hashes;
extern unsigned long long g_verified_identity_cache_hits;
extern unsigned long long g_verified_identity_content_hits;
extern unsigned long long g_verified_identity_invalidations;
extern unsigned long long g_stamp_identity_scans;
extern unsigned long long g_stamp_identity_hashes;

// ---- Functions defined in runner_main.cpp ----
void golden_sun_obj_f0_entry_capture(std::uint32_t entry_pc);
bool golden_sun_wide_diagnostics_enabled();
void golden_sun_obj_staging_handoff( std::uint32_t entry_pc, std::uint32_t staging_address_override = 0u, std::uint32_t destination_override = 0u);
void golden_sun_note_projected_obj_placement(std::uint32_t entry_pc);
void record_golden_sun_obj_boulder_calc_entry(std::uint32_t entry_pc);
int enemy_hp_bar_name_width(std::uint32_t name);

// ---- Functions defined in runner_settings.cpp ----
void load_game_options(const char* argv0);
void load_menu_font();
void note_settings_loop_frame();
void play_menu_sounds();
int settings_page_repeat_keys(std::uint32_t* out_value);
std::string settings_text(const char* english);
int battle_speed_wait_mask(std::uint32_t* out_value);
int golden_sun_swi_override(std::uint32_t return_pc, std::uint32_t swi_num);
bool battle_speed_indicator_visible();
void paint_battle_speed_indicator(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height);

// ---- Functions defined in runner_cheats.cpp ----
void question_hold_new_game_entry();
void question_hold_naming_entry();
bool story_flag(std::uint32_t flag);
void skip_opening_apply();
void question_hold_frame_entry();
void seamless_on_entry(std::uint32_t entry_pc);
void seamless_frame_end();
std::uint16_t cheat_menu_keyinput_filter(std::uint16_t keyinput);
void cheat_menu_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height);
void paint_custom_battle_panel(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height, const gsr::custom_battle::PanelView& view);
int cheat_reward_write(std::uint32_t pc, std::uint32_t addr, std::uint32_t requested, std::uint32_t width, std::uint32_t* out_value);
int cheat_drop_branch(std::uint32_t pc, std::uint32_t original, std::uint32_t* out_decision);
bool battle_spell_position_pending(const gba::GbaPpu& ppu);

// ---- Functions defined in runner_ram_code.cpp ----
void init_ram_code_page_masks();
void set_revision_identities(const std::vector<std::uint8_t>& rom);
void reset_golden_sun_func1dc8_writer_diagnostics();
bool golden_sun_func1dc8_writer_pc(std::uint32_t pc, gsr::Func1dc8WriterRoute route);
void challenge_spare_for_lesson();
void golden_sun_function_entry_observer(std::uint32_t entry_pc);
void verified_ram_dispatch_return_hook(std::uint32_t return_pc, std::uint32_t call_stack_depth);
void verified_ram_dispatch_outer_boundary();
RuntimeGuestFn verified_ram_dispatch(std::uint32_t pc, int thumb);
int verified_ram_identity_confirmed(std::uint32_t pc, int thumb);
void print_usage();
std::span<const TransientCodeImage> transient_code_images();
void reset_verified_identity_cache();
void reset_relocatable_identity_cache();
void print_ram_cache_image_detail();

// ---- Inline helpers shared by the settings page and the cheat/prompt windows ----
// pixel(x, y): a native pixel's RGB in the presented picture (widescreen
// offset applied), null off it.
inline auto native_pixel_at(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    const int ox = static_cast<int>(g_golden_sun_wide_extra_left);
    const int oy = static_cast<int>(g_golden_sun_wide_extra_top);
    return [=](int x, int y) -> std::uint8_t* {
        x += ox;
        y += oy;
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
            return nullptr;
        return rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
    };
}

// Paints native pixels (widescreen offset applied) in the window palette:
// the live one when bank 15 holds it, else the last one seen, else the
// measured default. put(x, y, index); index 0 is transparent.
inline auto window_pixel_painter(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    using namespace gsr::cheat_menu;
    if (gba::GbaBus* bus = gbarecomp::active_bus()) {
        const std::uint8_t* pal = bus->pal_ptr() + kWindowBank * 32;
        std::uint16_t bank[16];
        for (int i = 0; i < 16; ++i)
            bank[i] = static_cast<std::uint16_t>(pal[i * 2] |
                                                 (pal[i * 2 + 1] << 8));
        if (bank[1] == 0x7FFFu && bank[3] == 0u) {
            std::copy(bank, bank + 16, g_cheat_window_palette);
            g_cheat_window_palette_known = true;
        }
    }
    const std::uint16_t* palette = g_cheat_window_palette_known
        ? g_cheat_window_palette : kDefaultWindowPalette;
    const int ox = static_cast<int>(g_golden_sun_wide_extra_left);
    const int oy = static_cast<int>(g_golden_sun_wide_extra_top);
    return [=](int x, int y, std::uint8_t index) {
        x += ox;
        y += oy;
        if (index == 0 || x < 0 || y < 0 || x >= static_cast<int>(width) ||
            y >= static_cast<int>(height)) return;
        const std::uint16_t c = palette[index & 0xFu];
        std::uint8_t* p = rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
        const unsigned r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
        p[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
        p[1] = static_cast<std::uint8_t>((g << 3) | (g >> 2));
        p[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
    };
}

// The window, drawn over the picture in the game's own pieces and colours
// (src/cheat_menu.h), `tiles` x `rows` native tiles from tile (tile_x, tile_y).
// `content(draw_text, text_width, inner_x, inner_y, pixel)` paints inside it;
// `pixel(x, y)` is the RGB of a native pixel, null off the picture. Shared by
// the Hard Mode prompt and the Custom Battle opponent panel.
template <typename Content>
void menu_window_paint(std::uint8_t* rgb, std::uint32_t width,
                       std::uint32_t height, int tile_x, int tile_y, int tiles,
                       int rows, Content content) {
    using namespace gsr::cheat_menu;
    if (!g_settings_font_loaded.load(std::memory_order_acquire)) return;
    auto put = window_pixel_painter(rgb, width, height);
    auto pixel_of = [&](unsigned char code, int gx, int gy) {
        const std::uint8_t b = g_settings_font.tiles[code * 32u + gy * 4 + gx / 2];
        return static_cast<std::uint8_t>((b >> (4 * (gx & 1))) & 0xFu);
    };
    for (int ty = 0; ty < rows; ++ty) {
        for (int tx = 0; tx < tiles; ++tx) {
            const bool top = ty == 0, bottom = ty == rows - 1;
            const bool left = tx == 0, right = tx == tiles - 1;
            const unsigned char code =
                top ? (left ? kTopLeft : right ? kTopRight : kTop)
                : bottom ? (left ? kBottomLeft : right ? kBottomRight : kBottom)
                : left ? kLeft : right ? kRight : kFill;
            for (int gy = 0; gy < 8; ++gy)
                for (int gx = 0; gx < 8; ++gx)
                    put((tile_x + tx) * 8 + gx,
                        (tile_y + ty) * 8 + gy, pixel_of(code, gx, gy));
        }
    }
    const int inner_x = (tile_x + 1) * 8;
    const int inner_y = (tile_y + 1) * 8;
    auto text_width = [&](const std::string& text) {
        int total = 0;
        for (const char c : text) {
            const auto code = static_cast<unsigned char>(c);
            if (code >= 0x20u) total += g_settings_font.widths[code - 0x20u];
        }
        return total;
    };
    // Letter and shadow pixels only; the window's own inside shows around
    // them, as it does behind the game's text. Pixels outside columns
    // [clip_left, clip_right) are left out (scrolling text).
    auto draw_text = [&](const std::string& text, int x, int y,
                         int clip_left = -0x7FFF, int clip_right = 0x7FFF) {
        for (const char c : text) {
            const auto code = static_cast<unsigned char>(c);
            if (code < 0x20u) continue;
            for (int gy = 0; gy < 8; ++gy) {
                for (int gx = 0; gx < 8; ++gx) {
                    const std::uint8_t v = pixel_of(code, gx, gy);
                    if (x + gx < clip_left || x + gx >= clip_right) continue;
                    if (v == gsr::settings_page::kGlyphLetter ||
                        v == gsr::settings_page::kGlyphShadow)
                        put(x + gx, y + gy, v);
                }
            }
            x += g_settings_font.widths[code - 0x20u];
        }
    };
    auto pixel = native_pixel_at(rgb, width, height);
    content(draw_text, text_width, inner_x, inner_y, pixel);
}

}  // namespace gsr_runner

namespace gsr_runner { void install_quality_of_life(); }
