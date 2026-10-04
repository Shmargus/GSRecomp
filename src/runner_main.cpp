#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <tuple>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "crc32.h"
#include "crash_handler.h"
#include "function_tracer.h"
#include "map_recorder.h"
#include "obj_recorder.h"
#include "battle_view.h"
#include "room_buffer.h"
#include "field_scene.h"
#include "effect_capture.h"
#include "mod_loader.h"
#include "earth_surge.h"
#include "move_probe.h"
#include "field_scene_renderer.h"
#include "world_map_source.h"
#include "cheat_menu.h"
#include "hard_mode.h"
#include "gpu_surface.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "gba_vram_trace.h"
#include "recompiled.h"
#include "relocatable_identity.h"
#include "relocatable_writer_policy.h"
#include "player_speed_cheat.h"
#include "text_speed_cheat.h"
#include "settings_page.h"
#include "battle_speed.h"
#include "runtime.h"
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

extern "C" unsigned g_ws_active;
extern "C" unsigned long long runtime_current_frame();
extern "C" void runtime_iwram_code_write_dump(const char* why);
extern "C" void runtime_iwram_code_write_dump_range(const char* why,
                                                    std::uint32_t lo,
                                                    std::uint32_t hi);
extern "C" void runtime_unpacker_slot_dump(const char* why);

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
// caller reaches. Replaces the four per-base corpora (0x03007ba4, 0x03007dbc,
// 0x03007dc4, 0x03007dc8) that were registered one build at a time before it
// was clear the set is bounded only by the call graph.
extern "C" const DispatchEntry gsr_func2cf4_pic_kDispatchTable[];
extern "C" const unsigned gsr_func2cf4_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func2cf4_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func2cf4_pic_kImageSize;
// Func_1dc8, the flash driver's relocatable working-area routine.
extern "C" const DispatchEntry gsr_func1dc8_pic_kDispatchTable[];
extern "C" const unsigned gsr_func1dc8_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_func1dc8_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_func1dc8_pic_kImageSize;
// Func_15afc and Func_15e10 share the 0x03003a84 staging slot. Both were
// registered fixed at that one base until a 10,800-frame campaign run copied
// Func_15afc to 0x030044f4 instead; see kRelocatableCodeImages below.
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
// (see kTransientCodeImages below), now also observed at 0x03002000. Routine
// and base vary independently (D-005); registered position-independent so a
// third base costs nothing. GS-011 follow-up.
extern "C" const DispatchEntry gsr_funca418_pic_kDispatchTable[];
extern "C" const unsigned gsr_funca418_pic_kDispatchTableLen;
extern "C" const std::uint32_t gsr_funca418_pic_kImageOrigin;
extern "C" const std::uint32_t gsr_funca418_pic_kImageSize;
// The spell canvas's six DMA3-copied stack blobs, each a single ARM
// STT_FUNC-sized routine copied by its own wrapper (Func_54e4, Func_5534,
// 0x08005490, 0x0800543C, 0x0800562C, 0x0800567C — all the same shape):
// "copy and fade" (ROM 0x08001ea8, 0x50 bytes), "copy and halve" (ROM
// 0x08001ef8, 0x40 bytes), Func_1f38 (0x80 bytes), Func_1fb8 (0x84 bytes),
// Func_203c (0x5c bytes) and Func_2098 (0x5c bytes). CORRECTED 2026-09-23:
// the Func_1ea8/Func_1ef8 images were previously sized 0x140/0x100 bytes,
// reading each wrapper's literal-pool word as a DMA3 word count; the
// wrapper code right-shifts that same word by 2 to get the word count, so
// it is a byte count already word-aligned. See
// config/usa/transient-func_1ea8-relocatable.toml and
// transient-func_1ef8-relocatable.toml for the corrected derivation, and
// transient-func_1f38-/1fb8-/2098-relocatable.toml for the three sibling
// wrappers found while re-checking this family.
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
// jumping into it. Directly confirmed at offset 0 in session
// 20260923_202001; corrects the earlier 16-byte interior-slice
// registration. See config/usa/transient-func_6c24-relocatable.toml.
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
// position-independent image (tools/build_stamp_variants.py).
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

namespace {

constexpr const char* kRomSha1 =
    "5c4695205413df7db52b9a184815a07783999971";

// One expanded view, not an aspect vocabulary: 360x240 is the native 240x160
// grown 50% in both axes, chosen 2026-09-05. The earlier 288x160 widescreen
// and the aspect cycle it belonged to are gone -- they predate the room
// buffer and were built on the per-pixel margin path that failed three times.
constexpr const char* kGoldenSunAspectLabels[] = {
    "Native 240x160",
    "Expanded View 360x240",
};
constexpr std::uint16_t kGoldenSunAspectWidths[] = {240u, 360u};
constexpr std::uint16_t kGoldenSunAspectHeights[] = {160u, 240u};

// These are the two object-source spans measured by the bounded writer
// census. Their endpoints are inclusive observed source addresses, not an
// inferred allocation extent.
constexpr std::uint32_t kGoldenSunObjSourceRegionAStart = 0x02033164u;
constexpr std::uint32_t kGoldenSunObjSourceRegionAEnd = 0x02033848u;
constexpr std::uint32_t kGoldenSunObjSourceRegionBStart = 0x020367c4u;
constexpr std::uint32_t kGoldenSunObjSourceRegionBEnd = 0x02036b84u;

const char* golden_sun_obj_source_region(std::uint32_t source) {
    if (source >= kGoldenSunObjSourceRegionAStart &&
        source <= kGoldenSunObjSourceRegionAEnd) return "region_a";
    if (source >= kGoldenSunObjSourceRegionBStart &&
        source <= kGoldenSunObjSourceRegionBEnd) return "region_b";
    return nullptr;
}

void fill_golden_sun_obj_boulder_registers(
    gsr::ObjBoulderTraceSample* sample);
void fill_golden_sun_obj_boulder_source_fields(
    gsr::ObjBoulderTraceSample* sample, std::uint32_t source,
    std::uint32_t pending_offset, std::uint32_t pending_value);
void record_golden_sun_obj_boulder_ec_entry(
    const gsr::ObjSourceCapture& observed);

std::uint32_t g_golden_sun_wide_extra_left = 0;
std::uint32_t g_golden_sun_wide_extra_right = 0;
std::uint32_t g_golden_sun_wide_extra_top = 0;
std::uint32_t g_golden_sun_wide_extra_bottom = 0;
// Battle presentation state, refreshed per rendered row by the margin policy
// below and read per pixel by the sample provider. The fallback keeps the
// game's native composition in the centre and extends enabled arena layers
// into horizontal margins inside their live scene band.
struct GoldenSunBattleBackdrop {
    bool active = false;
    int arena_centre = 0;        // middle of the band the arena is drawn in
    int band_top = 0;            // WIN0V top edge: start of the scene band
    int band_bottom = 0;         // WIN0V bottom edge: end of the scene band
    bool icons_visible = false;  // no window is hiding the command strip
    bool menus = false;          // the native menu layer is on
    // The measured BG2 geometry changes by battle state. Size code 1 has
    // captured 256x256 arena states; an effect overlay is kept out by the
    // signature below.
    bool affine_margin_allowed = false;
    // BG1 normally holds the arena backdrop, but the game also draws hit and
    // spell effects on it. Cleared while it is carrying one.
    bool backdrop_margin_allowed = true;
};
GoldenSunBattleBackdrop g_golden_sun_battle_backdrop_state;
bool g_golden_sun_mode0_field = false;
bool g_golden_sun_mode0_split_scroll = false;
gsr::widescreen::GoldenSunFieldAuthoredMap g_golden_sun_field_authored;
enum class GoldenSunFieldAuthScene : std::uint8_t { None, EqualScroll, SplitScroll };
GoldenSunFieldAuthScene g_golden_sun_field_auth_scene =
    GoldenSunFieldAuthScene::None;
bool g_golden_sun_expanded_obj_scene = false;
gsr::widescreen::GoldenSunMode0SplitScrollFrame
    g_golden_sun_mode0_split_scroll_frame;
// Terrain-only Mode0ScrollMismatch field (non-Palace split scroll): see
// gsr::widescreen::golden_sun_field_terrain_bg. Deliberately separate from
// g_golden_sun_mode0_field/g_golden_sun_mode0_split_scroll above so it
// cannot change BG0 suppression or the OBJ scene gate (g_golden_sun_
// expanded_obj_scene) that those two already-authenticated flags drive; it
// only widens the BG margin tilemap provider below, and only for the one
// field layer whose own raw scroll is trusted directly.
bool g_golden_sun_mode0_terrain_field = false;

// Declared here because the diagnostics-only B328 candidate table is defined
// before the staging-record helpers below.
struct GoldenSunObjStagingProvenance;
bool read_golden_sun_obj_staging_attrs(std::uint32_t address,
                                       std::uint16_t* attr0,
                                       std::uint16_t* attr1,
                                       std::uint16_t* attr2);
GoldenSunObjStagingProvenance* find_golden_sun_obj_staging(
    std::uint32_t staging_address);
bool golden_sun_func1dc8_writer_pc(std::uint32_t pc,
                                   gsr::Func1dc8WriterRoute route);
bool golden_sun_func1dc8_d4_store_pc(std::uint32_t pc);
void golden_sun_obj_staging_handoff(std::uint32_t entry_pc,
    std::uint32_t staging_address_override, std::uint32_t destination_override);
void remember_golden_sun_obj_b328_authorization(
    std::uint32_t record_base, std::uint16_t attr0, std::uint16_t attr1,
    std::uint16_t attr2);
void reset_golden_sun_func1dc8_writer_diagnostics();
// WIDE-01 body/shadow identity trace (see the full definition and comment
// near kGoldenSunObjCommitOrderLimit below); forward-declared so the EC/F0
// Func_1dc8 route observers above can call it.
enum class GoldenSunObjCommitOrderRoute : std::uint8_t { D4, EC, F0 };
void note_golden_sun_obj_commit_order(GoldenSunObjCommitOrderRoute route,
                                      std::uint32_t source, int slot,
                                      std::uint16_t attr0,
                                      std::uint16_t attr1,
                                      std::uint16_t attr2, bool x_valid,
                                      std::int16_t logical_x, bool y_valid,
                                      std::int16_t logical_y);
void record_golden_sun_obj_d4_event(
    const char* reason, int slot, std::uint32_t staging_address,
    std::uint32_t destination,
    const GoldenSunObjStagingProvenance* body_staging);
struct RelocatableCodeImage;
bool relocatable_resident_at(const RelocatableCodeImage& image,
                             std::uint32_t base, bool* prefix_passed);
unsigned int ram_code_page_epoch(std::uint32_t addr);
// Whole-image SHA-1 checks relocatable_resident_at ran (all callers) and the
// bytes they covered; the Func_1dc8 writer path's share separately. Printed
// with relocatable_cache_stats: the host profiler put SHA-1 at 11% of the
// heaviest Ragnarok frames (FACTS.md) without naming the caller.
unsigned long long g_relocatable_resident_hashes = 0;
unsigned long long g_relocatable_resident_hash_bytes = 0;
unsigned long long g_func1dc8_writer_hashes = 0;
std::uint64_t g_golden_sun_func1dc8_writer_recognized = 0;
std::uint64_t g_golden_sun_func1dc8_writer_identity_mismatch = 0;
std::uint64_t g_golden_sun_func1dc8_writer_unknown_variant = 0;
unsigned g_golden_sun_func1dc8_writer_logs = 0;
std::uint32_t g_golden_sun_func1dc8_last_base = 0;
std::uint64_t g_golden_sun_func1dc8_last_generation = 0;
struct GoldenSunObjPlacementProvenance {
    bool valid = false;
    bool x_valid = false;
    bool y_valid = false;
    std::int16_t logical_x = 0;
    std::int16_t logical_y = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    // Y-writer identity is kept separately from the X/Y freshness fields so
    // a later X placement cannot hide which branch supplied logical Y.
    std::uint32_t writer_branch_pc = 0;
    std::uint32_t target_address = 0;
    std::uint64_t writer_generation = 0;
    // ATTR0/1/2 copied from the exact staging record consumed by 030038D4.
    // This identity, rather than a short frame-age window, bounds the signed
    // coordinate's lifetime after the shadow->OAM DMA latch.
    bool oam_identity_valid = false;
    std::uint16_t expected_attr0 = 0;
    std::uint16_t expected_attr1 = 0;
    std::uint16_t expected_attr2 = 0;
    // Written by the F0 commit observer from the resolved placement, rather
    // than by D4 from the staging record. D4 must not overwrite one of these
    // within the same frame: it fires on a narrower gate and would replace a
    // shadow's own recovered position with its body's.
    bool commit_resolved = false;
};

// Diagnostic-only link between a B328 positive-Y candidate and the signed-Y
// cull routes that ran just before it. Keep register values only; never read
// or print guest payload bytes here.
struct GoldenSunObjSignedCullObservation {
    bool valid = false;
    std::uint64_t frame = UINT64_MAX;
    std::uint32_t pc = 0;
    std::int32_t operand = 0;
    std::uint32_t original_decision = 0;
    std::uint32_t final_decision = 0;
    bool overridden = false;
    std::uint32_t r7 = 0;
    std::uint32_t r10 = 0;
    std::uint32_t r11 = 0;
    std::uint32_t sp = 0;
    std::uint32_t lr = 0;
    std::uint32_t call_depth = 0;
    std::uint32_t call_return_pc = 0;
    std::uint64_t sequence = 0;
};

struct GoldenSunObjB27EParentRoute {
    bool valid = false;
    std::uint32_t pc = 0;
    std::uint32_t staging_address = 0;
    std::uint64_t frame = UINT64_MAX;
    std::int32_t operand = 0;
    std::uint32_t original_decision = 0;
    std::uint32_t final_decision = 0;
    bool overridden = false;
    std::uint32_t r7 = 0;
    std::uint32_t r10 = 0;
    std::uint32_t r11 = 0;
    std::uint32_t sp = 0;
    std::uint32_t lr = 0;
    std::uint32_t call_depth = 0;
    std::uint32_t call_return_pc = 0;
};

struct GoldenSunObjYCorrelation {
    bool valid = false;
    std::uint32_t b328_pc = 0;
    std::int32_t b328_operand = 0;
    std::uint32_t b328_r1 = 0;
    std::uint32_t b328_r3 = 0;
    std::uint32_t b328_r11 = 0;
    bool stack_inputs_valid = false;
    std::uint32_t b328_stack_sp_plus4 = 0;
    std::uint32_t b328_stack_sp_plus18 = 0;
    GoldenSunObjB27EParentRoute parent{};
    std::uint32_t b328_r6 = 0;
    std::uint32_t b328_r7 = 0;
    std::uint32_t b328_sp = 0;
    std::uint32_t b328_lr = 0;
    std::uint32_t b328_call_depth = 0;
    std::uint32_t b328_call_return_pc = 0;
    std::array<GoldenSunObjSignedCullObservation, 4> preceding{};
};

std::array<GoldenSunObjPlacementProvenance,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_pending_provenance{};
// The guest fills the IWRAM shadow before VBlank, then DMA copies that image
// to visible OAM.  Keep the two timelines separate: the renderer may only
// consume the image latched by the exact shadow->OAM handoff.
std::array<GoldenSunObjPlacementProvenance,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_visible_provenance{};

// Bumped on every change to either provenance store. The identity search in
// golden_sun_obj_provider_provenance memoises its result per OAM entry for a
// frame, and that memo is only exact while the stores hold still. The visible
// store is replaced wholesale at the shadow->OAM handoff, but the PENDING
// store is written per slot by the commit path DURING a frame -- so "once per
// frame" is not a safe memo key on its own, and a record committed after the
// first scanline would otherwise go unseen until the next frame. Pairing the
// frame with this counter makes the memo exact instead of merely usually
// right.
std::uint64_t g_golden_sun_obj_provenance_generation = 0;

// B324/B328 operate on the guest's transient 12-byte sprite records in
// IWRAM, not on the OAM shadow.  Keep the logical coordinates keyed by that
// record address until the copied 0300387C routine hands the record to its
// 030038D4 writer.  The writer entry has both identities: R6 is the staging
// record and R0 is the final OAM-shadow destination.
struct GoldenSunObjStagingProvenance {
    bool valid = false;
    bool x_valid = false;
    bool y_valid = false;
    std::uint32_t staging_address = 0;
    std::int16_t logical_x = 0;
    std::int16_t logical_y = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    std::uint32_t x_writer_branch_pc = 0;
    std::uint32_t y_writer_branch_pc = 0;
    GoldenSunObjYCorrelation y_correlation{};
};
std::array<GoldenSunObjStagingProvenance, 256>
    g_golden_sun_obj_staging_provenance{};

// The D4 entry hook runs immediately before its LDM and therefore sees the
// actual source pointer in R6. Keep that authenticated source across a
// same-frame resume to the D8 STM; never reconstruct it from OAM attributes.
struct GoldenSunObjD4SourceCapture {
    bool valid = false;
    bool blocked = false;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    std::uint32_t entry_pc = 0;
    std::uint32_t staging_address = 0;
    std::uint32_t target_address = 0;
};
std::array<GoldenSunObjD4SourceCapture,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_d4_source_captures{};

// A matched D4 source/store or exact F0 commit authenticates the actor record
// independently of its optional B27E parent. Two frames of OAM-slot capacity
// retain every possible previous-frame authorization without a guessed bound.
struct GoldenSunObjB328Authorization {
    bool valid = false;
    std::uint32_t record_base = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
};
std::array<GoldenSunObjB328Authorization,
           gsr::widescreen::kGoldenSunOamShadowSlotCount * 2u>
    g_golden_sun_obj_b328_authorizations{};

// F0's entry sees the source record before the generated writer advances R6;
// the later OAM write sees the destination slot. Keep that exact handoff for
// one frame so culling never has to infer a source from a reused OAM slot.
struct GoldenSunObjF0Context {
    bool valid = false;
    std::uint64_t frame = UINT64_MAX;
    std::uint32_t depth = 0;
    std::uint32_t return_pc = 0;
    std::uint32_t staging = 0;
    std::uint32_t record_base = 0;
    bool shadow = false;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
};
std::array<GoldenSunObjF0Context,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_f0_contexts{};
// Identity-invalid entries are kept separately for the recorder. They must
// never participate in the trusted context lookup, even when Enhanced
// Options and recording are enabled together.
std::array<GoldenSunObjF0Context,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_f0_rejected_contexts{};
std::uint64_t g_golden_sun_obj_f0_context_frame = UINT64_MAX;
// Recorder-only, one pending source per hardware slot. The exact destination
// address also distinguishes the primary and alternate shadow tables; a
// collision fails the join instead of borrowing another table's source.
std::array<gsr::ObjSourceCapture,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_f0_source_observations{};
std::uint64_t g_obj_lifetime_dma = 0;
std::array<GoldenSunObjB27EParentRoute, 256>
    g_golden_sun_obj_b27e_parent_routes{};
std::array<std::uint64_t,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_y_alias_last_frame{};
std::array<std::uint64_t,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_y_canonical_last_frame{};
unsigned g_golden_sun_obj_y_alias_logs_in_epoch = 0;
std::uint64_t g_golden_sun_obj_y_alias_logs_dropped = 0;
std::array<GoldenSunObjSignedCullObservation, 4>
    g_golden_sun_obj_signed_cull_observations{};
std::uint64_t g_golden_sun_obj_signed_cull_sequence = 0;
unsigned g_golden_sun_obj_y_correlation_logs_in_epoch = 0;
std::uint64_t g_golden_sun_obj_y_correlation_logs_dropped = 0;
std::array<unsigned, gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_y_correlation_slot_logs{};
struct GoldenSunObjYCorrelationKey {
    bool valid = false;
    std::uint32_t staging_address = 0;
    int slot = -1;
    std::int32_t b328_operand = 0;
    std::uint32_t b328_r1 = 0;
    std::uint32_t b328_r3 = 0;
    std::uint32_t b328_r11 = 0;
    std::uint32_t stack_sp_plus4 = 0;
    std::uint32_t stack_sp_plus18 = 0;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    std::array<std::uint64_t, 4> cull_sequences{};
};
std::array<GoldenSunObjYCorrelationKey, 64>
    g_golden_sun_obj_y_correlation_keys{};
// B328 can reject the final Y before a staging handoff exists. Keep a
// separate, payload-free decision trace for those calls so the B27E parent
// decision and the B328 decision can be compared using the transient staging
// record identity and the register/call context. This is diagnostics-only.
struct GoldenSunObjYCullDecisionKey {
    bool valid = false;
    std::uint32_t branch_pc = 0;
    std::uint32_t staging_address = 0;
    std::uint64_t frame = UINT64_MAX;
    std::int32_t operand = 0;
    std::uint32_t original_decision = 0;
    std::uint32_t final_decision = 0;
    std::uint32_t r1 = 0;
    std::uint32_t r3 = 0;
    std::uint32_t r11 = 0;
};
std::array<GoldenSunObjYCullDecisionKey, 64>
    g_golden_sun_obj_y_cull_decision_keys{};
unsigned g_golden_sun_obj_y_cull_decision_logs_in_frame = 0;
std::uint64_t g_golden_sun_obj_y_cull_decision_frame = UINT64_MAX;
std::uint64_t g_golden_sun_obj_y_cull_decision_logs_dropped = 0;
constexpr unsigned kGoldenSunObjB328ClassificationSampleLimit = 32u;

enum class GoldenSunObjB328ParentClassification : std::uint8_t {
    ExactCurrent = 0,
    SameStagingMismatch,
    NoParent,
    Count,
};

const char* golden_sun_obj_b328_parent_classification_name(
    GoldenSunObjB328ParentClassification classification) {
    switch (classification) {
        case GoldenSunObjB328ParentClassification::ExactCurrent:
            return "exact-current";
        case GoldenSunObjB328ParentClassification::SameStagingMismatch:
            return "same-staging-mismatch";
        case GoldenSunObjB328ParentClassification::NoParent:
            return "no-parent";
        case GoldenSunObjB328ParentClassification::Count:
            break;
    }
    return "unknown";
}

constexpr unsigned kB328ParentMismatchFrame = 1u << 0;
constexpr unsigned kB328ParentMismatchCallDepth = 1u << 1;
constexpr unsigned kB328ParentMismatchReturnPc = 1u << 2;

struct GoldenSunObjB328ClassificationSample {
    bool valid = false;
    GoldenSunObjB328ParentClassification classification =
        GoldenSunObjB328ParentClassification::NoParent;
    unsigned mismatch_flags = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint32_t staging_address = 0;
    std::int32_t operand = 0;
    std::uint64_t parent_frame = UINT64_MAX;
    std::uint32_t parent_call_depth = 0;
    std::uint32_t parent_return_pc = 0;
};

enum class GoldenSunObjB328WriterOutcome : std::uint8_t {
    Pending = 0,
    Consumed,
    Unrelated,
    IdentityUnproven,
    Expired,
    ContextMismatch,
    AttrMismatch,
    Count,
};

struct GoldenSunObjB328AcceptedCandidate {
    bool valid = false;
    bool handoff_seen = false;
    GoldenSunObjB328ParentClassification classification =
        GoldenSunObjB328ParentClassification::NoParent;
    std::uint32_t staging_address = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    std::int32_t operand = 0;
    std::uint32_t call_depth = 0;
    std::uint32_t call_return_pc = 0;
    bool attr_identity_valid = false;
    std::uint16_t expected_attr0 = 0;
    std::uint16_t expected_attr1 = 0;
    std::uint16_t expected_attr2 = 0;
    GoldenSunObjB328WriterOutcome f0_outcome =
        GoldenSunObjB328WriterOutcome::Pending;
    bool f0_writer_seen = false;
    std::uint64_t f0_writer_frame = UINT64_MAX;
    std::uint32_t f0_writer_depth = 0;
    std::uint32_t f0_writer_return_pc = 0;
    std::uint32_t f0_target_address = 0;
    int f0_slot = -1;
    std::uint16_t f0_attr0 = 0;
    std::uint16_t f0_attr1 = 0;
    std::uint16_t f0_attr2 = 0;
    std::uint32_t f0_register_mask = 0;
    bool f0_entry_seen = false;
    std::uint64_t f0_entry_frame = UINT64_MAX;
    std::uint32_t f0_entry_depth = 0;
    std::uint32_t f0_entry_return_pc = 0;
    std::uint32_t f0_entry_staging = 0;
};

enum class GoldenSunObjB328WriterRoute : std::uint8_t {
    D4 = 0,
    F0,
    Count,
};

const char* golden_sun_obj_b328_writer_route_name(
    GoldenSunObjB328WriterRoute route) {
    return route == GoldenSunObjB328WriterRoute::D4 ? "D4" : "F0";
}

const char* golden_sun_obj_b328_writer_outcome_name(
    GoldenSunObjB328WriterOutcome outcome) {
    switch (outcome) {
        case GoldenSunObjB328WriterOutcome::Pending: return "pending";
        case GoldenSunObjB328WriterOutcome::Consumed: return "consumed";
        case GoldenSunObjB328WriterOutcome::Unrelated: return "unrelated";
        case GoldenSunObjB328WriterOutcome::IdentityUnproven:
            return "identity-unproven";
        case GoldenSunObjB328WriterOutcome::Expired: return "expired";
        case GoldenSunObjB328WriterOutcome::ContextMismatch:
            return "context-mismatch";
        case GoldenSunObjB328WriterOutcome::AttrMismatch:
            return "attr-mismatch";
        case GoldenSunObjB328WriterOutcome::Count: break;
    }
    return "unknown";
}

struct GoldenSunObjB328RejectedRoute {
    GoldenSunObjB328WriterOutcome outcome =
        GoldenSunObjB328WriterOutcome::Pending;
    bool writer_seen = false;
    std::uint64_t writer_frame = UINT64_MAX;
    std::uint32_t writer_depth = 0;
    std::uint32_t writer_return_pc = 0;
    std::uint32_t target_address = 0;
    std::uint32_t writer_staging = 0;
    int slot = -1;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    std::uint32_t f0_register_mask = 0;
    bool f0_entry_seen = false;
    std::uint64_t f0_entry_frame = UINT64_MAX;
    std::uint32_t f0_entry_depth = 0;
    std::uint32_t f0_entry_return_pc = 0;
    std::uint32_t f0_entry_staging = 0;
};

struct GoldenSunObjB328RejectedCandidate {
    bool valid = false;
    GoldenSunObjB328ParentClassification classification =
        GoldenSunObjB328ParentClassification::NoParent;
    unsigned mismatch_flags = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    std::uint32_t staging_address = 0;
    std::int32_t operand = 0;
    std::uint32_t call_depth = 0;
    std::uint32_t call_return_pc = 0;
    bool attr_identity_valid = false;
    std::uint16_t expected_attr0 = 0;
    std::uint16_t expected_attr1 = 0;
    std::uint16_t expected_attr2 = 0;
    std::array<GoldenSunObjB328RejectedRoute,
               static_cast<std::size_t>(GoldenSunObjB328WriterRoute::Count)>
        routes{};
};

struct GoldenSunObjB328RejectedSample {
    bool valid = false;
    GoldenSunObjB328WriterRoute route = GoldenSunObjB328WriterRoute::D4;
    GoldenSunObjB328WriterOutcome outcome =
        GoldenSunObjB328WriterOutcome::Expired;
    GoldenSunObjB328ParentClassification classification =
        GoldenSunObjB328ParentClassification::NoParent;
    unsigned mismatch_flags = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t writer_frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    std::uint32_t staging_address = 0;
    std::uint32_t writer_staging = 0;
    std::int32_t operand = 0;
    std::uint32_t call_depth = 0;
    std::uint32_t call_return_pc = 0;
    std::uint32_t writer_depth = 0;
    std::uint32_t writer_return_pc = 0;
    std::uint32_t f0_register_mask = 0;
    bool f0_entry_seen = false;
    std::uint64_t f0_entry_frame = UINT64_MAX;
    std::uint32_t f0_entry_depth = 0;
    std::uint32_t f0_entry_return_pc = 0;
    std::uint32_t f0_entry_staging = 0;
    std::uint16_t expected_attr0 = 0;
    std::uint16_t expected_attr1 = 0;
    std::uint16_t expected_attr2 = 0;
    int slot = -1;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
};

struct GoldenSunObjB328AcceptedF0Sample {
    bool valid = false;
    GoldenSunObjB328WriterOutcome outcome =
        GoldenSunObjB328WriterOutcome::Expired;
    GoldenSunObjB328ParentClassification classification =
        GoldenSunObjB328ParentClassification::NoParent;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t writer_frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    std::uint32_t staging_address = 0;
    std::int32_t operand = 0;
    std::uint32_t call_depth = 0;
    std::uint32_t call_return_pc = 0;
    std::uint32_t writer_depth = 0;
    std::uint32_t writer_return_pc = 0;
    std::uint32_t target_address = 0;
    bool f0_entry_seen = false;
    std::uint64_t f0_entry_frame = UINT64_MAX;
    std::uint32_t f0_entry_depth = 0;
    std::uint32_t f0_entry_return_pc = 0;
    std::uint32_t f0_entry_staging = 0;
    std::uint32_t f0_register_mask = 0;
    std::uint16_t expected_attr0 = 0;
    std::uint16_t expected_attr1 = 0;
    std::uint16_t expected_attr2 = 0;
    int slot = -1;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
};

struct GoldenSunObjB328Diagnostics {
    std::array<std::uint64_t, static_cast<std::size_t>(
        GoldenSunObjB328ParentClassification::Count)> classifications{};
    std::uint64_t mismatch_frame = 0;
    std::uint64_t mismatch_call_depth = 0;
    std::uint64_t mismatch_return_pc = 0;
    std::array<GoldenSunObjB328ClassificationSample,
               kGoldenSunObjB328ClassificationSampleLimit> samples{};
    std::size_t sample_count = 0;
    std::uint64_t samples_dropped = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(
        GoldenSunObjB328ParentClassification::Count)> accepted{};
    std::array<std::uint64_t, static_cast<std::size_t>(
        GoldenSunObjB328ParentClassification::Count)> handoffs{};
    std::array<std::uint64_t, static_cast<std::size_t>(
        GoldenSunObjB328ParentClassification::Count)> no_handoff{};
    std::uint64_t accepted_tracking_dropped = 0;
    std::array<GoldenSunObjB328AcceptedCandidate, 256> candidates{};
    std::uint64_t rejected_total = 0;
    std::array<std::array<std::uint64_t,
                          static_cast<std::size_t>(
                              GoldenSunObjB328WriterOutcome::Count)>,
               static_cast<std::size_t>(GoldenSunObjB328WriterRoute::Count)>
        rejected_outcomes{};
    std::array<GoldenSunObjB328RejectedCandidate, 256> rejected{};
    std::uint64_t rejected_tracking_dropped = 0;
    std::array<GoldenSunObjB328RejectedSample, 32> rejected_samples{};
    std::size_t rejected_sample_count = 0;
    std::uint64_t rejected_samples_dropped = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(
        GoldenSunObjB328WriterOutcome::Count)> accepted_f0_outcomes{};
    std::uint64_t accepted_f0_tracking_dropped = 0;
    std::array<GoldenSunObjB328AcceptedF0Sample, 32> accepted_f0_samples{};
    std::size_t accepted_f0_sample_count = 0;
    std::uint64_t accepted_f0_samples_dropped = 0;

    void reset() { *this = {}; }
};
GoldenSunObjB328Diagnostics g_golden_sun_obj_b328_diagnostics{};

// A bounded handoff token for the legitimate-looking parentless B328 route.
// It is diagnostic-only: the branch decision remains unchanged.
constexpr std::size_t kGoldenSunObjB328ParentlessTokenLimit = 64u;
struct GoldenSunObjB328ParentlessToken {
    bool valid = false;
    std::uint64_t sequence = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint64_t auth_epoch = 0;
    std::uint32_t staging = 0;
    std::int32_t operand = 0;
    std::uint32_t r6 = 0;
    std::uint32_t r7 = 0;
    std::uint32_t call_depth = 0;
    std::uint32_t return_pc = 0;
    std::uint32_t entry_pc = 0x0800B328u;
    bool attr_valid = false;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    bool ec_seen = false;
    std::uint32_t ec_entry_pc = 0;
    std::uint32_t ec_r6 = 0;
    std::uint32_t ec_r7 = 0;
    std::uint32_t ec_source = 0;
    const char* ec_reason = "not-seen";
    bool f0_seen = false;
    std::uint32_t f0_entry_pc = 0;
    std::uint32_t f0_r6 = 0;
    std::uint32_t f0_r7 = 0;
    std::uint32_t f0_source = 0;
    std::uint32_t f0_target = 0;
    int f0_slot = -1;
    std::uint16_t f0_attr0 = 0;
    std::uint16_t f0_attr1 = 0;
    std::uint16_t f0_attr2 = 0;
    const char* f0_reason = "not-seen";
};
std::array<GoldenSunObjB328ParentlessToken,
           kGoldenSunObjB328ParentlessTokenLimit>
    g_golden_sun_obj_b328_parentless_tokens{};
std::uint64_t g_golden_sun_obj_b328_parentless_sequence = 0;
std::uint64_t g_golden_sun_obj_b328_parentless_dropped = 0;
struct GoldenSunObjYAcceptedState {
    bool valid = false;
    std::uint8_t raw_y = 0;
    std::int16_t logical_y = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint32_t writer_branch_pc = 0;
    std::uint32_t target_address = 0;
    std::uint64_t writer_generation = 0;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
};
std::array<GoldenSunObjYAcceptedState,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_y_accepted_state{};
std::array<gsr::widescreen::GoldenSunObjYEdgeAliasState,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_y_edge_alias_state{};
std::uint64_t g_golden_sun_obj_y_edge_alias_activations = 0;
unsigned g_golden_sun_obj_y_jump_logs_in_epoch = 0;
std::uint64_t g_golden_sun_obj_y_jump_logs_dropped = 0;
std::uint64_t g_golden_sun_field_auth_epoch = 0;
std::array<std::uint8_t, 0x20> g_golden_sun_wide_line_io{};
bool g_golden_sun_wide_line_io_valid = false;
std::uint16_t g_golden_sun_wide_line_dispcnt = 0;
std::vector<std::uint64_t> g_golden_sun_wide_scene_signatures;

constexpr std::size_t kGoldenSunFieldMapOffset = 0x10000u;
constexpr std::size_t kGoldenSunFieldMapBytes = 128u * 128u * 4u;
constexpr std::size_t kGoldenSunFieldRawOffset = 0x20000u;
constexpr std::size_t kGoldenSunFieldRawBytes = 4096u * 8u;
constexpr std::uint32_t kGoldenSunFieldMapAddress = 0x02010000u;
constexpr std::uint32_t kGoldenSunFieldRawAddress = 0x02020000u;
constexpr unsigned kGoldenSunWidePolicyTransitionLimit = 16u;
constexpr unsigned kGoldenSunWidePolicySampleLimit = 16u;
constexpr unsigned kGoldenSunFieldTableCpuLogLimitPerEpoch = 64u;
constexpr unsigned kGoldenSunFieldTableDmaLogLimitPerEpoch = 32u;
constexpr unsigned kGoldenSunObjYProvenanceLogLimitPerEpoch = 128u;
constexpr unsigned kGoldenSunObjYAliasLogLimitPerEpoch = 64u;
constexpr unsigned kGoldenSunObjYJumpLogLimitPerEpoch = 64u;
constexpr unsigned kGoldenSunObjYCorrelationLogLimitPerEpoch = 64u;
constexpr unsigned kGoldenSunObjYCorrelationPerSlotLimit = 4u;
constexpr unsigned kGoldenSunObjYCorrelationPerStagingLimit = 2u;
constexpr unsigned kGoldenSunObjYCullDecisionPerStagingLimit = 4u;
// The cull decision stream keeps the existing numeric 64-row bound, but resets its
// bounded dedupe window at each guest frame so a stationary object cannot
// hide a later transition.
constexpr unsigned kGoldenSunObjYCullDecisionLogLimitPerFrame =
    kGoldenSunObjYCorrelationLogLimitPerEpoch;
constexpr unsigned kGoldenSunFieldMapIdAttributionLimit = 32u;
constexpr unsigned kGoldenSunPalaceMarginSampleLimit = 96u;
// Payload-free production seam census. These counters distinguish a missing
// fast-IWRAM callback from a writer-identity or source-identity rejection in
// a normal Enhanced run without enabling the full recorder.
std::uint64_t g_golden_sun_obj_oam_observer_calls = 0;
std::uint64_t g_golden_sun_obj_f0_commits = 0;
std::uint64_t g_golden_sun_obj_f0_placement_calls = 0;
std::uint64_t g_golden_sun_obj_f0_identified = 0;
std::uint64_t g_golden_sun_obj_f0_considered = 0;
std::uint64_t g_golden_sun_obj_f0_placed = 0;
std::uint64_t g_golden_sun_obj_f0_placed_prev_frame = 0;

const char* golden_sun_field_margin_region(int hw_x, int screen_y,
                                           std::size_t* out_region);
bool golden_sun_wide_diagnostics_enabled();
bool golden_sun_experimental_fixes_enabled();
bool golden_sun_expanded_obj_view_active();

enum class GoldenSunPalaceMarginOutcome : std::uint8_t {
    AcceptedNativeSeed = 0,
    AcceptedConnected,
    RejectedFill,
    RejectedMask,
    RejectedRaw,
    RejectedLookup,
    Count,
};

const char* golden_sun_palace_margin_outcome_name(
    GoldenSunPalaceMarginOutcome outcome) {
    switch (outcome) {
        case GoldenSunPalaceMarginOutcome::AcceptedNativeSeed:
            return "accepted-native-seed";
        case GoldenSunPalaceMarginOutcome::AcceptedConnected:
            return "accepted-connected";
        case GoldenSunPalaceMarginOutcome::RejectedFill:
            return "rejected-fill";
        case GoldenSunPalaceMarginOutcome::RejectedMask:
            return "rejected-mask";
        case GoldenSunPalaceMarginOutcome::RejectedRaw:
            return "rejected-raw";
        case GoldenSunPalaceMarginOutcome::RejectedLookup:
            return "rejected-lookup";
        case GoldenSunPalaceMarginOutcome::Count:
            break;
    }
    return "unknown";
}

struct GoldenSunPalaceMarginSample {
    std::uint8_t bg = 0;
    std::uint8_t region = 0;
    GoldenSunPalaceMarginOutcome outcome =
        GoldenSunPalaceMarginOutcome::RejectedLookup;
    std::int16_t hw_x = 0;
    std::int16_t screen_y = 0;
    std::uint16_t map_x = 0;
    std::uint16_t map_y = 0;
    std::uint16_t map_id = 0;
    bool metadata_valid = false;
};

struct GoldenSunPalaceMarginDiagnostics {
    static constexpr std::size_t kBgCount = 3u;
    static constexpr std::size_t kRegionCount = 4u;
    static constexpr std::size_t kOutcomeCount = static_cast<std::size_t>(
        GoldenSunPalaceMarginOutcome::Count);

    std::array<std::array<std::array<std::uint64_t, kOutcomeCount>,
                          kRegionCount>, kBgCount>
        counts{};
    std::array<std::array<std::array<bool, kOutcomeCount>, kRegionCount>,
               kBgCount>
        sample_seen{};
    std::array<GoldenSunPalaceMarginSample,
               kGoldenSunPalaceMarginSampleLimit>
        samples{};
    std::size_t sample_count = 0;
    std::uint64_t samples_dropped = 0;

    void reset() {
        counts = {};
        sample_seen = {};
        sample_count = 0;
        samples_dropped = 0;
    }

    void record(int bg, int hw_x, int screen_y,
                GoldenSunPalaceMarginOutcome outcome,
                const gsr::widescreen::GoldenSunFieldTilemapMetadata& metadata,
                bool metadata_valid) {
        if (bg < 1 || bg > 3) return;
        std::size_t region = 0;
        golden_sun_field_margin_region(hw_x, screen_y, &region);
        const std::size_t bg_index = static_cast<std::size_t>(bg - 1);
        const std::size_t outcome_index = static_cast<std::size_t>(outcome);
        ++counts[bg_index][region][outcome_index];
        if (sample_seen[bg_index][region][outcome_index]) return;
        sample_seen[bg_index][region][outcome_index] = true;
        if (sample_count >= samples.size()) {
            ++samples_dropped;
            return;
        }
        auto& sample = samples[sample_count++];
        sample.bg = static_cast<std::uint8_t>(bg);
        sample.region = static_cast<std::uint8_t>(region);
        sample.outcome = outcome;
        sample.hw_x = static_cast<std::int16_t>(hw_x);
        sample.screen_y = static_cast<std::int16_t>(screen_y);
        sample.map_x = static_cast<std::uint16_t>(metadata.map_x);
        sample.map_y = static_cast<std::uint16_t>(metadata.map_y);
        sample.map_id = metadata.map_id;
        sample.metadata_valid = metadata_valid;
    }
};

enum class GoldenSunObjYOutcome : std::uint8_t {
    CoordinateRejected = 0,
    ProvenanceMissing,
    ProvenanceStaleFrame,
    ProvenanceStaleEpoch,
    ProvenanceYMismatch,
    AcceptedProvenance,
    OutputUnavailable,
    Count,
};

const char* golden_sun_obj_y_outcome_name(GoldenSunObjYOutcome outcome) {
    switch (outcome) {
        case GoldenSunObjYOutcome::CoordinateRejected: return "coordinate-rejected";
        case GoldenSunObjYOutcome::ProvenanceMissing: return "provenance-missing";
        case GoldenSunObjYOutcome::ProvenanceStaleFrame: return "provenance-stale-frame";
        case GoldenSunObjYOutcome::ProvenanceStaleEpoch: return "provenance-stale-epoch";
        case GoldenSunObjYOutcome::ProvenanceYMismatch: return "provenance-y-mismatch";
        case GoldenSunObjYOutcome::AcceptedProvenance: return "accepted-provenance";
        case GoldenSunObjYOutcome::OutputUnavailable: return "output-unavailable";
        case GoldenSunObjYOutcome::Count: break;
    }
    return "unknown";
}

struct GoldenSunObjYSlotDiagnostics {
    std::uint64_t calls = 0;
    std::uint64_t raw_160_191 = 0;
    std::uint64_t raw_192 = 0;
    std::uint64_t raw_193_199 = 0;
    std::uint64_t raw_200_255 = 0;
    std::array<std::uint64_t,
               static_cast<std::size_t>(GoldenSunObjYOutcome::Count)>
        outcomes{};
};
std::array<GoldenSunObjYSlotDiagnostics,
           gsr::widescreen::kGoldenSunOamShadowSlotCount>
    g_golden_sun_obj_y_slot_diagnostics{};

// Pixel-relevant provider seam diagnostics. This is deliberately separate
// from the older steady-state counters: those counters only answer how often
// a slot was queried and conflate several canonical fallbacks. Keep the
// aggregate uncapped, while samples are first/change-per-slot and bounded.
enum class GoldenSunObjYTransitionResolution : std::uint8_t {
    Signed = 0,
    Canonical,
    Count,
};

enum class GoldenSunObjYTransitionReason : std::uint8_t {
    NoProvenance = 0,
    StaleFrame,
    StaleEpoch,
    TargetMismatch,
    RawMismatch,
    Attr0Mismatch,
    Attr1Mismatch,
    Attr2Mismatch,
    Accepted,
    OutputUnavailable,
    Count,
};

enum class GoldenSunObjYTransitionRegion : std::uint8_t {
    Top = 0,
    Native,
    Bottom,
    Offscreen,
    Count,
};

// Keep the three evidence questions independent. A flood of canonical
// top-wrap records must not consume the budget needed for accepted bottom
// candidates or canonical<->signed transitions.
constexpr unsigned kGoldenSunObjYTransitionSampleLimit = 128u;
constexpr unsigned kGoldenSunObjYTransitionBucketCount = 3u;

enum class GoldenSunObjYTransitionBucket : std::uint8_t {
    ActiveCandidateCanonical = 0,
    SignedBottom,
    OutcomeTransition,
    Count,
};

struct GoldenSunObjYTransitionSample {
    bool valid = false;
    std::uint64_t frame = UINT64_MAX;
    int slot = -1;
    int raw_y = 0;
    int canonical_y = 0;
    int output_y = 0;
    GoldenSunObjYTransitionResolution resolution =
        GoldenSunObjYTransitionResolution::Canonical;
    GoldenSunObjYTransitionReason reason =
        GoldenSunObjYTransitionReason::NoProvenance;
    GoldenSunObjYTransitionRegion region =
        GoldenSunObjYTransitionRegion::Offscreen;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    std::uint16_t expected_attr0 = 0;
    std::uint16_t expected_attr1 = 0;
    std::uint16_t expected_attr2 = 0;
    std::uint32_t expected_target = 0;
    std::uint64_t provenance_frame = UINT64_MAX;
    std::uint64_t provenance_epoch = 0;
    std::uint32_t target_address = 0;
    std::uint32_t writer_pc = 0;
    std::uint64_t writer_generation = 0;
};

struct GoldenSunObjYTransitionIdentity {
    bool valid = false;
    int slot = -1;
    std::uint32_t target = 0;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    std::uint16_t expected_attr0 = 0;
    std::uint16_t expected_attr1 = 0;
    std::uint16_t expected_attr2 = 0;
};

struct GoldenSunObjYTransitionOutcomeState {
    bool valid = false;
    int slot = -1;
    std::uint32_t target = 0;
    GoldenSunObjYTransitionResolution resolution =
        GoldenSunObjYTransitionResolution::Canonical;
};

std::array<std::array<std::array<std::uint64_t,
                                 static_cast<std::size_t>(
                                     GoldenSunObjYTransitionResolution::Count)>,
                              static_cast<std::size_t>(
                                  GoldenSunObjYTransitionRegion::Count)>,
                     static_cast<std::size_t>(
                         GoldenSunObjYTransitionReason::Count)>
    g_golden_sun_obj_y_transition_counts{};
std::array<std::array<GoldenSunObjYTransitionSample,
                      kGoldenSunObjYTransitionSampleLimit>,
           kGoldenSunObjYTransitionBucketCount>
    g_golden_sun_obj_y_transition_samples{};
std::array<unsigned, kGoldenSunObjYTransitionBucketCount>
    g_golden_sun_obj_y_transition_sample_counts{};
std::array<std::uint64_t, kGoldenSunObjYTransitionBucketCount>
    g_golden_sun_obj_y_transition_samples_dropped{};
std::array<std::array<GoldenSunObjYTransitionIdentity,
                      kGoldenSunObjYTransitionSampleLimit>,
           2>
    g_golden_sun_obj_y_transition_seen{};
std::array<unsigned, 2> g_golden_sun_obj_y_transition_seen_counts{};
std::array<GoldenSunObjYTransitionOutcomeState, 256>
    g_golden_sun_obj_y_transition_outcome_states{};

const char* golden_sun_obj_y_transition_resolution_name(
    GoldenSunObjYTransitionResolution resolution) {
    return resolution == GoldenSunObjYTransitionResolution::Signed
        ? "signed" : "canonical";
}

const char* golden_sun_obj_y_transition_reason_name(
    GoldenSunObjYTransitionReason reason) {
    switch (reason) {
        case GoldenSunObjYTransitionReason::NoProvenance: return "no-provenance";
        case GoldenSunObjYTransitionReason::StaleFrame: return "stale-frame";
        case GoldenSunObjYTransitionReason::StaleEpoch: return "stale-epoch";
        case GoldenSunObjYTransitionReason::TargetMismatch: return "target-mismatch";
        case GoldenSunObjYTransitionReason::RawMismatch: return "raw-mismatch";
        case GoldenSunObjYTransitionReason::Attr0Mismatch: return "attr0-mismatch";
        case GoldenSunObjYTransitionReason::Attr1Mismatch: return "attr1-mismatch";
        case GoldenSunObjYTransitionReason::Attr2Mismatch: return "attr2-mismatch";
        case GoldenSunObjYTransitionReason::Accepted: return "accepted";
        case GoldenSunObjYTransitionReason::OutputUnavailable:
            return "output-unavailable";
        case GoldenSunObjYTransitionReason::Count: break;
    }
    return "unknown";
}

const char* golden_sun_obj_y_transition_region_name(
    GoldenSunObjYTransitionRegion region) {
    switch (region) {
        case GoldenSunObjYTransitionRegion::Top: return "top";
        case GoldenSunObjYTransitionRegion::Native: return "native";
        case GoldenSunObjYTransitionRegion::Bottom: return "bottom";
        case GoldenSunObjYTransitionRegion::Offscreen: return "offscreen";
        case GoldenSunObjYTransitionRegion::Count: break;
    }
    return "unknown";
}

const char* golden_sun_obj_y_transition_bucket_name(
    GoldenSunObjYTransitionBucket bucket) {
    switch (bucket) {
        case GoldenSunObjYTransitionBucket::ActiveCandidateCanonical:
            return "active-candidate-canonical";
        case GoldenSunObjYTransitionBucket::SignedBottom:
            return "signed-bottom";
        case GoldenSunObjYTransitionBucket::OutcomeTransition:
            return "outcome-transition";
        case GoldenSunObjYTransitionBucket::Count:
            break;
    }
    return "unknown";
}

GoldenSunObjYTransitionRegion golden_sun_obj_y_transition_region(int output_y) {
    // Expanded logical rows are -40..199. Keep rows outside that actual
    // output envelope distinct from top/bottom margin rows.
    if (output_y < -40 || output_y >= 200)
        return GoldenSunObjYTransitionRegion::Offscreen;
    if (output_y < 0) return GoldenSunObjYTransitionRegion::Top;
    if (output_y < 160) return GoldenSunObjYTransitionRegion::Native;
    return GoldenSunObjYTransitionRegion::Bottom;
}

int golden_sun_obj_oam_truncated(int logical, int bits);

GoldenSunObjYTransitionReason classify_golden_sun_obj_y_transition(
    const GoldenSunObjPlacementProvenance& provenance, std::uint64_t frame,
    std::uint64_t auth_epoch, std::uint32_t expected_target, int raw_y,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    if (!provenance.valid || !provenance.y_valid ||
        !provenance.oam_identity_valid)
        return GoldenSunObjYTransitionReason::NoProvenance;
    if (!gsr::widescreen::golden_sun_obj_provenance_frame_fresh(
            provenance.frame, frame))
        return GoldenSunObjYTransitionReason::StaleFrame;
    if (provenance.auth_epoch != auth_epoch)
        return GoldenSunObjYTransitionReason::StaleEpoch;
    if (provenance.target_address != expected_target)
        return GoldenSunObjYTransitionReason::TargetMismatch;
    if (golden_sun_obj_oam_truncated(provenance.logical_y, 8) != raw_y)
        return GoldenSunObjYTransitionReason::RawMismatch;
    if (provenance.expected_attr0 != attr0)
        return GoldenSunObjYTransitionReason::Attr0Mismatch;
    if (provenance.expected_attr1 != attr1)
        return GoldenSunObjYTransitionReason::Attr1Mismatch;
    if (provenance.expected_attr2 != attr2)
        return GoldenSunObjYTransitionReason::Attr2Mismatch;
    return GoldenSunObjYTransitionReason::Accepted;
}

bool golden_sun_obj_y_steady_state() {
    return golden_sun_wide_diagnostics_enabled() && g_ws_active &&
           g_golden_sun_expanded_obj_scene &&
           (g_golden_sun_mode0_field || g_golden_sun_mode0_split_scroll);
}

void record_golden_sun_obj_y_outcome(int slot, int raw_y,
                                     GoldenSunObjYOutcome outcome) {
    if (!golden_sun_obj_y_steady_state() || slot < 0 ||
        static_cast<std::size_t>(slot) >= g_golden_sun_obj_y_slot_diagnostics.size() ||
        raw_y < 160 || raw_y > 255) return;
    auto& stat = g_golden_sun_obj_y_slot_diagnostics[static_cast<std::size_t>(slot)];
    ++stat.calls;
    if (raw_y <= 191) ++stat.raw_160_191;
    else if (raw_y == 192) ++stat.raw_192;
    else if (raw_y <= 199) ++stat.raw_193_199;
    else ++stat.raw_200_255;
    ++stat.outcomes[static_cast<std::size_t>(outcome)];
}

void reset_golden_sun_obj_y_alias_diagnostics() {
    g_golden_sun_obj_y_alias_last_frame.fill(UINT64_MAX);
    g_golden_sun_obj_y_canonical_last_frame.fill(UINT64_MAX);
    g_golden_sun_obj_y_alias_logs_in_epoch = 0;
    g_golden_sun_obj_y_alias_logs_dropped = 0;
    g_golden_sun_obj_y_accepted_state = {};
    g_golden_sun_obj_y_edge_alias_state = {};
    g_golden_sun_obj_y_edge_alias_activations = 0;
    g_golden_sun_obj_y_jump_logs_in_epoch = 0;
    g_golden_sun_obj_y_jump_logs_dropped = 0;
    g_golden_sun_obj_signed_cull_observations = {};
    g_golden_sun_obj_signed_cull_sequence = 0;
    g_golden_sun_obj_y_correlation_logs_in_epoch = 0;
    g_golden_sun_obj_y_correlation_logs_dropped = 0;
    g_golden_sun_obj_y_correlation_slot_logs = {};
    g_golden_sun_obj_y_correlation_keys = {};
    g_golden_sun_obj_y_cull_decision_keys = {};
    g_golden_sun_obj_y_cull_decision_logs_in_frame = 0;
    g_golden_sun_obj_y_cull_decision_frame = UINT64_MAX;
    g_golden_sun_obj_y_cull_decision_logs_dropped = 0;
    g_golden_sun_obj_b328_diagnostics.reset();
    g_golden_sun_obj_b27e_parent_routes = {};
    g_golden_sun_obj_b328_parentless_tokens = {};
    g_golden_sun_obj_b328_parentless_sequence = 0;
    g_golden_sun_obj_b328_parentless_dropped = 0;
    if (golden_sun_wide_diagnostics_enabled()) {
        g_golden_sun_obj_y_transition_counts = {};
        g_golden_sun_obj_y_transition_samples = {};
        g_golden_sun_obj_y_transition_sample_counts = {};
        g_golden_sun_obj_y_transition_samples_dropped = {};
        g_golden_sun_obj_y_transition_seen = {};
        g_golden_sun_obj_y_transition_seen_counts = {};
        g_golden_sun_obj_y_transition_outcome_states = {};
    }
}

std::uint32_t golden_sun_obj_call_return_pc(std::uint32_t depth) {
    if (depth == 0u) return 0u;
    const std::uint32_t* stack = runtime_call_stack_data();
    return stack ? (stack[depth - 1u] & ~1u) : 0u;
}

GoldenSunObjB27EParentRoute* find_golden_sun_obj_b27e_parent(
    std::uint32_t staging_address) {
    for (auto& route : g_golden_sun_obj_b27e_parent_routes) {
        if (route.valid && route.staging_address == staging_address)
            return &route;
    }
    for (auto& route : g_golden_sun_obj_b27e_parent_routes) {
        if (!route.valid) {
            route.staging_address = staging_address;
            return &route;
        }
    }
    return nullptr;
}

void record_golden_sun_b27e_parent_route(std::uint32_t original_decision,
                                         bool overridden,
                                         std::uint32_t final_decision,
                                         std::int32_t operand) {
    if (!golden_sun_expanded_obj_view_active()) return;
    auto* route = find_golden_sun_obj_b27e_parent(g_cpu.R[7]);
    if (!route) return;
    route->valid = true;
    route->pc = 0x0800B27Eu;
    route->frame = runtime_current_frame();
    route->operand = operand;
    route->original_decision = original_decision;
    route->final_decision = final_decision;
    route->overridden = overridden;
    route->r7 = g_cpu.R[7];
    route->r10 = g_cpu.R[10];
    route->r11 = g_cpu.R[11];
    route->sp = g_cpu.R[13];
    route->lr = g_cpu.R[14];
    route->call_depth = runtime_call_stack_depth();
    route->call_return_pc = golden_sun_obj_call_return_pc(route->call_depth);
}

const GoldenSunObjB27EParentRoute* find_golden_sun_current_b27e_parent(
    std::uint32_t staging_address, std::uint64_t frame,
    std::uint32_t call_depth, std::uint32_t call_return_pc) {
    for (const auto& route : g_golden_sun_obj_b27e_parent_routes) {
        if (route.valid && route.staging_address == staging_address &&
            route.frame == frame && route.call_depth == call_depth &&
            route.call_return_pc == call_return_pc) {
            return &route;
        }
    }
    return nullptr;
}

const GoldenSunObjB27EParentRoute* find_golden_sun_any_b27e_parent(
    std::uint32_t staging_address) {
    for (const auto& route : g_golden_sun_obj_b27e_parent_routes) {
        if (route.valid && route.staging_address == staging_address)
            return &route;
    }
    return nullptr;
}

struct GoldenSunObjB328ClassificationResult {
    GoldenSunObjB328ParentClassification classification =
        GoldenSunObjB328ParentClassification::NoParent;
    unsigned mismatch_flags = 0;
    const GoldenSunObjB27EParentRoute* parent = nullptr;
};

GoldenSunObjB328ClassificationResult classify_golden_sun_b328_parent(
    std::uint32_t staging_address, std::uint64_t frame,
    std::uint32_t call_depth, std::uint32_t call_return_pc) {
    GoldenSunObjB328ClassificationResult result{};
    result.parent = find_golden_sun_any_b27e_parent(staging_address);
    if (!result.parent) return result;
    if (result.parent->frame != frame)
        result.mismatch_flags |= kB328ParentMismatchFrame;
    if (result.parent->call_depth != call_depth)
        result.mismatch_flags |= kB328ParentMismatchCallDepth;
    if (result.parent->call_return_pc != call_return_pc)
        result.mismatch_flags |= kB328ParentMismatchReturnPc;
    result.classification = result.mismatch_flags == 0u
        ? GoldenSunObjB328ParentClassification::ExactCurrent
        : GoldenSunObjB328ParentClassification::SameStagingMismatch;
    return result;
}

void record_golden_sun_b328_classification(
    std::int32_t operand, const GoldenSunObjB328ClassificationResult& result,
    std::uint32_t staging_address, std::uint64_t frame,
    std::uint32_t call_depth, std::uint32_t call_return_pc) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active() || operand < 160 ||
        operand > 199) return;
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    const auto class_index = static_cast<std::size_t>(result.classification);
    ++stats.classifications[class_index];
    if (result.mismatch_flags & kB328ParentMismatchFrame)
        ++stats.mismatch_frame;
    if (result.mismatch_flags & kB328ParentMismatchCallDepth)
        ++stats.mismatch_call_depth;
    if (result.mismatch_flags & kB328ParentMismatchReturnPc)
        ++stats.mismatch_return_pc;

    for (std::size_t i = 0; i < stats.sample_count; ++i) {
        const auto& sample = stats.samples[i];
        if (sample.classification == result.classification &&
            sample.mismatch_flags == result.mismatch_flags &&
            sample.staging_address == staging_address &&
            sample.frame == frame && sample.operand == operand) return;
    }
    if (stats.sample_count >= stats.samples.size()) {
        ++stats.samples_dropped;
        return;
    }
    auto& sample = stats.samples[stats.sample_count++];
    sample.valid = true;
    sample.classification = result.classification;
    sample.mismatch_flags = result.mismatch_flags;
    sample.frame = frame;
    sample.staging_address = staging_address;
    sample.operand = operand;
    if (result.parent) {
        sample.parent_frame = result.parent->frame;
        sample.parent_call_depth = result.parent->call_depth;
        sample.parent_return_pc = result.parent->call_return_pc;
    }
    (void)call_depth;
    (void)call_return_pc;
}

void record_golden_sun_b328_accepted_candidate(
    std::int32_t operand,
    GoldenSunObjB328ParentClassification classification,
    std::uint32_t staging_address, std::uint64_t frame,
    std::uint32_t call_depth, std::uint32_t call_return_pc) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active() || operand < 160 ||
        operand > 199) return;
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    const auto class_index = static_cast<std::size_t>(classification);
    ++stats.accepted[class_index];
    for (auto& candidate : stats.candidates) {
        if (!candidate.valid) {
            candidate.valid = true;
            candidate.handoff_seen = false;
            candidate.classification = classification;
            candidate.staging_address = staging_address;
            candidate.frame = frame;
            candidate.auth_epoch = g_golden_sun_field_auth_epoch;
            candidate.operand = operand;
            candidate.call_depth = call_depth;
            candidate.call_return_pc = call_return_pc;
            candidate.attr_identity_valid = read_golden_sun_obj_staging_attrs(
                staging_address, &candidate.expected_attr0,
                &candidate.expected_attr1, &candidate.expected_attr2);
            return;
        }
    }
    ++stats.accepted_tracking_dropped;
    ++stats.accepted_f0_tracking_dropped;
}

void record_golden_sun_b328_parentless_token(
    std::uint64_t frame, std::uint32_t staging, std::int32_t operand,
    std::uint32_t depth, std::uint32_t return_pc) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active()) return;
    for (const auto& token : g_golden_sun_obj_b328_parentless_tokens) {
        if (token.valid && token.frame == frame && token.auth_epoch ==
            g_golden_sun_field_auth_epoch && token.staging == staging &&
            token.operand == operand) return;
    }
    GoldenSunObjB328ParentlessToken* token = nullptr;
    for (auto& candidate : g_golden_sun_obj_b328_parentless_tokens) {
        if (!candidate.valid) {
            token = &candidate;
            break;
        }
    }
    if (!token) {
        ++g_golden_sun_obj_b328_parentless_dropped;
        return;
    }
    *token = {};
    token->valid = true;
    token->sequence = ++g_golden_sun_obj_b328_parentless_sequence;
    token->frame = frame;
    token->auth_epoch = g_golden_sun_field_auth_epoch;
    token->staging = staging;
    token->operand = operand;
    token->r6 = g_cpu.R[6];
    token->r7 = g_cpu.R[7];
    token->call_depth = depth;
    token->return_pc = return_pc;
    token->attr_valid = read_golden_sun_obj_staging_attrs(
        staging, &token->attr0, &token->attr1, &token->attr2);
}

void link_golden_sun_b328_parentless_ec(
    std::uint64_t frame, std::uint32_t staging, std::uint32_t depth,
    std::uint32_t return_pc, std::uint32_t entry_pc) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    GoldenSunObjB328ParentlessToken* found = nullptr;
    for (auto& token : g_golden_sun_obj_b328_parentless_tokens) {
        if (!token.valid || token.frame != frame || token.auth_epoch !=
            g_golden_sun_field_auth_epoch || token.staging != staging) continue;
        if (found) {
            found->ec_reason = "ambiguous-token";
            return;
        }
        found = &token;
    }
    if (!found) return;
    found->ec_entry_pc = entry_pc;
    found->ec_r6 = g_cpu.R[6];
    found->ec_r7 = g_cpu.R[7];
    found->ec_source = staging;
    if (found->call_depth != depth || found->return_pc != return_pc) {
        found->ec_reason = "context-mismatch";
        return;
    }
    found->ec_seen = true;
    found->ec_reason = "exact-token";
}

void link_golden_sun_b328_parentless_f0(
    std::uint64_t frame, std::uint32_t depth, std::uint32_t return_pc,
    std::uint32_t entry_pc, std::uint32_t target, int slot,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    GoldenSunObjB328ParentlessToken* found = nullptr;
    GoldenSunObjB328ParentlessToken* context_match = nullptr;
    bool context_ambiguous = false;
    for (auto& token : g_golden_sun_obj_b328_parentless_tokens) {
        if (!token.valid || token.frame != frame || token.auth_epoch !=
            g_golden_sun_field_auth_epoch || token.call_depth != depth ||
            token.return_pc != return_pc) continue;
        if (context_match) {
            context_ambiguous = true;
        } else {
            context_match = &token;
        }
        if (!token.attr_valid || token.attr0 != attr0 ||
            token.attr1 != attr1 || token.attr2 != attr2) continue;
        if (found) {
            found->f0_reason = "ambiguous-token";
            return;
        }
        found = &token;
    }
    if (context_ambiguous) {
        if (found) found->f0_reason = "ambiguous-token";
        return;
    }
    if (!found) {
        if (context_match && context_match->ec_seen)
            context_match->f0_reason = "attr-mismatch";
        return;
    }
    found->f0_seen = true;
    found->f0_entry_pc = entry_pc;
    found->f0_r6 = g_cpu.R[6];
    found->f0_r7 = g_cpu.R[7];
    found->f0_source = found->staging;
    found->f0_target = target;
    found->f0_slot = slot;
    found->f0_attr0 = attr0;
    found->f0_attr1 = attr1;
    found->f0_attr2 = attr2;
    found->f0_reason = "exact-token";
}

void record_golden_sun_b328_rejected_candidate(
    std::int32_t operand,
    GoldenSunObjB328ParentClassification classification,
    unsigned mismatch_flags, std::uint32_t staging_address,
    std::uint64_t frame, std::uint32_t call_depth,
    std::uint32_t call_return_pc) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active() || operand < 160 ||
        operand > 199) return;
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    ++stats.rejected_total;
    for (auto& candidate : stats.rejected) {
        if (!candidate.valid) {
            candidate.valid = true;
            candidate.classification = classification;
            candidate.mismatch_flags = mismatch_flags;
            candidate.frame = frame;
            candidate.auth_epoch = g_golden_sun_field_auth_epoch;
            candidate.staging_address = staging_address;
            candidate.operand = operand;
            candidate.call_depth = call_depth;
            candidate.call_return_pc = call_return_pc;
            candidate.attr_identity_valid = read_golden_sun_obj_staging_attrs(
                staging_address, &candidate.expected_attr0,
                &candidate.expected_attr1, &candidate.expected_attr2);
            return;
        }
    }
    // The aggregate remains uncapped; only the short-lived correlation table
    // can overflow.
    ++stats.rejected_tracking_dropped;
}

void record_golden_sun_b328_rejected_sample(
    const GoldenSunObjB328RejectedCandidate& candidate,
    GoldenSunObjB328WriterRoute route,
    const GoldenSunObjB328RejectedRoute& result) {
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    if (stats.rejected_sample_count >= stats.rejected_samples.size()) {
        ++stats.rejected_samples_dropped;
        return;
    }
    auto& sample = stats.rejected_samples[stats.rejected_sample_count++];
    sample.valid = true;
    sample.route = route;
    sample.outcome = result.outcome;
    sample.classification = candidate.classification;
    sample.mismatch_flags = candidate.mismatch_flags;
    sample.frame = candidate.frame;
    sample.writer_frame = result.writer_frame;
    sample.auth_epoch = candidate.auth_epoch;
    sample.staging_address = candidate.staging_address;
    sample.writer_staging = result.writer_staging;
    sample.operand = candidate.operand;
    sample.call_depth = candidate.call_depth;
    sample.call_return_pc = candidate.call_return_pc;
    sample.writer_depth = result.writer_depth;
    sample.writer_return_pc = result.writer_return_pc;
    sample.f0_register_mask = result.f0_register_mask;
    sample.f0_entry_seen = result.f0_entry_seen;
    sample.f0_entry_frame = result.f0_entry_frame;
    sample.f0_entry_depth = result.f0_entry_depth;
    sample.f0_entry_return_pc = result.f0_entry_return_pc;
    sample.f0_entry_staging = result.f0_entry_staging;
    sample.expected_attr0 = candidate.expected_attr0;
    sample.expected_attr1 = candidate.expected_attr1;
    sample.expected_attr2 = candidate.expected_attr2;
    sample.slot = result.slot;
    sample.attr0 = result.attr0;
    sample.attr1 = result.attr1;
    sample.attr2 = result.attr2;
}

void record_golden_sun_b328_rejected_writer(
    GoldenSunObjB328WriterRoute route, std::uint32_t writer_staging,
    int slot, std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2,
    std::uint64_t writer_frame, std::uint32_t writer_depth,
    std::uint32_t writer_return_pc, std::uint32_t f0_register_mask) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active()) return;
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    const std::size_t route_index = static_cast<std::size_t>(route);
    for (auto& candidate : stats.rejected) {
        if (!candidate.valid || candidate.auth_epoch !=
            g_golden_sun_field_auth_epoch) continue;
        auto& result = candidate.routes[route_index];
        if (result.outcome != GoldenSunObjB328WriterOutcome::Pending)
            continue;
        const bool same_frame = candidate.frame == writer_frame;
        const bool same_context = same_frame &&
            candidate.call_depth == writer_depth &&
            candidate.call_return_pc == writer_return_pc;
        std::uint32_t observed_f0_register_mask = f0_register_mask;
        if (route == GoldenSunObjB328WriterRoute::F0) {
            observed_f0_register_mask = 0u;
            for (unsigned reg = 0; reg < 16u; ++reg) {
                if (g_cpu.R[reg] == candidate.staging_address)
                    observed_f0_register_mask |= 1u << reg;
            }
        }
        const bool same_staging = route == GoldenSunObjB328WriterRoute::D4 &&
            candidate.staging_address == writer_staging;
        const bool f0_entry_identity = route == GoldenSunObjB328WriterRoute::F0 &&
            result.f0_entry_seen &&
            result.f0_entry_staging == candidate.staging_address;
        const bool f0_entry_context = f0_entry_identity &&
            result.f0_entry_frame == candidate.frame &&
            result.f0_entry_depth == candidate.call_depth &&
            result.f0_entry_return_pc == candidate.call_return_pc &&
            same_context;
        result.writer_seen = true;
        result.writer_frame = writer_frame;
        result.writer_depth = writer_depth;
        result.writer_return_pc = writer_return_pc;
        result.writer_staging = route == GoldenSunObjB328WriterRoute::F0 &&
                result.f0_entry_seen ? result.f0_entry_staging : writer_staging;
        result.slot = slot;
        result.attr0 = attr0;
        result.attr1 = attr1;
        result.attr2 = attr2;
        result.f0_register_mask = observed_f0_register_mask;
        if (route == GoldenSunObjB328WriterRoute::D4 && same_staging &&
            same_context) {
            result.outcome = GoldenSunObjB328WriterOutcome::Consumed;
        } else if (route == GoldenSunObjB328WriterRoute::F0 &&
                   f0_entry_context) {
            const bool attrs_match = candidate.attr_identity_valid &&
                candidate.expected_attr0 == attr0 &&
                candidate.expected_attr1 == attr1 &&
                candidate.expected_attr2 == attr2;
            result.outcome = attrs_match
                ? GoldenSunObjB328WriterOutcome::Consumed
                : GoldenSunObjB328WriterOutcome::AttrMismatch;
        } else if (route == GoldenSunObjB328WriterRoute::F0 &&
                   f0_entry_identity) {
            result.outcome = GoldenSunObjB328WriterOutcome::ContextMismatch;
        } else if (route == GoldenSunObjB328WriterRoute::F0 && same_frame) {
            // A writer with no matching 030038EC entry is retained as
            // observational evidence only.
            result.outcome = GoldenSunObjB328WriterOutcome::IdentityUnproven;
        } else if (same_frame || same_staging) {
            result.outcome = GoldenSunObjB328WriterOutcome::Unrelated;
        } else {
            continue;
        }
        ++stats.rejected_outcomes[route_index][static_cast<std::size_t>(
            result.outcome)];
        record_golden_sun_b328_rejected_sample(candidate, route, result);
    }
}

void record_golden_sun_b328_accepted_f0_sample(
    const GoldenSunObjB328AcceptedCandidate& candidate) {
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    if (stats.accepted_f0_sample_count >= stats.accepted_f0_samples.size()) {
        ++stats.accepted_f0_samples_dropped;
        return;
    }
    auto& sample = stats.accepted_f0_samples[
        stats.accepted_f0_sample_count++];
    sample.valid = true;
    sample.outcome = candidate.f0_outcome;
    sample.classification = candidate.classification;
    sample.frame = candidate.frame;
    sample.writer_frame = candidate.f0_writer_frame;
    sample.auth_epoch = candidate.auth_epoch;
    sample.staging_address = candidate.staging_address;
    sample.operand = candidate.operand;
    sample.call_depth = candidate.call_depth;
    sample.call_return_pc = candidate.call_return_pc;
    sample.writer_depth = candidate.f0_writer_depth;
    sample.writer_return_pc = candidate.f0_writer_return_pc;
    sample.target_address = candidate.f0_target_address;
    sample.f0_entry_seen = candidate.f0_entry_seen;
    sample.f0_entry_frame = candidate.f0_entry_frame;
    sample.f0_entry_depth = candidate.f0_entry_depth;
    sample.f0_entry_return_pc = candidate.f0_entry_return_pc;
    sample.f0_entry_staging = candidate.f0_entry_staging;
    sample.f0_register_mask = candidate.f0_register_mask;
    sample.expected_attr0 = candidate.expected_attr0;
    sample.expected_attr1 = candidate.expected_attr1;
    sample.expected_attr2 = candidate.expected_attr2;
    sample.slot = candidate.f0_slot;
    sample.attr0 = candidate.f0_attr0;
    sample.attr1 = candidate.f0_attr1;
    sample.attr2 = candidate.f0_attr2;
}

void record_golden_sun_b328_accepted_f0_writer(
    std::uint32_t target_address, int slot, std::uint16_t attr0,
    std::uint16_t attr1,
    std::uint16_t attr2, std::uint64_t writer_frame,
    std::uint32_t writer_depth, std::uint32_t writer_return_pc) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active()) return;
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    for (auto& candidate : stats.candidates) {
        if (!candidate.valid || candidate.auth_epoch !=
            g_golden_sun_field_auth_epoch || candidate.f0_outcome !=
            GoldenSunObjB328WriterOutcome::Pending) continue;
        const bool same_frame = candidate.frame == writer_frame;
        const bool same_context = same_frame &&
            candidate.call_depth == writer_depth &&
            candidate.call_return_pc == writer_return_pc;
        std::uint32_t register_mask = 0u;
        for (unsigned reg = 0; reg < 16u; ++reg) {
            if (g_cpu.R[reg] == candidate.staging_address)
                register_mask |= 1u << reg;
        }
        const bool entry_identity = candidate.f0_entry_seen &&
            candidate.f0_entry_staging == candidate.staging_address;
        const bool entry_context = entry_identity &&
            candidate.f0_entry_frame == candidate.frame &&
            candidate.f0_entry_depth == candidate.call_depth &&
            candidate.f0_entry_return_pc == candidate.call_return_pc &&
            same_context;
        candidate.f0_writer_seen = true;
        candidate.f0_writer_frame = writer_frame;
        candidate.f0_writer_depth = writer_depth;
        candidate.f0_writer_return_pc = writer_return_pc;
        candidate.f0_target_address = target_address;
        candidate.f0_slot = slot;
        candidate.f0_attr0 = attr0;
        candidate.f0_attr1 = attr1;
        candidate.f0_attr2 = attr2;
        candidate.f0_register_mask = register_mask;
        if (entry_context) {
            const bool attrs_match = candidate.attr_identity_valid &&
                candidate.expected_attr0 == attr0 &&
                candidate.expected_attr1 == attr1 &&
                candidate.expected_attr2 == attr2;
            candidate.f0_outcome = attrs_match
                ? GoldenSunObjB328WriterOutcome::Consumed
                : GoldenSunObjB328WriterOutcome::AttrMismatch;
        } else if (entry_identity) {
            candidate.f0_outcome =
                GoldenSunObjB328WriterOutcome::ContextMismatch;
        } else if (same_frame) {
            candidate.f0_outcome = GoldenSunObjB328WriterOutcome::Unrelated;
        } else {
            candidate.f0_writer_seen = false;
            continue;
        }
        ++stats.accepted_f0_outcomes[static_cast<std::size_t>(
            candidate.f0_outcome)];
        record_golden_sun_b328_accepted_f0_sample(candidate);
    }
}

void record_golden_sun_shadow_cpu_writes(
    std::uint32_t writer_pc, std::uint32_t address, std::uint32_t size) {
    if (!gsr::obj_recorder_enabled() || size == 0u) return;
    const std::uint64_t write_first = address;
    const std::uint64_t write_last = write_first + size;
    const std::uint32_t tables[] = {
        gsr::widescreen::kGoldenSunOamShadowStart,
        gsr::widescreen::kGoldenSunOamShadowAltStart};
    for (const std::uint32_t table : tables) {
        const std::uint64_t table_first = table;
        const std::uint64_t table_last = table_first +
            gsr::widescreen::kGoldenSunOamBytes;
        if (write_first >= table_last || write_last <= table_first) continue;
        const std::uint64_t overlap_first =
            std::max(write_first, table_first);
        const std::uint64_t overlap_last =
            std::min(write_last, table_last);
        const int first_slot = static_cast<int>(
            (overlap_first - table_first) /
            gsr::widescreen::kGoldenSunOamShadowSlotBytes);
        const int last_slot = static_cast<int>(
            (overlap_last - 1u - table_first) /
            gsr::widescreen::kGoldenSunOamShadowSlotBytes);
        for (int slot = first_slot; slot <= last_slot; ++slot) {
            const std::uint32_t slot_address = table +
                static_cast<std::uint32_t>(slot) *
                    gsr::widescreen::kGoldenSunOamShadowSlotBytes;
            const std::uint16_t attr0 = bus_read_u16(slot_address);
            const std::uint16_t attr1 = bus_read_u16(slot_address + 2u);
            const std::uint16_t attr2 = bus_read_u16(slot_address + 4u);
            const auto touched = [&](std::uint32_t offset) {
                const std::uint64_t attr_first = slot_address + offset;
                const std::uint64_t attr_last = attr_first + 2u;
                return write_first < attr_last && write_last > attr_first;
            };
            gsr::ObjShadowWriteSample sample;
            sample.event = "cpu";
            sample.frame = runtime_current_frame();
            sample.epoch = g_golden_sun_field_auth_epoch;
            sample.dma = g_obj_lifetime_dma;
            sample.slot = slot;
            sample.writer_pc = writer_pc;
            sample.address = address;
            sample.size = size;
            sample.attr_mask = static_cast<std::uint8_t>(
                (touched(0u) ? 1u : 0u) | (touched(2u) ? 2u : 0u) |
                (touched(4u) ? 4u : 0u));
            sample.attr0 = attr0;
            sample.attr1 = attr1;
            sample.attr2 = attr2;
            const std::uint32_t depth = runtime_call_stack_depth();
            sample.return_pc = golden_sun_obj_call_return_pc(depth);
            sample.caller_return_pc =
                depth > 1u ? golden_sun_obj_call_return_pc(depth - 1u) : 0u;
            gsr::obj_recorder_note_shadow_write(sample);
        }
    }
}

bool golden_sun_obj_record_identity(std::uint32_t staging,
                                             std::uint32_t* record_base,
                                             bool* shadow) {
    // The measured actor-record array: base 0x03002000, stride 0x38, body
    // coordinates at +0x00 and the paired shadow at +0x0C (FACTS.md,
    // "Sprites, NPCs and shadows").
    //
    // This predicate used to stop at a measured 0x030022E0 and then admit one
    // hand-authenticated extra body, 0x03002348 (N=15). That end was never a
    // property of the guest -- it was how far the producer audit had reached
    // -- so every actor past the audited prefix was refused a trusted
    // position and fell back to the native rectangle, which is what clips a
    // sprite standing in the expanded top margin. Authenticating one actor at
    // a time does not scale to the whole cast, so what is checked here is the
    // shape of the array, and the bound is hardware rather than an audit
    // boundary.
    //
    // Admission is deliberately not proof. The caller
    // (golden_sun_obj_resolve_placement) still requires the record's own
    // staged ATTR0/1/2 to equal the committed OAM attributes, its staged
    // coordinates to truncate to the committed OAM bytes, and its staging
    // entry to carry the same frame and the same auth epoch. A pointer that
    // is merely stride-aligned cannot pass those gates.
    constexpr std::uint32_t kRecordBase = 0x03002000u;
    constexpr std::uint32_t kRecordStride = 0x38u;
    // Hardware bound only: the whole record must lie inside IWRAM so the
    // caller's record reads stay in range.
    constexpr std::uint32_t kIwramEnd = 0x03008000u;
    if (!record_base || !shadow || staging < kRecordBase) return false;
    const std::uint32_t offset = staging - kRecordBase;
    const std::uint32_t sub_offset = offset % kRecordStride;
    if (sub_offset != 0u && sub_offset != 0x0Cu) return false;
    const std::uint32_t base = staging - sub_offset;
    if (base > kIwramEnd - kRecordStride) return false;
    *record_base = base;
    *shadow = sub_offset == 0x0Cu;
    return true;
}

void reset_golden_sun_obj_f0_contexts_for_frame(std::uint64_t frame) {
    if (g_golden_sun_obj_f0_context_frame == frame) return;
    g_golden_sun_obj_f0_contexts = {};
    g_golden_sun_obj_f0_rejected_contexts = {};
    g_golden_sun_obj_f0_context_frame = frame;
}

void remember_golden_sun_obj_f0_context_in(
    std::array<GoldenSunObjF0Context,
               gsr::widescreen::kGoldenSunOamShadowSlotCount>& contexts,
    std::uint64_t frame, std::uint32_t depth, std::uint32_t return_pc,
    std::uint32_t staging, std::uint32_t record_base, bool shadow,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    GoldenSunObjF0Context* context = nullptr;
    for (auto& candidate : contexts) {
        if (candidate.valid && candidate.staging == staging &&
            candidate.depth == depth && candidate.return_pc == return_pc) {
            context = &candidate;
            break;
        }
    }
    if (!context) {
        for (auto& candidate : contexts) {
            if (!candidate.valid) {
                context = &candidate;
                break;
            }
        }
    }
    // More than one source in a frame is possible. If the bounded table ever
    // fills, leave later writes unclassified instead of recycling an identity.
    if (!context) return;
    *context = {true, frame, depth, return_pc, staging, record_base, shadow,
                attr0, attr1, attr2};
}

void remember_golden_sun_obj_f0_context(
    std::uint64_t frame, std::uint32_t depth, std::uint32_t return_pc,
    std::uint32_t staging, std::uint32_t record_base, bool shadow,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    reset_golden_sun_obj_f0_contexts_for_frame(frame);
    remember_golden_sun_obj_f0_context_in(
        g_golden_sun_obj_f0_contexts, frame, depth, return_pc, staging,
        record_base, shadow, attr0, attr1, attr2);
}

void remember_golden_sun_obj_f0_rejected_context(
    std::uint64_t frame, std::uint32_t depth, std::uint32_t return_pc,
    std::uint32_t staging, std::uint16_t attr0, std::uint16_t attr1,
    std::uint16_t attr2) {
    reset_golden_sun_obj_f0_contexts_for_frame(frame);
    remember_golden_sun_obj_f0_context_in(
        g_golden_sun_obj_f0_rejected_contexts, frame, depth, return_pc,
        staging, 0u, false, attr0, attr1, attr2);
}

// How stale a per-frame record may be and still describe the sprite being
// committed.
//
// Measured in logs/objrec_20260909_203451 (the boulder cutscene): of 4,339
// committed sprites, 3,479 failed with reason "context-frame" -- the context
// table's stamp did not equal the commit's frame -- and not one failed with
// "context-no-match". In all 647 frames that contained both a success and a
// failure, every failure came before every success, with no interleaving at
// all. That is the signature of ordering, not of a sprite the hooks never
// see: the table is cleared at the frame's first staging entry, so everything
// committed before that point is looked up against the previous frame's stamp
// and refused even though the matching record is still resident. Because the
// table is cleared exactly once per frame, one frame is the whole window --
// this is the measured bound, not a chosen tolerance.
//
// Age is not what authenticates a record, and never was. The caller still
// requires an exact ATTR0/1/2 match and requires the staged coordinates to
// truncate to the committed OAM bytes, so a stale position that no longer
// describes this sprite is still rejected.
constexpr std::uint64_t kGoldenSunObjRecordMaxFrameAge = 1u;

bool golden_sun_obj_record_frame_current(std::uint64_t writer_frame,
                                         std::uint64_t record_frame) {
    return record_frame <= writer_frame &&
           writer_frame - record_frame <= kGoldenSunObjRecordMaxFrameAge;
}

const GoldenSunObjF0Context* find_golden_sun_obj_f0_context(
    std::uint64_t frame, std::uint32_t depth, std::uint32_t return_pc,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2,
    const char** reason) {
    *reason = "context-frame";
    if (!golden_sun_obj_record_frame_current(
            frame, g_golden_sun_obj_f0_context_frame)) return nullptr;
    *reason = "context-no-match";
    const GoldenSunObjF0Context* found = nullptr;
    for (const auto& candidate : g_golden_sun_obj_f0_contexts) {
        if (!candidate.valid ||
            !golden_sun_obj_record_frame_current(frame, candidate.frame) ||
            candidate.depth != depth || candidate.return_pc != return_pc ||
            candidate.attr0 != attr0 || candidate.attr1 != attr1 ||
            candidate.attr2 != attr2) continue;
        if (found) { *reason = "context-duplicate"; return nullptr; }
        found = &candidate;
    }
    if (found) *reason = "ok";
    return found;
}


const GoldenSunObjF0Context* find_golden_sun_obj_f0_rejected_context(
    std::uint64_t frame, std::uint32_t depth, std::uint32_t return_pc,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2,
    const char** reason) {
    if (!golden_sun_obj_record_frame_current(
            frame, g_golden_sun_obj_f0_context_frame)) return nullptr;
    const GoldenSunObjF0Context* found = nullptr;
    for (const auto& candidate : g_golden_sun_obj_f0_rejected_contexts) {
        if (!candidate.valid ||
            !golden_sun_obj_record_frame_current(frame, candidate.frame) ||
            candidate.depth != depth || candidate.return_pc != return_pc ||
            candidate.attr0 != attr0 || candidate.attr1 != attr1 ||
            candidate.attr2 != attr2) continue;
        if (found) {
            if (reason) *reason = "context-duplicate";
            return nullptr;
        }
        found = &candidate;
    }
    return found;
}

// Why a committed sprite found no matching context.
//
// Measured 2026-09-09 in logs/objrec_20260909_211716: after the one-frame
// window, 3,988 of 6,319 commits fail with "context-no-match" and only 219
// with "context-frame", so a context table for the right frame exists and
// simply has no entry for these sprites. They cluster on one call site
// (writer 0x030038f0 returning to 0x030038b4, 2,707 failures against 80
// successes) and on one sprite class (256-colour tall sprites, ATTR2 0xd524)
// that never appears among the successes.
//
// The context key is (frame, depth, return_pc, ATTR0/1/2). EC entry and the
// F0 store are two points in the same routine invocation, so depth and
// return_pc cannot disagree for one sprite -- which leaves two candidates,
// and this census separates them: either the EC seam never ran for that call
// (no context with that depth/return_pc at all), or it ran and read a
// different ATTR triple than the one committed, in which case the pointer it
// read from is what we need to see. Bounded, deduped, diagnostics-only, and
// consulted by nothing that draws.
struct GoldenSunObjContextNearMiss {
    bool used = false;
    std::uint32_t return_pc = 0;
    std::uint32_t depth = 0;
    std::uint32_t staging = 0;
    bool any_context = false;
    std::uint16_t context_attr0 = 0, context_attr1 = 0, context_attr2 = 0;
    std::uint16_t committed_attr0 = 0, committed_attr1 = 0, committed_attr2 = 0;
    std::uint64_t hits = 0;
};
constexpr std::size_t kGoldenSunObjContextNearMissLimit = 32;
std::array<GoldenSunObjContextNearMiss, kGoldenSunObjContextNearMissLimit>
    g_golden_sun_obj_context_near_misses{};
std::uint64_t g_golden_sun_obj_context_near_miss_overflow = 0;

void note_golden_sun_obj_context_near_miss(
    std::uint64_t frame, std::uint32_t depth, std::uint32_t return_pc,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    // Find any context from this same call, ignoring the attributes that are
    // exactly what failed to match.
    const GoldenSunObjF0Context* found = nullptr;
    for (const auto* table : {&g_golden_sun_obj_f0_contexts,
                              &g_golden_sun_obj_f0_rejected_contexts}) {
        for (const auto& candidate : *table) {
            if (!candidate.valid || candidate.depth != depth ||
                candidate.return_pc != return_pc ||
                !golden_sun_obj_record_frame_current(frame, candidate.frame))
                continue;
            found = &candidate;
            break;
        }
        if (found) break;
    }
    const std::uint32_t staging = found ? found->staging : 0u;
    GoldenSunObjContextNearMiss* slot = nullptr;
    for (auto& candidate : g_golden_sun_obj_context_near_misses) {
        if (candidate.used && candidate.return_pc == return_pc &&
            candidate.depth == depth && candidate.staging == staging &&
            candidate.any_context == (found != nullptr)) {
            slot = &candidate;
            break;
        }
    }
    if (!slot) {
        for (auto& candidate : g_golden_sun_obj_context_near_misses) {
            if (candidate.used) continue;
            candidate = {};
            candidate.used = true;
            candidate.return_pc = return_pc;
            candidate.depth = depth;
            candidate.staging = staging;
            candidate.any_context = found != nullptr;
            candidate.context_attr0 = found ? found->attr0 : 0u;
            candidate.context_attr1 = found ? found->attr1 : 0u;
            candidate.context_attr2 = found ? found->attr2 : 0u;
            candidate.committed_attr0 = attr0;
            candidate.committed_attr1 = attr1;
            candidate.committed_attr2 = attr2;
            slot = &candidate;
            break;
        }
    }
    if (!slot) {
        ++g_golden_sun_obj_context_near_miss_overflow;
        return;
    }
    ++slot->hits;
}

void report_golden_sun_obj_context_near_misses() {
    for (const auto& miss : g_golden_sun_obj_context_near_misses) {
        if (!miss.used) continue;
        std::fprintf(stderr,
                     "[wide-obj-nearmiss] return_pc=0x%08x depth=%u "
                     "context=%s staging=0x%08x "
                     "ctx_attr=%04x/%04x/%04x oam_attr=%04x/%04x/%04x "
                     "hits=%llu\n",
                     miss.return_pc, miss.depth,
                     miss.any_context ? "yes" : "none", miss.staging,
                     miss.context_attr0, miss.context_attr1, miss.context_attr2,
                     miss.committed_attr0, miss.committed_attr1,
                     miss.committed_attr2,
                     static_cast<unsigned long long>(miss.hits));
    }
    if (g_golden_sun_obj_context_near_miss_overflow != 0u) {
        std::fprintf(stderr, "[wide-obj-nearmiss-overflow] dropped=%llu\n",
                     static_cast<unsigned long long>(
                         g_golden_sun_obj_context_near_miss_overflow));
    }
}


// One committed sprite's full-precision position, or the reason there isn't
// one. See widescreen_policy.h's GoldenSunObjPlacementOutcome for why this
// vocabulary exists and what each outcome means for vertical wrap.
struct GoldenSunObjPlacement {
    gsr::ObjLifetimeSample diagnostic{};
    // False for an entry that is disabled, empty or dormant -- not a sprite,
    // so neither the culler nor the census should count it.
    bool considered = false;
    gsr::widescreen::GoldenSunObjPlacementOutcome outcome =
        gsr::widescreen::GoldenSunObjPlacementOutcome::SourceUnavailable;
    // The guest actor record this sprite was traced to, when it was traced.
    bool record_identified = false;
    std::uint32_t staging = 0;
    std::uint32_t record_base = 0;
    bool shadow = false;
    // Rotation/scaling sprite. Independent of `outcome`: its POSITION is
    // recoverable like any other sprite's, but its on-screen BOUNDS need the
    // affine transform, so the off-screen culler must still refuse it. Kept
    // as its own flag rather than an outcome so the two questions -- "can we
    // place it?" and "can we cull it?" -- stop being answered by one bit.
    bool affine = false;
    // Valid only when golden_sun_obj_placement_is_exact(outcome).
    // For a shadow these are the SHADOW's own coordinates, recovered from its
    // paired body -- not the body's. paired_offset_* is how far it sits from
    // that body, reported so the recovery's one assumption (that the pair is
    // closer together than half the truncated field) stays measured.
    int resolved_x = 0;
    int resolved_y = 0;
    int paired_offset_x = 0;
    int paired_offset_y = 0;
    int width = 0;
    int height = 0;
    unsigned shape = 0;
    unsigned size = 0;
};

// Recover a committed sprite's position from the guest's own pre-truncation
// coordinate rather than from OAM.
//
// Extracted 2026-09-06 from the experimental off-screen culler, which was
// the only caller. It is now shared with the GSR_OBJ_RECORD census
// (obj_recorder.h), because the census has to measure exactly the lookup the
// renderer would eventually depend on -- measuring a reimplementation of it
// would prove nothing. Read-only: it inspects guest memory and the per-frame
// F0 context table and changes neither.
GoldenSunObjPlacement golden_sun_obj_resolve_placement(
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2,
    std::uint64_t writer_frame, std::uint32_t writer_depth,
    std::uint32_t writer_return_pc) {
    using Outcome = gsr::widescreen::GoldenSunObjPlacementOutcome;
    GoldenSunObjPlacement out;
    const bool affine = (attr0 & 0x0100u) != 0u;
    const bool already_disabled = !affine && (attr0 & 0x0200u) != 0u;
    if (already_disabled || (attr0 == 0u && attr1 == 0u && attr2 == 0u))
        return out;
    out.considered = true;
    out.affine = affine;
    out.shape = static_cast<unsigned>((attr0 >> 14) & 0x3u);
    out.size = static_cast<unsigned>((attr1 >> 14) & 0x3u);
    if (!gsr::widescreen::golden_sun_obj_dimensions(out.shape, out.size,
                                                    &out.width, &out.height)) {
        out.diagnostic.reason = "invalid-shape-size";
        out.outcome = Outcome::InvalidShapeSize;
        return out;
    }
    // Affine sprites are resolved like any other. They used to return here,
    // which measured 3,225 of 6,493 committed sprites (49.7%) as unplaceable
    // in session objrec_20260906_110334 -- but that was the CULLER's
    // limitation leaking into the placement answer. Golden Sun scales sprites
    // constantly (walk squash, shadows), so refusing to place them left half
    // the screen falling back to the 8-bit byte, which is exactly the
    // population that wraps. `out.affine` carries the restriction to the
    // culler instead. 2026-09-06.
    const auto* context = find_golden_sun_obj_f0_context(
        writer_frame, writer_depth, writer_return_pc, attr0, attr1, attr2,
        &out.diagnostic.reason);
    // Rejected contexts are diagnostic-only and are considered only after
    // the trusted lookup fails. This keeps them from creating ambiguity for
    // an otherwise valid placement when both toggles are enabled.
    if (!context && gsr::obj_recorder_enabled()) {
        context = find_golden_sun_obj_f0_rejected_context(
            writer_frame, writer_depth, writer_return_pc, attr0, attr1,
            attr2, &out.diagnostic.reason);
    }
    if (!context) {
        note_golden_sun_obj_context_near_miss(writer_frame, writer_depth,
                                              writer_return_pc, attr0, attr1,
                                              attr2);
    }
    out.diagnostic.context_frame = g_golden_sun_obj_f0_context_frame;
    // For an identity-rejected F0 source, retain the raw pointer in the
    // lifetime row so the next capture can verify it at the writer seam. It
    // remains untrusted: record_identified stays false and the predicate
    // below still controls placement/culling.
    if (context) out.diagnostic.staging = context->staging;
    std::uint32_t record_base = 0;
    bool shadow = false;
    if (!context || !golden_sun_obj_record_identity(context->staging,
                                                    &record_base, &shadow)) {
        if (context) out.diagnostic.reason = "record-identity";
        out.outcome = Outcome::SourceUnavailable;
        return out;
    }
    out.record_identified = true;
    out.staging = context->staging;
    out.record_base = record_base;
    out.shadow = shadow;
    // A shadow carries no coordinate of its own: it is placed from the body
    // record it is paired with (body at +0x00, shadow at +0x0C), which is
    // what keeps a body and its shadow from ever being placed by two
    // different decisions.
    const std::uint32_t body_source = record_base;
    auto* staging = find_golden_sun_obj_staging(body_source);
    std::uint16_t body_attr0 = 0, body_attr1 = 0, body_attr2 = 0;
    const bool body_attrs_valid = read_golden_sun_obj_staging_attrs(
        body_source, &body_attr0, &body_attr1, &body_attr2);
    int width = out.width;
    int height = out.height;
    const bool body_dimensions_valid = body_attrs_valid &&
        gsr::widescreen::golden_sun_obj_dimensions(
            (body_attr0 >> 14) & 0x3u, (body_attr1 >> 14) & 0x3u, &width,
            &height);
    // The body's own affine bit is deliberately NOT a rejection here, for the
    // same reason: a rotated sprite still has an exact top-left position, and
    // that position is all the renderer needs.
    const bool body_provenance_valid = staging && staging->valid &&
        staging->x_valid && staging->y_valid &&
        staging->auth_epoch == g_golden_sun_field_auth_epoch &&
        golden_sun_obj_record_frame_current(writer_frame, staging->frame);
    out.diagnostic.staging = context->staging;
    if (staging) {
        out.diagnostic.body_frame = staging->frame;
        out.diagnostic.body_epoch = staging->auth_epoch;
    }
    int checked_x = 0, checked_y = 0;
    const bool body_raw_matches = body_provenance_valid &&
        gsr::widescreen::golden_sun_obj_resolve_oam_x(
            body_attr1 & 0x1FFu, true, staging->logical_x, &checked_x) &&
        gsr::widescreen::golden_sun_obj_resolve_oam_y(
            body_attr0 & 0xFFu, true, staging->logical_y, &checked_y);
    const bool body_identity_matches = body_provenance_valid &&
        (shadow || (body_attr0 == attr0 && body_attr1 == attr1 &&
                    body_attr2 == attr2));
    // Independent bits retain simultaneous failures, rather than only the first.
    out.diagnostic.checks = (body_attrs_valid ? 1u : 0u) |
        (body_dimensions_valid ? 2u : 0u) | (staging && staging->valid ? 4u : 0u) |
        (staging && staging->x_valid ? 8u : 0u) |
        (staging && staging->y_valid ? 16u : 0u) |
        (staging && staging->auth_epoch == g_golden_sun_field_auth_epoch ? 32u : 0u) |
        (staging && golden_sun_obj_record_frame_current(
             writer_frame, staging->frame) ? 64u : 0u) |
        (body_raw_matches ? 128u : 0u) | (body_identity_matches ? 256u : 0u);
    if (!body_attrs_valid || !body_dimensions_valid || !body_provenance_valid ||
        !body_raw_matches || !body_identity_matches) {
        out.diagnostic.reason = "body-checks";
        out.outcome = Outcome::PlacementUnavailable;
        return out;
    }
    out.width = width;
    out.height = height;
    if (shadow) {
        // The body's position is not the shadow's. Recover the shadow's own
        // coordinates from its paired body plus its own committed fields --
        // without this the renderer would draw every shadow on top of the
        // sprite it belongs to.
        out.resolved_x = gsr::widescreen::golden_sun_obj_paired_coordinate(
            staging->logical_x, attr1 & 0x1FFu, 9u);
        out.resolved_y = gsr::widescreen::golden_sun_obj_paired_coordinate(
            staging->logical_y, attr0 & 0x00FFu, 8u);
        out.paired_offset_x = out.resolved_x - staging->logical_x;
        out.paired_offset_y = out.resolved_y - staging->logical_y;
        out.outcome = Outcome::PairedBody;
    } else {
        out.resolved_x = staging->logical_x;
        out.resolved_y = staging->logical_y;
        out.outcome = Outcome::Exact;
    }
    return out;
}

void observe_golden_sun_obj_f0_source(
    std::uint32_t entry_pc, std::uint64_t frame, std::uint32_t depth,
    std::uint32_t return_pc, std::uint32_t source) {
    if (!gsr::obj_recorder_enabled()) return;
    const std::uint32_t target = g_cpu.R[0];
    const int slot =
        gsr::widescreen::golden_sun_oam_shadow_slot_in_any_table(target, 0u);
    if (slot < 0) return;
    auto& observed = g_golden_sun_obj_f0_source_observations[
        static_cast<std::size_t>(slot)];
    observed = {};
    observed.state = "entry";
    observed.frame = frame;
    observed.epoch = g_golden_sun_field_auth_epoch;
    observed.pc = entry_pc;
    observed.source = source;
    observed.target = target;
    observed.depth = depth;
    observed.return_pc = return_pc;

    // The authenticated EC LDM consumes three words. Observe only their OAM
    // identity fields and only in physical RAM, never ROM, BIOS or MMIO.
    // Placement's IWRAM-only reader remains unchanged: this wider read is
    // diagnostic evidence for an unknown source, not permission to draw it.
    const bool readable = (source & 3u) == 0u &&
        ((source >= 0x02000000u && source <= 0x02040000u - 12u) ||
         (source >= 0x03000000u && source <= 0x03008000u - 12u));
    if (readable) {
        const auto word1 = bus_read_u32(source + 4u);
        const auto word2 = bus_read_u32(source + 8u);
        observed.attr0 = static_cast<std::uint16_t>(word1);
        observed.attr1 = static_cast<std::uint16_t>(word1 >> 16);
        observed.attr2 = static_cast<std::uint16_t>(word2);
        observed.flags |= 1u;
    }
    std::uint32_t base = 0;
    bool shadow = false;
    const bool known = golden_sun_obj_record_identity(source, &base, &shadow);
    if (known) observed.flags |= 2u;
    observed.stage_source = known && shadow ? base : source;
    const auto* staged = find_golden_sun_obj_staging(observed.stage_source);
    if (staged) {
        observed.flags |= 4u | (staged->x_valid ? 8u : 0u) |
            (staged->y_valid ? 16u : 0u);
        observed.stage_frame = staged->frame;
        observed.stage_epoch = staged->auth_epoch;
        if (staged->x_valid) observed.stage_x = staged->logical_x;
        if (staged->y_valid) observed.stage_y = staged->logical_y;
    }
    record_golden_sun_obj_boulder_ec_entry(observed);
}

gsr::ObjSourceCapture consume_golden_sun_obj_f0_source(
    int slot, std::uint32_t target, std::uint32_t writer_pc,
    std::uint64_t frame, std::uint32_t depth, std::uint32_t return_pc,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    gsr::ObjSourceCapture result;
    if (!gsr::obj_recorder_enabled()) return result;
    result.state = "missing-entry";
    if (slot < 0 || static_cast<std::size_t>(slot) >=
                        g_golden_sun_obj_f0_source_observations.size())
        return result;
    auto& pending = g_golden_sun_obj_f0_source_observations[
        static_cast<std::size_t>(slot)];
    if (pending.pc == 0u) return result;
    result = pending;
    pending = {};  // A source belongs to one commit, not later slot occupants.
    if (result.target != target || result.frame != frame ||
        result.epoch != g_golden_sun_field_auth_epoch ||
        result.depth != depth || result.return_pc != return_pc ||
        result.pc + (gsr::kFunc1dc8F0Offset - gsr::kFunc1dc8EcOffset) !=
            writer_pc) {
        result.state = "entry-context-mismatch";
    } else if ((result.flags & 1u) == 0u) {
        result.state = "entry-unreadable";
    } else if (result.attr0 != attr0 || result.attr1 != attr1 ||
               result.attr2 != attr2) {
        result.state = "entry-attrs-mismatch";
    } else {
        result.state = "matched-entry";
    }
    return result;
}

void note_golden_sun_obj_camera_sample(const gsr::ObjLifetimeSample& d,
                                       std::uint32_t writer_pc) {
    if (!gsr::obj_recorder_enabled() || std::strcmp(d.event, "commit") != 0)
        return;
    const std::uint32_t source = d.entry.source;
    const char* region = golden_sun_obj_source_region(source);
    if (!region) return;
    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* io = bus->io().raw();
    const auto io16 = [io](std::uint32_t offset) {
        return static_cast<std::uint16_t>(io[offset] |
                                          (io[offset + 1u] << 8));
    };

    gsr::ObjCameraSample sample;
    sample.reason = d.reason;
    sample.source_state = d.entry.state;
    sample.source_region = region;
    sample.frame = d.frame;
    sample.epoch = d.epoch;
    sample.slot = d.slot;
    sample.writer_pc = writer_pc;
    sample.target = d.target;
    sample.source = source;
    sample.entry_frame = d.entry.frame;
    sample.entry_epoch = d.entry.epoch;
    sample.entry_pc = d.entry.pc;
    sample.entry_depth = d.entry.depth;
    sample.entry_return_pc = d.entry.return_pc;
    sample.attr0 = d.attr0;
    sample.attr1 = d.attr1;
    sample.attr2 = d.attr2;
    sample.dispcnt = io16(0x00u);
    sample.bg0_hofs = io16(0x10u);
    sample.bg0_vofs = io16(0x12u);
    sample.bg1_hofs = io16(0x14u);
    sample.bg1_vofs = io16(0x16u);
    sample.bg2_hofs = io16(0x18u);
    sample.bg2_vofs = io16(0x1Au);
    sample.bg3_hofs = io16(0x1Cu);
    sample.bg3_vofs = io16(0x1Eu);

    // The writer census identified these two Region B field offsets. Read
    // them only when both complete words remain inside the measured span;
    // there is no inferred table extent beyond that span.
    const std::uint32_t region_end = kGoldenSunObjSourceRegionBEnd;
    const auto field_in_span = [source, region_end](std::uint32_t offset) {
        return source <= region_end && offset <= region_end - source &&
               4u <= region_end - source - offset + 1u;
    };
    if (std::strcmp(region, "region_b") == 0 &&
        field_in_span(0x0Cu) && field_in_span(0x14u)) {
        sample.field_a_offset = 0x0Cu;
        sample.field_a_value = bus_read_u32(source + 0x0Cu);
        sample.field_b_offset = 0x14u;
        sample.field_b_value = bus_read_u32(source + 0x14u);
        sample.candidate_fields_valid = true;
    }
    gsr::obj_recorder_note_camera_sample(sample);
}

void record_golden_sun_obj_boulder_ec_entry(
    const gsr::ObjSourceCapture& observed) {
    const char* region = golden_sun_obj_source_region(observed.source);
    if (!gsr::obj_recorder_enabled() || !region ||
        std::strcmp(region, "region_b") != 0)
        return;
    gsr::ObjBoulderTraceSample sample;
    sample.event = "ec-entry";
    sample.source_state = "entry";
    sample.outcome = "not-committed";
    sample.frame = observed.frame;
    sample.epoch = observed.epoch;
    sample.slot = gsr::widescreen::golden_sun_oam_shadow_slot_in_any_table(
        observed.target, 0u);
    sample.target = observed.target;
    sample.writer_pc = observed.pc;
    sample.return_pc = observed.return_pc;
    sample.depth = observed.depth;
    sample.entry_pc = observed.pc;
    sample.entry_depth = observed.depth;
    sample.entry_return_pc = observed.return_pc;
    fill_golden_sun_obj_boulder_registers(&sample);
    sample.attr0 = observed.attr0;
    sample.attr1 = observed.attr1;
    sample.attr2 = observed.attr2;
    sample.raw_x = observed.attr1 & 0x1FFu;
    sample.raw_y = observed.attr0 & 0x00FFu;
    fill_golden_sun_obj_boulder_source_fields(&sample, observed.source, 0u,
                                              0u);
    gsr::obj_recorder_note_boulder_trace(sample);
}

void record_golden_sun_obj_boulder_commit(
    const gsr::ObjSourceCapture& observed, const GoldenSunObjPlacement& placement,
    std::uint32_t writer_pc, std::uint64_t frame, std::uint32_t depth,
    std::uint32_t return_pc, int slot, std::uint32_t target,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    const char* region = golden_sun_obj_source_region(observed.source);
    if (!gsr::obj_recorder_enabled() || !region ||
        std::strcmp(region, "region_b") != 0)
        return;
    gsr::ObjBoulderTraceSample sample;
    sample.event = "f0-commit";
    sample.source_state = observed.state;
    sample.outcome =
        gsr::widescreen::golden_sun_obj_placement_outcome_name(
            placement.outcome);
    sample.frame = frame;
    sample.epoch = g_golden_sun_field_auth_epoch;
    sample.slot = slot;
    sample.target = target;
    sample.writer_pc = writer_pc;
    sample.return_pc = return_pc;
    sample.depth = depth;
    sample.entry_pc = observed.pc;
    sample.entry_depth = observed.depth;
    sample.entry_return_pc = observed.return_pc;
    fill_golden_sun_obj_boulder_registers(&sample);
    sample.attr0 = attr0;
    sample.attr1 = attr1;
    sample.attr2 = attr2;
    sample.raw_x = attr1 & 0x1FFu;
    sample.raw_y = attr0 & 0x00FFu;
    sample.resolved_x = placement.resolved_x;
    sample.resolved_y = placement.resolved_y;
    fill_golden_sun_obj_boulder_source_fields(&sample, observed.source, 0u,
                                              0u);
    gsr::obj_recorder_note_boulder_trace(sample);
}

void golden_sun_obj_f0_entry_capture(std::uint32_t entry_pc) {
    // EC is the exact generated entry immediately before the LDM that
    // destroys R6. The relocation-aware resolver authenticates its image and
    // base before capture; rendering and guest state are never modified.
    if (!golden_sun_func1dc8_writer_pc(
            entry_pc, gsr::Func1dc8WriterRoute::EC) ||
        (!golden_sun_wide_diagnostics_enabled() &&
         !golden_sun_experimental_fixes_enabled() &&
         !gsr::obj_recorder_enabled()) ||
        !golden_sun_expanded_obj_view_active()) return;
    const std::uint64_t frame = runtime_current_frame();
    const std::uint32_t depth = runtime_call_stack_depth();
    const std::uint32_t return_pc = golden_sun_obj_call_return_pc(depth);
    const std::uint32_t staging = g_cpu.R[6];
    observe_golden_sun_obj_f0_source(entry_pc, frame, depth, return_pc, staging);
    link_golden_sun_b328_parentless_ec(
        frame, staging, depth, return_pc, entry_pc);
    if (golden_sun_experimental_fixes_enabled() ||
        gsr::obj_recorder_enabled()) {
        std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
        std::uint32_t record_base = 0;
        bool shadow = false;
        // Keep a readable but identity-rejected pointer in the same
        // frame/depth/return context. This is an observational source-to-F0
        // join; golden_sun_obj_resolve_placement still applies the identity
        // predicate before returning any trusted placement.
        if (read_golden_sun_obj_staging_attrs(staging, &attr0, &attr1,
                                              &attr2)) {
            const bool record_identity = golden_sun_obj_record_identity(
                staging, &record_base, &shadow);
            if (record_identity) {
                remember_golden_sun_obj_f0_context(
                    frame, depth, return_pc, staging, record_base, shadow,
                    attr0, attr1, attr2);
            } else if (gsr::obj_recorder_enabled()) {
                remember_golden_sun_obj_f0_rejected_context(
                    frame, depth, return_pc, staging, attr0, attr1, attr2);
            }
        }
    }
    // WIDE-01 body/shadow identity: EC fires unconditionally on every
    // active object, every frame (unlike D4, which only fires on a
    // successful, gated commit), and the ATTR-shaped bytes at this seam
    // are still mid-update from other per-frame writers -- logging every
    // EC entry flooded the cap and stalled scene transitions in an earlier
    // pass of this diagnostic (see the dated note below). Admission is two
    // narrow, bounded cases, not a general trace: (a) the small/fixed
    // shadow-candidate signature (tile=0, shape=1, size=0) identified in
    // WIDE-01_NPC_IDENTITY.md, and (b) a handful of record slots (index
    // < kGoldenSunObjCommitOrderBodySlotLimit, computed directly from
    // `staging` -- this is a body event, so `staging` is the record base
    // with no +0x0C offset) -- added to capture independent body-sprite
    // ground truth for the scroll-transform check in
    // WIDE-01_NPC_IDENTITY.md, since D4's own attrs are read from the same
    // record and cannot serve as ground truth for that question. Anything
    // outside those two cases is silently skipped (not deduped -- never
    // recorded at all), keeping volume bounded regardless of how often EC
    // itself fires. Slot is unresolved at this seam (only known later, at
    // F0), so this reports slot=-1, truthfully. Signed logical X/Y is
    // likewise not available without threading through the D4 route's
    // staging-provenance lookup; left invalid rather than approximated.
    {
        std::uint16_t ec_attr0 = 0, ec_attr1 = 0, ec_attr2 = 0;
        if (read_golden_sun_obj_staging_attrs(staging, &ec_attr0, &ec_attr1,
                                              &ec_attr2)) {
            const std::uint16_t ec_tile = ec_attr2 & 0x3FFu;
            const std::uint16_t ec_shape =
                static_cast<std::uint16_t>((ec_attr0 >> 14) & 0x3u);
            const std::uint16_t ec_size =
                static_cast<std::uint16_t>((ec_attr1 >> 14) & 0x3u);
            const bool ec_is_shadow_signature =
                ec_tile == 0u && ec_shape == 1u && ec_size == 0u;
            constexpr std::uint32_t kBodyRecordBase = 0x03002000u;
            constexpr std::uint32_t kBodyRecordStride = 0x38u;
            constexpr std::uint32_t kGoldenSunObjCommitOrderBodySlotLimit = 6u;
            const bool ec_is_sampled_body_slot =
                staging >= kBodyRecordBase &&
                ((staging - kBodyRecordBase) % kBodyRecordStride) == 0u &&
                ((staging - kBodyRecordBase) / kBodyRecordStride) <
                    kGoldenSunObjCommitOrderBodySlotLimit;
            if (ec_is_shadow_signature || ec_is_sampled_body_slot) {
                note_golden_sun_obj_commit_order(
                    GoldenSunObjCommitOrderRoute::EC, staging, -1, ec_attr0,
                    ec_attr1, ec_attr2, false, 0, false, 0);
            }
        }
    }
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    for (auto& candidate : stats.candidates) {
        if (!candidate.valid || candidate.auth_epoch !=
            g_golden_sun_field_auth_epoch || candidate.f0_entry_seen ||
            candidate.f0_outcome != GoldenSunObjB328WriterOutcome::Pending ||
            candidate.staging_address != staging) continue;
        candidate.f0_entry_seen = true;
        candidate.f0_entry_frame = frame;
        candidate.f0_entry_depth = depth;
        candidate.f0_entry_return_pc = return_pc;
        candidate.f0_entry_staging = staging;
    }
    for (auto& candidate : stats.rejected) {
        if (!candidate.valid || candidate.auth_epoch !=
            g_golden_sun_field_auth_epoch) continue;
        auto& result = candidate.routes[static_cast<std::size_t>(
            GoldenSunObjB328WriterRoute::F0)];
        if (result.outcome != GoldenSunObjB328WriterOutcome::Pending ||
            result.f0_entry_seen || candidate.staging_address != staging)
            continue;
        result.f0_entry_seen = true;
        result.f0_entry_frame = frame;
        result.f0_entry_depth = depth;
        result.f0_entry_return_pc = return_pc;
        result.f0_entry_staging = staging;
    }
}

void record_golden_sun_obj_d4_event(
    const char* reason, int slot, std::uint32_t staging_address,
    std::uint32_t destination,
    const GoldenSunObjStagingProvenance* body_staging) {
    if (!gsr::obj_recorder_enabled()) return;
    gsr::ObjLifetimeSample d;
    d.event = "d4";
    d.reason = reason;
    d.frame = runtime_current_frame();
    d.epoch = g_golden_sun_field_auth_epoch;
    d.dma = g_obj_lifetime_dma;
    d.slot = slot;
    d.source = staging_address;
    d.staging = staging_address;
    d.context_frame = d.frame;
    d.body_frame = body_staging ? body_staging->frame : UINT64_MAX;
    d.body_epoch = body_staging ? body_staging->auth_epoch : 0;
    d.target = destination;
    d.checks = body_staging && body_staging->valid ? 4u : 0u;
    gsr::obj_recorder_note_lifetime(d);
}

bool golden_sun_oam_shadow_write_range(std::uint32_t address,
                                       std::uint32_t size) {
    if (size == 0u) return false;
    const std::uint64_t write_last =
        static_cast<std::uint64_t>(address) + size;
    return
        gsr::widescreen::golden_sun_oam_shadow_table_base(address) != 0u ||
        (write_last <= UINT32_MAX &&
         gsr::widescreen::golden_sun_oam_shadow_table_base(
             static_cast<std::uint32_t>(write_last - 1u)) != 0u);
}

void golden_sun_oam_shadow_write_observer(std::uint32_t writer_pc,
                                          std::uint32_t address,
                                          std::uint32_t size) {
    if (!golden_sun_oam_shadow_write_range(address, size)) return;
    ++g_golden_sun_obj_oam_observer_calls;
    // This probe intentionally runs before the authenticated F0 filter. The
    // failing identity was present in a full upload without a matching F0
    // commit, so the raw post-write seam records every affected table slot
    // and can identify a fast/generated or otherwise unclassified CPU store.
    record_golden_sun_shadow_cpu_writes(writer_pc, address, size);
    // F0's register identity is captured at the authenticated EC entry,
    // before its LDM overwrites R6. D4's STM is the second instruction after
    // its entry; on a resumed call the entry hook is skipped, so use only a
    // same-frame source pointer captured before that LDM. Both routes remain
    // image- and identity-authenticated.
    // This function is also the WIDE-01 experimental off-screen cull safety
    // net's hook point (see below), so it must run when either diagnostics
    // or the "Enhanced Options" toggle is on -- not diagnostics alone. All
    // diagnostics-only work inside self-gates on
    // golden_sun_wide_diagnostics_enabled() independently.
    const bool d4_store = size != 0u &&
        golden_sun_func1dc8_d4_store_pc(writer_pc);
    const bool f0_store = !d4_store &&
        golden_sun_func1dc8_writer_pc(
            writer_pc, gsr::Func1dc8WriterRoute::F0);
    if ((!golden_sun_wide_diagnostics_enabled() &&
         !golden_sun_experimental_fixes_enabled() &&
         !gsr::obj_recorder_enabled()) ||
        !golden_sun_expanded_obj_view_active() ||
        (!d4_store && !f0_store) || size == 0u) return;
    // Either sprite table, not just the first: the game builds its list in
    // two places and uploads whichever is current, so watching one address
    // left 31% of frames unobserved (session_20260906_131954, 79 of 256
    // uploads). See kGoldenSunOamShadowAltStart.
    const std::uint32_t table_base =
        gsr::widescreen::golden_sun_oam_shadow_table_base(address);
    const int slot = gsr::widescreen::golden_sun_oam_shadow_slot_in_any_table(
        address, 4u);
    if (table_base == 0u || slot < 0) return;
    const std::uint32_t slot_address =
        table_base + static_cast<std::uint32_t>(slot) *
                         gsr::widescreen::kGoldenSunOamShadowSlotBytes;
    if (d4_store && address == slot_address + 4u) {
        // The D4 LDM has no writeback: post-LDM R6 is not a source pointer.
        // Only the authenticated pre-LDM capture may complete this handoff;
        // a resumed store without that token stays unclassified.
        const int primary_slot =
            gsr::widescreen::golden_sun_oam_shadow_slot(slot_address);
        if (primary_slot >= 0 && static_cast<std::size_t>(primary_slot) <
                                     g_golden_sun_obj_d4_source_captures.size()) {
            auto& capture = g_golden_sun_obj_d4_source_captures[
                static_cast<std::size_t>(primary_slot)];
            const std::uint32_t entry_pc = writer_pc - 4u;
            const bool token_present = capture.valid;
            const bool token_matches =
                token_present && capture.frame == runtime_current_frame() &&
                capture.auth_epoch == g_golden_sun_field_auth_epoch &&
                capture.entry_pc == entry_pc &&
                capture.target_address == slot_address;
            if (gsr::obj_recorder_enabled()) {
                const std::uint32_t observed_staging = token_present
                    ? capture.staging_address : 0u;
                std::uint32_t record_base = 0;
                bool shadow = false;
                const auto* body_staging =
                    observed_staging != 0u &&
                            golden_sun_obj_record_identity(
                                observed_staging, &record_base, &shadow)
                        ? find_golden_sun_obj_staging(
                              shadow ? record_base : observed_staging)
                        : nullptr;
                record_golden_sun_obj_d4_event(
                    token_matches ? "store-token-found"
                                  : token_present ? "store-token-mismatch"
                                                  : "store-token-missing",
                    primary_slot, observed_staging, slot_address,
                    body_staging);
            }
            if (token_matches) {
                // A token belongs to one store, not every later occupant of
                // the same slot in this frame.
                const auto staging_address = capture.staging_address;
                capture.valid = false;
                golden_sun_obj_staging_handoff(
                    entry_pc, staging_address, slot_address);
            }
        }
    }
    // D4 has its own source token. Do not resolve it using F0 entry context.
    if (d4_store) return;
    ++g_golden_sun_obj_f0_commits;
    const auto writer_frame = runtime_current_frame();
    const auto writer_depth = runtime_call_stack_depth();
    const auto writer_return_pc = golden_sun_obj_call_return_pc(writer_depth);
    const std::uint16_t committed_attr0 = bus_read_u16(slot_address);
    const std::uint16_t committed_attr1 = bus_read_u16(slot_address + 2u);
    const std::uint16_t committed_attr2 = bus_read_u16(slot_address + 4u);
    const auto source_observation = consume_golden_sun_obj_f0_source(
        slot, slot_address, writer_pc, writer_frame, writer_depth,
        writer_return_pc, committed_attr0, committed_attr1, committed_attr2);
    if (golden_sun_wide_diagnostics_enabled()) {
        // Capture the committed ATTR payload before the optional experimental
        // culler can mark it hidden; diagnostics must describe the guest
        // write, not our later presentation-only change.
        link_golden_sun_b328_parentless_f0(
            writer_frame, writer_depth, writer_return_pc, writer_pc,
            slot_address, slot, bus_read_u16(slot_address),
            bus_read_u16(slot_address + 2u),
            bus_read_u16(slot_address + 4u));
    }
    // Placement lookup, shared by the GSR_OBJ_RECORD census and the
    // experimental off-screen culler. The census has to run without the
    // culler being armed -- it measures, it must not change what is drawn --
    // so the gate is either toggle while the culler's guest write below
    // stays inside its own.
    if (gsr::obj_recorder_enabled() ||
        golden_sun_experimental_fixes_enabled()) {
        ++g_golden_sun_obj_f0_placement_calls;
        const GoldenSunObjPlacement placement =
            golden_sun_obj_resolve_placement(
                committed_attr0, committed_attr1, committed_attr2,
                writer_frame, writer_depth, writer_return_pc);
        record_golden_sun_obj_boulder_commit(
            source_observation, placement, writer_pc, writer_frame,
            writer_depth, writer_return_pc, slot, slot_address,
            committed_attr0, committed_attr1, committed_attr2);
        if (placement.record_identified) ++g_golden_sun_obj_f0_identified;
        // Disabled, empty and dormant entries are not sprites: neither the
        // culler nor the census counts them.
        if (placement.considered) {
            // Coverage of the seam that matters on screen: how many
            // committed sprites ended up with a trusted full-precision
            // position rather than the 8-bit OAM byte. Payload-free, so a
            // normal Enhanced run reports it without the recorder.
            ++g_golden_sun_obj_f0_considered;
            if (gsr::widescreen::golden_sun_obj_placement_is_exact(
                    placement.outcome)) {
                ++g_golden_sun_obj_f0_placed;
                // F0 is an independently authenticated commit route. The
                // marked pillar capture joins its exact body record and ATTR
                // tuple to the next frame's parentless B328 rejection.
                if (placement.record_identified && !placement.shadow &&
                    placement.staging == placement.record_base) {
                    remember_golden_sun_obj_b328_authorization(
                        placement.record_base, committed_attr0,
                        committed_attr1, committed_attr2);
                }
                // How much of the coverage comes from a record staged in the
                // previous frame, i.e. from the sprites committed before this
                // frame's first staging entry.
                if (placement.diagnostic.body_frame != writer_frame)
                    ++g_golden_sun_obj_f0_placed_prev_frame;
            }
            if (gsr::obj_recorder_enabled()) {
                auto d = placement.diagnostic;
                d.frame = writer_frame; d.epoch = g_golden_sun_field_auth_epoch;
                d.dma = g_obj_lifetime_dma; d.slot = slot; d.source = table_base;
                d.attr0 = committed_attr0; d.attr1 = committed_attr1;
                d.attr2 = committed_attr2;
                d.entry = source_observation;
                d.target = slot_address;
                gsr::obj_recorder_note_lifetime(d);
                note_golden_sun_obj_camera_sample(d, writer_pc);
            }
            const std::uint32_t raw_x = committed_attr1 & 0x1FFu;
            const std::uint32_t raw_y = committed_attr0 & 0x00FFu;
            if (gsr::obj_recorder_enabled()) {
                gsr::ObjPlacementSample sample;
                sample.frame = writer_frame;
                sample.slot = slot;
                sample.slot_address = slot_address;
                sample.writer_pc = writer_pc;
                sample.return_pc = writer_return_pc;
                sample.depth = writer_depth;
                sample.staging = placement.staging;
                sample.record_base = placement.record_base;
                sample.shadow = placement.shadow;
                sample.record_identified = placement.record_identified;
                sample.affine = placement.affine;
                sample.paired_offset_x = placement.paired_offset_x;
                sample.paired_offset_y = placement.paired_offset_y;
                sample.table_base = table_base;
                sample.attr0 = committed_attr0;
                sample.attr1 = committed_attr1;
                sample.attr2 = committed_attr2;
                sample.raw_x = raw_x;
                sample.raw_y = raw_y;
                sample.resolved_x = placement.resolved_x;
                sample.resolved_y = placement.resolved_y;
                sample.width = placement.width;
                sample.height = placement.height;
                sample.outcome = placement.outcome;
                gsr::obj_recorder_note_placement(sample);
            }
            // Keep the resolved commit pending until the complete shadow
            // table is transferred to OAM. Publishing here exposes a
            // half-built table: later writes can change another ATTR slot
            // before the DMA, so the renderer can combine coordinates from
            // different images and make a sprite jump at the cull edge.
            // The provider may use this record as a same-frame fallback,
            // but it must never replace the DMA-latched visible table.
            if (gsr::widescreen::golden_sun_obj_placement_is_exact(
                    placement.outcome) &&
                slot >= 0 && static_cast<std::size_t>(slot) <
                    g_golden_sun_obj_pending_provenance.size()) {
                auto& pending = g_golden_sun_obj_pending_provenance[
                    static_cast<std::size_t>(slot)];
                ++g_golden_sun_obj_provenance_generation;
                pending.valid = true;
                pending.x_valid = true;
                pending.y_valid = true;
                pending.logical_x =
                    static_cast<std::int16_t>(placement.resolved_x);
                pending.logical_y =
                    static_cast<std::int16_t>(placement.resolved_y);
                pending.frame = writer_frame;
                pending.auth_epoch = g_golden_sun_field_auth_epoch;
                pending.writer_branch_pc = writer_pc;
                pending.target_address = slot_address;
                pending.writer_generation = 0;
                pending.oam_identity_valid = true;
                pending.expected_attr0 = committed_attr0;
                pending.expected_attr1 = committed_attr1;
                pending.expected_attr2 = committed_attr2;
                pending.commit_resolved = true;
            }
        }
    }
    record_golden_sun_b328_accepted_f0_writer(
        slot_address, slot, bus_read_u16(slot_address),
        bus_read_u16(slot_address + 2u),
        bus_read_u16(slot_address + 4u), writer_frame, writer_depth,
        writer_return_pc);
    record_golden_sun_b328_rejected_writer(
        GoldenSunObjB328WriterRoute::F0, 0u, slot,
        bus_read_u16(slot_address), bus_read_u16(slot_address + 2u),
        bus_read_u16(slot_address + 4u), writer_frame, writer_depth,
        writer_return_pc, 0u);
    // WIDE-01 body/shadow identity: F0 fires on every committed OAM-shadow
    // write for every active object, and ATTR0/ATTR1 carry the live raw
    // Y/X, so an unfiltered trace changes almost every frame for every
    // moving object -- logging it unconditionally flooded the cap and
    // stalled scene transitions in an earlier pass of this diagnostic.
    // D4 already gives a complete, low-volume trace of body writes with
    // sequence numbers in the same shared per-frame counter, so this route
    // is restricted to two narrow, bounded cases: (a) the small/fixed
    // shadow-candidate signature (tile=0, shape=1, size=0) identified in
    // WIDE-01_NPC_IDENTITY.md, and (b) a handful of destination OAM slots
    // (index < kGoldenSunObjCommitOrderBodySlotLimit) -- added to capture
    // independent, ground-truth body-sprite ATTR for the scroll-transform
    // check in WIDE-01_NPC_IDENTITY.md, since D4's own attrs are read from
    // the record itself and cannot serve as ground truth for that
    // question. This destination-slot carve-out does not use the same
    // numbering as the EC route's record-slot carve-out (OAM destination
    // slots are reused/reassigned independently of record slots, per the
    // existing findings), so the two do not guarantee a 1:1 pairing on
    // the same object every time -- only frames where both happen to
    // admit the same object are usable for that cross-check, and that is
    // determined empirically in the doc, not assumed here. The originating
    // source record address is not resolved at this seam (only the EC
    // entry captured it); reported as source=0, never fabricated. Signed
    // logical X/Y is likewise not available here (the ATTR bytes are
    // already hardware-shaped, a different concept from the pre-truncation
    // logical coordinate the D4 route reports).
    {
        const std::uint16_t f0_attr0 = bus_read_u16(slot_address);
        const std::uint16_t f0_attr1 = bus_read_u16(slot_address + 2u);
        const std::uint16_t f0_attr2 = bus_read_u16(slot_address + 4u);
        const std::uint16_t f0_tile = f0_attr2 & 0x3FFu;
        const std::uint16_t f0_shape =
            static_cast<std::uint16_t>((f0_attr0 >> 14) & 0x3u);
        const std::uint16_t f0_size =
            static_cast<std::uint16_t>((f0_attr1 >> 14) & 0x3u);
        const bool f0_is_shadow_signature =
            f0_tile == 0u && f0_shape == 1u && f0_size == 0u;
        constexpr int kGoldenSunObjCommitOrderBodySlotLimit = 6;
        const bool f0_is_sampled_body_slot =
            slot < kGoldenSunObjCommitOrderBodySlotLimit;
        if (f0_is_shadow_signature || f0_is_sampled_body_slot) {
            note_golden_sun_obj_commit_order(
                GoldenSunObjCommitOrderRoute::F0, 0u, slot, f0_attr0,
                f0_attr1, f0_attr2, false, 0, false, 0);
        }
    }
}

void expire_golden_sun_b328_rejected_candidates() {
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    for (auto& candidate : stats.rejected) {
        if (!candidate.valid) continue;
        for (std::size_t route = 0; route < candidate.routes.size(); ++route) {
            auto& result = candidate.routes[route];
            if (result.outcome != GoldenSunObjB328WriterOutcome::Pending)
                continue;
            result.outcome = GoldenSunObjB328WriterOutcome::Expired;
            ++stats.rejected_outcomes[route][static_cast<std::size_t>(
                result.outcome)];
            record_golden_sun_b328_rejected_sample(
                candidate, static_cast<GoldenSunObjB328WriterRoute>(route),
                result);
        }
        candidate.valid = false;
    }
    for (auto& candidate : stats.candidates) {
        if (!candidate.valid) continue;
        if (candidate.f0_outcome == GoldenSunObjB328WriterOutcome::Pending) {
            candidate.f0_outcome = GoldenSunObjB328WriterOutcome::Expired;
            ++stats.accepted_f0_outcomes[static_cast<std::size_t>(
                candidate.f0_outcome)];
            record_golden_sun_b328_accepted_f0_sample(candidate);
        }
        candidate.valid = false;
    }
}

void record_golden_sun_b328_handoff(std::uint32_t staging_address,
                                    std::uint64_t frame,
                                    bool attr_identity_valid) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active() || !attr_identity_valid) return;
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    for (auto it = stats.candidates.rbegin(); it != stats.candidates.rend();
         ++it) {
        if (!it->valid || it->handoff_seen ||
            it->staging_address != staging_address || it->frame != frame ||
            it->auth_epoch != g_golden_sun_field_auth_epoch) continue;
        it->handoff_seen = true;
        ++stats.handoffs[static_cast<std::size_t>(it->classification)];
        return;
    }
}

void report_golden_sun_b328_parentless_tokens(std::uint64_t frame,
                                              const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled() || !reason) return;
    for (const auto& token : g_golden_sun_obj_b328_parentless_tokens) {
        if (!token.valid) continue;
        std::fprintf(
            stderr,
            "[wide-obj-y-parentless-token] frame=%llu auth_epoch=%llu "
            "reason=%s sequence=%llu b328_entry_pc=0x%08x "
            "staging=0x%08x operand=%d r6=0x%08x r7=0x%08x depth=%u "
            "return_pc=0x%08x attr_valid=%u attr0=0x%04x attr1=0x%04x "
            "attr2=0x%04x ec_seen=%u ec_entry_pc=0x%08x ec_r6=0x%08x "
            "ec_r7=0x%08x ec_source=0x%08x ec_reason=%s f0_seen=%u "
            "f0_entry_pc=0x%08x f0_r6=0x%08x f0_r7=0x%08x "
            "f0_source=0x%08x target=0x%08x slot=%d f0_attr0=0x%04x "
            "f0_attr1=0x%04x f0_attr2=0x%04x raw_y=%u f0_reason=%s\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(token.auth_epoch), reason,
            static_cast<unsigned long long>(token.sequence), token.entry_pc,
            token.staging, token.operand, token.r6, token.r7,
            token.call_depth, token.return_pc, token.attr_valid ? 1u : 0u,
            static_cast<unsigned>(token.attr0),
            static_cast<unsigned>(token.attr1),
            static_cast<unsigned>(token.attr2), token.ec_seen ? 1u : 0u,
            token.ec_entry_pc, token.ec_r6, token.ec_r7, token.ec_source,
            token.ec_reason, token.f0_seen ? 1u : 0u, token.f0_entry_pc,
            token.f0_r6, token.f0_r7, token.f0_source, token.f0_target,
            token.f0_slot, static_cast<unsigned>(token.f0_attr0),
            static_cast<unsigned>(token.f0_attr1),
            static_cast<unsigned>(token.f0_attr2),
            static_cast<unsigned>(token.f0_attr0 & 0xFFu), token.f0_reason);
    }
    std::fprintf(stderr,
                 "[wide-obj-y-parentless-token-summary] frame=%llu "
                 "auth_epoch=%llu reason=%s dropped=%llu\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                 reason,
                 static_cast<unsigned long long>(
                     g_golden_sun_obj_b328_parentless_dropped));
}

void report_golden_sun_obj_b328_diagnostics(std::uint64_t frame,
                                            const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    auto& stats = g_golden_sun_obj_b328_diagnostics;
    report_golden_sun_b328_parentless_tokens(frame, reason);
    expire_golden_sun_b328_rejected_candidates();
    for (auto& candidate : stats.candidates) {
        if (!candidate.valid) continue;
        if (!candidate.handoff_seen) {
            ++stats.no_handoff[static_cast<std::size_t>(
                candidate.classification)];
        }
        candidate.valid = false;
    }
    std::fprintf(stderr,
                 "[wide-obj-y-b328-summary] frame=%llu auth_epoch=%llu "
                 "reason=%s candidates=%llu exact=%llu mismatch=%llu "
                 "no_parent=%llu mismatch_frame=%llu mismatch_depth=%llu "
                 "mismatch_return=%llu accepted_exact=%llu accepted_mismatch=%llu "
                 "accepted_no_parent=%llu handoff_exact=%llu "
                 "handoff_mismatch=%llu handoff_no_parent=%llu "
                 "no_handoff_exact=%llu no_handoff_mismatch=%llu "
                 "no_handoff_no_parent=%llu tracking_dropped=%llu "
                 "samples=%zu samples_dropped=%llu\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                 reason,
                 static_cast<unsigned long long>(stats.classifications[0] +
                                                 stats.classifications[1] +
                                                 stats.classifications[2]),
                 static_cast<unsigned long long>(stats.classifications[0]),
                 static_cast<unsigned long long>(stats.classifications[1]),
                 static_cast<unsigned long long>(stats.classifications[2]),
                 static_cast<unsigned long long>(stats.mismatch_frame),
                 static_cast<unsigned long long>(stats.mismatch_call_depth),
                 static_cast<unsigned long long>(stats.mismatch_return_pc),
                 static_cast<unsigned long long>(stats.accepted[0]),
                 static_cast<unsigned long long>(stats.accepted[1]),
                 static_cast<unsigned long long>(stats.accepted[2]),
                 static_cast<unsigned long long>(stats.handoffs[0]),
                 static_cast<unsigned long long>(stats.handoffs[1]),
                 static_cast<unsigned long long>(stats.handoffs[2]),
                 static_cast<unsigned long long>(stats.no_handoff[0]),
                 static_cast<unsigned long long>(stats.no_handoff[1]),
                 static_cast<unsigned long long>(stats.no_handoff[2]),
                 static_cast<unsigned long long>(stats.accepted_tracking_dropped),
                 stats.sample_count,
                 static_cast<unsigned long long>(stats.samples_dropped));
    std::fprintf(
        stderr,
        "[wide-obj-y-b328-writer-summary] frame=%llu auth_epoch=%llu "
        "rejected=%llu d4_consumed=%llu d4_unrelated=%llu "
        "d4_expired=%llu f0_consumed=%llu f0_unrelated=%llu "
        "f0_identity_unproven=%llu f0_expired=%llu "
        "f0_context_mismatch=%llu f0_attr_mismatch=%llu tracking_dropped=%llu "
        "samples=%zu samples_dropped=%llu\n",
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
        static_cast<unsigned long long>(stats.rejected_total),
        static_cast<unsigned long long>(stats.rejected_outcomes[0][1]),
        static_cast<unsigned long long>(stats.rejected_outcomes[0][2]),
        static_cast<unsigned long long>(stats.rejected_outcomes[0][4]),
        static_cast<unsigned long long>(stats.rejected_outcomes[1][1]),
        static_cast<unsigned long long>(stats.rejected_outcomes[1][2]),
        static_cast<unsigned long long>(stats.rejected_outcomes[1][3]),
        static_cast<unsigned long long>(stats.rejected_outcomes[1][4]),
        static_cast<unsigned long long>(stats.rejected_outcomes[1][5]),
        static_cast<unsigned long long>(stats.rejected_outcomes[1][6]),
        static_cast<unsigned long long>(stats.rejected_tracking_dropped),
        stats.rejected_sample_count,
        static_cast<unsigned long long>(stats.rejected_samples_dropped));
    for (std::size_t i = 0; i < stats.rejected_sample_count; ++i) {
        const auto& sample = stats.rejected_samples[i];
        std::fprintf(
            stderr,
            "[wide-obj-y-b328-writer-sample] frame=%llu writer_frame=%llu "
            "auth_epoch=%llu route=%s outcome=%s classification=%s "
            "mismatch_flags=0x%x "
            "staging=0x%08x writer_staging=0x%08x operand=%d "
            "depth=%u return_pc=0x%08x writer_depth=%u "
            "writer_return_pc=0x%08x slot=%d attr0=0x%04x attr1=0x%04x "
            "attr2=0x%04x f0_register_mask=0x%04x "
            "f0_entry_seen=%u f0_entry_frame=%llu f0_entry_depth=%u "
            "f0_entry_return_pc=0x%08x f0_entry_staging=0x%08x "
            "expected_attr0=0x%04x expected_attr1=0x%04x expected_attr2=0x%04x\n",
            static_cast<unsigned long long>(sample.frame),
            static_cast<unsigned long long>(sample.writer_frame),
            static_cast<unsigned long long>(sample.auth_epoch),
            golden_sun_obj_b328_writer_route_name(sample.route),
            golden_sun_obj_b328_writer_outcome_name(sample.outcome),
            golden_sun_obj_b328_parent_classification_name(
                sample.classification), sample.mismatch_flags,
            sample.staging_address,
            sample.writer_staging, sample.operand, sample.call_depth,
            sample.call_return_pc, sample.writer_depth,
            sample.writer_return_pc, sample.slot,
            static_cast<unsigned>(sample.attr0),
            static_cast<unsigned>(sample.attr1),
            static_cast<unsigned>(sample.attr2),
            static_cast<unsigned>(sample.f0_register_mask),
            sample.f0_entry_seen ? 1u : 0u,
            static_cast<unsigned long long>(sample.f0_entry_frame),
            sample.f0_entry_depth, sample.f0_entry_return_pc,
            sample.f0_entry_staging,
            static_cast<unsigned>(sample.expected_attr0),
            static_cast<unsigned>(sample.expected_attr1),
            static_cast<unsigned>(sample.expected_attr2));
    }
    std::fprintf(
        stderr,
        "[wide-obj-y-b328-accepted-f0-summary] frame=%llu "
        "auth_epoch=%llu accepted=%llu consumed=%llu unrelated=%llu "
        "expired=%llu context_mismatch=%llu attr_mismatch=%llu "
        "tracking_dropped=%llu samples=%zu samples_dropped=%llu\n",
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
        static_cast<unsigned long long>(stats.accepted_f0_outcomes[1] +
                                         stats.accepted_f0_outcomes[2] +
                                         stats.accepted_f0_outcomes[4] +
                                         stats.accepted_f0_outcomes[5] +
                                         stats.accepted_f0_outcomes[6]),
        static_cast<unsigned long long>(stats.accepted_f0_outcomes[1]),
        static_cast<unsigned long long>(stats.accepted_f0_outcomes[2]),
        static_cast<unsigned long long>(stats.accepted_f0_outcomes[4]),
        static_cast<unsigned long long>(stats.accepted_f0_outcomes[5]),
        static_cast<unsigned long long>(stats.accepted_f0_outcomes[6]),
        static_cast<unsigned long long>(stats.accepted_f0_tracking_dropped),
        stats.accepted_f0_sample_count,
        static_cast<unsigned long long>(stats.accepted_f0_samples_dropped));
    for (std::size_t i = 0; i < stats.accepted_f0_sample_count; ++i) {
        const auto& sample = stats.accepted_f0_samples[i];
        std::fprintf(
            stderr,
            "[wide-obj-y-b328-accepted-f0-sample] frame=%llu "
            "writer_frame=%llu auth_epoch=%llu outcome=%s "
            "classification=%s staging=0x%08x operand=%d "
            "depth=%u return_pc=0x%08x writer_depth=%u "
            "writer_return_pc=0x%08x target=0x%08x slot=%d "
            "attr0=0x%04x attr1=0x%04x "
            "attr2=0x%04x f0_register_mask=0x%04x f0_entry_seen=%u "
            "f0_entry_frame=%llu f0_entry_depth=%u "
            "f0_entry_return_pc=0x%08x f0_entry_staging=0x%08x "
            "expected_attr0=0x%04x expected_attr1=0x%04x "
            "expected_attr2=0x%04x\n",
            static_cast<unsigned long long>(sample.frame),
            static_cast<unsigned long long>(sample.writer_frame),
            static_cast<unsigned long long>(sample.auth_epoch),
            golden_sun_obj_b328_writer_outcome_name(sample.outcome),
            golden_sun_obj_b328_parent_classification_name(
            sample.classification), sample.staging_address, sample.operand,
            sample.call_depth, sample.call_return_pc, sample.writer_depth,
            sample.writer_return_pc, sample.target_address, sample.slot,
            static_cast<unsigned>(sample.attr0),
            static_cast<unsigned>(sample.attr1),
            static_cast<unsigned>(sample.attr2), sample.f0_register_mask,
            sample.f0_entry_seen ? 1u : 0u,
            static_cast<unsigned long long>(sample.f0_entry_frame),
            sample.f0_entry_depth, sample.f0_entry_return_pc,
            sample.f0_entry_staging,
            static_cast<unsigned>(sample.expected_attr0),
            static_cast<unsigned>(sample.expected_attr1),
            static_cast<unsigned>(sample.expected_attr2));
    }
    static constexpr const char* kMismatchNames[] = {
        "frame", "call-depth", "return-pc"};
    for (std::size_t i = 0; i < stats.sample_count; ++i) {
        const auto& sample = stats.samples[i];
        std::fprintf(stderr,
                     "[wide-obj-y-b328-sample] frame=%llu auth_epoch=%llu "
                     "classification=%s staging=0x%08x operand=%d "
                     "mismatch=",
                     static_cast<unsigned long long>(sample.frame),
                     static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                     golden_sun_obj_b328_parent_classification_name(
                         sample.classification), sample.staging_address,
                     sample.operand);
        bool first = true;
        for (unsigned bit = 0; bit < 3u; ++bit) {
            if ((sample.mismatch_flags & (1u << bit)) == 0u) continue;
            std::fprintf(stderr, "%s%s", first ? "" : ",",
                         kMismatchNames[bit]);
            first = false;
        }
        if (first) std::fputs("none", stderr);
        std::fprintf(stderr,
                     " parent_frame=%llu parent_depth=%u parent_return_pc=0x%08x\n",
                     static_cast<unsigned long long>(sample.parent_frame),
                     sample.parent_call_depth, sample.parent_return_pc);
    }
}

bool read_golden_sun_obj_stack_word(std::uint32_t sp, std::uint32_t offset,
                                    std::uint32_t* out) {
    constexpr std::uint32_t kIwramStart = 0x03000000u;
    constexpr std::uint32_t kIwramEnd = 0x03008000u;
    if (!out || (sp & 3u) != 0u || (offset != 4u && offset != 0x18u) ||
        sp < kIwramStart || sp > kIwramEnd - 4u - offset) return false;
    *out = bus_read_u32(sp + offset);
    return true;
}

void record_golden_sun_signed_y_cull(
    std::uint32_t pc, std::int32_t operand,
    std::uint32_t original_decision, std::uint32_t final_decision,
    bool overridden) {
    if (!golden_sun_expanded_obj_view_active() ||
        (pc != 0x0800B3E6u && pc != 0x0800B3ECu &&
         pc != 0x0800C702u && pc != 0x0800C708u)) return;
    GoldenSunObjSignedCullObservation observation{};
    observation.valid = true;
    observation.frame = runtime_current_frame();
    observation.pc = pc;
    observation.operand = operand;
    observation.original_decision = original_decision;
    observation.final_decision = final_decision;
    observation.overridden = overridden;
    observation.r7 = g_cpu.R[7];
    observation.r10 = g_cpu.R[10];
    observation.r11 = g_cpu.R[11];
    observation.sp = g_cpu.R[13];
    observation.lr = g_cpu.R[14];
    observation.call_depth = runtime_call_stack_depth();
    observation.call_return_pc = golden_sun_obj_call_return_pc(
        observation.call_depth);
    observation.sequence = ++g_golden_sun_obj_signed_cull_sequence;
    g_golden_sun_obj_signed_cull_observations[
        pc == 0x0800B3E6u ? 0u : pc == 0x0800B3ECu ? 1u
        : pc == 0x0800C702u ? 2u : 3u] = observation;
}

GoldenSunObjYCorrelation capture_golden_sun_obj_y_correlation(
    std::int32_t operand) {
    GoldenSunObjYCorrelation result{};
    if (!golden_sun_expanded_obj_view_active() || operand < 160 ||
        operand > 199) return result;
    result.valid = true;
    result.b328_pc = 0x0800B328u;
    result.b328_operand = operand;
    result.b328_r1 = g_cpu.R[1];
    result.b328_r3 = g_cpu.R[3];
    result.b328_r11 = g_cpu.R[11];
    result.b328_r6 = g_cpu.R[6];
    result.b328_r7 = g_cpu.R[7];
    result.b328_sp = g_cpu.R[13];
    result.b328_lr = g_cpu.R[14];
    result.b328_call_depth = runtime_call_stack_depth();
    result.b328_call_return_pc = golden_sun_obj_call_return_pc(
        result.b328_call_depth);
    result.stack_inputs_valid =
        read_golden_sun_obj_stack_word(g_cpu.R[13], 4u,
                                       &result.b328_stack_sp_plus4) &&
        read_golden_sun_obj_stack_word(g_cpu.R[13], 0x18u,
                                       &result.b328_stack_sp_plus18);
    for (const auto& parent : g_golden_sun_obj_b27e_parent_routes) {
        if (parent.valid && parent.staging_address == result.b328_r7 &&
            parent.frame == runtime_current_frame() &&
            parent.call_depth == result.b328_call_depth &&
            parent.call_return_pc == result.b328_call_return_pc) {
            result.parent = parent;
            break;
        }
    }
    const std::uint32_t pcs[] = {
        0x0800B3E6u, 0x0800B3ECu, 0x0800C702u, 0x0800C708u};
    for (std::size_t i = 0; i < result.preceding.size(); ++i) {
        const auto& candidate = g_golden_sun_obj_signed_cull_observations[i];
        if (candidate.valid && candidate.frame == runtime_current_frame() &&
            candidate.call_depth == result.b328_call_depth &&
            candidate.call_return_pc == result.b328_call_return_pc &&
            candidate.sequence <= g_golden_sun_obj_signed_cull_sequence) {
            result.preceding[i] = candidate;
        } else {
            result.preceding[i].pc = pcs[i];
        }
    }
    return result;
}

void trace_golden_sun_obj_y_cull_decision(
    std::uint32_t branch_pc, std::uint32_t original_decision,
    std::uint32_t final_decision, bool overridden, std::int32_t operand) {
    // B27E/B328 are the reviewed object-record Y routes. The signed-Y
    // precull routes are retained in the existing four-entry correlation
    // table, where their decisions can be joined to a later B328 object.
    // This is diagnostics-only: no cull result or fallback path is changed.
    const bool object_y_route = branch_pc == 0x0800B27Eu ||
        branch_pc == 0x0800B328u;
    if (!object_y_route) return;
    const auto policy_lower = static_cast<std::int32_t>(
        gsr::widescreen::kNativeHeight);
    const auto policy_upper = static_cast<std::int32_t>(
        gsr::widescreen::kNativeHeight + gsr::widescreen::kExpandedExtraY);
    const bool in_policy_band = operand >= policy_lower && operand < policy_upper;
    const bool outside_policy_band = operand >= policy_upper;
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active() ||
        (!in_policy_band && !outside_policy_band)) return;
    const std::uint64_t frame = runtime_current_frame();
    // The numeric 64-row/4-per-branch bounds remain unchanged, but their
    // in-memory dedupe window is per guest frame. A stationary object must
    // not consume the entire stream before its later bottom crossing.
    if (g_golden_sun_obj_y_cull_decision_frame != frame) {
        g_golden_sun_obj_y_cull_decision_keys = {};
        g_golden_sun_obj_y_cull_decision_logs_in_frame = 0;
        g_golden_sun_obj_y_cull_decision_frame = frame;
    }
    const auto correlation = branch_pc == 0x0800B328u && in_policy_band
        ? capture_golden_sun_obj_y_correlation(operand)
        : GoldenSunObjYCorrelation{};
    const std::uint32_t staging_address = g_cpu.R[7];
    GoldenSunObjYCullDecisionKey key{};
    key.branch_pc = branch_pc;
    key.staging_address = staging_address;
    key.frame = frame;
    key.operand = operand;
    key.original_decision = original_decision;
    key.final_decision = final_decision;
    key.r1 = g_cpu.R[1];
    key.r3 = g_cpu.R[3];
    key.r11 = g_cpu.R[11];
    for (const auto& seen : g_golden_sun_obj_y_cull_decision_keys) {
        if (seen.valid && seen.branch_pc == key.branch_pc &&
            seen.staging_address == key.staging_address &&
            seen.frame == key.frame && seen.operand == key.operand &&
            seen.original_decision == key.original_decision &&
            seen.final_decision == key.final_decision && seen.r1 == key.r1 &&
            seen.r3 == key.r3 && seen.r11 == key.r11) return;
    }
    unsigned staging_records = 0;
    for (const auto& seen : g_golden_sun_obj_y_cull_decision_keys) {
        if (seen.valid && seen.branch_pc == key.branch_pc &&
            seen.staging_address == key.staging_address)
            ++staging_records;
    }
    if (g_golden_sun_obj_y_cull_decision_logs_in_frame >=
            kGoldenSunObjYCullDecisionLogLimitPerFrame ||
        staging_records >= kGoldenSunObjYCullDecisionPerStagingLimit) {
        ++g_golden_sun_obj_y_cull_decision_logs_dropped;
        return;
    }
    for (auto& seen : g_golden_sun_obj_y_cull_decision_keys) {
        if (!seen.valid) {
            seen = key;
            seen.valid = true;
            break;
        }
    }
    ++g_golden_sun_obj_y_cull_decision_logs_in_frame;
    const auto& parent = correlation.parent;
    std::uint32_t record_base = 0;
    bool shadow = false;
    // Gate the only guest-memory read below with the existing measured actor
    // record identity predicate. The words are a same-hook staging snapshot;
    // they are not an authenticated current/committed dimension claim.
    const bool record_identity_valid = object_y_route &&
        golden_sun_obj_record_identity(staging_address, &record_base, &shadow);
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    const bool staging_snapshot_valid = record_identity_valid &&
        read_golden_sun_obj_staging_attrs(
            staging_address, &attr0, &attr1, &attr2);
    const unsigned snapshot_shape = staging_snapshot_valid
        ? static_cast<unsigned>((attr0 >> 14) & 0x3u) : 0u;
    const unsigned snapshot_size = staging_snapshot_valid
        ? static_cast<unsigned>((attr1 >> 14) & 0x3u) : 0u;
    int snapshot_width = 0;
    int snapshot_height = 0;
    const bool staging_snapshot_dimensions_valid = staging_snapshot_valid &&
        gsr::widescreen::golden_sun_obj_dimensions(
            snapshot_shape, snapshot_size, &snapshot_width, &snapshot_height);
    const GoldenSunObjStagingProvenance* staged = object_y_route
        ? find_golden_sun_obj_staging(staging_address) : nullptr;
    const bool stage_valid = staged && staged->valid;
    const bool stage_current = stage_valid &&
        staged->frame == key.frame && staged->auth_epoch ==
            g_golden_sun_field_auth_epoch;
    const bool position_x_valid = stage_current && staged->x_valid;
    const bool position_y_valid = object_y_route;
    const int position_x = position_x_valid ? staged->logical_x : 0;
    const int position_y = position_y_valid ? operand : 0;
    const bool stage_x_valid = stage_current && staged->x_valid;
    const bool stage_y_valid = stage_current && staged->y_valid;
    const int stage_x = stage_x_valid ? staged->logical_x : 0;
    const int stage_y = stage_y_valid ? staged->logical_y : 0;
    std::fprintf(
        stderr,
        "[wide-obj-y-cull-decision] frame=%llu auth_epoch=%llu "
        "branch_pc=0x%08x staging=0x%08x operand=%d original=%u final=%u "
        "overridden=%u position_x_valid=%u position_x=%d "
        "position_y_valid=%u position_y=%d staging_snapshot_valid=%u "
        "staging_snapshot_attr0=0x%04x staging_snapshot_attr1=0x%04x "
        "staging_snapshot_attr2=0x%04x staging_snapshot_shape=%u "
        "staging_snapshot_size=%u staging_snapshot_affine=%u "
        "staging_snapshot_affine_double=%u "
        "staging_snapshot_dimensions_valid=%u "
        "staging_snapshot_pixels=%dx%d "
        "record_identity_valid=%u record_base=0x%08x shadow=%u "
        "stage_valid=%u stage_current=%u stage_frame=%llu stage_epoch=%llu "
        "stage_x_valid=%u stage_x=%d stage_y_valid=%u stage_y=%d "
        "r1=0x%08x r3=0x%08x r6=0x%08x r7=0x%08x r10=0x%08x "
        "r11=0x%08x sp=0x%08x lr=0x%08x depth=%u return_pc=0x%08x "
        "stack_valid=%u stack_sp4=0x%08x stack_sp18=0x%08x "
        "parent_valid=%u parent_frame=%llu parent_operand=%d "
        "parent_original=%u parent_final=%u parent_overridden=%u "
        "parent_r7=0x%08x dropped=%llu\n",
        static_cast<unsigned long long>(runtime_current_frame()),
        static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
        branch_pc, key.staging_address, operand, original_decision,
        final_decision, overridden ? 1u : 0u, position_x_valid ? 1u : 0u,
        position_x, position_y_valid ? 1u : 0u, position_y,
        staging_snapshot_valid ? 1u : 0u, static_cast<unsigned>(attr0),
        static_cast<unsigned>(attr1), static_cast<unsigned>(attr2),
        snapshot_shape, snapshot_size,
        staging_snapshot_valid && (attr0 & 0x0100u) ? 1u : 0u,
        staging_snapshot_valid && (attr0 & 0x0300u) == 0x0300u ? 1u : 0u,
        staging_snapshot_dimensions_valid ? 1u : 0u,
        snapshot_width, snapshot_height,
        record_identity_valid ? 1u : 0u, record_base, shadow ? 1u : 0u,
        stage_valid ? 1u : 0u, stage_current ? 1u : 0u,
        stage_valid ? static_cast<unsigned long long>(staged->frame)
                    : static_cast<unsigned long long>(UINT64_MAX),
        stage_valid ? static_cast<unsigned long long>(staged->auth_epoch) : 0ull,
        stage_x_valid ? 1u : 0u, stage_x, stage_y_valid ? 1u : 0u, stage_y,
        g_cpu.R[1], g_cpu.R[3], g_cpu.R[6], g_cpu.R[7], g_cpu.R[10], g_cpu.R[11],
        g_cpu.R[13], g_cpu.R[14],
        runtime_call_stack_depth(),
        golden_sun_obj_call_return_pc(runtime_call_stack_depth()),
        correlation.stack_inputs_valid ? 1u : 0u,
        correlation.b328_stack_sp_plus4, correlation.b328_stack_sp_plus18,
        parent.valid ? 1u : 0u,
        static_cast<unsigned long long>(parent.frame), parent.operand,
        parent.original_decision, parent.final_decision,
        parent.overridden ? 1u : 0u, parent.r7,
        static_cast<unsigned long long>(
            g_golden_sun_obj_y_cull_decision_logs_dropped));
}

void trace_golden_sun_obj_y_correlation(
    const GoldenSunObjYCorrelation& correlation, std::uint32_t staging_address,
    int slot, std::uint16_t attr0, std::uint16_t attr1,
    std::uint16_t attr2) {
    if (!correlation.valid || !golden_sun_expanded_obj_view_active() || slot < 0 ||
        static_cast<std::size_t>(slot) >=
            g_golden_sun_obj_y_correlation_slot_logs.size()) return;
    GoldenSunObjYCorrelationKey key{};
    key.staging_address = staging_address;
    key.slot = slot;
    key.b328_operand = correlation.b328_operand;
    key.b328_r1 = correlation.b328_r1;
    key.b328_r3 = correlation.b328_r3;
    key.b328_r11 = correlation.b328_r11;
    key.stack_sp_plus4 = correlation.b328_stack_sp_plus4;
    key.stack_sp_plus18 = correlation.b328_stack_sp_plus18;
    key.attr0 = attr0;
    key.attr1 = attr1;
    key.attr2 = attr2;
    for (std::size_t i = 0; i < key.cull_sequences.size(); ++i)
        key.cull_sequences[i] = correlation.preceding[i].valid
            ? correlation.preceding[i].sequence : 0u;
    for (const auto& seen : g_golden_sun_obj_y_correlation_keys) {
        if (seen.valid && seen.staging_address == key.staging_address &&
            seen.slot == key.slot &&
            seen.b328_operand == key.b328_operand && seen.attr0 == key.attr0 &&
            seen.b328_r1 == key.b328_r1 && seen.b328_r3 == key.b328_r3 &&
            seen.b328_r11 == key.b328_r11 &&
            seen.stack_sp_plus4 == key.stack_sp_plus4 &&
            seen.stack_sp_plus18 == key.stack_sp_plus18 &&
            seen.attr1 == key.attr1 && seen.attr2 == key.attr2 &&
            seen.cull_sequences == key.cull_sequences) return;
    }
    unsigned staging_records = 0;
    for (const auto& seen : g_golden_sun_obj_y_correlation_keys) {
        if (seen.valid && seen.staging_address == staging_address)
            ++staging_records;
    }
    if (g_golden_sun_obj_y_correlation_slot_logs[static_cast<std::size_t>(slot)] >=
            kGoldenSunObjYCorrelationPerSlotLimit ||
        staging_records >= kGoldenSunObjYCorrelationPerStagingLimit ||
        g_golden_sun_obj_y_correlation_logs_in_epoch >=
            kGoldenSunObjYCorrelationLogLimitPerEpoch) {
        ++g_golden_sun_obj_y_correlation_logs_dropped;
        return;
    }
    for (auto& seen : g_golden_sun_obj_y_correlation_keys) {
        if (!seen.valid) {
            seen = key;
            seen.valid = true;
            break;
        }
    }
    ++g_golden_sun_obj_y_correlation_slot_logs[static_cast<std::size_t>(slot)];
    ++g_golden_sun_obj_y_correlation_logs_in_epoch;
    std::fprintf(stderr,
        "[wide-obj-y-cull-correlation] frame=%llu auth_epoch=%llu "
        "b328_pc=0x%08x b328_operand=%d b328_r1=0x%08x b328_r3=0x%08x "
        "b328_r11=0x%08x stack_valid=%u stack_sp4=0x%08x stack_sp18=0x%08x "
        "b328_r6=0x%08x b328_r7=0x%08x "
        "b328_sp=0x%08x b328_lr=0x%08x depth=%u return_pc=0x%08x "
        "staging=0x%08x slot=%d attr0=0x%04x attr1=0x%04x attr2=0x%04x "
        "parent_valid=%u parent_pc=0x%08x parent_operand=%d "
        "parent_original=%u parent_overridden=%u parent_final=%u "
        "parent_r7=0x%08x parent_r10=0x%08x parent_r11=0x%08x "
        "parent_sp=0x%08x parent_lr=0x%08x parent_depth=%u "
        "parent_return_pc=0x%08x "
        "b3e6_valid=%u b3e6_operand=%d b3e6_original=%u b3e6_final=%u "
        "b3e6_overridden=%u b3ec_valid=%u b3ec_operand=%d "
        "b3ec_original=%u b3ec_final=%u b3ec_overridden=%u "
        "c702_valid=%u c702_operand=%d c702_original=%u c702_final=%u "
        "c702_overridden=%u c708_valid=%u c708_operand=%d "
        "c708_original=%u c708_final=%u c708_overridden=%u dropped=%llu\n",
        static_cast<unsigned long long>(runtime_current_frame()),
        static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
        correlation.b328_pc, correlation.b328_operand, correlation.b328_r1,
        correlation.b328_r3, correlation.b328_r11,
        correlation.stack_inputs_valid ? 1u : 0u,
        correlation.b328_stack_sp_plus4, correlation.b328_stack_sp_plus18,
        correlation.b328_r6, correlation.b328_r7, correlation.b328_sp,
        correlation.b328_lr,
        correlation.b328_call_depth, correlation.b328_call_return_pc,
        staging_address, slot, static_cast<unsigned>(attr0),
        static_cast<unsigned>(attr1), static_cast<unsigned>(attr2),
        correlation.parent.valid ? 1u : 0u, correlation.parent.pc,
        correlation.parent.operand, correlation.parent.original_decision,
        correlation.parent.overridden ? 1u : 0u,
        correlation.parent.final_decision, correlation.parent.r7,
        correlation.parent.r10, correlation.parent.r11, correlation.parent.sp,
        correlation.parent.lr, correlation.parent.call_depth,
        correlation.parent.call_return_pc,
        correlation.preceding[0].valid ? 1u : 0u,
        correlation.preceding[0].operand,
        correlation.preceding[0].original_decision,
        correlation.preceding[0].final_decision,
        correlation.preceding[0].overridden ? 1u : 0u,
        correlation.preceding[1].valid ? 1u : 0u,
        correlation.preceding[1].operand,
        correlation.preceding[1].original_decision,
        correlation.preceding[1].final_decision,
        correlation.preceding[1].overridden ? 1u : 0u,
        correlation.preceding[2].valid ? 1u : 0u,
        correlation.preceding[2].operand,
        correlation.preceding[2].original_decision,
        correlation.preceding[2].final_decision,
        correlation.preceding[2].overridden ? 1u : 0u,
        correlation.preceding[3].valid ? 1u : 0u,
        correlation.preceding[3].operand,
        correlation.preceding[3].original_decision,
        correlation.preceding[3].final_decision,
        correlation.preceding[3].overridden ? 1u : 0u,
        static_cast<unsigned long long>(g_golden_sun_obj_y_correlation_logs_dropped));
}

void trace_golden_sun_obj_y_jump(
    int slot, int raw_y, int logical_y,
    const GoldenSunObjPlacementProvenance& provenance,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    // A large accepted-Y transition is the signature of a recycled slot or
    // an accidental modulo-256 interpretation. Keep this automatic and
    // bounded so it remains useful when the broad WIDE probe is disabled.
    if (!golden_sun_expanded_obj_view_active() || slot < 0 ||
        static_cast<std::size_t>(slot) >=
            g_golden_sun_obj_y_accepted_state.size()) return;
    auto& previous = g_golden_sun_obj_y_accepted_state[
        static_cast<std::size_t>(slot)];
    const auto frame = runtime_current_frame();
    if (previous.valid) {
        const int delta = logical_y - static_cast<int>(previous.logical_y);
        const int magnitude = delta < 0 ? -delta : delta;
        if (magnitude >= 64) {
            if (g_golden_sun_obj_y_jump_logs_in_epoch >=
                kGoldenSunObjYJumpLogLimitPerEpoch) {
                ++g_golden_sun_obj_y_jump_logs_dropped;
            } else {
                ++g_golden_sun_obj_y_jump_logs_in_epoch;
                std::fprintf(
                    stderr,
                    "[wide-obj-y-jump] frame=%llu previous_frame=%llu "
                    "auth_epoch=%llu slot=%d delta=%d "
                    "previous_raw_y=%u raw_y=%d previous_logical_y=%d "
                    "logical_y=%d previous_writer_branch_pc=0x%08x "
                    "writer_branch_pc=0x%08x previous_target_address=0x%08x "
                    "target_address=0x%08x previous_generation=%llu "
                    "writer_generation=%llu previous_attr0=0x%04x "
                    "attr0=0x%04x previous_attr1=0x%04x attr1=0x%04x "
                    "previous_attr2=0x%04x attr2=0x%04x\n",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(previous.frame),
                    static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                    slot, delta, static_cast<unsigned>(previous.raw_y), raw_y,
                    static_cast<int>(previous.logical_y), logical_y,
                    previous.writer_branch_pc, provenance.writer_branch_pc,
                    previous.target_address, provenance.target_address,
                    static_cast<unsigned long long>(previous.writer_generation),
                    static_cast<unsigned long long>(provenance.writer_generation),
                    static_cast<unsigned>(previous.attr0),
                    static_cast<unsigned>(attr0),
                    static_cast<unsigned>(previous.attr1),
                    static_cast<unsigned>(attr1),
                    static_cast<unsigned>(previous.attr2),
                    static_cast<unsigned>(attr2));
            }
        }
    }
    previous.valid = true;
    previous.raw_y = static_cast<std::uint8_t>(raw_y);
    previous.logical_y = static_cast<std::int16_t>(logical_y);
    previous.frame = frame;
    previous.writer_branch_pc = provenance.writer_branch_pc;
    previous.target_address = provenance.target_address;
    previous.writer_generation = provenance.writer_generation;
    previous.attr0 = attr0;
    previous.attr1 = attr1;
    previous.attr2 = attr2;
}

void trace_golden_sun_obj_y_alias(
    const char* reason, int slot, int raw_y, int logical_y,
    const GoldenSunObjPlacementProvenance& provenance,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2,
    bool canonical_rejection) {
    // This is the one bounded diagnostic that must remain visible even when
    // the broad WIDE-01 probe is off. Keep it tied strictly to the geometry
    // whose signed-Y path it diagnoses, with no scene/authentication gate.
    if (!golden_sun_expanded_obj_view_active() || slot < 0 ||
        static_cast<std::size_t>(slot) >=
            g_golden_sun_obj_y_alias_last_frame.size() ||
        (!canonical_rejection &&
         !gsr::widescreen::golden_sun_obj_y_alias_candidate(
             raw_y, logical_y))) return;
    const std::uint64_t frame = runtime_current_frame();
    auto& last_frame = canonical_rejection
        ? g_golden_sun_obj_y_canonical_last_frame[static_cast<std::size_t>(slot)]
        : g_golden_sun_obj_y_alias_last_frame[static_cast<std::size_t>(slot)];
    if (last_frame == frame) return;
    last_frame = frame;
    if (g_golden_sun_obj_y_alias_logs_in_epoch >=
        kGoldenSunObjYAliasLogLimitPerEpoch) {
        ++g_golden_sun_obj_y_alias_logs_dropped;
        return;
    }
    ++g_golden_sun_obj_y_alias_logs_in_epoch;
    std::fprintf(
        stderr,
        "[wide-obj-y-alias] frame=%llu provenance_frame=%llu "
        "auth_epoch=%llu reason=%s slot=%d raw_y=%d logical_y=%d "
        "writer_branch_pc=0x%08x target_address=0x%08x "
        "writer_generation=%llu attr0=0x%04x attr1=0x%04x attr2=0x%04x\n",
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(provenance.frame),
        static_cast<unsigned long long>(g_golden_sun_field_auth_epoch), reason,
        slot, raw_y, logical_y, provenance.writer_branch_pc,
        provenance.target_address,
        static_cast<unsigned long long>(provenance.writer_generation),
        static_cast<unsigned>(attr0), static_cast<unsigned>(attr1),
        static_cast<unsigned>(attr2));
}

void trace_golden_sun_obj_y_edge_alias(
    int slot, std::uint32_t target_address, std::uint64_t frame,
    std::uint64_t auth_epoch, std::uint16_t attr0, std::uint16_t attr1,
    std::uint16_t attr2) {
    ++g_golden_sun_obj_y_edge_alias_activations;
    if (!golden_sun_wide_diagnostics_enabled()) return;
    std::fprintf(stderr,
                 "[wide-obj-y-edge-alias] frame=%llu auth_epoch=%llu "
                 "slot=%d target=0x%08x raw_y=159 logical_y=-97 "
                 "attr0=0x%04x attr1=0x%04x attr2=0x%04x activations=%llu\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(auth_epoch), slot,
                 target_address, static_cast<unsigned>(attr0),
                 static_cast<unsigned>(attr1), static_cast<unsigned>(attr2),
                 static_cast<unsigned long long>(
                     g_golden_sun_obj_y_edge_alias_activations));
}

void begin_golden_sun_field_auth_epoch();

// GBARECOMP_VRAM_MAP_TRACE is the existing explicit kill switch for this
// payload-free map investigation. The default is deliberately off in main().
bool golden_sun_wide_diagnostics_enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GBARECOMP_VRAM_MAP_TRACE");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();
    return enabled;
}

// The launcher's "Enhanced Options" checkbox (src/launcher_main.cpp), always
// sent explicitly (0 or 1), same discipline as the diagnostics kill switch
// above. Default off; normal play is byte-for-byte unaffected. It gates the
// expanded-sprite and shadow guards below.
bool golden_sun_experimental_fixes_enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GBARECOMP_EXPERIMENTAL_FIXES");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();
    return enabled;
}

struct GoldenSunFieldMapCensus {
    std::uint64_t auth_epoch = 0;
    std::uint32_t map_crc = 0;
    std::uint32_t raw_crc = 0;
};
std::vector<GoldenSunFieldMapCensus> g_golden_sun_wide_field_map_census;
std::uint64_t g_golden_sun_wide_field_map_trace_frame = UINT64_MAX;
bool g_golden_sun_palace_table_authorized = false;
bool g_golden_sun_palace_table_invalidated = false;
bool g_golden_sun_palace_table_auth_attempted = false;
gsr::widescreen::GoldenSunPalaceActiveRegion
    g_golden_sun_palace_active_region;
std::array<std::uint16_t, 6> g_golden_sun_palace_region_scroll{};
bool g_golden_sun_palace_region_scroll_valid = false;
bool g_golden_sun_palace_region_build_attempted = false;
unsigned g_golden_sun_obj_y_provenance_logs_in_epoch = 0;
using GoldenSunWidePolicyReason = gsr::widescreen::GoldenSunWidePolicyReason;

void invalidate_golden_sun_palace_table_authorization();
bool refresh_golden_sun_palace_table_authorization();

void reset_golden_sun_palace_active_region() {
    if (g_golden_sun_palace_region_scroll_valid ||
        g_golden_sun_palace_region_build_attempted) {
        g_golden_sun_palace_active_region.reset();
    }
    g_golden_sun_palace_region_scroll_valid = false;
    g_golden_sun_palace_region_build_attempted = false;
}

// Payload-free provider accounting separates a missing/invalid atlas source
// from a successful replacement. The existing map census proves which table
// image was resident, while these counters prove what the PPU actually did
// with margin requests; neither path retains or prints guest tile bytes.
struct GoldenSunFieldProviderTrace {
    std::uint64_t calls = 0;
    std::uint64_t equal_scroll_calls = 0;
    std::uint64_t split_scroll_calls = 0;
    std::uint64_t replacements = 0;
    std::uint64_t unavailable = 0;
    std::uint64_t precondition_rejects = 0;
    std::uint64_t boundary_rejects = 0;
    std::uint64_t palace_region_rejects = 0;
    std::uint64_t raw_unavailable = 0;
    std::uint64_t lookup_misses = 0;
    std::int32_t min_x = INT32_MAX;
    std::int32_t max_x = INT32_MIN;
    std::int32_t min_y = INT32_MAX;
    std::int32_t max_y = INT32_MIN;
};
std::array<GoldenSunFieldProviderTrace, 4>
    g_golden_sun_field_provider_trace{};
GoldenSunPalaceMarginDiagnostics g_golden_sun_palace_margin_diagnostics{};

struct GoldenSunFieldMapIdAttribution {
    std::uint16_t map_id = 0;
    std::uint64_t replacements = 0;
};

// A bounded top-of-observed set per layer and margin region. This records no
// map/table payload and avoids a 4x4x4096 hot-path counter matrix.
std::array<std::array<std::array<GoldenSunFieldMapIdAttribution,
                                  kGoldenSunFieldMapIdAttributionLimit>, 4>, 4>
    g_golden_sun_field_map_id_attribution{};
std::array<std::array<std::uint64_t, 4>, 4>
    g_golden_sun_field_map_id_attribution_overflow{};
GoldenSunWidePolicyReason g_golden_sun_wide_policy_last_reason =
    GoldenSunWidePolicyReason::UnsupportedMode;
bool g_golden_sun_wide_policy_seen = false;
std::uint64_t g_golden_sun_wide_policy_transition_count = 0;
std::uint64_t g_golden_sun_wide_policy_logged_transitions = 0;
std::uint64_t g_golden_sun_wide_policy_sample_count = 0;
unsigned g_golden_sun_wide_policy_last_flags =
    gsr::widescreen::kPillarboxAll;
std::uint64_t g_golden_sun_wide_policy_sample_frame = UINT64_MAX;
std::uint64_t g_golden_sun_wide_policy_sample_end_frame = UINT64_MAX;
std::uint64_t g_golden_sun_wide_policy_sample_transition = 0;
unsigned g_golden_sun_wide_policy_samples_in_window = 0;

// The PPU-side margin observer reports only counters and layer/source
// categories. Keep a cumulative copy for the bounded exit summary while also
// emitting a sparse per-frame line that can identify the side of a seam.
std::uint64_t g_golden_sun_margin_diagnostic_callbacks = 0;
std::uint64_t g_golden_sun_margin_diagnostic_logged = 0;
std::uint64_t g_golden_sun_margin_diagnostic_last_log_frame = UINT64_MAX;
gba::WsMarginDiagnostics g_golden_sun_margin_diagnostic_total{};
gba::WsMarginDiagnostics g_golden_sun_margin_diagnostic_last{};

void accumulate_golden_sun_margin_diagnostics(
    gba::WsMarginDiagnostics& total, const gba::WsMarginDiagnostics& sample) {
    total.margin_pixels += sample.margin_pixels;
    total.left_margin_pixels += sample.left_margin_pixels;
    total.right_margin_pixels += sample.right_margin_pixels;
    total.top_margin_pixels += sample.top_margin_pixels;
    total.bottom_margin_pixels += sample.bottom_margin_pixels;
    for (std::size_t bg = 0; bg < total.provider_results.size(); ++bg) {
        for (std::size_t result = 0;
             result < total.provider_results[bg].size(); ++result) {
            total.provider_results[bg][result] +=
                sample.provider_results[bg][result];
        }
    }
    for (std::size_t layer = 0;
         layer < total.final_selected.size(); ++layer) {
        for (std::size_t source = 0;
             source < total.final_selected[layer].size(); ++source) {
            total.final_selected[layer][source] +=
                sample.final_selected[layer][source];
        }
    }
    for (std::size_t side = 0;
         side < total.horizontal_final_selected.size(); ++side) {
        for (std::size_t layer = 0;
             layer < total.horizontal_final_selected[side].size(); ++layer) {
            for (std::size_t source = 0;
                 source < total.horizontal_final_selected[side][layer].size();
                 ++source) {
                total.horizontal_final_selected[side][layer][source] +=
                    sample.horizontal_final_selected[side][layer][source];
            }
        }
    }
}

const char* golden_sun_wide_policy_reason_name(
    GoldenSunWidePolicyReason reason) {
    switch (reason) {
        case GoldenSunWidePolicyReason::AuthorizedMode2:
            return "authorized-mode2";
        case GoldenSunWidePolicyReason::AuthorizedMode0:
            return "authorized-mode0";
        case GoldenSunWidePolicyReason::AuthorizedMode0SplitScroll:
            return "authorized-mode0-split-scroll";
        case GoldenSunWidePolicyReason::MissingIo:
            return "missing-io";
        case GoldenSunWidePolicyReason::ForcedBlank:
            return "forced-blank";
        case GoldenSunWidePolicyReason::WindowControl:
            return "window-control";
        case GoldenSunWidePolicyReason::UnsupportedMode:
            return "unsupported-mode";
        case GoldenSunWidePolicyReason::Mode2Layers:
            return "mode2-layers";
        case GoldenSunWidePolicyReason::Mode2Geometry:
            return "mode2-geometry";
        case GoldenSunWidePolicyReason::Mode0Layers:
            return "mode0-layers";
        case GoldenSunWidePolicyReason::Mode0Geometry:
            return "mode0-geometry";
        case GoldenSunWidePolicyReason::Mode0ScrollMismatch:
            return "mode0-scroll-mismatch";
        case GoldenSunWidePolicyReason::Mode0SplitLayers:
            return "mode0-split-layers";
        case GoldenSunWidePolicyReason::Mode0SplitGeometry:
            return "mode0-split-geometry";
        case GoldenSunWidePolicyReason::Mode0SplitScrollMismatch:
            return "mode0-split-scroll-mismatch";
    }
    return "unknown";
}

void report_golden_sun_margin_diagnostic_sample(
    std::uint64_t frame, const char* reason,
    const gba::WsMarginDiagnostics& sample) {
    const auto final = [&](std::size_t layer, std::size_t source) {
        return sample.final_selected[layer][source];
    };
    const auto horizontal = [&](std::size_t side, std::size_t layer,
                                std::size_t source) {
        return sample.horizontal_final_selected[side][layer][source];
    };
    const auto provider = [&](std::size_t bg, std::size_t result) {
        return sample.provider_results[bg][result];
    };
    const auto u64 = [](std::uint64_t value) {
        return static_cast<unsigned long long>(value);
    };
    std::fprintf(
        stderr,
        "[wide-margin] frame=%llu auth_epoch=%llu reason=%s "
        "pixels=%llu left=%llu right=%llu top=%llu bottom=%llu "
        "provider=bg0:%llu/%llu/%llu,bg1:%llu/%llu/%llu,bg2:%llu/%llu/%llu,bg3:%llu/%llu/%llu "
        "final_bg0=%llu/%llu/%llu final_bg1=%llu/%llu/%llu "
        "final_bg2=%llu/%llu/%llu final_bg3=%llu/%llu/%llu "
        "final_obj=%llu final_backdrop=%llu final_pillarbox=%llu "
        "final_forced_blank=%llu "
        "left_bg0=%llu/%llu/%llu left_bg1=%llu/%llu/%llu "
        "left_bg2=%llu/%llu/%llu left_bg3=%llu/%llu/%llu "
        "right_bg0=%llu/%llu/%llu right_bg1=%llu/%llu/%llu "
        "right_bg2=%llu/%llu/%llu right_bg3=%llu/%llu/%llu "
        "right_obj=%llu right_backdrop=%llu right_pillarbox=%llu\n",
        u64(frame), u64(g_golden_sun_field_auth_epoch), reason,
        u64(sample.margin_pixels), u64(sample.left_margin_pixels),
        u64(sample.right_margin_pixels), u64(sample.top_margin_pixels),
        u64(sample.bottom_margin_pixels),
        u64(provider(0, gba::kWsMarginProviderReplace)),
        u64(provider(0, gba::kWsMarginProviderKeepWrapped)),
        u64(provider(0, gba::kWsMarginProviderUnavailable)),
        u64(provider(1, gba::kWsMarginProviderReplace)),
        u64(provider(1, gba::kWsMarginProviderKeepWrapped)),
        u64(provider(1, gba::kWsMarginProviderUnavailable)),
        u64(provider(2, gba::kWsMarginProviderReplace)),
        u64(provider(2, gba::kWsMarginProviderKeepWrapped)),
        u64(provider(2, gba::kWsMarginProviderUnavailable)),
        u64(provider(3, gba::kWsMarginProviderReplace)),
        u64(provider(3, gba::kWsMarginProviderKeepWrapped)),
        u64(provider(3, gba::kWsMarginProviderUnavailable)),
        u64(final(gba::kWsMarginTraceBg0, gba::kWsMarginSourceWrapped)),
        u64(final(gba::kWsMarginTraceBg0,
                  gba::kWsMarginSourceProviderReplace)),
        u64(final(gba::kWsMarginTraceBg0,
                  gba::kWsMarginSourceProviderKeepWrapped)),
        u64(final(gba::kWsMarginTraceBg1, gba::kWsMarginSourceWrapped)),
        u64(final(gba::kWsMarginTraceBg1,
                  gba::kWsMarginSourceProviderReplace)),
        u64(final(gba::kWsMarginTraceBg1,
                  gba::kWsMarginSourceProviderKeepWrapped)),
        u64(final(gba::kWsMarginTraceBg2, gba::kWsMarginSourceWrapped)),
        u64(final(gba::kWsMarginTraceBg2,
                  gba::kWsMarginSourceProviderReplace)),
        u64(final(gba::kWsMarginTraceBg2,
                  gba::kWsMarginSourceProviderKeepWrapped)),
        u64(final(gba::kWsMarginTraceBg3, gba::kWsMarginSourceWrapped)),
        u64(final(gba::kWsMarginTraceBg3,
                  gba::kWsMarginSourceProviderReplace)),
        u64(final(gba::kWsMarginTraceBg3,
                  gba::kWsMarginSourceProviderKeepWrapped)),
        u64(final(gba::kWsMarginTraceObj, gba::kWsMarginSourceObj)),
        u64(final(gba::kWsMarginTraceBackdrop, gba::kWsMarginSourceBackdrop)),
        u64(final(gba::kWsMarginTracePillarbox,
                  gba::kWsMarginSourcePillarbox)),
        u64(final(gba::kWsMarginTraceForcedBlank,
                  gba::kWsMarginSourceForcedBlank)),
        u64(horizontal(0, gba::kWsMarginTraceBg0,
                        gba::kWsMarginSourceWrapped)),
        u64(horizontal(0, gba::kWsMarginTraceBg0,
                        gba::kWsMarginSourceProviderReplace)),
        u64(horizontal(0, gba::kWsMarginTraceBg0,
                        gba::kWsMarginSourceProviderKeepWrapped)),
        u64(horizontal(0, gba::kWsMarginTraceBg1,
                        gba::kWsMarginSourceWrapped)),
        u64(horizontal(0, gba::kWsMarginTraceBg1,
                        gba::kWsMarginSourceProviderReplace)),
        u64(horizontal(0, gba::kWsMarginTraceBg1,
                        gba::kWsMarginSourceProviderKeepWrapped)),
        u64(horizontal(0, gba::kWsMarginTraceBg2,
                        gba::kWsMarginSourceWrapped)),
        u64(horizontal(0, gba::kWsMarginTraceBg2,
                        gba::kWsMarginSourceProviderReplace)),
        u64(horizontal(0, gba::kWsMarginTraceBg2,
                        gba::kWsMarginSourceProviderKeepWrapped)),
        u64(horizontal(0, gba::kWsMarginTraceBg3,
                        gba::kWsMarginSourceWrapped)),
        u64(horizontal(0, gba::kWsMarginTraceBg3,
                        gba::kWsMarginSourceProviderReplace)),
        u64(horizontal(0, gba::kWsMarginTraceBg3,
                        gba::kWsMarginSourceProviderKeepWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceBg0,
                        gba::kWsMarginSourceWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceBg0,
                        gba::kWsMarginSourceProviderReplace)),
        u64(horizontal(1, gba::kWsMarginTraceBg0,
                        gba::kWsMarginSourceProviderKeepWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceBg1,
                        gba::kWsMarginSourceWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceBg1,
                        gba::kWsMarginSourceProviderReplace)),
        u64(horizontal(1, gba::kWsMarginTraceBg1,
                        gba::kWsMarginSourceProviderKeepWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceBg2,
                        gba::kWsMarginSourceWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceBg2,
                        gba::kWsMarginSourceProviderReplace)),
        u64(horizontal(1, gba::kWsMarginTraceBg2,
                        gba::kWsMarginSourceProviderKeepWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceBg3,
                        gba::kWsMarginSourceWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceBg3,
                        gba::kWsMarginSourceProviderReplace)),
        u64(horizontal(1, gba::kWsMarginTraceBg3,
                        gba::kWsMarginSourceProviderKeepWrapped)),
        u64(horizontal(1, gba::kWsMarginTraceObj, gba::kWsMarginSourceObj)),
        u64(horizontal(1, gba::kWsMarginTraceBackdrop,
                        gba::kWsMarginSourceBackdrop)),
        u64(horizontal(1, gba::kWsMarginTracePillarbox,
                        gba::kWsMarginSourcePillarbox)));
}

void golden_sun_wide_margin_diagnostics_callback(
    const gba::WsMarginDiagnostics& sample) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    ++g_golden_sun_margin_diagnostic_callbacks;
    g_golden_sun_margin_diagnostic_last = sample;
    accumulate_golden_sun_margin_diagnostics(
        g_golden_sun_margin_diagnostic_total, sample);
    const std::uint64_t frame = runtime_current_frame();
    if (g_golden_sun_margin_diagnostic_logged >= 32u ||
        (g_golden_sun_margin_diagnostic_last_log_frame != UINT64_MAX &&
         frame < g_golden_sun_margin_diagnostic_last_log_frame + 120u)) {
        return;
    }
    ++g_golden_sun_margin_diagnostic_logged;
    g_golden_sun_margin_diagnostic_last_log_frame = frame;
    report_golden_sun_margin_diagnostic_sample(frame, "frame", sample);
}

struct GoldenSunFieldTableStats {
    std::uint64_t writes = 0;
    std::uint64_t bytes = 0;
    std::uint64_t first_frame = UINT64_MAX;
    std::uint64_t last_frame = UINT64_MAX;
    std::uint32_t first_pc = 0;
    std::uint32_t last_pc = 0;
};
GoldenSunFieldTableStats g_golden_sun_field_map_writes;
GoldenSunFieldTableStats g_golden_sun_field_raw_writes;
GoldenSunFieldTableStats g_golden_sun_field_map_dma_writes;
GoldenSunFieldTableStats g_golden_sun_field_raw_dma_writes;

// The detailed per-store trace is intentionally capped at 64 records per
// authentication epoch. That cap hid raw-table producers when map stores
// arrived first, so keep a separate payload-free histogram/range summary keyed
// by writer PC. It is bounded, reset per epoch, and diagnostics-only.
constexpr std::size_t kGoldenSunFieldProducerLimit = 32u;
struct GoldenSunFieldProducerStats {
    bool used = false;
    std::uint32_t pc = 0;
    std::uint64_t writes = 0;
    std::uint64_t bytes = 0;
    std::uint64_t first_frame = UINT64_MAX;
    std::uint64_t last_frame = UINT64_MAX;
    std::uint32_t min_address = UINT32_MAX;
    std::uint32_t max_end = 0;
};
std::array<GoldenSunFieldProducerStats, kGoldenSunFieldProducerLimit>
    g_golden_sun_field_map_producers{};
std::array<GoldenSunFieldProducerStats, kGoldenSunFieldProducerLimit>
    g_golden_sun_field_raw_producers{};
unsigned g_golden_sun_field_map_producer_overflow = 0;
unsigned g_golden_sun_field_raw_producer_overflow = 0;

void reset_golden_sun_field_producers() {
    g_golden_sun_field_map_producers = {};
    g_golden_sun_field_raw_producers = {};
    g_golden_sun_field_map_producer_overflow = 0;
    g_golden_sun_field_raw_producer_overflow = 0;
}

void record_golden_sun_field_producer(
    std::array<GoldenSunFieldProducerStats, kGoldenSunFieldProducerLimit>&
        producers,
    unsigned* overflow, std::uint32_t pc, std::uint32_t address,
    std::uint32_t size, std::uint64_t bytes, std::uint64_t frame) {
    if (!overflow || size == 0u || bytes == 0u) return;
    GoldenSunFieldProducerStats* producer = nullptr;
    for (auto& candidate : producers) {
        if (candidate.used && candidate.pc == pc) {
            producer = &candidate;
            break;
        }
    }
    if (!producer) {
        for (auto& candidate : producers) {
            if (!candidate.used) {
                candidate = {};
                candidate.used = true;
                candidate.pc = pc;
                producer = &candidate;
                break;
            }
        }
    }
    if (!producer) {
        ++*overflow;
        return;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(address) + size;
    ++producer->writes;
    producer->bytes += bytes;
    producer->first_frame = std::min(producer->first_frame, frame);
    producer->last_frame = producer->writes == 1u
        ? frame : std::max(producer->last_frame, frame);
    producer->min_address = std::min(producer->min_address, address);
    producer->max_end = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        end, UINT32_MAX));
}

void report_golden_sun_field_producers(const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled() || !reason) return;
    const auto report = [&](const char* table,
                            const auto& producers, unsigned overflow) {
        for (const auto& producer : producers) {
            if (!producer.used) continue;
            std::fprintf(
                stderr,
                "[wide-field-producer] auth_epoch=%llu reason=%s "
                "table=%s writer_pc=0x%08x writes=%llu bytes=%llu "
                "addr=0x%08x..0x%08x frames=%llu..%llu\n",
                static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                reason, table, producer.pc,
                static_cast<unsigned long long>(producer.writes),
                static_cast<unsigned long long>(producer.bytes),
                producer.min_address, producer.max_end,
                static_cast<unsigned long long>(producer.first_frame),
                static_cast<unsigned long long>(producer.last_frame));
        }
        if (overflow != 0u) {
            std::fprintf(stderr,
                         "[wide-field-producer-overflow] auth_epoch=%llu "
                         "reason=%s table=%s producers=%u\n",
                         static_cast<unsigned long long>(
                             g_golden_sun_field_auth_epoch),
                         reason, table, overflow);
        }
    };
    report("map", g_golden_sun_field_map_producers,
           g_golden_sun_field_map_producer_overflow);
    report("raw", g_golden_sun_field_raw_producers,
           g_golden_sun_field_raw_producer_overflow);
}

// WIDE-01 entity-producer census: bounded IWRAM write census for the
// previously measured 0x38-stride object staging prefix at
// 0x03002000..0x030022E0. The newly authenticated N=15 body is admitted by
// the commit identity rule, but this write census stays at its old boundary
// until its producer writes are measured directly. Diagnostics-only, deduped
// by (address, pc, size), capped per epoch, and
// reset with the same auth-epoch boundaries as the field producer trace
// above. Produces nothing unless widescreen diagnostics are enabled; never
// consulted by gameplay/render code.
constexpr std::uint32_t kGoldenSunObjRecordCensusStart = 0x03002000u;
constexpr std::uint32_t kGoldenSunObjRecordCensusEnd = 0x030022E0u;
// The 64-entry cap truncated (Bilibin overflow ~20005, Palace ~2912 write
// *events*, not unique keys -- overflow increments per rejected write, not
// per distinct key). The address range is exactly 0x2E0 = 736 bytes; the
// worst pattern measured so far is one writer identity per byte (Palace's
// sequential fill) and a handful of distinct (pc,size) writers per byte in
// the steady-state field case. 4096 covers 736 bytes x up to ~5 distinct
// (pc,size) identities per byte with headroom, while staying a few hundred
// KB and a bounded linear scan restricted to writes inside this one 736-byte
// window (never the hot path for the rest of guest memory).
constexpr std::size_t kGoldenSunObjRecordCensusLimit = 4096u;
struct GoldenSunObjRecordCensusStat {
    bool used = false;
    std::uint32_t address = 0;
    std::uint32_t pc = 0;
    std::uint32_t size = 0;
    std::uint64_t writes = 0;
    std::uint64_t first_frame = UINT64_MAX;
    std::uint64_t last_frame = UINT64_MAX;
};
std::array<GoldenSunObjRecordCensusStat, kGoldenSunObjRecordCensusLimit>
    g_golden_sun_obj_record_census{};
unsigned g_golden_sun_obj_record_census_overflow = 0;

void reset_golden_sun_obj_record_census() {
    g_golden_sun_obj_record_census = {};
    g_golden_sun_obj_record_census_overflow = 0;
}

void note_golden_sun_obj_record_write(std::uint32_t pc, std::uint32_t address,
                                      std::uint32_t size) {
    if (size == 0u) return;
    const std::uint64_t first = address;
    const std::uint64_t last = static_cast<std::uint64_t>(address) + size;
    if (last <= kGoldenSunObjRecordCensusStart ||
        first >= kGoldenSunObjRecordCensusEnd) return;
    const std::uint64_t frame = runtime_current_frame();
    GoldenSunObjRecordCensusStat* stat = nullptr;
    for (auto& candidate : g_golden_sun_obj_record_census) {
        if (candidate.used && candidate.address == address &&
            candidate.pc == pc && candidate.size == size) {
            stat = &candidate;
            break;
        }
    }
    if (!stat) {
        for (auto& candidate : g_golden_sun_obj_record_census) {
            if (!candidate.used) {
                candidate = {};
                candidate.used = true;
                candidate.address = address;
                candidate.pc = pc;
                candidate.size = size;
                stat = &candidate;
                break;
            }
        }
    }
    if (!stat) {
        ++g_golden_sun_obj_record_census_overflow;
        return;
    }
    ++stat->writes;
    stat->first_frame = std::min(stat->first_frame, frame);
    stat->last_frame = stat->writes == 1u ? frame
                                          : std::max(stat->last_frame, frame);
}

void report_golden_sun_obj_record_census(const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled() || !reason) return;
    for (const auto& stat : g_golden_sun_obj_record_census) {
        if (!stat.used) continue;
        std::fprintf(
            stderr,
            "[wide-obj-record-writer] auth_epoch=%llu reason=%s "
            "addr=0x%08x pc=0x%08x size=%u writes=%llu frames=%llu..%llu\n",
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            reason, stat.address, stat.pc, stat.size,
            static_cast<unsigned long long>(stat.writes),
            static_cast<unsigned long long>(stat.first_frame),
            static_cast<unsigned long long>(stat.last_frame));
    }
    if (g_golden_sun_obj_record_census_overflow != 0u) {
        std::fprintf(stderr,
                     "[wide-obj-record-writer-overflow] auth_epoch=%llu "
                     "reason=%s overflow=%u\n",
                     static_cast<unsigned long long>(
                         g_golden_sun_field_auth_epoch),
                     reason, g_golden_sun_obj_record_census_overflow);
    }
}

// WIDE-01 entity-producer census: bounded value-event trace for the same
// 0x38-stride record, restricted to relative offsets 0x00..0x13 of each
// slot -- the region under test for a signed X/Y correlation against
// [wide-obj-stage]/[wide-obj-handoff]. The offset window is layout-relative
// (not tied to any specific map), so it applies unchanged to a different
// town using the same record shape. Values logged here are decoded guest
// game state (small integers/coordinates), not ROM/asset payload bytes.
// Diagnostics-only, capped, and reset with the same epoch boundary as the
// rest of this census.
constexpr std::uint32_t kGoldenSunObjRecordValueOffsetEnd = 0x14u;
constexpr std::size_t kGoldenSunObjRecordValueLimit = 4096u;
struct GoldenSunObjRecordValueEvent {
    std::uint64_t frame = 0;
    std::uint32_t address = 0;
    std::uint32_t pc = 0;
    std::uint32_t size = 0;
    std::uint32_t value = 0;
};
std::array<GoldenSunObjRecordValueEvent, kGoldenSunObjRecordValueLimit>
    g_golden_sun_obj_record_value_events{};
std::size_t g_golden_sun_obj_record_value_count = 0;
std::uint64_t g_golden_sun_obj_record_value_dropped = 0;

void reset_golden_sun_obj_record_value_events() {
    g_golden_sun_obj_record_value_count = 0;
    g_golden_sun_obj_record_value_dropped = 0;
}

void note_golden_sun_obj_record_value(std::uint32_t pc, std::uint32_t address,
                                      std::uint32_t size) {
    if (address < kGoldenSunObjRecordCensusStart) return;
    if (address >= kGoldenSunObjRecordCensusEnd) return;
    const std::uint32_t offset_in_slot =
        (address - kGoldenSunObjRecordCensusStart) % 0x38u;
    if (offset_in_slot >= kGoldenSunObjRecordValueOffsetEnd) return;
    if (g_golden_sun_obj_record_value_count >=
            kGoldenSunObjRecordValueLimit) {
        ++g_golden_sun_obj_record_value_dropped;
        return;
    }
    std::uint32_t value = 0;
    switch (size) {
        case 1u: value = bus_read_u8(address); break;
        case 2u: value = bus_read_u16(address); break;
        case 4u: value = bus_read_u32(address); break;
        default: return;
    }
    auto& event = g_golden_sun_obj_record_value_events[
        g_golden_sun_obj_record_value_count++];
    event.frame = runtime_current_frame();
    event.address = address;
    event.pc = pc;
    event.size = size;
    event.value = value;
}

void report_golden_sun_obj_record_values(const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled() || !reason) return;
    for (std::size_t i = 0; i < g_golden_sun_obj_record_value_count; ++i) {
        const auto& event = g_golden_sun_obj_record_value_events[i];
        std::fprintf(
            stderr,
            "[wide-obj-record-value] auth_epoch=%llu reason=%s frame=%llu "
            "addr=0x%08x pc=0x%08x size=%u value=0x%08x\n",
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            reason, static_cast<unsigned long long>(event.frame),
            event.address, event.pc, event.size, event.value);
    }
    if (g_golden_sun_obj_record_value_dropped != 0u) {
        std::fprintf(stderr,
                     "[wide-obj-record-value-overflow] auth_epoch=%llu "
                     "reason=%s dropped=%llu\n",
                     static_cast<unsigned long long>(
                         g_golden_sun_field_auth_epoch),
                     reason, static_cast<unsigned long long>(
                         g_golden_sun_obj_record_value_dropped));
    }
}

// WIDE-01 body/shadow identity: bounded per-frame write-order trace of the
// OAM-shadow commit, across all three Func_1dc8 writer routes (D4/EC/F0 --
// golden_sun_obj_staging_handoff, golden_sun_obj_f0_entry_capture, and
// golden_sun_oam_shadow_write_observer respectively; see
// src/relocatable_writer_policy.h for the route offsets). Records each
// committed write in the exact order the guest performs it within a frame,
// tagged with its route, so a body write and its shadow write for the same
// in-game entity can later be matched by adjacent sequence numbers within
// one frame regardless of which route produced them. Diagnostics-only,
// deduped against the immediately preceding entry for the same (route,
// slot) pair (skips frames where that slot's committed source/ATTR/
// coordinates are unchanged from the previous frame on that same route --
// this is the "only log frames where the set of active slots changes"
// bound), capped, and reset at the same auth-epoch boundaries as the rest
// of this census. Never consulted by gameplay/render code. See
// docs/issues/WIDE-01_NPC_IDENTITY.md.
// (GoldenSunObjCommitOrderRoute is forward-declared near the top of this
// file, alongside note_golden_sun_obj_commit_order.)
constexpr const char* golden_sun_obj_commit_order_route_name(
    GoldenSunObjCommitOrderRoute route) {
    switch (route) {
        case GoldenSunObjCommitOrderRoute::D4: return "D4";
        case GoldenSunObjCommitOrderRoute::EC: return "EC";
        case GoldenSunObjCommitOrderRoute::F0: return "F0";
    }
    return "?";
}
// 256 was sized for the D4 route alone (3 events dropped once EC/F0 were
// added in a follow-up pass); 512 covers all three routes with headroom
// while staying well under the per-epoch volume that caused the
// synchronous-fprintf transition stalls documented in
// WIDE-01_ENTITY_PRODUCER_FINDINGS.md ("Performance").
constexpr std::size_t kGoldenSunObjCommitOrderLimit = 512u;
constexpr std::size_t kGoldenSunObjCommitOrderSlots = 128u;
constexpr std::size_t kGoldenSunObjCommitOrderRouteCount = 3u;
struct GoldenSunObjCommitOrderEvent {
    std::uint64_t frame = 0;
    std::uint32_t sequence = 0;
    std::uint32_t source = 0;
    int slot = -1;
    GoldenSunObjCommitOrderRoute route = GoldenSunObjCommitOrderRoute::D4;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    std::int16_t logical_x = 0;
    std::int16_t logical_y = 0;
    bool x_valid = false;
    bool y_valid = false;
    bool used = false;
};
std::array<GoldenSunObjCommitOrderEvent, kGoldenSunObjCommitOrderLimit>
    g_golden_sun_obj_commit_order_events{};
std::size_t g_golden_sun_obj_commit_order_count = 0;
std::uint64_t g_golden_sun_obj_commit_order_dropped = 0;
std::uint64_t g_golden_sun_obj_commit_order_frame = UINT64_MAX;
std::uint32_t g_golden_sun_obj_commit_order_sequence_in_frame = 0;
// Last committed identity per (route, OAM slot), used only to dedupe
// repeat writes; never consulted by gameplay/render code.
std::array<std::array<GoldenSunObjCommitOrderEvent, kGoldenSunObjCommitOrderSlots>,
           kGoldenSunObjCommitOrderRouteCount>
    g_golden_sun_obj_commit_order_last_by_route_slot{};

void reset_golden_sun_obj_commit_order() {
    g_golden_sun_obj_commit_order_count = 0;
    g_golden_sun_obj_commit_order_dropped = 0;
    g_golden_sun_obj_commit_order_frame = UINT64_MAX;
    g_golden_sun_obj_commit_order_sequence_in_frame = 0;
    g_golden_sun_obj_commit_order_last_by_route_slot = {};
}

void note_golden_sun_obj_commit_order(GoldenSunObjCommitOrderRoute route,
                                      std::uint32_t source, int slot,
                                      std::uint16_t attr0,
                                      std::uint16_t attr1,
                                      std::uint16_t attr2, bool x_valid,
                                      std::int16_t logical_x, bool y_valid,
                                      std::int16_t logical_y) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    if (slot >= static_cast<int>(kGoldenSunObjCommitOrderSlots)) return;
    // EC fires before the destination OAM slot is resolved (that only
    // becomes known at the later F0 write), so it reports slot=-1 --
    // truthfully "not yet assigned", never fabricated. Dedupe such entries
    // by a deterministic bucket derived from the source record address
    // instead of by slot; this is bookkeeping only; the reported event
    // still carries the real slot value (-1) unchanged.
    const std::size_t dedupe_index = slot >= 0
        ? static_cast<std::size_t>(slot)
        : static_cast<std::size_t>((source >> 3) %
                                    kGoldenSunObjCommitOrderSlots);
    const std::uint64_t frame = runtime_current_frame();
    if (frame != g_golden_sun_obj_commit_order_frame) {
        g_golden_sun_obj_commit_order_frame = frame;
        g_golden_sun_obj_commit_order_sequence_in_frame = 0;
    }
    const std::uint32_t sequence =
        g_golden_sun_obj_commit_order_sequence_in_frame++;
    auto& last = g_golden_sun_obj_commit_order_last_by_route_slot[
        static_cast<std::size_t>(route)][dedupe_index];
    // D4 fires only on a gated, successful commit (already low-volume), so
    // it keeps the fine-grained equality check (logs every real coordinate
    // change). EC/F0 are restricted to the shadow-candidate signature
    // (tile=0/shape=1/size=0) at their call sites, but a *moving* shadow's
    // raw ATTR0/ATTR1 (Y/X) still changes almost every frame, which would
    // defeat a fine-grained check the same way the unfiltered routes did --
    // this flooded the cap and stalled scene transitions in an earlier
    // pass. For EC/F0, dedupe coarsely on (route, dedupe_index) alone: log
    // only the first frame this object is seen in the epoch (or the first
    // frame after any absence), not every per-frame position update. That
    // is still enough to answer "does this route ever carry the shadow
    // signature, and where in write order" -- the fine per-frame offset
    // tracking that question also wants comes from D4's own logical X/Y,
    // already fully sampled.
    const bool unchanged = route == GoldenSunObjCommitOrderRoute::D4
        ? (last.used && last.route == route && last.slot == slot &&
           last.source == source && last.attr0 == attr0 &&
           last.attr1 == attr1 && last.attr2 == attr2 &&
           last.x_valid == x_valid && last.y_valid == y_valid &&
           last.logical_x == logical_x && last.logical_y == logical_y)
        : (last.used && last.route == route);
    GoldenSunObjCommitOrderEvent event;
    event.frame = frame;
    event.sequence = sequence;
    event.source = source;
    event.slot = slot;
    event.route = route;
    event.attr0 = attr0;
    event.attr1 = attr1;
    event.attr2 = attr2;
    event.logical_x = logical_x;
    event.logical_y = logical_y;
    event.x_valid = x_valid;
    event.y_valid = y_valid;
    event.used = true;
    last = event;
    if (unchanged) return;
    if (g_golden_sun_obj_commit_order_count >=
            kGoldenSunObjCommitOrderLimit) {
        ++g_golden_sun_obj_commit_order_dropped;
        return;
    }
    g_golden_sun_obj_commit_order_events[
        g_golden_sun_obj_commit_order_count++] = event;
}

void report_golden_sun_obj_commit_order(const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled() || !reason) return;
    for (std::size_t i = 0; i < g_golden_sun_obj_commit_order_count; ++i) {
        const auto& event = g_golden_sun_obj_commit_order_events[i];
        std::fprintf(
            stderr,
            "[wide-obj-commit-order] auth_epoch=%llu reason=%s frame=%llu "
            "sequence=%u route=%s source=0x%08x slot=%d attr0=0x%04x "
            "attr1=0x%04x attr2=0x%04x x_valid=%u logical_x=%d y_valid=%u "
            "logical_y=%d\n",
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            reason, static_cast<unsigned long long>(event.frame),
            event.sequence,
            golden_sun_obj_commit_order_route_name(event.route),
            event.source, event.slot, static_cast<unsigned>(event.attr0),
            static_cast<unsigned>(event.attr1),
            static_cast<unsigned>(event.attr2), event.x_valid ? 1u : 0u,
            static_cast<int>(event.logical_x), event.y_valid ? 1u : 0u,
            static_cast<int>(event.logical_y));
    }
    if (g_golden_sun_obj_commit_order_dropped != 0u) {
        std::fprintf(stderr,
                     "[wide-obj-commit-order-overflow] auth_epoch=%llu "
                     "reason=%s dropped=%llu\n",
                     static_cast<unsigned long long>(
                         g_golden_sun_field_auth_epoch),
                     reason, static_cast<unsigned long long>(
                         g_golden_sun_obj_commit_order_dropped));
    }
}

std::uint64_t g_golden_sun_field_epoch_map_writes = 0;
std::uint64_t g_golden_sun_field_epoch_raw_writes = 0;
std::uint64_t g_golden_sun_field_epoch_map_dma_writes = 0;
std::uint64_t g_golden_sun_field_epoch_raw_dma_writes = 0;
unsigned g_golden_sun_field_table_cpu_logs_in_epoch = 0;
unsigned g_golden_sun_field_table_dma_logs_in_epoch = 0;

bool golden_sun_field_table_overlap(std::uint32_t address,
                                    std::uint32_t size,
                                    std::uint32_t table_start,
                                    std::uint32_t table_end,
                                    std::uint64_t* out_bytes) {
    if (size == 0u || !out_bytes) return false;
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    const std::uint64_t lo = std::max<std::uint64_t>(first, table_start);
    const std::uint64_t hi = std::min<std::uint64_t>(last, table_end);
    if (hi <= lo) return false;
    *out_bytes = hi - lo;
    return true;
}

void invalidate_golden_sun_palace_table_authorization() {
    g_golden_sun_palace_table_authorized = false;
    g_golden_sun_palace_table_invalidated = true;
    reset_golden_sun_palace_active_region();
}

void invalidate_golden_sun_palace_table_if_overlapping(
    std::uint32_t address, std::uint32_t size) {
    std::uint64_t overlap = 0;
    if (golden_sun_field_table_overlap(
            address, size, kGoldenSunFieldMapAddress,
            kGoldenSunFieldMapAddress + 0x10000u, &overlap) ||
        golden_sun_field_table_overlap(
            address, size, kGoldenSunFieldRawAddress,
            kGoldenSunFieldRawAddress + 0x8000u, &overlap)) {
        invalidate_golden_sun_palace_table_authorization();
    }
}

void record_golden_sun_field_table_write(std::uint32_t address,
                                         std::uint32_t size) {
    if (!golden_sun_wide_diagnostics_enabled() || size == 0u) return;
    const std::uint64_t frame = runtime_current_frame();
    const std::uint32_t pc = runtime_current_pc();
    const auto record = [&](const char* table_name,
                            std::uint32_t table_start,
                            std::uint32_t table_end,
                            GoldenSunFieldTableStats* stats,
                            std::uint64_t* epoch_writes) {
        std::uint64_t bytes = 0;
        if (!golden_sun_field_table_overlap(
                address, size, table_start, table_end, &bytes)) {
            return;
        }
        ++stats->writes;
        stats->bytes += bytes;
        if (stats->first_frame == UINT64_MAX) {
            stats->first_frame = frame;
            stats->first_pc = pc;
        }
        stats->last_frame = frame;
        stats->last_pc = pc;
        ++*epoch_writes;
        if (table_start == kGoldenSunFieldMapAddress) {
            record_golden_sun_field_producer(
                g_golden_sun_field_map_producers,
                &g_golden_sun_field_map_producer_overflow, pc, address,
                size, bytes, frame);
        } else {
            record_golden_sun_field_producer(
                g_golden_sun_field_raw_producers,
                &g_golden_sun_field_raw_producer_overflow, pc, address,
                size, bytes, frame);
        }
        if (g_golden_sun_field_table_cpu_logs_in_epoch >=
                kGoldenSunFieldTableCpuLogLimitPerEpoch) {
            return;
        }
        ++g_golden_sun_field_table_cpu_logs_in_epoch;
        std::fprintf(
            stderr,
            "[wide-field-table] frame=%llu auth_epoch=%llu table=%s "
            "writer_pc=0x%08x addr=0x%08x size=%u overlap=%llu reason=%s\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            table_name, pc, address, size,
            static_cast<unsigned long long>(bytes),
            golden_sun_wide_policy_reason_name(
                g_golden_sun_wide_policy_last_reason));
    };
    record("map", kGoldenSunFieldMapAddress,
           kGoldenSunFieldMapAddress + 0x10000u,
           &g_golden_sun_field_map_writes, &g_golden_sun_field_epoch_map_writes);
    record("raw", kGoldenSunFieldRawAddress,
           kGoldenSunFieldRawAddress + 0x8000u,
           &g_golden_sun_field_raw_writes, &g_golden_sun_field_epoch_raw_writes);
}

bool golden_sun_dma_destination_bounds(std::uint32_t destination,
                                       std::uint32_t bytes,
                                       std::uint16_t control,
                                       std::uint32_t* out_first,
                                       std::uint32_t* out_size) {
    if (!out_first || !out_size || bytes == 0u) return false;
    const std::uint32_t step = (control & 0x0400u) != 0u ? 4u : 2u;
    if (bytes < step || (bytes % step) != 0u) return false;
    const std::uint32_t dest_control = (control >> 5) & 3u;
    const std::uint64_t first = dest_control == 1u
        ? (destination >= bytes - step
              ? static_cast<std::uint64_t>(destination) - (bytes - step)
              : 0u)
        : destination;
    const std::uint64_t last = dest_control == 2u
        ? static_cast<std::uint64_t>(destination) + step
        : (dest_control == 1u
              ? static_cast<std::uint64_t>(destination) + step
              : static_cast<std::uint64_t>(destination) + bytes);
    if (last <= first || last > 0x100000000ull) return false;
    *out_first = static_cast<std::uint32_t>(first);
    *out_size = static_cast<std::uint32_t>(last - first);
    return true;
}

void record_golden_sun_shadow_dma_slots(
    std::uint32_t pc, std::uint32_t source, std::uint32_t destination,
    std::uint32_t bytes, std::uint16_t control, int start_mode) {
    if (!gsr::obj_recorder_enabled() || bytes == 0u) return;
    const std::uint32_t unit = (control & 0x0400u) != 0u ? 4u : 2u;
    if ((bytes % unit) != 0u) return;
    const std::uint32_t source_mode = (control >> 7) & 3u;
    const std::uint32_t destination_mode = (control >> 5) & 3u;
    if (source_mode > 2u || destination_mode > 3u) return;
    std::uint32_t range_first = 0;
    std::uint32_t range_size = 0;
    if (!golden_sun_dma_destination_bounds(
            destination, bytes, control, &range_first, &range_size)) return;
    const std::uint64_t range_last =
        static_cast<std::uint64_t>(range_first) + range_size;
    const std::uint32_t tables[] = {
        gsr::widescreen::kGoldenSunOamShadowStart,
        gsr::widescreen::kGoldenSunOamShadowAltStart};
    const auto source_u16_for_destination = [&](std::uint32_t address,
                                                std::uint16_t* out) {
        if (!out) return false;
        const std::uint64_t d = address;
        const std::uint64_t start = destination;
        std::uint64_t index = 0;
        std::uint64_t intra = 0;
        if (destination_mode == 0u || destination_mode == 3u) {
            if (d < start) return false;
            const std::uint64_t delta = d - start;
            index = delta / unit;
            intra = delta % unit;
        } else if (destination_mode == 1u) {
            // Units descend; bytes within each unit still ascend.
            if (d >= start) {
                intra = d - start;
            } else {
                const std::uint64_t delta = start - d;
                index = (delta + unit - 1u) / unit;
                intra = index * unit - delta;
            }
        } else {
            if (d < start) return false;
            // A fixed destination retains the final source unit.
            index = bytes / unit - 1u;
            intra = d - start;
        }
        if (index >= bytes / unit) return false;
        const std::uint64_t source_delta = index * unit;
        std::uint64_t source_address = source;
        if (source_mode == 0u) {
            source_address += source_delta;
        } else if (source_mode == 1u) {
            if (source_delta > source) return false;
            source_address -= source_delta;
        }
        if (intra + 2u > unit ||
            source_address > 0xFFFFFFFFull -
                intra)
            return false;
        *out = bus_read_u16(static_cast<std::uint32_t>(source_address +
                                                        intra));
        return true;
    };
    for (const std::uint32_t table : tables) {
        const std::uint64_t table_first = table;
        const std::uint64_t table_last = table_first +
            gsr::widescreen::kGoldenSunOamBytes;
        if (range_first >= table_last || range_last <= table_first) continue;
        const int first_slot = static_cast<int>(
            (std::max<std::uint64_t>(range_first, table_first) -
             table_first) / gsr::widescreen::kGoldenSunOamShadowSlotBytes);
        const int last_slot = static_cast<int>(
            (std::min<std::uint64_t>(range_last, table_last) - 1u -
             table_first) / gsr::widescreen::kGoldenSunOamShadowSlotBytes);
        for (int slot = first_slot; slot <= last_slot; ++slot) {
            const std::uint32_t slot_address = table +
                static_cast<std::uint32_t>(slot) *
                    gsr::widescreen::kGoldenSunOamShadowSlotBytes;
            std::uint8_t attr_mask = 0;
            std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
            if (source_u16_for_destination(slot_address, &attr0))
                attr_mask |= 1u;
            if (source_u16_for_destination(slot_address + 2u, &attr1))
                attr_mask |= 2u;
            if (source_u16_for_destination(slot_address + 4u, &attr2))
                attr_mask |= 4u;
            gsr::ObjShadowWriteSample sample;
            sample.event = "dma";
            sample.frame = runtime_current_frame();
            sample.epoch = g_golden_sun_field_auth_epoch;
            sample.dma = g_obj_lifetime_dma;
            sample.slot = slot;
            sample.writer_pc = pc;
            sample.size = bytes;
            sample.source = source;
            sample.destination = destination;
            sample.control = control;
            sample.start_mode = start_mode;
            sample.attr_mask = attr_mask;
            sample.attr0 = attr0;
            sample.attr1 = attr1;
            sample.attr2 = attr2;
            gsr::obj_recorder_note_shadow_write(sample);
        }
    }
}

// Per-OAM-upload tally of which sprite table an upload's source address
// belongs to: 0 primary (kGoldenSunOamShadowStart), 1 alt
// (kGoldenSunOamShadowAltStart), 2 the unrecognised-geometry 0x03002000
// address, 3 anything else. An "upload" is any DMA with destination
// kGoldenSunOamStart and bytes == kGoldenSunOamBytes, regardless of whether
// golden_sun_obj_provenance_dma_handoff() publishes it -- this only counts,
// it never changes what gets published. Measurement for FACTS.md 2026-09-06
// / 2026-09-11: which table is live during the Move Psynergy collapse.
// Accumulated monotonically, never reset here; the tracer reads the
// per-frame delta. Same plain-array, C-linkage pattern as
// g_ws_obj_reject_totals below.
extern "C" unsigned long long g_ws_oam_src_totals[4] = {0, 0, 0, 0};

// Diagnostic-only snapshot shared by upload and render events.
void note_obj_lifetime(const char* event, const char* reason, int slot,
    std::uint32_t source, std::uint16_t a0, std::uint16_t a1, std::uint16_t a2,
    const GoldenSunObjPlacementProvenance& p) {
    gsr::ObjLifetimeSample d;
    d.event = event; d.reason = reason;
    d.frame = runtime_current_frame(); d.epoch = g_golden_sun_field_auth_epoch;
    d.dma = g_obj_lifetime_dma; d.slot = slot; d.source = source;
    d.attr0 = a0; d.attr1 = a1; d.attr2 = a2;
    d.provenance_frame = p.frame; d.provenance_epoch = p.auth_epoch;
    d.target = p.target_address;
    d.expected0 = p.expected_attr0; d.expected1 = p.expected_attr1;
    d.expected2 = p.expected_attr2;
    d.checks = (p.valid ? 1u : 0u) | (p.x_valid ? 2u : 0u) |
        (p.y_valid ? 4u : 0u) | (p.oam_identity_valid ? 8u : 0u) |
        (p.commit_resolved ? 16u : 0u);
    gsr::obj_recorder_note_lifetime(d);
}

void golden_sun_wide_dma_descriptor_observer(
    int channel, std::uint32_t pc, std::uint32_t source,
    std::uint32_t destination, std::uint32_t bytes, std::uint16_t control,
    int start_mode) {
    // Read the source image before the transfer while the descriptor still
    // identifies the guest producer. This closes the missing table-DMA side
    // of the failing identity without retaining guest bytes.
    record_golden_sun_shadow_dma_slots(
        pc, source, destination, bytes, control, start_mode);
    // The descriptor observer runs immediately before the DMA transfer. At
    // this point the shadow is the complete image that is about to become
    // visible OAM, while later shadow writes must wait for the next handoff.
    // Publish only this exact measured 1024-byte transfer; partial/unrelated
    // DMAs must never make pending provenance visible.
    if (gsr::widescreen::golden_sun_obj_provenance_dma_handoff(
            source, destination, bytes, control)) {
        g_golden_sun_obj_visible_provenance =
            g_golden_sun_obj_pending_provenance;
        ++g_golden_sun_obj_provenance_generation;
    }
    // Count every OAM upload by source table, independent of diagnostics
    // flags and of whether this exact transfer gets published above -- see
    // g_ws_oam_src_totals.
    if (destination == gsr::widescreen::kGoldenSunOamStart &&
        bytes == gsr::widescreen::kGoldenSunOamBytes) {
        const std::size_t bucket =
            source == gsr::widescreen::kGoldenSunOamShadowStart ? 0u :
            source == gsr::widescreen::kGoldenSunOamShadowAltStart ? 1u :
            source == 0x03002000u ? 2u : 3u;
        ++g_ws_oam_src_totals[bucket];
    }
    if (gsr::obj_recorder_enabled() && destination == 0x07000000u &&
        bytes == gsr::widescreen::kGoldenSunOamShadowSlotCount *
            gsr::widescreen::kGoldenSunOamShadowSlotBytes) {
        ++g_obj_lifetime_dma;
        const bool published = gsr::widescreen::golden_sun_obj_provenance_dma_handoff(
            source, destination, bytes, control);
        for (std::size_t slot = 0; slot < g_golden_sun_obj_visible_provenance.size(); ++slot) {
            // Decode the DMA source addressing mode; metadata only, never copy assets.
            const auto source_mode = (control >> 7) & 3u;
            const auto unit = (control & 0x0400u) ? 4u : 2u;
            const auto read_attr = [&](std::uint32_t offset) {
                const auto step = offset / unit * unit;
                const auto address = source_mode == 0 ? source + step :
                    source_mode == 1 ? source - step : source;
                return bus_read_u16(address + offset % unit);
            };
            const auto offset = static_cast<std::uint32_t>(slot) *
                gsr::widescreen::kGoldenSunOamShadowSlotBytes;
            note_obj_lifetime("dma", published ? "published" : "not-published",
                static_cast<int>(slot), source, read_attr(offset),
                read_attr(offset + 2u), read_attr(offset + 4u),
                g_golden_sun_obj_visible_provenance[slot]);
        }
    }
    std::uint32_t first = 0;
    std::uint32_t span = 0;
    if (!golden_sun_dma_destination_bounds(
            destination, bytes, control, &first, &span)) return;
    // A DMA copy is still a table write for Palace authorization, even when
    // diagnostics are disabled and no authored-cell bits are changed.
    invalidate_golden_sun_palace_table_if_overlapping(first, span);
    if (!golden_sun_wide_diagnostics_enabled()) return;
    const std::uint64_t frame = runtime_current_frame();
    const auto record = [&](const char* table_name,
                            std::uint32_t table_start,
                            std::uint32_t table_end,
                            GoldenSunFieldTableStats* stats,
                            std::uint64_t* epoch_writes) {
        std::uint64_t overlap = 0;
        if (!golden_sun_field_table_overlap(
                first, span, table_start, table_end, &overlap)) return;
        ++stats->writes;
        stats->bytes += overlap;
        if (stats->first_frame == UINT64_MAX) {
            stats->first_frame = frame;
            stats->first_pc = pc;
        }
        stats->last_frame = frame;
        stats->last_pc = pc;
        ++*epoch_writes;
        if (g_golden_sun_field_table_dma_logs_in_epoch >=
                kGoldenSunFieldTableDmaLogLimitPerEpoch) return;
        ++g_golden_sun_field_table_dma_logs_in_epoch;
        std::fprintf(
            stderr,
            "[wide-field-table-dma] frame=%llu auth_epoch=%llu table=%s "
            "writer_pc=0x%08x src=0x%08x dst=0x%08x size=%u "
            "overlap=%llu channel=%d start_mode=%d cnt_h=0x%04x "
            "reason=%s authored_cells=%u\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            table_name, pc, source, destination, bytes,
            static_cast<unsigned long long>(overlap), channel, start_mode,
            control, golden_sun_wide_policy_reason_name(
                         g_golden_sun_wide_policy_last_reason),
            g_golden_sun_field_authored.authored_count());
    };
    record("map", kGoldenSunFieldMapAddress,
           kGoldenSunFieldMapAddress + 0x10000u,
           &g_golden_sun_field_map_dma_writes,
           &g_golden_sun_field_epoch_map_dma_writes);
    record("raw", kGoldenSunFieldRawAddress,
           kGoldenSunFieldRawAddress + 0x8000u,
           &g_golden_sun_field_raw_dma_writes,
           &g_golden_sun_field_epoch_raw_dma_writes);
}

std::uint32_t golden_sun_field_table_crc(const std::uint8_t* ewram,
                                         std::size_t offset,
                                         std::size_t bytes) {
    return ewram ? gba::crc32(ewram + offset, bytes) : 0u;
}

bool refresh_golden_sun_palace_table_authorization() {
    if (!g_golden_sun_mode0_split_scroll) {
        g_golden_sun_palace_table_authorized = false;
        return false;
    }
    if (g_golden_sun_palace_table_invalidated ||
        g_golden_sun_palace_table_auth_attempted) {
        return g_golden_sun_palace_table_authorized;
    }
    // Fingerprint once after a complete clean split-scroll frame. Subsequent
    // scanlines rely on the CPU/DMA table-write invalidation observers; CRC is
    // deliberately not part of the per-scanline provider path.
    g_golden_sun_palace_table_auth_attempted = true;
    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) {
        g_golden_sun_palace_table_authorized = false;
        return false;
    }
    const std::uint8_t* ewram = bus->ewram_ptr();
    const std::uint32_t map_crc = golden_sun_field_table_crc(
        ewram, kGoldenSunFieldMapOffset, kGoldenSunFieldMapBytes);
    const std::uint32_t raw_crc = golden_sun_field_table_crc(
        ewram, kGoldenSunFieldRawOffset, kGoldenSunFieldRawBytes);
    const bool fingerprint_match =
        gsr::widescreen::golden_sun_palace_table_fingerprint_matches(
            map_crc, raw_crc);
    g_golden_sun_palace_table_authorized = fingerprint_match;
    if (!fingerprint_match) {
        // A mismatch is evidence that this is not the measured Palace table.
        // Latch the failure for this scene/authentication epoch.
        g_golden_sun_palace_table_invalidated = true;
    }
    return g_golden_sun_palace_table_authorized;
}

bool prepare_golden_sun_palace_active_region(
    const gba::GbaBus* bus, const std::uint8_t* io) {
    if (!bus || !io || !g_golden_sun_mode0_split_scroll ||
        !g_golden_sun_palace_table_authorized) {
        return false;
    }
    const std::array<std::uint16_t, 6> scroll{{
        gsr::widescreen::read_io16(io, 0x14u),
        gsr::widescreen::read_io16(io, 0x16u),
        gsr::widescreen::read_io16(io, 0x18u),
        gsr::widescreen::read_io16(io, 0x1Au),
        gsr::widescreen::read_io16(io, 0x1Cu),
        gsr::widescreen::read_io16(io, 0x1Eu),
    }};
    if (!g_golden_sun_palace_region_scroll_valid ||
        g_golden_sun_palace_region_scroll != scroll) {
        g_golden_sun_palace_active_region.reset();
        g_golden_sun_palace_region_scroll = scroll;
        g_golden_sun_palace_region_scroll_valid = true;
        g_golden_sun_palace_region_build_attempted = false;
    }
    if (!g_golden_sun_palace_region_build_attempted) {
        g_golden_sun_palace_region_build_attempted = true;
        g_golden_sun_palace_active_region.build(
            g_golden_sun_wide_line_dispcnt, io, 0x20u, bus->ewram_ptr(),
            256u * 1024u, true);
    }
    return true;
}

void trace_golden_sun_field_map(const std::uint8_t* ewram) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        !ewram || g_golden_sun_wide_field_map_census.size() >= 16u) return;
    const std::uint64_t frame = runtime_current_frame();
    if (g_golden_sun_wide_field_map_trace_frame == frame) return;
    g_golden_sun_wide_field_map_trace_frame = frame;
    const std::uint32_t map_crc = golden_sun_field_table_crc(
        ewram, kGoldenSunFieldMapOffset, kGoldenSunFieldMapBytes);
    const std::uint32_t raw_crc = golden_sun_field_table_crc(
        ewram, kGoldenSunFieldRawOffset, kGoldenSunFieldRawBytes);
    for (const auto& census : g_golden_sun_wide_field_map_census) {
        if (census.auth_epoch == g_golden_sun_field_auth_epoch &&
            census.map_crc == map_crc && census.raw_crc == raw_crc) {
            return;
        }
    }
    g_golden_sun_wide_field_map_census.push_back(
        {g_golden_sun_field_auth_epoch, map_crc, raw_crc});
    std::array<std::uint16_t, 4096> counts{};
    for (std::size_t i = 0; i < 128u * 128u; ++i) {
        const std::size_t off = kGoldenSunFieldMapOffset + i * 4u;
        const std::uint16_t id = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(ewram[off]) |
             static_cast<std::uint16_t>(ewram[off + 1u] << 8)) & 0x0FFFu);
        ++counts[id];
    }
    std::array<std::uint16_t, 4> top_ids{};
    for (std::uint16_t id = 0; id < counts.size(); ++id) {
        for (std::size_t rank = 0; rank < top_ids.size(); ++rank) {
            if (counts[id] > counts[top_ids[rank]]) {
                for (std::size_t j = top_ids.size() - 1u; j > rank; --j)
                    top_ids[j] = top_ids[j - 1u];
                top_ids[rank] = id;
                break;
            }
        }
    }
    std::fprintf(stderr,
        "[wide-field-map] frame=%llu auth_epoch=%llu auth_cells=%u "
        "map_crc=%08x raw_crc=%08x top=%03x:%u,%03x:%u,%03x:%u,%03x:%u\n",
        frame, static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
        g_golden_sun_field_authored.authored_count(), map_crc, raw_crc,
        top_ids[0], counts[top_ids[0]], top_ids[1], counts[top_ids[1]],
        top_ids[2], counts[top_ids[2]], top_ids[3], counts[top_ids[3]]);
}

void trace_golden_sun_wide_scene(std::uint16_t dispcnt,
                                 const std::uint8_t* io,
                                 unsigned flags) {
    if (!golden_sun_wide_diagnostics_enabled() || !g_ws_active || !io ||
        g_golden_sun_wide_scene_signatures.size() >= 64u)
        return;
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&](std::uint16_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    // Deduplicate scene evidence by raster configuration and coarse camera
    // position. Raw scroll values change every frame while walking and used
    // to consume the entire 64-entry bound before a later scene was reached.
    mix(dispcnt);
    mix(static_cast<std::uint16_t>(flags));
    for (std::size_t off = 0x08u; off <= 0x0Eu; off += 2u)
        mix(gsr::widescreen::read_io16(io, off));
    for (std::size_t off = 0x14u; off <= 0x1Eu; off += 2u)
        mix(static_cast<std::uint16_t>(
            gsr::widescreen::read_io16(io, off) & ~0x001Fu));
    if (std::find(g_golden_sun_wide_scene_signatures.begin(),
                  g_golden_sun_wide_scene_signatures.end(), hash) !=
        g_golden_sun_wide_scene_signatures.end()) {
        return;
    }
    g_golden_sun_wide_scene_signatures.push_back(hash);
    std::fprintf(stderr,
        "[wide-scene] frame=%llu dispcnt=%04x flags=%x "
        "bgcnt=%04x/%04x/%04x/%04x scroll=%04x,%04x/%04x,%04x/%04x,%04x\n",
        runtime_current_frame(), dispcnt, flags,
        gsr::widescreen::read_io16(io, 0x08u),
        gsr::widescreen::read_io16(io, 0x0Au),
        gsr::widescreen::read_io16(io, 0x0Cu),
        gsr::widescreen::read_io16(io, 0x0Eu),
        gsr::widescreen::read_io16(io, 0x14u),
        gsr::widescreen::read_io16(io, 0x16u),
        gsr::widescreen::read_io16(io, 0x18u),
        gsr::widescreen::read_io16(io, 0x1Au),
        gsr::widescreen::read_io16(io, 0x1Cu),
        gsr::widescreen::read_io16(io, 0x1Eu));
}

void trace_golden_sun_wide_policy_sample(
    std::uint64_t frame, std::uint16_t dispcnt, const std::uint8_t* io,
    unsigned flags, GoldenSunWidePolicyReason reason) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        g_golden_sun_wide_policy_sample_end_frame == UINT64_MAX ||
        frame > g_golden_sun_wide_policy_sample_end_frame ||
        g_golden_sun_wide_policy_samples_in_window >=
            kGoldenSunWidePolicySampleLimit ||
        g_golden_sun_wide_policy_sample_frame == frame) {
        return;
    }
    g_golden_sun_wide_policy_sample_frame = frame;
    ++g_golden_sun_wide_policy_samples_in_window;
    ++g_golden_sun_wide_policy_sample_count;
    const auto read = [&](std::size_t offset) -> std::uint16_t {
        return io ? gsr::widescreen::read_io16(io, offset) : 0u;
    };
    const std::uint16_t bg1_hofs = read(0x14u);
    const std::uint16_t bg1_vofs = read(0x16u);
    const std::uint16_t bg2_hofs = read(0x18u);
    const std::uint16_t bg2_vofs = read(0x1Au);
    const std::uint16_t bg3_hofs = read(0x1Cu);
    const std::uint16_t bg3_vofs = read(0x1Eu);
    std::uint32_t map_crc = 0;
    std::uint32_t raw_crc = 0;
    if (const gba::GbaBus* bus = gbarecomp::active_bus()) {
        const std::uint8_t* ewram = bus->ewram_ptr();
        map_crc = golden_sun_field_table_crc(
            ewram, kGoldenSunFieldMapOffset, kGoldenSunFieldMapBytes);
        raw_crc = golden_sun_field_table_crc(
            ewram, kGoldenSunFieldRawOffset, kGoldenSunFieldRawBytes);
        // Reuse the existing bounded census/top-ID trace. It records no map
        // bytes and deduplicates by auth epoch plus both table CRCs.
        trace_golden_sun_field_map(ewram);
    }
    std::fprintf(
        stderr,
        "[wide-policy-sample] frame=%llu transition=%llu sample=%u "
        "reason=%s flags=%x dispcnt=%04x "
        "bgcnt=%04x/%04x/%04x/%04x "
        "raw_scroll=%04x,%04x/%04x,%04x/%04x,%04x "
        "effective_scroll=%03x,%03x/%03x,%03x/%03x,%03x "
        "auth_epoch=%llu auth_cells=%u map_crc=%08x raw_crc=%08x "
        "map_writes=%llu raw_writes=%llu epoch_map_writes=%llu "
        "epoch_raw_writes=%llu epoch_map_dma=%llu epoch_raw_dma=%llu\n",
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(g_golden_sun_wide_policy_sample_transition),
        g_golden_sun_wide_policy_samples_in_window,
        golden_sun_wide_policy_reason_name(reason), flags, dispcnt,
        read(0x08u), read(0x0Au), read(0x0Cu), read(0x0Eu),
        bg1_hofs, bg1_vofs, bg2_hofs, bg2_vofs, bg3_hofs, bg3_vofs,
        bg1_hofs & 0x01FFu, bg1_vofs & 0x01FFu,
        bg2_hofs & 0x01FFu, bg2_vofs & 0x01FFu,
        bg3_hofs & 0x01FFu, bg3_vofs & 0x01FFu,
        static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
        g_golden_sun_field_authored.authored_count(), map_crc, raw_crc,
        static_cast<unsigned long long>(g_golden_sun_field_map_writes.writes),
        static_cast<unsigned long long>(g_golden_sun_field_raw_writes.writes),
        static_cast<unsigned long long>(g_golden_sun_field_epoch_map_writes),
        static_cast<unsigned long long>(g_golden_sun_field_epoch_raw_writes),
        static_cast<unsigned long long>(
            g_golden_sun_field_epoch_map_dma_writes),
        static_cast<unsigned long long>(
            g_golden_sun_field_epoch_raw_dma_writes));
}

void trace_golden_sun_wide_policy(std::uint16_t dispcnt,
                                  const std::uint8_t* io,
                                  unsigned flags,
                                  GoldenSunWidePolicyReason reason) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    const std::uint64_t frame = runtime_current_frame();
    const bool changed = !g_golden_sun_wide_policy_seen ||
        g_golden_sun_wide_policy_last_flags != flags ||
        g_golden_sun_wide_policy_last_reason != reason;
    if (changed) {
        const char* previous = g_golden_sun_wide_policy_seen
            ? golden_sun_wide_policy_reason_name(
                  g_golden_sun_wide_policy_last_reason)
            : "none";
        ++g_golden_sun_wide_policy_transition_count;
        g_golden_sun_wide_policy_last_flags = flags;
        g_golden_sun_wide_policy_last_reason = reason;
        g_golden_sun_wide_policy_seen = true;
        if (g_golden_sun_wide_policy_logged_transitions <
                kGoldenSunWidePolicyTransitionLimit) {
            ++g_golden_sun_wide_policy_logged_transitions;
            g_golden_sun_wide_policy_sample_transition =
                g_golden_sun_wide_policy_transition_count;
            g_golden_sun_wide_policy_sample_frame = UINT64_MAX;
            g_golden_sun_wide_policy_samples_in_window = 0;
            g_golden_sun_wide_policy_sample_end_frame =
                frame + kGoldenSunWidePolicySampleLimit - 1u;
            const auto read = [&](std::size_t offset) -> std::uint16_t {
                return io ? gsr::widescreen::read_io16(io, offset) : 0u;
            };
            std::fprintf(
                stderr,
                "[wide-policy] frame=%llu transition=%llu from=%s to=%s "
                "flags=%x dispcnt=%04x bgcnt=%04x/%04x/%04x/%04x "
                "raw_scroll=%04x,%04x/%04x,%04x/%04x,%04x "
                "effective_scroll=%03x,%03x/%03x,%03x/%03x,%03x "
                "auth_epoch=%llu\n",
                static_cast<unsigned long long>(frame),
                static_cast<unsigned long long>(
                    g_golden_sun_wide_policy_transition_count), previous,
                golden_sun_wide_policy_reason_name(reason), flags, dispcnt,
                read(0x08u), read(0x0Au), read(0x0Cu), read(0x0Eu),
                read(0x14u), read(0x16u), read(0x18u), read(0x1Au),
                read(0x1Cu), read(0x1Eu), read(0x14u) & 0x01FFu,
                read(0x16u) & 0x01FFu, read(0x18u) & 0x01FFu,
                read(0x1Au) & 0x01FFu, read(0x1Cu) & 0x01FFu,
                read(0x1Eu) & 0x01FFu,
                static_cast<unsigned long long>(g_golden_sun_field_auth_epoch));
        } else {
            g_golden_sun_wide_policy_sample_end_frame = UINT64_MAX;
        }
    }
    trace_golden_sun_wide_policy_sample(
        frame, dispcnt, io, flags, reason);
}

void report_golden_sun_field_provider_trace(std::uint64_t frame,
                                            const char* reason);
void report_golden_sun_palace_margin_diagnostics(std::uint64_t frame,
                                                 const char* reason);
void report_golden_sun_obj_y_steady_state_diagnostics(std::uint64_t frame,
                                                     const char* reason);
void report_golden_sun_obj_y_transition_diagnostics(std::uint64_t frame,
                                                    const char* reason);

const char* golden_sun_field_margin_region(int hw_x, int screen_y,
                                           std::size_t* out_region) {
    std::size_t region = 3u; // bottom (the normal provider margin case)
    const char* name = "bottom";
    // Vertical rows span the whole output width, so classify them before the
    // horizontal sides; corners remain attributable to their vertical row.
    if (screen_y < 0) {
        region = 2u;
        name = "top";
    } else if (screen_y >= static_cast<int>(gsr::widescreen::kNativeHeight)) {
        region = 3u;
        name = "bottom";
    } else if (hw_x < 0) {
        region = 0u;
        name = "left";
    } else if (hw_x >= static_cast<int>(gsr::widescreen::kNativeWidth)) {
        region = 1u;
        name = "right";
    }
    if (out_region) *out_region = region;
    return name;
}

void record_golden_sun_field_map_id_attribution(
    int bg, int hw_x, int screen_y,
    const gsr::widescreen::GoldenSunFieldTilemapMetadata& metadata) {
    if (!golden_sun_wide_diagnostics_enabled() || bg < 0 || bg >= 4 ||
        !metadata.has_raw_entry) return;
    std::size_t region = 0;
    golden_sun_field_margin_region(hw_x, screen_y, &region);
    auto& entries = g_golden_sun_field_map_id_attribution[
        static_cast<std::size_t>(bg)][region];
    for (auto& entry : entries) {
        if (entry.replacements != 0u && entry.map_id == metadata.map_id) {
            ++entry.replacements;
            return;
        }
    }
    for (auto& entry : entries) {
        if (entry.replacements == 0u) {
            entry.map_id = metadata.map_id;
            entry.replacements = 1u;
            return;
        }
    }
    ++g_golden_sun_field_map_id_attribution_overflow[
        static_cast<std::size_t>(bg)][region];
}

void report_golden_sun_field_map_id_attribution(std::uint64_t frame,
                                                const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    static constexpr const char* kRegions[] = {
        "left", "right", "top", "bottom"};
    for (std::size_t bg = 0; bg < 4u; ++bg) {
        for (std::size_t region = 0; region < 4u; ++region) {
            const auto& entries = g_golden_sun_field_map_id_attribution[bg][region];
            bool any = g_golden_sun_field_map_id_attribution_overflow[bg][region] != 0u;
            for (const auto& entry : entries) any |= entry.replacements != 0u;
            if (!any) continue;
            std::fprintf(stderr,
                         "[wide-field-map-id] frame=%llu auth_epoch=%llu "
                         "reason=%s bg=%zu region=%s overflow=%llu ids=",
                         static_cast<unsigned long long>(frame),
                         static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                         reason, bg, kRegions[region],
                         static_cast<unsigned long long>(
                             g_golden_sun_field_map_id_attribution_overflow[bg][region]));
            bool first = true;
            for (const auto& entry : entries) {
                if (entry.replacements == 0u) continue;
                std::fprintf(stderr, "%s%03x:%llu", first ? "" : ",",
                             entry.map_id,
                             static_cast<unsigned long long>(entry.replacements));
                first = false;
            }
            std::fputc('\n', stderr);
        }
    }
}

void write_golden_sun_wide_scroll_trace_csv();

void report_golden_sun_wide_diagnostics_at_exit() {
    if (golden_sun_experimental_fixes_enabled()) {
        std::fprintf(stderr,
            "[wide-obj-hooks] fast_iwram=%s oam_commit=%s "
            "observer_calls=%llu f0_commits=%llu f0_placements=%llu "
            "f0_identified=%llu f0_considered=%llu f0_placed=%llu "
            "f0_placed_prev_frame=%llu\n",
            g_runtime_fast_iwram_write_observer ? "armed" : "off",
            "armed",
            static_cast<unsigned long long>(
                g_golden_sun_obj_oam_observer_calls),
            static_cast<unsigned long long>(g_golden_sun_obj_f0_commits),
            static_cast<unsigned long long>(
                g_golden_sun_obj_f0_placement_calls),
            static_cast<unsigned long long>(g_golden_sun_obj_f0_identified),
            static_cast<unsigned long long>(g_golden_sun_obj_f0_considered),
            static_cast<unsigned long long>(g_golden_sun_obj_f0_placed),
            static_cast<unsigned long long>(
                g_golden_sun_obj_f0_placed_prev_frame));
        report_golden_sun_obj_context_near_misses();
    }
    if (!golden_sun_wide_diagnostics_enabled()) return;
    std::fprintf(stderr,
        "[wide-obj-writer-summary] recognized=%llu identity_mismatch=%llu "
        "unknown_variant=%llu\n",
        static_cast<unsigned long long>(
            g_golden_sun_func1dc8_writer_recognized),
        static_cast<unsigned long long>(
            g_golden_sun_func1dc8_writer_identity_mismatch),
        static_cast<unsigned long long>(
            g_golden_sun_func1dc8_writer_unknown_variant));
    if (g_golden_sun_margin_diagnostic_callbacks != 0u) {
        std::fprintf(
            stderr,
            "[wide-margin-summary] callbacks=%llu logged=%llu\n",
            static_cast<unsigned long long>(
                g_golden_sun_margin_diagnostic_callbacks),
            static_cast<unsigned long long>(g_golden_sun_margin_diagnostic_logged));
        report_golden_sun_margin_diagnostic_sample(
            runtime_current_frame(), "summary", g_golden_sun_margin_diagnostic_total);
    }
    gba::vram_trace::OamShadowTraceStats oam_shadow{};
    gba::vram_trace::OamDmaTraceStats oam_dma{};
    gba::vram_trace::get_oam_shadow_trace_stats(&oam_shadow);
    gba::vram_trace::get_oam_dma_trace_stats(&oam_dma);
    std::fprintf(
        stderr,
        "[oam-shadow-summary] writes=%llu bytes=%llu slot_events=%llu "
        "dma_writes=%llu dma_bytes=%llu "
        "slot_overwrites=%llu unique_slots=%llu overwritten_slots=%llu "
        "records_dropped=%llu\n",
        static_cast<unsigned long long>(oam_shadow.write_calls),
        static_cast<unsigned long long>(oam_shadow.bytes),
        static_cast<unsigned long long>(oam_shadow.slot_write_events),
        static_cast<unsigned long long>(oam_shadow.dma_write_calls),
        static_cast<unsigned long long>(oam_shadow.dma_bytes),
        static_cast<unsigned long long>(oam_shadow.slot_overwrite_events),
        static_cast<unsigned long long>(oam_shadow.unique_slots),
        static_cast<unsigned long long>(oam_shadow.overwritten_slots),
        static_cast<unsigned long long>(oam_shadow.records_dropped));
    std::fprintf(
        stderr,
        "[oam-dma-summary] transfers=%llu bytes=%llu used_slots=%llu "
        "visible_slots=%llu nonzero_slots=%llu records_dropped=%llu "
        "last_src=0x%08x last_dst=0x%08x last_size=%u last_used=%u "
        "last_visible=%u last_nonzero=%u last_raw_x_ge_240=%u "
        "last_raw_y_ge_160=%u\n",
        static_cast<unsigned long long>(oam_dma.transfers),
        static_cast<unsigned long long>(oam_dma.bytes),
        static_cast<unsigned long long>(oam_dma.used_slot_total),
        static_cast<unsigned long long>(oam_dma.visible_slot_total),
        static_cast<unsigned long long>(oam_dma.nonzero_slot_total),
        static_cast<unsigned long long>(oam_dma.records_dropped),
        oam_dma.last_source, oam_dma.last_destination, oam_dma.last_size,
        oam_dma.last_used_slots, oam_dma.last_visible_slots,
        oam_dma.last_nonzero_slots, oam_dma.last_raw_x_ge_240,
        oam_dma.last_raw_y_ge_160);
    gba::vram_trace::OamAttr0TraceStats attr0{};
    gba::vram_trace::get_oam_attr0_trace_stats(&attr0);
    std::fprintf(stderr,
        "[oam-attr0-summary] post_copies=%llu candidates=%llu exact_192=%llu "
        "other=%llu unseen=%llu groups_dropped=%llu groups=",
        static_cast<unsigned long long>(attr0.post_copies),
        static_cast<unsigned long long>(attr0.candidate_slots),
        static_cast<unsigned long long>(attr0.exact_192_slots),
        static_cast<unsigned long long>(attr0.other_slots),
        static_cast<unsigned long long>(attr0.unseen_slots),
        static_cast<unsigned long long>(attr0.groups_dropped));
    for (std::uint32_t i = 0; i < attr0.group_count; ++i) {
        const auto& g = attr0.groups[i];
        const char* kind = g.kind == gba::vram_trace::OamAttr0WriterKind::Cpu
            ? "cpu" : (g.kind == gba::vram_trace::OamAttr0WriterKind::Dma
                ? "dma" : "unseen");
        std::fprintf(stderr, "%s%u:%s:0x%08x:%u..%u/%u:g%llu:c%llu",
            i == 0u ? "" : ";", static_cast<unsigned>(g.raw_y), kind,
            g.writer_pc, g.slot_first, g.slot_last, g.slot_count,
            static_cast<unsigned long long>(g.generation),
            static_cast<unsigned long long>(g.cycle));
    }
    std::fputc('\n', stderr);
    std::fprintf(
        stderr,
        "[wide-policy-summary] frame=%llu transitions=%llu logged=%llu "
        "samples=%llu auth_epoch=%llu final_reason=%s final_flags=%x\n",
        static_cast<unsigned long long>(runtime_current_frame()),
        static_cast<unsigned long long>(g_golden_sun_wide_policy_transition_count),
        static_cast<unsigned long long>(g_golden_sun_wide_policy_logged_transitions),
        static_cast<unsigned long long>(g_golden_sun_wide_policy_sample_count),
        static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
        golden_sun_wide_policy_reason_name(g_golden_sun_wide_policy_last_reason),
        g_golden_sun_wide_policy_last_flags);
    report_golden_sun_field_provider_trace(runtime_current_frame(), "exit");
    report_golden_sun_palace_margin_diagnostics(runtime_current_frame(), "exit");
    report_golden_sun_obj_b328_diagnostics(runtime_current_frame(), "exit");
    report_golden_sun_obj_y_transition_diagnostics(runtime_current_frame(), "exit");
    report_golden_sun_obj_y_steady_state_diagnostics(runtime_current_frame(), "exit");
    report_golden_sun_field_map_id_attribution(runtime_current_frame(), "exit");
    report_golden_sun_field_producers("exit");
    report_golden_sun_obj_record_census("exit");
    report_golden_sun_obj_record_values("exit");
    report_golden_sun_obj_commit_order("exit");
    write_golden_sun_wide_scroll_trace_csv();
    std::fprintf(
        stderr,
        "[wide-field-table-summary] map_writes=%llu map_bytes=%llu "
        "map_first_frame=%llu map_first_pc=0x%08x map_last_frame=%llu "
        "map_last_pc=0x%08x raw_writes=%llu raw_bytes=%llu "
        "raw_first_frame=%llu raw_first_pc=0x%08x raw_last_frame=%llu "
        "raw_last_pc=0x%08x auth_cells=%u\n",
        static_cast<unsigned long long>(g_golden_sun_field_map_writes.writes),
        static_cast<unsigned long long>(g_golden_sun_field_map_writes.bytes),
        static_cast<unsigned long long>(g_golden_sun_field_map_writes.first_frame),
        g_golden_sun_field_map_writes.first_pc,
        static_cast<unsigned long long>(g_golden_sun_field_map_writes.last_frame),
        g_golden_sun_field_map_writes.last_pc,
        static_cast<unsigned long long>(g_golden_sun_field_raw_writes.writes),
        static_cast<unsigned long long>(g_golden_sun_field_raw_writes.bytes),
        static_cast<unsigned long long>(g_golden_sun_field_raw_writes.first_frame),
        g_golden_sun_field_raw_writes.first_pc,
        static_cast<unsigned long long>(g_golden_sun_field_raw_writes.last_frame),
        g_golden_sun_field_raw_writes.last_pc,
        g_golden_sun_field_authored.authored_count());
    std::fprintf(
        stderr,
        "[wide-field-table-dma-summary] map_writes=%llu map_bytes=%llu "
        "raw_writes=%llu raw_bytes=%llu epoch_map_writes=%llu "
        "epoch_raw_writes=%llu authored_cells=%u\n",
        static_cast<unsigned long long>(
            g_golden_sun_field_map_dma_writes.writes),
        static_cast<unsigned long long>(
            g_golden_sun_field_map_dma_writes.bytes),
        static_cast<unsigned long long>(
            g_golden_sun_field_raw_dma_writes.writes),
        static_cast<unsigned long long>(
            g_golden_sun_field_raw_dma_writes.bytes),
        static_cast<unsigned long long>(
            g_golden_sun_field_epoch_map_dma_writes),
        static_cast<unsigned long long>(
            g_golden_sun_field_epoch_raw_dma_writes),
        g_golden_sun_field_authored.authored_count());
}

void report_golden_sun_field_provider_trace(std::uint64_t frame,
                                             const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    for (std::size_t bg = 0; bg < g_golden_sun_field_provider_trace.size();
         ++bg) {
        const GoldenSunFieldProviderTrace& stat =
            g_golden_sun_field_provider_trace[bg];
        const std::int32_t min_x = stat.calls != 0u ? stat.min_x : 0;
        const std::int32_t max_x = stat.calls != 0u ? stat.max_x : 0;
        const std::int32_t min_y = stat.calls != 0u ? stat.min_y : 0;
        const std::int32_t max_y = stat.calls != 0u ? stat.max_y : 0;
        std::fprintf(
            stderr,
            "[wide-field-provider] frame=%llu auth_epoch=%llu reason=%s "
            "bg=%zu calls=%llu equal=%llu split=%llu replacements=%llu "
            "unavailable=%llu precondition=%llu boundary=%llu raw=%llu "
            "palace_region=%llu lookup_miss=%llu coord=%d..%d,%d..%d\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            reason, bg, static_cast<unsigned long long>(stat.calls),
            static_cast<unsigned long long>(stat.equal_scroll_calls),
            static_cast<unsigned long long>(stat.split_scroll_calls),
            static_cast<unsigned long long>(stat.replacements),
            static_cast<unsigned long long>(stat.unavailable),
            static_cast<unsigned long long>(stat.precondition_rejects),
            static_cast<unsigned long long>(stat.boundary_rejects),
            static_cast<unsigned long long>(stat.raw_unavailable),
            static_cast<unsigned long long>(stat.palace_region_rejects),
            static_cast<unsigned long long>(stat.lookup_misses), min_x,
            max_x, min_y, max_y);
    }
}

void report_golden_sun_palace_margin_diagnostics(std::uint64_t frame,
                                                 const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    static constexpr const char* kRegions[] = {
        "left", "right", "top", "bottom"};
    const auto& diagnostic = g_golden_sun_palace_margin_diagnostics;
    for (std::size_t bg = 0; bg < GoldenSunPalaceMarginDiagnostics::kBgCount;
         ++bg) {
        for (std::size_t region = 0;
             region < GoldenSunPalaceMarginDiagnostics::kRegionCount;
             ++region) {
            for (std::size_t outcome = 0;
                 outcome < GoldenSunPalaceMarginDiagnostics::kOutcomeCount;
                 ++outcome) {
                const std::uint64_t count =
                    diagnostic.counts[bg][region][outcome];
                if (count == 0u) continue;
                std::fprintf(
                    stderr,
                    "[wide-palace-margin] frame=%llu auth_epoch=%llu "
                    "reason=%s bg=%zu region=%s outcome=%s count=%llu\n",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(
                        g_golden_sun_field_auth_epoch),
                    reason, bg + 1u, kRegions[region],
                    golden_sun_palace_margin_outcome_name(
                        static_cast<GoldenSunPalaceMarginOutcome>(outcome)),
                    static_cast<unsigned long long>(count));
            }
        }
    }
    for (std::size_t i = 0; i < diagnostic.sample_count; ++i) {
        const auto& sample = diagnostic.samples[i];
        static constexpr const char* kSampleRegions[] = {
            "left", "right", "top", "bottom"};
        std::fprintf(
            stderr,
            "[wide-palace-margin-sample] frame=%llu auth_epoch=%llu "
            "reason=%s bg=%u region=%s outcome=%s hw_x=%d screen_y=%d "
            "metadata=%u map_x=%u map_y=%u map_id=0x%03x\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            reason, static_cast<unsigned>(sample.bg),
            kSampleRegions[sample.region],
            golden_sun_palace_margin_outcome_name(sample.outcome),
            static_cast<int>(sample.hw_x), static_cast<int>(sample.screen_y),
            sample.metadata_valid ? 1u : 0u,
            static_cast<unsigned>(sample.map_x),
            static_cast<unsigned>(sample.map_y),
            static_cast<unsigned>(sample.map_id));
    }
    std::fprintf(stderr,
                 "[wide-palace-margin-summary] frame=%llu auth_epoch=%llu "
                 "reason=%s samples=%zu dropped=%llu\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                 reason, diagnostic.sample_count,
                 static_cast<unsigned long long>(diagnostic.samples_dropped));
}

void report_golden_sun_obj_y_transition_diagnostics(std::uint64_t frame,
                                                   const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    for (std::size_t r = 0; r < static_cast<std::size_t>(
             GoldenSunObjYTransitionReason::Count); ++r) {
        for (std::size_t region = 0; region < static_cast<std::size_t>(
                 GoldenSunObjYTransitionRegion::Count); ++region) {
            for (std::size_t resolution = 0; resolution < static_cast<std::size_t>(
                     GoldenSunObjYTransitionResolution::Count); ++resolution) {
                const auto count = g_golden_sun_obj_y_transition_counts
                    [r][region][resolution];
                if (count == 0u) continue;
                std::fprintf(stderr,
                    "[wide-obj-y-transition-summary] frame=%llu "
                    "auth_epoch=%llu reason=%s resolution=%s region=%s "
                    "count=%llu\n",
                    static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                    golden_sun_obj_y_transition_reason_name(
                        static_cast<GoldenSunObjYTransitionReason>(r)),
                    golden_sun_obj_y_transition_resolution_name(
                        static_cast<GoldenSunObjYTransitionResolution>(resolution)),
                    golden_sun_obj_y_transition_region_name(
                        static_cast<GoldenSunObjYTransitionRegion>(region)),
                    static_cast<unsigned long long>(count));
            }
        }
    }
    for (unsigned bucket = 0; bucket < kGoldenSunObjYTransitionBucketCount;
         ++bucket) {
        const auto transition_bucket =
            static_cast<GoldenSunObjYTransitionBucket>(bucket);
        for (unsigned i = 0;
             i < g_golden_sun_obj_y_transition_sample_counts[bucket]; ++i) {
        const auto& sample = g_golden_sun_obj_y_transition_samples[bucket][i];
        std::fprintf(stderr,
            "[wide-obj-y-transition-sample] bucket=%s frame=%llu "
            "auth_epoch=%llu "
            "slot=%d raw_y=%d canonical_y=%d output_y=%d resolution=%s "
            "reason=%s region=%s attr0=0x%04x attr1=0x%04x attr2=0x%04x "
            "expected_attr0=0x%04x expected_attr1=0x%04x expected_attr2=0x%04x "
            "expected_target=0x%08x provenance_frame=%llu "
            "provenance_epoch=%llu target=0x%08x "
            "writer_pc=0x%08x writer_generation=%llu\n",
            golden_sun_obj_y_transition_bucket_name(transition_bucket),
            static_cast<unsigned long long>(sample.frame),
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            sample.slot, sample.raw_y, sample.canonical_y, sample.output_y,
            golden_sun_obj_y_transition_resolution_name(sample.resolution),
            golden_sun_obj_y_transition_reason_name(sample.reason),
            golden_sun_obj_y_transition_region_name(sample.region),
            sample.attr0, sample.attr1, sample.attr2,
            sample.expected_attr0, sample.expected_attr1, sample.expected_attr2,
            sample.expected_target,
            static_cast<unsigned long long>(sample.provenance_frame),
            static_cast<unsigned long long>(sample.provenance_epoch),
            sample.target_address, sample.writer_pc,
            static_cast<unsigned long long>(sample.writer_generation));
        }
    }
    std::fprintf(stderr,
        "[wide-obj-y-transition] frame=%llu auth_epoch=%llu reason=%s "
        "raw_y=159..199 dedup=active-candidate-identity+slot-target "
        "active_candidate_canonical_samples=%u cap=%u dropped=%llu "
        "signed_bottom_samples=%u cap=%u dropped=%llu "
        "outcome_transition_samples=%u cap=%u dropped=%llu\n",
        static_cast<unsigned long long>(frame),
        static_cast<unsigned long long>(g_golden_sun_field_auth_epoch), reason,
        g_golden_sun_obj_y_transition_sample_counts[static_cast<std::size_t>(
            GoldenSunObjYTransitionBucket::ActiveCandidateCanonical)],
        kGoldenSunObjYTransitionSampleLimit,
        static_cast<unsigned long long>(g_golden_sun_obj_y_transition_samples_dropped[
            static_cast<std::size_t>(GoldenSunObjYTransitionBucket::ActiveCandidateCanonical)]),
        g_golden_sun_obj_y_transition_sample_counts[static_cast<std::size_t>(
            GoldenSunObjYTransitionBucket::SignedBottom)],
        kGoldenSunObjYTransitionSampleLimit,
        static_cast<unsigned long long>(g_golden_sun_obj_y_transition_samples_dropped[
            static_cast<std::size_t>(GoldenSunObjYTransitionBucket::SignedBottom)]),
        g_golden_sun_obj_y_transition_sample_counts[static_cast<std::size_t>(
            GoldenSunObjYTransitionBucket::OutcomeTransition)],
        kGoldenSunObjYTransitionSampleLimit,
        static_cast<unsigned long long>(g_golden_sun_obj_y_transition_samples_dropped[
            static_cast<std::size_t>(GoldenSunObjYTransitionBucket::OutcomeTransition)]));
}

void report_golden_sun_obj_y_steady_state_diagnostics(std::uint64_t frame,
                                                     const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    std::uint64_t total_calls = 0;
    std::uint64_t total_192 = 0;
    for (std::size_t slot = 0; slot < g_golden_sun_obj_y_slot_diagnostics.size(); ++slot) {
        const auto& stat = g_golden_sun_obj_y_slot_diagnostics[slot];
        if (stat.calls == 0u) continue;
        total_calls += stat.calls;
        total_192 += stat.raw_192;
        std::fprintf(stderr,
                     "[wide-obj-y-slot] frame=%llu auth_epoch=%llu reason=%s "
                     "slot=%zu calls=%llu raw_160_191=%llu raw_192=%llu "
                     "raw_193_199=%llu raw_200_255=%llu outcomes=",
                     static_cast<unsigned long long>(frame),
                     static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                     reason, slot, static_cast<unsigned long long>(stat.calls),
                     static_cast<unsigned long long>(stat.raw_160_191),
                     static_cast<unsigned long long>(stat.raw_192),
                     static_cast<unsigned long long>(stat.raw_193_199),
                     static_cast<unsigned long long>(stat.raw_200_255));
        bool first = true;
        for (std::size_t i = 0; i < stat.outcomes.size(); ++i) {
            if (stat.outcomes[i] == 0u) continue;
            std::fprintf(stderr, "%s%s:%llu", first ? "" : ",",
                         golden_sun_obj_y_outcome_name(
                             static_cast<GoldenSunObjYOutcome>(i)),
                         static_cast<unsigned long long>(stat.outcomes[i]));
            first = false;
        }
        std::fputc('\n', stderr);
    }
    std::fprintf(stderr,
                 "[wide-obj-y-summary] frame=%llu auth_epoch=%llu reason=%s "
                 "slots=%zu calls=%llu exact_192=%llu "
                 "alias_records=%u alias_dropped=%llu "
                 "edge_alias_activations=%llu "
                 "steady_state_only=1 transition_records_excluded=1\n",
                 static_cast<unsigned long long>(frame),
                 static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                 reason, g_golden_sun_obj_y_slot_diagnostics.size(),
                 static_cast<unsigned long long>(total_calls),
                 static_cast<unsigned long long>(total_192),
                 g_golden_sun_obj_y_alias_logs_in_epoch,
                 static_cast<unsigned long long>(
                     g_golden_sun_obj_y_alias_logs_dropped),
                 static_cast<unsigned long long>(
                     g_golden_sun_obj_y_edge_alias_activations));
}

// WIDE-01 parallax hypothesis capture (temporary): does each BG layer's
// scroll hold a constant offset/ratio to the gameplay layer within a room,
// observable live from hardware scroll registers alone? Gated by the same
// GBARECOMP_VRAM_MAP_TRACE toggle as the rest of this file; off by default,
// zero cost when disabled. One row per authentic visible frame (not per
// scanline -- the margin-policy callback fires once per raster line). A
// fixed-size ring bounds the CSV to at most kGoldenSunWideScrollTraceCap
// frames (~5.5 minutes at 60 fps) so a play session cannot grow this file
// without limit; oldest rows are dropped first.
constexpr std::size_t kGoldenSunWideScrollTraceCap = 20000u;

struct GoldenSunWideScrollTraceRow {
    std::uint64_t frame = 0;
    std::uint16_t dispcnt = 0;
    std::uint16_t bg0cnt = 0;
    std::uint16_t bg1cnt = 0;
    std::uint16_t bg2cnt = 0;
    std::uint16_t bg3cnt = 0;
    std::uint16_t bg0_hofs = 0;
    std::uint16_t bg0_vofs = 0;
    std::uint16_t bg1_hofs = 0;
    std::uint16_t bg1_vofs = 0;
    std::uint16_t bg2_hofs = 0;
    std::uint16_t bg2_vofs = 0;
    std::uint16_t bg3_hofs = 0;
    std::uint16_t bg3_vofs = 0;
    std::uint8_t margin_reason = 0;
    std::uint8_t split_reason = 0;
    std::uint8_t flags = 0;
    std::uint32_t map_crc = 0;
    std::uint32_t raw_crc = 0;
    std::uint64_t auth_epoch = 0;
};
std::array<GoldenSunWideScrollTraceRow, kGoldenSunWideScrollTraceCap>
    g_golden_sun_wide_scroll_trace{};
std::uint64_t g_golden_sun_wide_scroll_trace_count = 0;
std::uint64_t g_golden_sun_wide_scroll_trace_last_frame = UINT64_MAX;

// Record at most one row per frame, using the field-map census helper
// already used elsewhere in this file (golden_sun_field_table_crc over the
// same EWRAM offsets as trace_golden_sun_field_map / the Palace fingerprint)
// so this adds no new identity scheme.
void trace_golden_sun_wide_scroll_row(std::uint16_t dispcnt,
                                      const std::uint8_t* io,
                                      GoldenSunWidePolicyReason reason,
                                      GoldenSunWidePolicyReason split_reason,
                                      unsigned flags) {
    if (!golden_sun_wide_diagnostics_enabled() || !io) return;
    const std::uint64_t frame = runtime_current_frame();
    if (frame == g_golden_sun_wide_scroll_trace_last_frame) return;
    g_golden_sun_wide_scroll_trace_last_frame = frame;
    std::uint32_t map_crc = 0;
    std::uint32_t raw_crc = 0;
    if (const gba::GbaBus* bus = gbarecomp::active_bus()) {
        const std::uint8_t* ewram = bus->ewram_ptr();
        map_crc = golden_sun_field_table_crc(
            ewram, kGoldenSunFieldMapOffset, kGoldenSunFieldMapBytes);
        raw_crc = golden_sun_field_table_crc(
            ewram, kGoldenSunFieldRawOffset, kGoldenSunFieldRawBytes);
    }
    GoldenSunWideScrollTraceRow row{};
    row.frame = frame;
    row.dispcnt = dispcnt;
    row.bg0cnt = gsr::widescreen::read_io16(io, 0x08u);
    row.bg1cnt = gsr::widescreen::read_io16(io, 0x0Au);
    row.bg2cnt = gsr::widescreen::read_io16(io, 0x0Cu);
    row.bg3cnt = gsr::widescreen::read_io16(io, 0x0Eu);
    row.bg0_hofs = gsr::widescreen::read_io16(io, 0x10u);
    row.bg0_vofs = gsr::widescreen::read_io16(io, 0x12u);
    row.bg1_hofs = gsr::widescreen::read_io16(io, 0x14u);
    row.bg1_vofs = gsr::widescreen::read_io16(io, 0x16u);
    row.bg2_hofs = gsr::widescreen::read_io16(io, 0x18u);
    row.bg2_vofs = gsr::widescreen::read_io16(io, 0x1Au);
    row.bg3_hofs = gsr::widescreen::read_io16(io, 0x1Cu);
    row.bg3_vofs = gsr::widescreen::read_io16(io, 0x1Eu);
    row.margin_reason = static_cast<std::uint8_t>(reason);
    row.split_reason = static_cast<std::uint8_t>(split_reason);
    row.flags = static_cast<std::uint8_t>(flags);
    row.map_crc = map_crc;
    row.raw_crc = raw_crc;
    row.auth_epoch = g_golden_sun_field_auth_epoch;
    g_golden_sun_wide_scroll_trace[
        g_golden_sun_wide_scroll_trace_count % kGoldenSunWideScrollTraceCap] =
        row;
    ++g_golden_sun_wide_scroll_trace_count;
}

// Bounded ring flush, same discipline as hang_trace.csv in
// gbarecomp/src/runtime/runtime_bus_bridge.cpp: metadata-only rows written
// once at exit from the always-collected in-memory ring.
void write_golden_sun_wide_scroll_trace_csv() {
    if (!golden_sun_wide_diagnostics_enabled() ||
        g_golden_sun_wide_scroll_trace_count == 0u) return;
    FILE* f = std::fopen("wide_scroll_trace.csv", "w");
    if (!f) return;
    std::fprintf(f,
        "frame,dispcnt,bg0cnt,bg1cnt,bg2cnt,bg3cnt,"
        "bg0_hofs,bg0_vofs,bg1_hofs,bg1_vofs,bg2_hofs,bg2_vofs,"
        "bg3_hofs,bg3_vofs,margin_reason,split_reason,flags,"
        "map_crc,raw_crc,auth_epoch\n");
    const std::uint64_t count = std::min<std::uint64_t>(
        g_golden_sun_wide_scroll_trace_count, kGoldenSunWideScrollTraceCap);
    const std::uint64_t start = g_golden_sun_wide_scroll_trace_count > count
        ? g_golden_sun_wide_scroll_trace_count % kGoldenSunWideScrollTraceCap
        : 0u;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto& row = g_golden_sun_wide_scroll_trace[
            (start + i) % kGoldenSunWideScrollTraceCap];
        std::fprintf(f,
            "%llu,0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,"
            "0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,0x%04x,"
            "%s,%s,0x%x,0x%08x,0x%08x,%llu\n",
            static_cast<unsigned long long>(row.frame), row.dispcnt,
            row.bg0cnt, row.bg1cnt, row.bg2cnt, row.bg3cnt,
            row.bg0_hofs, row.bg0_vofs, row.bg1_hofs, row.bg1_vofs,
            row.bg2_hofs, row.bg2_vofs, row.bg3_hofs, row.bg3_vofs,
            golden_sun_wide_policy_reason_name(
                static_cast<GoldenSunWidePolicyReason>(row.margin_reason)),
            golden_sun_wide_policy_reason_name(
                static_cast<GoldenSunWidePolicyReason>(row.split_reason)),
            row.flags, row.map_crc, row.raw_crc,
            static_cast<unsigned long long>(row.auth_epoch));
    }
    std::fclose(f);
    std::fprintf(stderr,
        "[wide-scroll-trace] wrote wide_scroll_trace.csv (%llu of %llu "
        "frame rows, ring cap %zu)\n",
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(g_golden_sun_wide_scroll_trace_count),
        kGoldenSunWideScrollTraceCap);
}

void maybe_report_golden_sun_cull_trace();

// Decide once per rendered row whether this is a battle frame and where its
// live scene band starts and ends. The per-pixel provider below extends the
// enabled arena layers through the side margins; the native rows remain on
// the normal streaming path.

void golden_sun_update_battle_backdrop(std::uint16_t dispcnt,
                                       const std::uint8_t* io) {
    GoldenSunBattleBackdrop next;
    if (g_ws_active && io != nullptr &&
        gsr::battle::is_battle_frame(dispcnt)) {
        const std::uint16_t win0v =
            static_cast<std::uint16_t>(io[0x44] | (io[0x45] << 8));
        int band_top_row = 0;
        int band_bottom_row = 0;
        if (gsr::battle::band_bounds(
                win0v, static_cast<int>(gsr::widescreen::kNativeHeight),
                &band_top_row,
                &band_bottom_row)) {
            const int arena_centre =
                gsr::battle::band_centre(band_top_row, band_bottom_row);
            if (arena_centre >= 0) {
                next.active = true;
                next.arena_centre = arena_centre;
                next.band_top = band_top_row;
                next.band_bottom = band_bottom_row;
                next.menus =
                    (dispcnt & (0x0100u << gsr::battle::kMenuLayer)) != 0u;
                next.icons_visible = (dispcnt & 0x6000u) == 0u;
                const std::uint16_t bg1cnt =
                    static_cast<std::uint16_t>(io[0x0Au] | (io[0x0Bu] << 8));
                const std::uint16_t bg2cnt =
                    static_cast<std::uint16_t>(io[0x0Cu] | (io[0x0Du] << 8));
                const unsigned bg2_size_code = (bg2cnt >> 14) & 0x3u;
                const std::uint16_t bldcnt =
                    static_cast<std::uint16_t>(io[0x50u] | (io[0x51u] << 8));
                // Which layer is carrying an EFFECT rather than the arena.
                //
                // Both captured effects have the same two properties and the
                // arena never has both: the layer is a first target of an
                // alpha blend, and it sits at priority 1. Measured across
                // every battle snapshot on disk --
                //   Ray, logs/maprec_20260913_190642 snap_00095: the graphic
                //     is BG2, BG2CNT 0x6785 (priority 1), BLDCNT 0x3f44.
                //   Auto-attack hit, logs/maprec_20260913_193104 snap_00034:
                //     the graphic is BG1, BG1CNT 0x1f81 (priority 1), BLDCNT
                //     0x3f42.
                // The arena states are BG1 0x1f83/0x0c04 (priority 3 and 0)
                // and BG2 0x678a/0x6147/0x7349 (priorities 2, 3, 1). BG1 IS
                // alpha-blended while holding the arena in 12 captured frames
                // (0x1f83 + eff 1 + BG1 targeted, verified by rendering the
                // layer: a grass field), which is why the blend alone is not
                // enough and the priority is part of the test.
                //
                // An effect is drawn where the battlers are; continuing it
                // sideways repeats it at the layer's map period, which is what
                // both duplicates were.
                const bool alpha_effect = ((bldcnt >> 6) & 0x3u) == 1u;
                const auto effect_overlay = [&](std::uint16_t bgcnt,
                                                unsigned layer) {
                    return alpha_effect &&
                           (bldcnt & (1u << layer)) != 0u &&
                           (bgcnt & 0x3u) == 1u;
                };
                next.affine_margin_allowed =
                    bg2_size_code == 1u &&
                    !effect_overlay(bg2cnt, gsr::battle::kArenaAffineLayer);
                next.backdrop_margin_allowed =
                    !effect_overlay(bg1cnt, gsr::battle::kBackdropLayer);
            }
        }
    }
    g_golden_sun_battle_backdrop_state = next;
    const unsigned layers = next.active
        ? (static_cast<unsigned>(dispcnt >> 8) & 0x0Fu) : 0u;
    gba::g_ws_bg_sample_provider_layers = layers;
    const unsigned arena_layers =
        (next.backdrop_margin_allowed
             ? (1u << gsr::battle::kBackdropLayer) : 0u) |
        (next.affine_margin_allowed
             ? (1u << gsr::battle::kArenaAffineLayer) : 0u);
    // Only remapped arena margin samples bypass the guest window. Native
    // center samples keep the ordinary WIN0/WIN1/OBJ-window and blend
    // decisions; the affine renderer explicitly retains that behavior for
    // provider no-ops on native columns.
    gba::g_ws_bg_sample_provider_ignore_window_layers =
        next.active ? (layers & arena_layers) : 0u;
    // No vertical remap is used, so every native row stays on the normal
    // streaming path for battle and field frames alike.
    gba::g_ws_defer_native_rows = 0;

    // Alongside the renderer's own row dump (armed by the same launcher
    // toggle): what this rule decided, once per frame. Without it a capture
    // shows what the game did but not what we did about it.
    if (gba::g_ws_row_state_dump_mode >= 0) {
        static std::uint64_t last_frame = ~std::uint64_t{0};
        static unsigned long long lines = 0;
        const std::uint64_t frame = runtime_current_frame();
        if (frame != last_frame && lines < 20000ull) {
            last_frame = frame;
            if (FILE* f = std::fopen("logs/battle_rule.csv", "a")) {
                if (lines == 0) {
                    std::fprintf(f,
                        "frame,dispcnt,active,band_bottom,arena_centre,"
                        "menus,icons_visible,layers\n");
                }
                ++lines;
                std::fprintf(f, "%llu,%u,%d,%d,%d,%d,%d,%u\n",
                             static_cast<unsigned long long>(frame),
                             static_cast<unsigned>(dispcnt),
                             next.active ? 1 : 0, next.band_bottom,
                             next.arena_centre,
                             next.menus ? 1 : 0, next.icons_visible ? 1 : 0,
                             layers);
                std::fclose(f);
            }
        }
    }
}

// Keep the native battle composition in the centre. Enabled battle BG layers
// answer side-margin samples only inside the live WIN0V scene band; menus and
// every other layer remain suppressed outside the authentic
// 240x160 rectangle.
int golden_sun_battle_bg_sample_provider(int bg, int output_x, int screen_y,
                                         int* out_hw_x, int* out_hw_y) {
    const GoldenSunBattleBackdrop& st = g_golden_sun_battle_backdrop_state;
    if (!st.active || out_hw_x == nullptr || out_hw_y == nullptr) return 0;
    const int native_w = static_cast<int>(gsr::widescreen::kNativeWidth);
    const int native_h = static_cast<int>(gsr::widescreen::kNativeHeight);
    const int out_x =
        output_x - static_cast<int>(g_golden_sun_wide_extra_left);
    const bool side_margin = out_x < 0 || out_x >= native_w;
    const bool native_row = screen_y >= 0 && screen_y < native_h;

    if (!side_margin) return 0;
    if ((bg == static_cast<int>(gsr::battle::kBackdropLayer) &&
         st.backdrop_margin_allowed) ||
        (bg == static_cast<int>(gsr::battle::kArenaAffineLayer) &&
         st.affine_margin_allowed)) {
        if (!native_row) return -1;
        if (screen_y < st.band_top || screen_y >= st.band_bottom) return -1;
        *out_hw_x = out_x;
        *out_hw_y = screen_y;
        return 1;
    }
    // BG0 menus and any other BG remain inside the native rectangle. Returning
    // -1 prevents the generic wrapped margin fetch while leaving each center
    // pixel on the hardware compositor's normal path.
    return -1;
}

unsigned golden_sun_room_buffer_margin_policy(std::uint16_t dispcnt,
                                              const std::uint8_t* io) {
    golden_sun_update_battle_backdrop(dispcnt, io);
    if (g_golden_sun_battle_backdrop_state.active) {
        // Battle keeps the established rule: its arena layers are placed by
        // the sample provider, which already bypasses the guest window, and
        // the remaining battle layers must not be let into the margins.
        gba::g_ws_authored_margin_layers = 0;
        return gsr::widescreen::kPillarboxVertical;
    }
    // Outside battle the room buffer owns the margins, so an expanded column
    // lies outside every WIN0/WIN1 rectangle the game set for the authentic
    // 240 and WINOUT controls it -- the same contract the PPU documents for a
    // room provider. Without this the renderer blanks the WHOLE margin on
    // every row whose window does not span all 240 authentic columns
    // (black_nonuniform_window_margins). Measured 2026-09-13 in the Tret
    // approach: the drawn strip is exactly 240 columns wide across an 81-row
    // band (authentic rows 40..120), both margins black, which is that rule
    // firing for the light window the game runs there.
    gba::g_ws_authored_margin_layers = 1;
    return 0u;
}

unsigned golden_sun_wide_margin_policy_callback(
    std::uint16_t dispcnt, const std::uint8_t* io) {
    golden_sun_update_battle_backdrop(dispcnt, io);
    maybe_report_golden_sun_cull_trace();
    const GoldenSunWidePolicyReason reason =
        gsr::widescreen::golden_sun_wide_margin_policy_reason(dispcnt, io);
    const GoldenSunWidePolicyReason split_reason =
        gsr::widescreen::golden_sun_mode0_split_scroll_policy_reason(
            dispcnt, io);
    const unsigned generic_flags =
        gsr::widescreen::golden_sun_wide_margin_policy(
        dispcnt, io);
    const bool split_row = split_reason ==
        GoldenSunWidePolicyReason::AuthorizedMode0SplitScroll;
    // The runtime calls this policy once for each authentic visible raster
    // line. Signed top/bottom rows are synthesized later from the latched
    // edge state and must not make a clean guest frame impossible to complete.
    const std::uint32_t expected_rows = gsr::widescreen::kNativeHeight;
    const bool split_frame_authorized =
        g_golden_sun_mode0_split_scroll_frame.observe(
            runtime_current_frame(), split_row, expected_rows);
    // Terrain-only Mode0ScrollMismatch field (non-Palace split scroll): this
    // is a direct, immediate per-row classification like AuthorizedMode0
    // above, not a multi-frame stability gate -- there is no cross-layer
    // offset being trusted, only the terrain layer's own live register (see
    // golden_sun_field_terrain_bg / golden_sun_wide_tilemap_provider below).
    const bool terrain_field_row =
        !split_frame_authorized &&
        reason == GoldenSunWidePolicyReason::Mode0ScrollMismatch;
    g_golden_sun_mode0_terrain_field = terrain_field_row;
    const unsigned flags =
        g_golden_sun_battle_backdrop_state.active
            ? gsr::widescreen::kPillarboxVertical
            : (split_frame_authorized || terrain_field_row) ? 0u
                                                              : generic_flags;
    trace_golden_sun_wide_scene(dispcnt, io, flags);
    if (io) {
        std::memcpy(g_golden_sun_wide_line_io.data(), io,
                    g_golden_sun_wide_line_io.size());
        g_golden_sun_wide_line_io_valid = true;
        g_golden_sun_wide_line_dispcnt = dispcnt;
    } else {
        g_golden_sun_wide_line_io_valid = false;
        g_golden_sun_wide_line_dispcnt = 0;
    }
    // The BG X-provider has no IO arguments. Publish this per-scanline scene
    // bit immediately before the PPU invokes it; invalid/transition frames
    // therefore cannot inherit Mode 0 cutoff behavior.
    // Equal-scroll field/object authorization remains separate from the
    // complete-frame Palace split-scroll authorization. In particular, a
    // split-scroll frame must not inherit the equal-scroll object's culls.
    g_golden_sun_mode0_field =
        reason == GoldenSunWidePolicyReason::AuthorizedMode0;
    g_golden_sun_mode0_split_scroll = split_frame_authorized;
    if (!split_frame_authorized) {
        const bool had_palace_cache =
            g_golden_sun_palace_table_authorized ||
            g_golden_sun_palace_region_scroll_valid ||
            g_golden_sun_palace_region_build_attempted;
        g_golden_sun_palace_table_authorized = false;
        if (had_palace_cache) reset_golden_sun_palace_active_region();
    } else {
        refresh_golden_sun_palace_table_authorization();
    }
    // The metatile table's "authored" bitmap is keyed to this exact scene
    // signal. Clear it on every field/split/non-field transition so restored
    // contents cannot authorize a new scene.
    const GoldenSunFieldAuthScene auth_scene =
        g_golden_sun_mode0_field
            ? GoldenSunFieldAuthScene::EqualScroll
            : split_row ? GoldenSunFieldAuthScene::SplitScroll
                        : GoldenSunFieldAuthScene::None;
    if (auth_scene != g_golden_sun_field_auth_scene) {
        g_golden_sun_field_authored.reset();
        begin_golden_sun_field_auth_epoch();
        g_golden_sun_field_auth_scene = auth_scene;
    }
    // Func_b168 is measured in both the equal-scroll field and the two stable
    // Palace intervals. Keep object authorization narrower than generic Mode 0
    // by sharing only these authenticated map classes.
    g_golden_sun_expanded_obj_scene = g_golden_sun_mode0_field ||
        g_golden_sun_mode0_split_scroll;
    const GoldenSunWidePolicyReason effective_reason = split_frame_authorized
        ? split_reason : reason;
    trace_golden_sun_wide_policy(dispcnt, io, flags, effective_reason);
    trace_golden_sun_wide_scroll_row(dispcnt, io, reason, split_reason, flags);
    return flags;
}

int golden_sun_wide_tilemap_provider(int bg, int hw_x, int screen_y,
                                     std::uint16_t* out_entry) {
    const gba::GbaBus* bus = gbarecomp::active_bus();
    const bool equal_scroll_field = g_golden_sun_mode0_field;
    const bool split_scroll_palace = g_golden_sun_mode0_split_scroll &&
        g_golden_sun_palace_table_authorized;
    // Terrain-only non-Palace split-scroll field; see
    // golden_sun_field_terrain_bg. Mutually exclusive with the Palace path
    // by construction (g_golden_sun_mode0_terrain_field is only ever set
    // when split_frame_authorized is false), kept explicit here too.
    const bool terrain_field =
        g_golden_sun_mode0_terrain_field && !split_scroll_palace;
    const int terrain_bg = terrain_field
        ? gsr::widescreen::golden_sun_field_terrain_bg(
              g_golden_sun_wide_line_dispcnt)
        : 0;

    GoldenSunFieldProviderTrace* trace = nullptr;
    if (golden_sun_wide_diagnostics_enabled() && bg >= 0 && bg < 4) {
        trace = &g_golden_sun_field_provider_trace[
            static_cast<std::size_t>(bg)];
        ++trace->calls;
        if (equal_scroll_field) ++trace->equal_scroll_calls;
        else if (split_scroll_palace) ++trace->split_scroll_calls;
        trace->min_x = std::min(trace->min_x,
                                static_cast<std::int32_t>(hw_x));
        trace->max_x = std::max(trace->max_x,
                                static_cast<std::int32_t>(hw_x));
        trace->min_y = std::min(trace->min_y,
                                static_cast<std::int32_t>(screen_y));
        trace->max_y = std::max(trace->max_y,
                                static_cast<std::int32_t>(screen_y));
    }
    const auto reject = [&](bool precondition, bool boundary, bool raw,
                            bool lookup_miss) {
        if (trace) {
            ++trace->unavailable;
            if (precondition) ++trace->precondition_rejects;
            if (boundary) ++trace->boundary_rejects;
            if (raw) ++trace->raw_unavailable;
            if (lookup_miss) ++trace->lookup_misses;
        }
        return gba::kWsTilemapUnavailable;
    };
    const auto replace = [&] {
        if (trace) ++trace->replacements;
        return gba::kWsTilemapReplace;
    };
    if (!g_ws_active ||
        (!equal_scroll_field && !split_scroll_palace && !terrain_field) ||
        !g_golden_sun_wide_line_io_valid) {
        return reject(true, false, false, false);
    }
    // The terrain-only field draws margins solely from its own gameplay
    // layer; every other BG index contributes nothing outside the native
    // canvas rather than guessing a cross-layer position for it.
    if (terrain_field && bg != terrain_bg) {
        return reject(false, true, false, false);
    }
    // This is a presentation-time read of the already-active EWRAM image.
    // It does not use bus_read_* and therefore cannot mutate guest timing or
    // device state while the PPU is compositing the scanline.
    if (!bus) {
        return reject(true, false, false, false);
    }
    trace_golden_sun_field_map(bus->ewram_ptr());
    if (split_scroll_palace) {
        // The active-region cache is constructed only after the exact Palace
        // table fingerprint has authorized this scene.  A missing seed or a
        // stale cache therefore fails closed for every Palace layer.
        prepare_golden_sun_palace_active_region(
            bus, g_golden_sun_wide_line_io.data());
    }

    if (equal_scroll_field) {
        // BG3 remains the cross-layer boundary oracle for equal-scroll field
        // maps. Authored ownership is intentionally not required here because
        // restored tables may have no post-restore bitmap bits.
        std::uint16_t boundary_entry = 0;
        gsr::widescreen::GoldenSunFieldTilemapMetadata boundary_metadata;
        const bool boundary_resolved =
            gsr::widescreen::golden_sun_field_tilemap_entry(
                g_golden_sun_wide_line_dispcnt,
                g_golden_sun_wide_line_io.data(),
                g_golden_sun_wide_line_io.size(), bus->ewram_ptr(),
                256u * 1024u, 3, hw_x, screen_y, &boundary_entry,
                &boundary_metadata, nullptr);
        if (gsr::widescreen::golden_sun_field_atlas_unavailable(
                boundary_metadata)) {
            return reject(false, true, true, false);
        }

        if (bg == 3) {
            if (boundary_resolved) *out_entry = boundary_entry;
            if (boundary_resolved)
                record_golden_sun_field_map_id_attribution(
                    bg, hw_x, screen_y, boundary_metadata);
            return boundary_resolved ? replace()
                                     : reject(false, false, false, true);
        }
    }

    // The terrain layer (when active) and every equal-scroll/Palace layer
    // all read their own live raw scroll register directly -- no cross-layer
    // reconstruction.
    gsr::widescreen::GoldenSunFieldTilemapMetadata metadata;
    const bool resolved = gsr::widescreen::golden_sun_field_tilemap_entry(
                g_golden_sun_wide_line_dispcnt,
                g_golden_sun_wide_line_io.data(),
                g_golden_sun_wide_line_io.size(), bus->ewram_ptr(),
                256u * 1024u, bg, hw_x, screen_y, out_entry, &metadata,
                nullptr, false, split_scroll_palace, terrain_field);
    const auto record_palace = [&](GoldenSunPalaceMarginOutcome outcome) {
        if (split_scroll_palace)
            g_golden_sun_palace_margin_diagnostics.record(
                bg, hw_x, screen_y, outcome, metadata,
                resolved || metadata.has_raw_entry);
    };
    // Equal-scroll BG3 is the cross-layer boundary oracle above. Palace's
    // split-scroll class deliberately does not use BG3 for BG1/BG2; every
    // layer still rejects its own measured unavailable source and bad bounds.
    if (gsr::widescreen::golden_sun_field_atlas_unavailable(metadata)) {
        if (split_scroll_palace)
            record_palace(metadata.map_id == gsr::widescreen::kGoldenSunPalaceNoMapId
                              ? GoldenSunPalaceMarginOutcome::RejectedFill
                              : GoldenSunPalaceMarginOutcome::RejectedRaw);
        return reject(false, false, true, false);
    }
    if (split_scroll_palace &&
        !g_golden_sun_palace_active_region.reachable(
            bg, metadata.map_x, metadata.map_y)) {
        record_palace(metadata.map_id == gsr::widescreen::kGoldenSunPalaceNoMapId
                          ? GoldenSunPalaceMarginOutcome::RejectedFill
                          : GoldenSunPalaceMarginOutcome::RejectedMask);
        if (trace) ++trace->palace_region_rejects;
        return reject(false, true, false, false);
    }
    if (split_scroll_palace && resolved) {
        record_palace(g_golden_sun_palace_active_region.seeded(
                          bg, metadata.map_x, metadata.map_y)
                          ? GoldenSunPalaceMarginOutcome::AcceptedNativeSeed
                          : GoldenSunPalaceMarginOutcome::AcceptedConnected);
    } else if (split_scroll_palace) {
        record_palace(GoldenSunPalaceMarginOutcome::RejectedLookup);
    }
    if (resolved)
        record_golden_sun_field_map_id_attribution(
            bg, hw_x, screen_y, metadata);
    return resolved ? replace() : reject(false, false, false, true);
}

int golden_sun_wide_bg_x_provider(
    int bg, int output_x,
    int screen_y,  // Arrives in hardware space from the PPU: native rows are
                   // 0..159, margins are negative (top) or >=160 (bottom).
    int*) {
    // This provider is only for the expanded output. The runtime leaves
    // game-owned hooks installed across a live toggle, so avoid touching the
    // native/supersampled path while the view is back at 240 pixels.
    if (!g_ws_active) return 0;
    // golden_sun_suppress_bg0_margin expects canvas-space Y (0..extra_top +
    // 160 + extra_bottom, native window at [extra_top, extra_top+160)) to
    // match output_x's canvas space, so shift hardware-space screen_y here.
    const int canvas_y =
        screen_y + static_cast<int>(g_golden_sun_wide_extra_top);
    if (gsr::widescreen::golden_sun_suppress_bg0_margin(
            bg, output_x, canvas_y, g_golden_sun_wide_extra_left,
            g_golden_sun_wide_extra_right, g_golden_sun_wide_extra_top,
            g_golden_sun_wide_extra_bottom)) {
        return -1;
    }
    return 0;
}

int golden_sun_wide_obj_x_provider(int raw_x, int* out_x) {
    // The GBA OBJ X field is nine bits and normally decodes 0x100..0x1FF as
    // -256..-1.  Golden Sun's town field is already scene-authenticated by
    // the margin policy; reinterpret only the raw values needed to cover the
    // widened right edge.  Values outside that exact 24px envelope retain the
    // hardware signed decode, and non-field scenes never opt into this hook.
    if (!g_ws_active || !out_x ||
        !gsr::widescreen::golden_sun_field_obj_x_authorized(
            g_golden_sun_expanded_obj_scene, raw_x,
            g_golden_sun_wide_extra_right)) {
        return 0;
    }
    *out_x = raw_x;
    return 1;
}

void clear_golden_sun_obj_y_provenance() {
    g_golden_sun_obj_pending_provenance.fill({});
    g_golden_sun_obj_visible_provenance.fill({});
    g_golden_sun_obj_staging_provenance.fill({});
    g_golden_sun_obj_d4_source_captures.fill({});
    g_golden_sun_obj_b328_authorizations.fill({});
    g_golden_sun_obj_f0_contexts.fill({});
    g_golden_sun_obj_f0_rejected_contexts.fill({});
    g_golden_sun_obj_f0_source_observations.fill({});
    g_golden_sun_obj_f0_context_frame = UINT64_MAX;
    g_golden_sun_obj_y_edge_alias_state.fill({});
}

GoldenSunObjStagingProvenance* find_golden_sun_obj_staging(
    std::uint32_t staging_address) {
    for (auto& candidate : g_golden_sun_obj_staging_provenance) {
        if (candidate.valid && candidate.staging_address == staging_address)
            return &candidate;
    }
    return nullptr;
}

GoldenSunObjStagingProvenance* allocate_golden_sun_obj_staging(
    std::uint32_t staging_address) {
    if (auto* existing = find_golden_sun_obj_staging(staging_address))
        return existing;
    for (auto& candidate : g_golden_sun_obj_staging_provenance) {
        if (!candidate.valid) {
            candidate.staging_address = staging_address;
            return &candidate;
        }
    }
    // The guest normally uses only a small prefix of this bounded table. If
    // it ever fills, recycle the oldest entry rather than aliasing a record.
    auto* oldest = &g_golden_sun_obj_staging_provenance.front();
    for (auto& candidate : g_golden_sun_obj_staging_provenance) {
        if (candidate.frame < oldest->frame) oldest = &candidate;
    }
    *oldest = {};
    oldest->staging_address = staging_address;
    return oldest;
}

// Diagnostic only: does the guest's own actor record already hold the
// full-precision position B324/B328 just handed us, somewhere in its own
// bytes? This never changes what is drawn; it only tallies, per captured
// sample, whether the record's bytes equal the captured value under a
// handful of plausible encodings, so FACTS.md can say where (if anywhere)
// the true position lives in memory instead of only in flight.
//
// Window: the measured actor-record stride is 0x38 bytes
// (golden_sun_obj_record_identity, FACTS.md "Sprites, NPCs and shadows"),
// with the body's coordinates at +0x00 and the paired shadow's at +0x0C.
// Scanning exactly that stride -- not more -- means a hit can never be an
// alias into the next actor's record, and not less, because +0x0C is
// already known to matter and a narrower window would exclude it a priori.
//
// Encodings: unsigned/signed 16-bit, unsigned/signed 32-bit, and 16.16
// fixed point (the camera at 0x02030DB0 is 16.16, so it is a live
// candidate for other guest position fields too).
//
// Candidate kinds: the captured value (B324/B328) is a screen coordinate --
// it is what gets truncated into the OAM entry -- while a guest actor record
// would normally hold a world coordinate. Comparing the record's bytes only
// against the raw screen value asks whether the record stores screen space
// verbatim; it says nothing about world space. So each record byte pattern
// is also compared against the captured value with the room camera's
// current integer pixel position added and subtracted, covering both
// world-to-screen sign conventions. The camera itself (0x02030DB0, x then
// y, 16.16 fixed point) is read once per sample, exactly as room_buffer.cpp
// decodes it: the integer part is the high 16 bits of each 32-bit field,
// read unsigned via bus_read_u16.
constexpr std::uint32_t kGoldenSunObjPositionProbeWindowBytes = 0x38u;
enum GoldenSunObjPositionProbeEncoding {
    kGoldenSunObjPositionProbeU16 = 0,
    kGoldenSunObjPositionProbeI16,
    kGoldenSunObjPositionProbeU32,
    kGoldenSunObjPositionProbeI32,
    kGoldenSunObjPositionProbeFixed1616,
    kGoldenSunObjPositionProbeEncodingCount,
};
constexpr const char* kGoldenSunObjPositionProbeEncodingNames[
    kGoldenSunObjPositionProbeEncodingCount] = {
    "u16", "i16", "u32", "i32", "fixed16.16",
};
enum GoldenSunObjPositionProbeCandidateKind {
    kGoldenSunObjPositionProbeAsIs = 0,
    kGoldenSunObjPositionProbePlusCamera,
    kGoldenSunObjPositionProbeMinusCamera,
    kGoldenSunObjPositionProbeCandidateKindCount,
};
constexpr const char* kGoldenSunObjPositionProbeCandidateKindNames[
    kGoldenSunObjPositionProbeCandidateKindCount] = {
    "as-is", "+camera", "-camera",
};
constexpr std::uint32_t kGoldenSunObjPositionProbeCameraBase = 0x02030DB0u;
struct GoldenSunObjPositionProbeTally {
    std::uint64_t hits[kGoldenSunObjPositionProbeCandidateKindCount]
                       [kGoldenSunObjPositionProbeEncodingCount]
                       [kGoldenSunObjPositionProbeWindowBytes] = {};
};
// [0] = x axis, [1] = y axis.
GoldenSunObjPositionProbeTally g_golden_sun_obj_position_probe_hits[2];
std::uint64_t g_golden_sun_obj_position_probe_samples[2] = {0, 0};

void report_golden_sun_obj_position_probe() {
    static const char* const kAxisNames[2] = {"x", "y"};
    struct Candidate {
        int kind;
        int encoding;
        std::uint32_t offset;
        std::uint64_t hits;
    };
    for (int axis = 0; axis < 2; ++axis) {
        const std::uint64_t samples = g_golden_sun_obj_position_probe_samples[axis];
        std::fprintf(stderr,
                     "[obj-position-probe] axis=%s samples=%llu (record "
                     "window 0x00..0x%02x, keyed by the same record the "
                     "sprite recorder already trusts; candidate kinds: "
                     "as-is / +camera / -camera)\n",
                     kAxisNames[axis],
                     static_cast<unsigned long long>(samples),
                     kGoldenSunObjPositionProbeWindowBytes);
        if (samples == 0) continue;
        std::vector<Candidate> candidates;
        for (int kind = 0; kind < kGoldenSunObjPositionProbeCandidateKindCount;
             ++kind) {
            for (int enc = 0; enc < kGoldenSunObjPositionProbeEncodingCount;
                 ++enc) {
                for (std::uint32_t off = 0;
                     off < kGoldenSunObjPositionProbeWindowBytes; ++off) {
                    const std::uint64_t hits =
                        g_golden_sun_obj_position_probe_hits[axis]
                            .hits[kind][enc][off];
                    if (hits > 0) candidates.push_back({kind, enc, off, hits});
                }
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) {
                      return a.hits > b.hits;
                  });
        if (candidates.empty()) {
            std::fprintf(stderr,
                         "[obj-position-probe]   no offset/encoding/kind "
                         "matched any sample -- the true position is not "
                         "stored verbatim in this record; a different "
                         "approach is needed\n");
            continue;
        }
        const std::size_t top_n = std::min<std::size_t>(candidates.size(), 8);
        for (std::size_t i = 0; i < top_n; ++i) {
            const Candidate& c = candidates[i];
            std::fprintf(
                stderr,
                "[obj-position-probe]   kind=%-8s offset=+0x%02x "
                "encoding=%-10s hits=%llu/%llu (%.1f%%)\n",
                kGoldenSunObjPositionProbeCandidateKindNames[c.kind],
                c.offset, kGoldenSunObjPositionProbeEncodingNames[c.encoding],
                static_cast<unsigned long long>(c.hits),
                static_cast<unsigned long long>(samples),
                100.0 * static_cast<double>(c.hits) /
                    static_cast<double>(samples));
        }
        if (candidates.front().hits * 2 < samples) {
            std::fprintf(
                stderr,
                "[obj-position-probe]   best candidate covers only %.1f%% "
                "of samples -- this reads as a scatter of coincidental "
                "matches, not a real position field; do not treat it as an "
                "answer\n",
                100.0 * static_cast<double>(candidates.front().hits) /
                    static_cast<double>(samples));
        } else {
            const Candidate& c = candidates.front();
            std::fprintf(
                stderr,
                "[obj-position-probe]   kind=%s offset=+0x%02x "
                "encoding=%s matches nearly every sample (%.1f%%) -- this "
                "is the position field\n",
                kGoldenSunObjPositionProbeCandidateKindNames[c.kind],
                c.offset, kGoldenSunObjPositionProbeEncodingNames[c.encoding],
                100.0 * static_cast<double>(c.hits) /
                    static_cast<double>(samples));
        }
    }
}

// Called once per captured sample (never per pixel/scanline) from
// record_golden_sun_obj_staging, using exactly the staging key (R7) that
// function already keys its capture by. Read-only with respect to guest
// memory; the tally table above is the only state this adds.
void golden_sun_obj_position_probe_sample(std::uint32_t staging_address,
                                          bool x_axis,
                                          int logical_coordinate) {
    if (!gsr::obj_recorder_enabled()) return;
    std::uint32_t record_base = 0;
    bool shadow = false;
    if (!golden_sun_obj_record_identity(staging_address, &record_base,
                                        &shadow))
        return;
    static bool atexit_armed = false;
    if (!atexit_armed) {
        atexit_armed = true;
        std::atexit(report_golden_sun_obj_position_probe);
    }
    const int axis = x_axis ? 0 : 1;
    ++g_golden_sun_obj_position_probe_samples[axis];
    auto& tally = g_golden_sun_obj_position_probe_hits[axis];

    // Camera, read once per sample -- integer pixel part only, decoded
    // exactly as room_buffer.cpp reads it: the high 16 bits of each 32-bit
    // 16.16 field, unsigned, x then y.
    const std::int64_t camera_pixel = x_axis
        ? static_cast<std::int64_t>(bus_read_u16(
              kGoldenSunObjPositionProbeCameraBase + 2u))
        : static_cast<std::int64_t>(bus_read_u16(
              kGoldenSunObjPositionProbeCameraBase + 6u));

    const std::int64_t want_by_kind[kGoldenSunObjPositionProbeCandidateKindCount] = {
        static_cast<std::int64_t>(logical_coordinate),
        static_cast<std::int64_t>(logical_coordinate) + camera_pixel,
        static_cast<std::int64_t>(logical_coordinate) - camera_pixel,
    };
    for (int kind = 0; kind < kGoldenSunObjPositionProbeCandidateKindCount;
         ++kind) {
        const std::int64_t want = want_by_kind[kind];
        for (std::uint32_t offset = 0;
             offset + 2u <= kGoldenSunObjPositionProbeWindowBytes; ++offset) {
            const std::uint16_t raw16 = bus_read_u16(record_base + offset);
            if (static_cast<std::int64_t>(raw16) == want)
                ++tally.hits[kind][kGoldenSunObjPositionProbeU16][offset];
            if (static_cast<std::int64_t>(static_cast<std::int16_t>(raw16)) ==
                want)
                ++tally.hits[kind][kGoldenSunObjPositionProbeI16][offset];
        }
        for (std::uint32_t offset = 0;
             offset + 4u <= kGoldenSunObjPositionProbeWindowBytes; ++offset) {
            const std::uint32_t raw32 = bus_read_u32(record_base + offset);
            if (static_cast<std::int64_t>(raw32) == want)
                ++tally.hits[kind][kGoldenSunObjPositionProbeU32][offset];
            const std::int32_t signed32 = static_cast<std::int32_t>(raw32);
            if (static_cast<std::int64_t>(signed32) == want)
                ++tally.hits[kind][kGoldenSunObjPositionProbeI32][offset];
            // 16.16 fixed point, integer part only: arithmetic right shift
            // keeps the sign, matching how the 16.16 camera field is read
            // elsewhere in this file.
            if (static_cast<std::int64_t>(signed32 >> 16) == want)
                ++tally.hits[kind][kGoldenSunObjPositionProbeFixed1616][offset];
        }
    }
}

void record_golden_sun_obj_staging(std::uint32_t instruction_pc,
                                   bool x_axis, int logical_coordinate) {
    if (!golden_sun_expanded_obj_view_active()) return;
    auto* staging = allocate_golden_sun_obj_staging(g_cpu.R[7]);
    if (!staging) return;
    staging->valid = true;
    staging->frame = runtime_current_frame();
    staging->auth_epoch = g_golden_sun_field_auth_epoch;
    if (x_axis) {
        staging->x_valid = true;
        staging->logical_x = static_cast<std::int16_t>(logical_coordinate);
        staging->x_writer_branch_pc = instruction_pc;
        golden_sun_obj_position_probe_sample(g_cpu.R[7], true,
                                             staging->logical_x);
    } else {
        staging->y_valid = true;
        staging->logical_y = static_cast<std::int16_t>(logical_coordinate);
        staging->y_writer_branch_pc = instruction_pc;
        golden_sun_obj_position_probe_sample(g_cpu.R[7], false,
                                             staging->logical_y);
        if (instruction_pc == 0x0800B328u && logical_coordinate >= 160 &&
            logical_coordinate <= 199) {
            staging->y_correlation = capture_golden_sun_obj_y_correlation(
                logical_coordinate);
        } else {
            staging->y_correlation = {};
        }
    }
    if (golden_sun_wide_diagnostics_enabled() &&
        g_golden_sun_obj_y_provenance_logs_in_epoch <
            kGoldenSunObjYProvenanceLogLimitPerEpoch) {
        ++g_golden_sun_obj_y_provenance_logs_in_epoch;
        std::fprintf(stderr,
                     "[wide-obj-stage] frame=%llu auth_epoch=%llu "
                     "branch_pc=0x%08x axis=%c staging=0x%08x logical=%d\n",
                     static_cast<unsigned long long>(staging->frame),
                     static_cast<unsigned long long>(staging->auth_epoch),
                     instruction_pc, x_axis ? 'x' : 'y',
                     staging->staging_address, logical_coordinate);
    }
}

void clear_golden_sun_obj_staging(std::uint32_t staging_address) {
    if (auto* staging = find_golden_sun_obj_staging(staging_address))
        *staging = {};
}

bool read_golden_sun_obj_staging_attrs(std::uint32_t address,
                                       std::uint16_t* attr0,
                                       std::uint16_t* attr1,
                                       std::uint16_t* attr2) {
    constexpr std::uint32_t kIwramStart = 0x03000000u;
    constexpr std::uint32_t kIwramEnd = 0x03008000u;
    constexpr std::uint32_t kRecordBytes = 12u;
    if (!attr0 || !attr1 || !attr2 || (address & 3u) != 0u ||
        address < kIwramStart || address > kIwramEnd - kRecordBytes)
        return false;
    const std::uint32_t record_word1 = bus_read_u32(address + 4u);
    const std::uint32_t record_word2 = bus_read_u32(address + 8u);
    *attr0 = static_cast<std::uint16_t>(record_word1 & 0xFFFFu);
    *attr1 = static_cast<std::uint16_t>(record_word1 >> 16);
    *attr2 = static_cast<std::uint16_t>(record_word2 & 0xFFFFu);
    return true;
}

bool golden_sun_obj_b328_writer_authorized(std::uint32_t staging_address,
                                            std::uint64_t frame) {
    std::uint32_t record_base = 0;
    bool shadow = false;
    if (!golden_sun_obj_record_identity(staging_address, &record_base,
                                        &shadow) ||
        shadow || record_base != staging_address) return false;
    const GoldenSunObjB328Authorization* authorization = nullptr;
    for (const auto& candidate : g_golden_sun_obj_b328_authorizations) {
        if (candidate.valid && candidate.record_base == record_base) {
            authorization = &candidate;
            break;
        }
    }
    if (!authorization ||
        authorization->auth_epoch != g_golden_sun_field_auth_epoch ||
        authorization->frame == UINT64_MAX || authorization->frame >= frame ||
        frame - authorization->frame != 1u) return false;
    std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
    return read_golden_sun_obj_staging_attrs(
               staging_address, &attr0, &attr1, &attr2) &&
        attr0 == authorization->attr0 && attr1 == authorization->attr1 &&
        attr2 == authorization->attr2;
}

void remember_golden_sun_obj_b328_authorization(
    std::uint32_t record_base, std::uint16_t attr0, std::uint16_t attr1,
    std::uint16_t attr2) {
    GoldenSunObjB328Authorization* authorization = nullptr;
    for (auto& candidate : g_golden_sun_obj_b328_authorizations) {
        if (candidate.valid && candidate.record_base == record_base) {
            authorization = &candidate;
            break;
        }
        if (!candidate.valid && !authorization) authorization = &candidate;
    }
    if (!authorization) {
        authorization = &g_golden_sun_obj_b328_authorizations.front();
        for (auto& candidate : g_golden_sun_obj_b328_authorizations) {
            if (candidate.frame < authorization->frame)
                authorization = &candidate;
        }
    }
    *authorization = {true, record_base, runtime_current_frame(),
                      g_golden_sun_field_auth_epoch, attr0, attr1, attr2};
}

void capture_golden_sun_obj_d4_source(std::uint32_t entry_pc,
                                      std::uint32_t staging_address,
                                      std::uint32_t destination) {
    const int slot = gsr::widescreen::golden_sun_oam_shadow_slot(destination);
    if (slot < 0 || static_cast<std::size_t>(slot) >=
                         g_golden_sun_obj_d4_source_captures.size()) return;
    std::uint32_t record_base = 0;
    bool shadow = false;
    if (!golden_sun_obj_record_identity(staging_address, &record_base,
                                        &shadow)) return;
    (void)record_base;
    (void)shadow;
    auto& capture = g_golden_sun_obj_d4_source_captures[
        static_cast<std::size_t>(slot)];
    const std::uint64_t frame = runtime_current_frame();
    const std::uint64_t epoch = g_golden_sun_field_auth_epoch;
    if (capture.frame != frame || capture.auth_epoch != epoch) {
        capture = {};
        capture.frame = frame;
        capture.auth_epoch = epoch;
    }
    if (capture.blocked) return;
    if (!capture.valid) {
        capture.valid = true;
        capture.entry_pc = entry_pc;
        capture.staging_address = staging_address;
        capture.target_address = destination;
        return;
    }
    if (capture.entry_pc == entry_pc &&
        capture.staging_address == staging_address &&
        capture.target_address == destination) return;
    // Two authenticated D4 entries claimed the same slot in this frame with
    // different source identity. Do not let a later one win by accident.
    capture.valid = false;
    capture.blocked = true;
}

void golden_sun_obj_staging_handoff(
    std::uint32_t entry_pc, std::uint32_t staging_address_override = 0u,
    std::uint32_t destination_override = 0u) {
    // D4 loads {R6,R7,R8} from the staging record in R6, then its following
    // store writes R7/R8 to the OAM-shadow destination in R0.
    if (!golden_sun_func1dc8_writer_pc(
            entry_pc, gsr::Func1dc8WriterRoute::D4) ||
        !golden_sun_expanded_obj_view_active())
        return;
    const std::uint32_t staging_address = staging_address_override != 0u
        ? staging_address_override : g_cpu.R[6];
    const std::uint32_t destination = destination_override != 0u
        ? destination_override : g_cpu.R[0];
    const int slot = gsr::widescreen::golden_sun_oam_shadow_slot(destination);
    std::uint32_t record_base = 0;
    bool shadow = false;
    const bool record_identity_valid = golden_sun_obj_record_identity(
        staging_address, &record_base, &shadow);
    // This call is the pre-LDM seam: R6 is still the guest's actual source
    // pointer. Preserve it for a same-frame D8 resume before any provenance
    // lookup can reject the record. The override form is used only by that
    // resumed-store handoff and must never capture post-LDM R6.
    if (staging_address_override == 0u && destination_override == 0u)
        capture_golden_sun_obj_d4_source(entry_pc, g_cpu.R[6], destination);
    std::uint16_t observed_attr0 = 0;
    std::uint16_t observed_attr1 = 0;
    std::uint16_t observed_attr2 = 0;
    const bool observed_attrs_valid = read_golden_sun_obj_staging_attrs(
        staging_address, &observed_attr0, &observed_attr1, &observed_attr2);
    // Nonzero overrides are supplied only after the D8 store observer has
    // matched this store to its same-frame pre-LDM D4 source token. Preserve
    // that exact body-record identity and ATTR tuple for the measured
    // next-frame B328 authorization; entry-only observations cannot arm it.
    if (staging_address_override != 0u && destination_override != 0u &&
        record_identity_valid && !shadow && record_base == staging_address &&
        observed_attrs_valid && slot >= 0 &&
        static_cast<std::size_t>(slot) <
            g_golden_sun_obj_pending_provenance.size()) {
        remember_golden_sun_obj_b328_authorization(
            record_base, observed_attr0, observed_attr1, observed_attr2);
    }
    record_golden_sun_b328_rejected_writer(
        GoldenSunObjB328WriterRoute::D4, staging_address, slot,
        observed_attr0, observed_attr1, observed_attr2,
        runtime_current_frame(), runtime_call_stack_depth(),
        golden_sun_obj_call_return_pc(runtime_call_stack_depth()), 0u);
    auto* body_staging = record_identity_valid
        ? find_golden_sun_obj_staging(shadow ? record_base : staging_address)
        : nullptr;
    if (staging_address_override == 0u && destination_override == 0u &&
        record_identity_valid && shadow &&
        gsr::obj_recorder_enabled()) {
        bool token_found = false;
        if (slot >= 0 && static_cast<std::size_t>(slot) <
                             g_golden_sun_obj_d4_source_captures.size()) {
            const auto& capture = g_golden_sun_obj_d4_source_captures[
                static_cast<std::size_t>(slot)];
            token_found = capture.valid &&
                capture.frame == runtime_current_frame() &&
                capture.auth_epoch == g_golden_sun_field_auth_epoch &&
                capture.entry_pc == entry_pc &&
                capture.staging_address == staging_address &&
                capture.target_address == destination;
        }
        record_golden_sun_obj_d4_event(
            token_found ? "entry-token-found" : "entry-token-missing",
            slot, staging_address, destination, body_staging);
    }
    // The shadow record has no coordinates of its own.  D4 receives the
    // shadow record at body+0x0C, while the staging table is keyed by the
    // body record that B324/B328 updated.  Reuse that paired-body lookup here
    // as golden_sun_obj_resolve_placement does at the commit seam.
    if (!record_identity_valid) return;
    auto* staging = body_staging;
    if (!staging || !staging->valid || staging->auth_epoch !=
        g_golden_sun_field_auth_epoch ||
        staging->frame != runtime_current_frame()) {
        if (staging_address_override == 0u && destination_override == 0u &&
            shadow && gsr::obj_recorder_enabled()) {
            record_golden_sun_obj_d4_event(
                staging ? "handoff-body-stale" : "handoff-body-missing",
                slot, staging_address, destination, body_staging);
        }
        return;
    }
    if (slot < 0 || static_cast<std::size_t>(slot) >=
                         g_golden_sun_obj_pending_provenance.size()) return;
    int resolved_x = staging->logical_x;
    int resolved_y = staging->logical_y;
    if (shadow) {
        // Authenticate the paired body's current fields before recovering
        // the shadow's own offset, as the F0 placement resolver does.
        std::uint16_t body_attr0 = 0, body_attr1 = 0, body_attr2 = 0;
        int width = 0, height = 0, checked_x = 0, checked_y = 0;
        const bool body_matches = observed_attrs_valid &&
            staging->x_valid && staging->y_valid &&
            read_golden_sun_obj_staging_attrs(
                record_base, &body_attr0, &body_attr1, &body_attr2) &&
            gsr::widescreen::golden_sun_obj_dimensions(
                (body_attr0 >> 14) & 3u, (body_attr1 >> 14) & 3u,
                &width, &height) &&
            gsr::widescreen::golden_sun_obj_resolve_oam_x(
                body_attr1 & 0x1FFu, true, staging->logical_x, &checked_x) &&
            gsr::widescreen::golden_sun_obj_resolve_oam_y(
                body_attr0 & 0xFFu, true, staging->logical_y, &checked_y);
        if (!body_matches) {
            record_golden_sun_obj_d4_event("handoff-body-checks", slot,
                staging_address, destination, staging);
            return;
        }
        resolved_x = gsr::widescreen::golden_sun_obj_paired_coordinate(
            staging->logical_x, observed_attr1 & 0x1FFu, 9u);
        resolved_y = gsr::widescreen::golden_sun_obj_paired_coordinate(
            staging->logical_y, observed_attr0 & 0xFFu, 8u);
    }
    auto& output = g_golden_sun_obj_pending_provenance[
        static_cast<std::size_t>(slot)];
    if (output.commit_resolved && output.frame == staging->frame) return;
    ++g_golden_sun_obj_provenance_generation;

    // The authenticated D4 entry executes `ldmia r6, {r6,r7,r8}` before its
    // following store of R7/R8 as the two OAM words. Read that exact 12-byte
    // IWRAM record at the entry seam, with bounds/alignment checks, so the
    // provenance cannot be reused for a later slot occupant. Word 0 is the
    // record's working value; words 1/2 are ATTR0|ATTR1 and ATTR2|padding.
    std::uint16_t expected_attr0 = observed_attr0;
    std::uint16_t expected_attr1 = observed_attr1;
    std::uint16_t expected_attr2 = observed_attr2;
    const bool oam_identity_valid = observed_attrs_valid;
    output.valid = staging->x_valid || staging->y_valid;
    output.x_valid = staging->x_valid;
    output.y_valid = staging->y_valid;
    output.logical_x = static_cast<std::int16_t>(resolved_x);
    output.logical_y = static_cast<std::int16_t>(resolved_y);
    output.frame = staging->frame;
    output.auth_epoch = staging->auth_epoch;
    output.writer_branch_pc = staging->y_writer_branch_pc;
    output.target_address = destination;
    output.writer_generation = 0;
    output.oam_identity_valid = oam_identity_valid;
    output.expected_attr0 = expected_attr0;
    output.expected_attr1 = expected_attr1;
    output.expected_attr2 = expected_attr2;
    record_golden_sun_b328_handoff(
        staging_address, staging->frame, oam_identity_valid);
    note_golden_sun_obj_commit_order(
        GoldenSunObjCommitOrderRoute::D4, staging_address, slot,
        expected_attr0, expected_attr1, expected_attr2, staging->x_valid,
        output.logical_x, staging->y_valid, output.logical_y);
    if (oam_identity_valid) {
        trace_golden_sun_obj_y_correlation(
            staging->y_correlation, staging_address, slot, expected_attr0,
            expected_attr1, expected_attr2);
    }
    if (golden_sun_wide_diagnostics_enabled() &&
        g_golden_sun_obj_y_provenance_logs_in_epoch <
            kGoldenSunObjYProvenanceLogLimitPerEpoch) {
        ++g_golden_sun_obj_y_provenance_logs_in_epoch;
        std::fprintf(stderr,
                     "[wide-obj-handoff] frame=%llu auth_epoch=%llu "
                     "staging=0x%08x destination=0x%08x slot=%d "
                     "x_valid=%u logical_x=%d y_valid=%u logical_y=%d\n",
                     static_cast<unsigned long long>(runtime_current_frame()),
                     static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                     staging_address, destination, slot,
                     staging->x_valid ? 1u : 0u,
                     static_cast<int>(output.logical_x),
                     staging->y_valid ? 1u : 0u,
                     static_cast<int>(output.logical_y));
    }
}

// D4's post-LDM store is four bytes after the authenticated entry. A resumed
// D4 call can enter at that instruction without running the function-entry
// hook, so authenticate the preceding entry rather than accepting the store
// PC as a free-standing RAM address.
bool golden_sun_func1dc8_d4_store_pc(std::uint32_t pc) {
    return pc >= 4u && golden_sun_func1dc8_writer_pc(
        pc - 4u, gsr::Func1dc8WriterRoute::D4);
}

void trace_golden_sun_obj_y_attempt(std::uint32_t instruction_pc,
                                    const char* reason,
                                    std::uint32_t candidate_y,
                                    std::uint32_t address,
                                    bool address_valid, int slot) {
    if (!golden_sun_wide_diagnostics_enabled() ||
        g_golden_sun_obj_y_provenance_logs_in_epoch >=
            kGoldenSunObjYProvenanceLogLimitPerEpoch) return;
    ++g_golden_sun_obj_y_provenance_logs_in_epoch;
    std::fprintf(stderr,
                 "[wide-obj-y] frame=%llu auth_epoch=%llu reason=%s "
                 "branch_pc=0x%08x r7=0x%08x address=0x%08x "
                 "address_valid=%u slot=%d candidate_y=%u scene=%u "
                 "geometry=active:%u,top:%u,bottom:%u\n",
                 static_cast<unsigned long long>(runtime_current_frame()),
                 static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
                 reason, instruction_pc, g_cpu.R[7], address,
                 address_valid ? 1u : 0u, slot, candidate_y,
                 g_golden_sun_expanded_obj_scene ? 1u : 0u,
                 g_ws_active ? 1u : 0u, g_golden_sun_wide_extra_top,
                 g_golden_sun_wide_extra_bottom);
}

int golden_sun_obj_oam_truncated(int logical, int bits) {
    const int modulus = 1 << bits;
    int result = logical % modulus;
    if (result < 0) result += modulus;
    return result;
}

void record_golden_sun_obj_y_transition(
    int slot, int raw_y, int canonical_y, int output_y,
    GoldenSunObjYTransitionResolution resolution,
    GoldenSunObjYTransitionReason reason,
    const GoldenSunObjPlacementProvenance& provenance,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    if (slot < 0 || static_cast<std::size_t>(slot) >=
            gsr::widescreen::kGoldenSunOamShadowSlotCount) return;
    const bool in_transition_band = raw_y >= 159 && raw_y <= 199;
    // Recorder-only out-of-band calls carry a real rejection reason. Keep
    // accepted samples and the existing in-band samples available to the
    // recorder, while leaving the aggregate WIDE diagnostic gate below
    // unchanged.
    const bool record_lifetime = gsr::obj_recorder_enabled() &&
        (reason != GoldenSunObjYTransitionReason::Accepted ||
         (golden_sun_wide_diagnostics_enabled() && in_transition_band));
    if (record_lifetime) {
        // One sample per slot/frame/epoch/upload. No global first-N cap.
        struct Last {
            bool valid = false;
            std::uint64_t frame = 0, epoch = 0, dma = 0;
            GoldenSunObjYTransitionReason reason{};
            std::uint16_t a0 = 0, a1 = 0, a2 = 0;
        };
        static std::array<Last, gsr::widescreen::kGoldenSunOamShadowSlotCount> last{};
        auto& l = last[static_cast<std::size_t>(slot)];
        const auto frame = runtime_current_frame();
        if (!l.valid || l.frame != frame || l.epoch != g_golden_sun_field_auth_epoch ||
            l.dma != g_obj_lifetime_dma || l.reason != reason ||
            l.a0 != attr0 || l.a1 != attr1 || l.a2 != attr2) {
            l = {true, frame, g_golden_sun_field_auth_epoch, g_obj_lifetime_dma,
                reason, attr0, attr1, attr2};
            note_obj_lifetime("render", golden_sun_obj_y_transition_reason_name(reason),
                slot, 0x07000000u, attr0, attr1, attr2, provenance);
        }
    }
    if (!golden_sun_wide_diagnostics_enabled() || !in_transition_band) return;
    const auto region = golden_sun_obj_y_transition_region(output_y);
    auto& count = g_golden_sun_obj_y_transition_counts[
        static_cast<std::size_t>(reason)][static_cast<std::size_t>(region)]
        [static_cast<std::size_t>(resolution)];
    ++count;

    // ATTR0=0 and the hardware disabled bit identify empty/disabled OAM;
    // 0x00C0 is the measured dormant entry. Do not spend reserved evidence
    // budget on those slots, while retaining the uncapped aggregate above.
    const bool affine = (attr0 & 0x0100u) != 0u;
    const bool disabled = !affine && (attr0 & 0x0200u) != 0u;
    if ((attr0 == 0u && attr1 == 0u && attr2 == 0u) ||
        attr0 == 0x00C0u || disabled)
        return;

    const std::uint32_t expected_target =
        gsr::widescreen::kGoldenSunOamShadowStart +
        static_cast<std::uint32_t>(slot) *
            gsr::widescreen::kGoldenSunOamShadowSlotBytes;
    GoldenSunObjYTransitionSample sample{};
    sample.valid = true;
    sample.frame = runtime_current_frame();
    sample.slot = slot;
    sample.raw_y = raw_y;
    sample.canonical_y = canonical_y;
    sample.output_y = output_y;
    sample.resolution = resolution;
    sample.reason = reason;
    sample.region = region;
    sample.attr0 = attr0;
    sample.attr1 = attr1;
    sample.attr2 = attr2;
    sample.expected_attr0 = provenance.expected_attr0;
    sample.expected_attr1 = provenance.expected_attr1;
    sample.expected_attr2 = provenance.expected_attr2;
    sample.expected_target = expected_target;
    sample.provenance_frame = provenance.frame;
    sample.provenance_epoch = provenance.auth_epoch;
    sample.target_address = provenance.target_address;
    sample.writer_pc = provenance.writer_branch_pc;
    sample.writer_generation = provenance.writer_generation;

    const GoldenSunObjYTransitionIdentity identity{
        true, slot, expected_target, attr0, attr1, attr2,
        provenance.expected_attr0, provenance.expected_attr1,
        provenance.expected_attr2};
    const auto identity_matches = [](const GoldenSunObjYTransitionIdentity& a,
                                     const GoldenSunObjYTransitionIdentity& b) {
        return a.valid && b.valid && a.slot == b.slot &&
            a.target == b.target && a.attr0 == b.attr0 &&
            a.attr1 == b.attr1 && a.attr2 == b.attr2 &&
            a.expected_attr0 == b.expected_attr0 &&
            a.expected_attr1 == b.expected_attr1 &&
            a.expected_attr2 == b.expected_attr2;
    };
    const auto append_sample = [&](GoldenSunObjYTransitionBucket bucket) {
        const auto bucket_index = static_cast<std::size_t>(bucket);
        auto& sample_count = g_golden_sun_obj_y_transition_sample_counts[
            bucket_index];
        if (sample_count >= kGoldenSunObjYTransitionSampleLimit) {
            ++g_golden_sun_obj_y_transition_samples_dropped[bucket_index];
            return;
        }
        g_golden_sun_obj_y_transition_samples[bucket_index][sample_count++] =
            sample;
    };

    // Buckets A and B are first-sample-per stable hardware identity. Their
    // independent caps ensure top-wrap candidates cannot starve signed-bottom
    // evidence.
    const auto append_first_identity = [&](GoldenSunObjYTransitionBucket bucket) {
        const auto seen_index = bucket ==
            GoldenSunObjYTransitionBucket::ActiveCandidateCanonical ? 0u : 1u;
        for (unsigned i = 0; i < g_golden_sun_obj_y_transition_seen_counts[
                 seen_index]; ++i) {
            if (identity_matches(g_golden_sun_obj_y_transition_seen[seen_index][i],
                                 identity)) return;
        }
        auto& seen_count = g_golden_sun_obj_y_transition_seen_counts[seen_index];
        if (seen_count < kGoldenSunObjYTransitionSampleLimit)
            g_golden_sun_obj_y_transition_seen[seen_index][seen_count++] = identity;
        append_sample(bucket);
    };

    if (resolution == GoldenSunObjYTransitionResolution::Canonical) {
        append_first_identity(
            GoldenSunObjYTransitionBucket::ActiveCandidateCanonical);
    } else if (resolution == GoldenSunObjYTransitionResolution::Signed &&
               region == GoldenSunObjYTransitionRegion::Bottom) {
        append_first_identity(GoldenSunObjYTransitionBucket::SignedBottom);
    }

    // Bucket C records changes between canonical fallback and signed-bottom
    // outcomes for the same slot/target. It intentionally does not define a
    // semantic NPC identity beyond measured hardware fields.
    if (resolution == GoldenSunObjYTransitionResolution::Canonical ||
        (resolution == GoldenSunObjYTransitionResolution::Signed &&
         region == GoldenSunObjYTransitionRegion::Bottom)) {
        bool previous_found = false;
        GoldenSunObjYTransitionOutcomeState* state = nullptr;
        for (auto& candidate : g_golden_sun_obj_y_transition_outcome_states) {
            if (candidate.valid && candidate.slot == slot &&
                candidate.target == expected_target) {
                state = &candidate;
                previous_found = true;
                break;
            }
        }
        if (!state) {
            for (auto& candidate : g_golden_sun_obj_y_transition_outcome_states) {
                if (!candidate.valid) {
                    candidate.valid = true;
                    candidate.slot = slot;
                    candidate.target = expected_target;
                    state = &candidate;
                    break;
                }
            }
        }
        if (state) {
            if (previous_found && state->resolution != resolution)
                append_sample(GoldenSunObjYTransitionBucket::OutcomeTransition);
            state->resolution = resolution;
        }
    }
}

bool golden_sun_expanded_obj_view_active() {
    return g_ws_active && gsr::widescreen::golden_sun_expanded_view_active(
        gsr::widescreen::kExpandedWidth, gsr::widescreen::kExpandedHeight,
        g_golden_sun_wide_extra_left, g_golden_sun_wide_extra_right,
        g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom);
}

// First-failing-clause bucket for golden_sun_obj_provider_provenance's
// usable() predicate below. Measurement only: this labels which conjunct of
// the existing check rejected the candidate; it does not change which
// candidate is selected.
enum GoldenSunObjRejectReason : int {
    kGoldenSunObjRejectNoRecord = 0,
    kGoldenSunObjRejectWrongEpoch = 1,
    kGoldenSunObjRejectPendingFrame = 2,
    kGoldenSunObjRejectIdentity = 3,
    kGoldenSunObjRejectAttrsMoved = 4,
    kGoldenSunObjRejectTruncation = 5,
    kGoldenSunObjRejectOk = 6,
};

// Sub-buckets breaking down kGoldenSunObjRejectAttrsMoved, tallied only when
// that outcome is the one attributed to the object (see below). 7/8 are
// mutually exclusive and together equal bucket 4. 9/10/11 may overlap -- an
// object with two differing attributes increments two of them -- and at
// least one of the three fires whenever bucket 4 does and the candidate's
// identity was otherwise valid. Measurement only: these do not change which
// candidate is selected or the attrs_match rule itself.
constexpr std::size_t kGoldenSunObjRejectAttrsThisFrame = 7;
constexpr std::size_t kGoldenSunObjRejectAttrsOldFrame = 8;
constexpr std::size_t kGoldenSunObjRejectAttr0Differs = 9;
constexpr std::size_t kGoldenSunObjRejectAttr1Differs = 10;
constexpr std::size_t kGoldenSunObjRejectAttr2Differs = 11;

// Per-object-per-frame reject-reason tally for golden_sun_obj_provider_
// provenance, indexed by GoldenSunObjRejectReason (0..6) plus the
// attrs_moved breakdown sub-buckets above (7..11). Gated on
// gba::g_ws_obj_census_line (set by render_scanline_wide only while it
// renders logical_y == 0) so this is a once-per-frame count comparable to
// gba::g_ws_obj_trusted_total / g_ws_obj_untrusted_total, which use the same
// gate. Accumulated monotonically, never reset here; the tracer reads the
// per-frame delta. Follows the same plain-array, C-linkage pattern as
// gba::g_ws_expanded_diag.
extern "C" unsigned long long g_ws_obj_reject_totals[12] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// Per-object-per-frame tally of sprites resolved via the persistent position
// table (golden_sun_obj_track_lookup, defined below) rather than the record
// path above it or the raw-coordinate fallback beneath it. Same running-
// total-since-session-start shape as g_ws_obj_reject_totals, and likewise
// only incremented by the X provider so each object is counted once (the Y
// provider queries the same memoised table lookup but does not tally).
extern "C" unsigned long long g_ws_obj_from_track_total = 0;

// Forward declaration: golden_sun_obj_provider_provenance below calls this to
// populate the persistent position table (defined further down this file,
// alongside golden_sun_obj_track_lookup) the moment it authenticates a
// sprite. Declared here so that single call site can exist inside
// provenance's own once-per-object-per-frame memo recompute, which is above
// the table's definition in file order.
void golden_sun_obj_track_confirm(int x, int y, std::uint16_t attr0,
                                  std::uint16_t attr1, std::uint16_t attr2,
                                  std::uint64_t frame);

// Select the provenance for the OAM image currently being rendered. Slot
// recycling is live and frequent (FACTS.md 2026-09-06/09-11): the game
// reshuffles which character occupies which hardware OAM slot, so a record
// keyed by slot index cannot survive it -- the slot at oam_index this frame
// may hold a different sprite than the one that produced the record. Records
// are therefore found by IDENTITY instead: a candidate is eligible when its
// epoch, freshness (pending-frame rule), full ATTR0/1/2 identity and
// hardware-truncated coordinates all agree with what is being rendered,
// regardless of which slot it was filed under. The DMA-latched (visible)
// store is searched first and wins outright over the pending store.
//
// Matching ATTRs alone is insufficient: full precision coordinates separated
// by an OAM wrap can share them. The old slot key incidentally guarded
// against this; identity does not, so an eligible match must additionally be
// UNIQUE within its store. Two or more equally-eligible records means the
// object cannot be told apart from another -- refuse rather than guess.
//
// tally_rejection: attribute this object's outcome to g_ws_obj_reject_totals.
// Both the X and Y wide providers call this for the same object on the same
// frame; only the X provider passes true, so each object is counted once
// (Y deliberately does not tally). The bucket recorded is the VISIBLE
// slot's own reason (kept as a diagnostic baseline, unchanged from before
// this identity search existed), unless: the identity search actually found
// a usable record, in which case the bucket is "ok"; or the identity search
// found two-or-more equally-eligible records in either store, in which case
// the outcome is attributed to the existing "identity" bucket (ambiguous
// identity is a kind of identity failure, and this keeps signals.csv's
// column meanings unchanged rather than adding a new one).
//
// Called once per object PER SCANLINE by both providers (up to 160 times a
// frame per on-screen object), so the identity search is memoised per OAM
// entry. The memo is keyed by frame AND by
// g_golden_sun_obj_provenance_generation, not by frame alone: the visible
// store is replaced wholesale at the shadow->OAM handoff, but the PENDING
// store is written per slot by the commit path DURING a frame, so a
// frame-only key would hide a record committed after the first scanline until
// the next frame. With both in the key the memo is exact rather than usually
// right. Cost note: a frame that commits heavily while the PPU is drawing
// will invalidate and re-scan; that has not been measured as a problem, and
// correctness came first here -- if it ever shows up, measure before
// reshaping it.
const GoldenSunObjPlacementProvenance*
golden_sun_obj_provider_provenance(int oam_index, std::uint16_t attr0,
                                   std::uint16_t attr1, std::uint16_t attr2,
                                   int raw_x, int raw_y,
                                   bool tally_rejection,
                                   bool allow_track_confirm) {
    if (oam_index < 0 || static_cast<std::size_t>(oam_index) >=
                            g_golden_sun_obj_visible_provenance.size())
        return nullptr;
    const std::uint32_t slot_offset =
        static_cast<std::uint32_t>(oam_index) *
        gsr::widescreen::kGoldenSunOamShadowSlotBytes;
    const std::uint32_t main_target =
        gsr::widescreen::kGoldenSunOamShadowStart + slot_offset;
    // Same conjunction, same order, same short-circuiting as before this
    // instrumentation: each branch below still bails at the first failing
    // clause of the original expression, just labelled with which clause it
    // was instead of collapsing straight to false. check_slot selects
    // between the old by-slot identity check (kept only for the diagnostic
    // baseline below) and the new by-identity search, which every other
    // clause is shared with unchanged.
    const auto usable = [&](const GoldenSunObjPlacementProvenance& candidate,
                            bool pending,
                            bool check_slot) -> GoldenSunObjRejectReason {
        if (!candidate.valid || !candidate.x_valid || !candidate.y_valid)
            return kGoldenSunObjRejectNoRecord;
        if (candidate.auth_epoch != g_golden_sun_field_auth_epoch)
            return kGoldenSunObjRejectWrongEpoch;
        if (pending && candidate.frame != runtime_current_frame())
            return kGoldenSunObjRejectPendingFrame;
        if (!candidate.oam_identity_valid ||
            (check_slot && candidate.target_address != main_target))
            return kGoldenSunObjRejectIdentity;
        if (!gsr::widescreen::golden_sun_obj_provenance_attrs_match(
                candidate.oam_identity_valid, candidate.expected_attr0,
                candidate.expected_attr1, candidate.expected_attr2, attr0,
                attr1, attr2))
            return kGoldenSunObjRejectAttrsMoved;
        if (golden_sun_obj_oam_truncated(candidate.logical_x, 9) != raw_x ||
            golden_sun_obj_oam_truncated(candidate.logical_y, 8) != raw_y)
            return kGoldenSunObjRejectTruncation;
        return kGoldenSunObjRejectOk;
    };
    // Scan `store` for identity-eligible records (every usable() clause
    // except the slot key). Stops counting past 2: callers only need to
    // distinguish "none", "exactly one" and "ambiguous".
    const auto find_by_identity = [&](
        const std::array<GoldenSunObjPlacementProvenance,
                          gsr::widescreen::kGoldenSunOamShadowSlotCount>&
            store,
        bool pending) {
        struct Match {
            const GoldenSunObjPlacementProvenance* record = nullptr;
            int count = 0;
        } match;
        for (const auto& candidate : store) {
            if (usable(candidate, pending, /*check_slot=*/false) !=
                kGoldenSunObjRejectOk) continue;
            if (match.record == nullptr) {
                match.record = &candidate;
                match.count = 1;
                continue;
            }
            // Two eligible records that AGREE are not an ambiguity -- they
            // are the same character described twice, which is exactly what
            // a slot reshuffle leaves behind: the record filed under the old
            // slot and the one filed under the new slot both still describe
            // this sprite. Counting that as ambiguous would refuse precisely
            // the case this identity search exists to rescue. Only a genuine
            // disagreement about where the sprite is means we cannot tell
            // two sprites apart.
            if (candidate.logical_x != match.record->logical_x ||
                candidate.logical_y != match.record->logical_y) {
                match.count = 2;
                break;
            }
        }
        return match;
    };
    // Memo: identity resolution is exact for the whole frame (see the
    // function comment), so cache it per OAM entry and reuse it for every
    // scanline this frame instead of re-scanning up to 256 records per call.
    struct Memo {
        std::uint64_t frame = UINT64_MAX;
        std::uint64_t generation = UINT64_MAX;
        const GoldenSunObjPlacementProvenance* result = nullptr;
        GoldenSunObjRejectReason reason = kGoldenSunObjRejectNoRecord;
    };
    static std::array<Memo, gsr::widescreen::kGoldenSunOamShadowSlotCount>
        memo{};
    Memo& slot_memo = memo[static_cast<std::size_t>(oam_index)];
    const std::uint64_t frame_now = runtime_current_frame();
    const std::uint64_t generation_now = g_golden_sun_obj_provenance_generation;
    const auto& visible = g_golden_sun_obj_visible_provenance[
        static_cast<std::size_t>(oam_index)];
    if (slot_memo.frame != frame_now ||
        slot_memo.generation != generation_now) {
        const GoldenSunObjRejectReason visible_slot_reason =
            usable(visible, false, /*check_slot=*/true);
        const auto visible_match =
            find_by_identity(g_golden_sun_obj_visible_provenance, false);
        const GoldenSunObjPlacementProvenance* result = nullptr;
        bool ambiguous = false;
        if (visible_match.count == 1) {
            result = visible_match.record;
        } else if (visible_match.count >= 2) {
            ambiguous = true;
        } else {
            const auto pending_match =
                find_by_identity(g_golden_sun_obj_pending_provenance, true);
            if (pending_match.count == 1) {
                result = pending_match.record;
            } else if (pending_match.count >= 2) {
                ambiguous = true;
            }
        }
        slot_memo.frame = frame_now;
        slot_memo.generation = generation_now;
        slot_memo.result = result;
        slot_memo.reason = result   ? kGoldenSunObjRejectOk
                          : ambiguous ? kGoldenSunObjRejectIdentity
                                      : visible_slot_reason;
        // Feed the persistent position table (step 1 of the task spec):
        // this fires exactly once per object per frame, gated by the memo
        // recompute above, whenever the record path just authenticated a
        // position. Gated on the expanded view being active since that is
        // the table's only consumer (golden_sun_obj_track_lookup below).
        // allow_track_confirm is false for the diagnostic accessor below: it
        // reads live OAM at a different point in the frame than the renderer
        // does, and a measurement tool must never change what is drawn.
        if (result && allow_track_confirm &&
            golden_sun_expanded_obj_view_active()) {
            golden_sun_obj_track_confirm(result->logical_x, result->logical_y,
                                         attr0, attr1, attr2, frame_now);
        }
    }
    const GoldenSunObjPlacementProvenance* result = slot_memo.result;
    if (tally_rejection && gba::g_ws_obj_census_line) {
        const GoldenSunObjRejectReason bucket = slot_memo.reason;
        ++g_ws_obj_reject_totals[static_cast<std::size_t>(bucket)];
        // Breakdown below attributes only to the VISIBLE slot's own
        // frame/attrs fields, matching the fact that bucket ==
        // kGoldenSunObjRejectAttrsMoved here can only arise from the by-slot
        // diagnostic baseline (see slot_memo.reason above): neither the
        // identity search's pending fallback nor an ambiguous outcome
        // surfaces its own reason into `bucket`.
        if (bucket == kGoldenSunObjRejectAttrsMoved) {
            if (visible.frame == runtime_current_frame())
                ++g_ws_obj_reject_totals[kGoldenSunObjRejectAttrsThisFrame];
            else
                ++g_ws_obj_reject_totals[kGoldenSunObjRejectAttrsOldFrame];
            if (visible.oam_identity_valid) {
                if (visible.expected_attr0 != attr0)
                    ++g_ws_obj_reject_totals[kGoldenSunObjRejectAttr0Differs];
                if (visible.expected_attr1 != attr1)
                    ++g_ws_obj_reject_totals[kGoldenSunObjRejectAttr1Differs];
                if (visible.expected_attr2 != attr2)
                    ++g_ws_obj_reject_totals[kGoldenSunObjRejectAttr2Differs];
            }
        }
    }
    return result;
}

// Sentinel raw OAM coordinates for a parked/hidden sprite:
// (attr1 & 0x1FF) == 192 and (attr0 & 0xFF) == 192. Measured 2026-09-11
// (FACTS.md): ~85% of the 128 OAM entries sit here every frame (~107/frame).
// They are hidden by design and must never be given a trusted expanded-view
// position. Re-measure this constant (via signals.csv) before touching it if
// sprites start disappearing.
constexpr int kGoldenSunObjParkSentinelRawX = 192;
constexpr int kGoldenSunObjParkSentinelRawY = 192;

// Slack, in pixels, allowed beyond the expanded viewport edges when
// resolving the raw X wrap, so a sprite only partly off the side still
// resolves. X only; Y is decided by the band rule below.
constexpr int kGoldenSunObjFallbackSlackX = 64;

// Why the wrap cannot be resolved by asking "which reading is in view".
//
// That rule was tried on 2026-09-11 and reverted the same day with two
// user-reported artefacts: an NPC standing north of the camera drawn at the
// bottom of the screen during a Move cast, and shadows appearing detached at
// the top edge. The flaw is not in the arithmetic. A character who is
// genuinely out of sight is a legitimate answer, and in that case the only
// reading that LOOKS visible is the wrong one -- so "exactly one candidate is
// visible" happily selects it. Testing the sprite's full extent rather than
// its origin does not help; it widens the window in which a false reading
// looks plausible.
//
// The two Y candidates are 256px apart while the expanded view is 240px
// tall, so the bottom stripe and the region just above the view alias onto
// the same byte and no local test can separate them. Y is therefore resolved
// only where the byte can mean one thing:
//   0..159    the native screen rows; the other reading is far above the view
//   208..255  the top margin; the other reading is below the view
//   160..207  ALIASED -- refused, leaving the sprite in the native rectangle
//             exactly as it was before this fallback existed
//
// The known cost is the bottom stripe during an effect: a character genuinely
// standing there is not drawn there. That is deliberate. Drawing NPCs where
// they are not is worse, and it is what every attempt to be cleverer here has
// produced. The real fix is to stop reading the hardware's 8-bit shorthand at
// all -- see the sprite-table work in ROADMAP.md.
constexpr int kGoldenSunObjFallbackAliasFirstRawY = 160;
constexpr int kGoldenSunObjFallbackAliasLastRawY = 207;

// ---- World-map actor side ---------------------------------------------------
//
// FACTS.md, 2026-09-29 ("World-map objects below the screen"). The world-map
// overlay's bounds are widened (golden_sun_worldmap_range_box), so the game now
// emits towns and caves below the view with raw OAM Y 209..248, which overlaps
// the "above the view" encoding (raw 184..255). The raw byte cannot tell them
// apart (logs/gpu_frame_0086: town raw 206 is 162 units BELOW the player and
// was drawn at y=-50; cave raw 215 was drawn at y=-41; frame 0087, walked south:
// town raw 116, cave raw 127, correct). The game's own actor table knows which
// side each one is on:
//   actor pointers  [[0x03001EBC] + 0x14 + 4 * index] (0x0808BA1C, which
//                   returns 0 for an index above 0xBF); index 8..0x41 is the
//                   range OvlFunc_598 (0x02008598) walks; a null entry is unused
//   +0x50           pointer to the actor's sprite record; its halfwords at +4
//                   and +6 are attr0 and attr1 (one frame ahead of OAM)
//   +0x54           low nibble 1 = drawn this frame (Func_c62c draws only those)
//   +0x08 / +0x10   world x / z, signed 16.16
//   focus actor     [[0x03001EBC] + 0x1E0]
// dz = actor z - focus z: positive is below the player on screen (town 162.2 at
// frame 0086 -> raw 206; 72.7 at 0087 -> raw 116), negative is above.
// The same block-number test as world_map_margin_hold() decides "world map".
constexpr std::uint32_t kGoldenSunActorContextPtr = 0x03001EBCu;
constexpr std::uint32_t kGoldenSunActorTableOffset = 0x14u;
constexpr std::uint32_t kGoldenSunActorFocusOffset = 0x1E0u;
constexpr int kGoldenSunWorldActorFirst = 8;
constexpr int kGoldenSunWorldActorLast = 0x41;
constexpr std::uint32_t kGoldenSunActorSpriteOffset = 0x50u;
constexpr std::uint32_t kGoldenSunActorFlagOffset = 0x54u;
constexpr std::uint32_t kGoldenSunActorZOffset = 0x10u;

bool world_map_blocks_raw(const std::uint8_t* io) {
    const unsigned bg2 = io[0x0C] | (static_cast<unsigned>(io[0x0D]) << 8);
    const unsigned bg3 = io[0x0E] | (static_cast<unsigned>(io[0x0F]) << 8);
    return ((bg3 >> 8) & 0x1Fu) == 8u && ((bg2 >> 8) & 0x1Fu) == 10u;
}

bool golden_sun_ewram_span_ok(std::uint32_t addr, std::uint32_t bytes) {
    return addr >= 0x02000000u && addr <= 0x02040000u &&
           bytes <= 0x02040000u - addr;
}

struct GoldenSunWorldActorSide {
    std::uint16_t attr0;
    std::uint16_t attr1;
    std::int8_t side;  // +1 below the player, -1 above
};

// +1 when the world-map actor drawing this OAM entry (attr0/attr1 equal to its
// sprite record) is below the player, -1 when above, 0 when this is not the
// world map, no drawn actor matches, they disagree, or dz is exactly 0. The
// table is scanned at most once per frame.
int golden_sun_world_map_actor_side(std::uint16_t attr0, std::uint16_t attr1) {
    static std::uint64_t cache_frame = UINT64_MAX;
    static std::size_t cache_count = 0;
    static std::array<GoldenSunWorldActorSide,
                      kGoldenSunWorldActorLast - kGoldenSunWorldActorFirst + 1>
        cache{};
    const std::uint64_t frame_now = runtime_current_frame();
    if (cache_frame != frame_now) {
        cache_frame = frame_now;
        cache_count = 0;
        gba::GbaBus* bus = gbarecomp::active_bus();
        if (bus && world_map_blocks_raw(bus->io().raw())) {
            const std::uint32_t ctx = bus_read_u32(kGoldenSunActorContextPtr);
            const std::uint32_t table_end =
                kGoldenSunActorTableOffset +
                4u * static_cast<std::uint32_t>(kGoldenSunWorldActorLast + 1);
            std::uint32_t focus = 0u;
            if (golden_sun_ewram_span_ok(ctx, kGoldenSunActorFocusOffset + 4u) &&
                golden_sun_ewram_span_ok(ctx, table_end)) {
                focus = bus_read_u32(ctx + kGoldenSunActorFocusOffset);
            }
            if (golden_sun_ewram_span_ok(focus, kGoldenSunActorZOffset + 4u)) {
                const std::int32_t focus_z = static_cast<std::int32_t>(
                    bus_read_u32(focus + kGoldenSunActorZOffset));
                for (int i = kGoldenSunWorldActorFirst;
                     i <= kGoldenSunWorldActorLast; ++i) {
                    const std::uint32_t actor = bus_read_u32(
                        ctx + kGoldenSunActorTableOffset +
                        4u * static_cast<std::uint32_t>(i));
                    if (actor == 0u ||
                        !golden_sun_ewram_span_ok(
                            actor, kGoldenSunActorFlagOffset + 4u))
                        continue;
                    if ((bus_read_u32(actor + kGoldenSunActorFlagOffset) &
                         0xFu) != 1u)
                        continue;
                    const std::uint32_t sprite =
                        bus_read_u32(actor + kGoldenSunActorSpriteOffset);
                    if (!golden_sun_ewram_span_ok(sprite, 8u)) continue;
                    const std::int64_t dz =
                        static_cast<std::int64_t>(static_cast<std::int32_t>(
                            bus_read_u32(actor + kGoldenSunActorZOffset))) -
                        focus_z;
                    if (dz == 0) continue;
                    cache[cache_count++] = {
                        bus_read_u16(sprite + 4u), bus_read_u16(sprite + 6u),
                        static_cast<std::int8_t>(dz > 0 ? 1 : -1)};
                }
            }
        }
    }
    // Match on the bits that do not move (shape, affine/mode flags, size), then
    // take the nearest record by position. The sprite record is updated a
    // frame ahead of the OAM copy being drawn, so while the player walks the
    // Y/X bits differ by a pixel or so (gpu_frame_0088: cave OAM attr0 0x21D5,
    // its record 0x21D4) and an exact attr0/attr1 match missed on those
    // frames, which made the objects flicker between the top margin and
    // hidden (session 20260929_205119). attr1 is compared on its size bits
    // only: bits 9-13 are the rotation slot, which the game reassigns from
    // frame to frame (Vale's record 0x8001 against its OAM 0x8204, slot 0
    // against 1), so the town missed for a frame and was drawn at the top
    // edge (gpu_rewind_0065 frame 2428), or matched another town on the
    // wrong side and vanished (gpu_rewind_0071 frame 9115).
    int side = 0;
    int best = INT_MAX;
    bool tie_disagrees = false;
    for (std::size_t i = 0; i < cache_count; ++i) {
        if ((cache[i].attr0 & 0xFF00u) != (attr0 & 0xFF00u) ||
            (cache[i].attr1 & 0xC000u) != (attr1 & 0xC000u))
            continue;
        int dy = std::abs(static_cast<int>(cache[i].attr0 & 0xFFu) -
                          static_cast<int>(attr0 & 0xFFu));
        dy = std::min(dy, 256 - dy);
        int dx = std::abs(static_cast<int>(cache[i].attr1 & 0x1FFu) -
                          static_cast<int>(attr1 & 0x1FFu));
        dx = std::min(dx, 512 - dx);
        const int distance = dx + dy;
        if (distance < best) {
            best = distance;
            side = cache[i].side;
            tie_disagrees = false;
        } else if (distance == best && cache[i].side != side) {
            tie_disagrees = true;
        }
    }
    return tie_disagrees ? 0 : side;
}

bool game_window_open();

// game_window_open() at most once per frame: the providers ask per scanline.
bool game_window_open_this_frame() {
    static std::uint64_t frame = UINT64_MAX;
    static bool open = false;
    const std::uint64_t now = runtime_current_frame();
    if (frame != now) {
        frame = now;
        open = game_window_open();
    }
    return open;
}

// Fallback used by both wide OBJ attribute providers when
// golden_sun_obj_provider_provenance has no usable record for this object
// (the record path already declined -- this never runs ahead of it) and the
// expanded view is active.
//
// Both wide providers call this one helper with the same inputs so X and Y
// always agree on whether a sprite gets a fallback position: the renderer
// only trusts an object once both axes are trusted, and routing the decision
// through a single shared function guarantees that.
bool golden_sun_wide_obj_fallback_position(std::uint16_t attr0,
                                           std::uint16_t attr1,
                                           int* out_x, int* out_y) {
    if (!golden_sun_expanded_obj_view_active() || !out_x || !out_y)
        return false;
    const int raw_x = static_cast<int>(attr1 & 0x01FFu);
    const int raw_y = static_cast<int>(attr0 & 0x00FFu);
    if (raw_x == kGoldenSunObjParkSentinelRawX &&
        raw_y == kGoldenSunObjParkSentinelRawY) {
        return false;
    }
    const int min_x = -static_cast<int>(g_golden_sun_wide_extra_left) -
                       kGoldenSunObjFallbackSlackX;
    const int max_x = gsr::widescreen::kExpandedWidth -
                       static_cast<int>(g_golden_sun_wide_extra_left) +
                       kGoldenSunObjFallbackSlackX;
    // X: exactly one candidate must land inside the viewport. Zero means the
    // sprite is nowhere near the view; two means the coordinate does not say
    // where it is. Both are a refusal.
    const bool near_x = raw_x >= min_x && raw_x <= max_x;
    const bool wrapped_x = (raw_x - 512) >= min_x && (raw_x - 512) <= max_x;
    if (near_x == wrapped_x) return false;
    const int resolved_x = near_x ? raw_x : raw_x - 512;
    // Y: on the world map the actor table says which side the sprite is on,
    // for the whole ambiguous range (see golden_sun_world_map_actor_side).
    // Otherwise only the two unambiguous bands resolve; the aliased band
    // refuses. Not while a game window is open: a menu's own sprites are
    // not actors, and the selected party member bouncing to y = -4 (raw
    // 252) matched actor 16's 32x32 record and was sent below the screen
    // (logs/gpu_rewind_0061, frames 29-33, 2026-10-01).
    const int world_side = raw_y >= kGoldenSunObjFallbackAliasFirstRawY &&
                                   !game_window_open_this_frame()
        ? golden_sun_world_map_actor_side(attr0, attr1) : 0;
    if (world_side != 0) {
        *out_x = resolved_x;
        *out_y = world_side > 0 ? raw_y : raw_y - 256;
        return true;
    }
    if (raw_y >= kGoldenSunObjFallbackAliasFirstRawY &&
        raw_y <= kGoldenSunObjFallbackAliasLastRawY) return false;
    const int resolved_y = raw_y > kGoldenSunObjFallbackAliasLastRawY
        ? raw_y - 256 : raw_y;
    *out_x = resolved_x;
    *out_y = resolved_y;
    return true;
}

// ---- Persistent per-character position table ------------------------------
//
// FACTS.md 2026-09-11 ("The sprite-to-character gap ... positions are built
// per draw rather than stored"): a character's position exists only at the
// instant the game computes it -- the draw routine is handed a temporary
// block of coordinates, and five separate memory searches found no
// persistent per-character table anywhere in guest RAM. During an effect
// (Move/Lift/Carry) the game stops recalculating a held character's
// position, so nothing moves: the last position we saw IS the current one.
// This table is OUR OWN record of "the last place we saw this character",
// built only from positions the existing record path
// (golden_sun_obj_provider_provenance) already authenticated. It never
// invents a position; it only remembers one we already trusted.
// How many consecutive frames an entry may be carried by the lookup alone,
// with no confirmation from the record path. 300 frames (~5s) matches the
// expiry window: long enough to cross the ambiguous stripe during an effect,
// short enough that a tracker following the wrong sprite cannot persist.
// TODO-EVIDENCE: reasoned from the measured Move cast length (>=120 frames,
// FACTS.md 2026-09-11), not measured directly.
constexpr std::uint32_t kGoldenSunObjTrackOnlyRunLimit = 300;

struct GoldenSunObjTrackEntry {
    bool used = false;
    int screen_x = 0;
    int screen_y = 0;
    std::uint64_t confirmed_frame = 0;
    // Last-seen ATTR0/1/2, kept because the design calls for it (a future
    // refinement could require these to still roughly match before trusting
    // an entry); not consulted by the lookup below today.
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    // Consecutive frames this entry has been carried by the lookup alone,
    // with no confirmation from the record path. Bounded so an entry can
    // never follow a sprite indefinitely on its own evidence: a long
    // unconfirmed run is drift, and drift is how a tracker locks onto the
    // wrong character. Reset to zero by every real confirmation.
    std::uint32_t track_only_run = 0;
};

// Fixed, allocated once, never grows: at most 64 characters are tracked at a
// time, which comfortably covers Golden Sun's OAM budget (128 hardware
// slots, of which FACTS.md 2026-09-11 measured ~85% permanently parked at a
// single off-screen sentinel, leaving 17-35 real sprites on screen even
// during an effect).
constexpr std::size_t kGoldenSunObjTrackSlots = 64;
std::array<GoldenSunObjTrackEntry, kGoldenSunObjTrackSlots>
    g_golden_sun_obj_track_table{};

// Match radius, expanded-view pixels, used two ways below: to decide whether
// a freshly authenticated position belongs to an already-tracked character
// (rather than a new one), and to decide whether a raw-coordinate candidate
// during an effect belongs to a tracked character.
// TODO-EVIDENCE: chosen by reasoning, not measured in this repo. An ordinarily
// walking character moves on the order of a couple of pixels per frame, and a
// table entry can be a few frames old by the time an effect freezes the
// sprite, so a handful of pixels of slack covers that drift. The value must
// also stay far below half the smallest gap between the two OAM-wrap
// candidate readings used below and in golden_sun_wide_obj_fallback_position
// (256px on Y, 512px on X) so the two aliases of the SAME raw byte can never
// both fall within radius of a real entry at once -- 8px is more than an
// order of magnitude under that 128px ceiling, so this cannot itself
// reintroduce the Y-axis ambiguity that sank the three earlier local rules
// (FACTS.md 2026-09-11).
constexpr int kGoldenSunObjTrackRadius = 8;
constexpr long kGoldenSunObjTrackRadiusSq =
    static_cast<long>(kGoldenSunObjTrackRadius) * kGoldenSunObjTrackRadius;

// Frames an entry stays usable after its last confirmation, and thus how long
// a table slot survives an effect with no new confirmation before it is freed
// for reuse.
// TODO-EVIDENCE: chosen by reasoning from one measured data point. FACTS.md
// 2026-09-11 measured one Move cast still running at frame 120 of a
// before/during comparison (shadow_writes.csv, 180 frames before vs. 120
// during, the capture's own window rather than the cast's true length); the
// true distribution of Move/Lift/Carry cast lengths is not measured. 300
// frames (~5s at the GBA's ~59.7Hz, the same window flush_signal_log already
// treats as "a session tick" via kSignalFlushRows) is comfortably above the
// one measured floor while still short enough that a slot abandoned mid-play
// (character despawned, scrolled far off, etc.) frees up within a few
// seconds instead of squatting all session.
constexpr std::uint64_t kGoldenSunObjTrackExpiryFrames = 300;

// Record an authenticated position. Called from exactly one place --
// golden_sun_obj_provider_provenance's once-per-object-per-frame memo
// recompute, below -- so this never runs more than once per object per
// frame even though both wide providers query that memo every scanline.
//
// Finds the tracked entry whose last position is nearest (x, y); if one is
// within the match radius, that is the same character seen again and its
// entry is refreshed in place. Otherwise the position belongs to a character
// not currently tracked (or previously tracked in a now-expired/reused slot),
// so a free slot is claimed instead. Expired entries (unclaimed for longer
// than kGoldenSunObjTrackExpiryFrames) are freed opportunistically while
// scanning, which is how "unclaimed" slots actually get reused -- this scan
// runs on every authenticated object, and ordinary field play authenticates
// most of them (FACTS.md 2026-09-11), so an expired entry does not linger
// past the next few confirmed sprites.
void golden_sun_obj_track_confirm(int x, int y, std::uint16_t attr0,
                                  std::uint16_t attr1, std::uint16_t attr2,
                                  std::uint64_t frame) {
    GoldenSunObjTrackEntry* best = nullptr;
    long best_dist2 = 0;
    GoldenSunObjTrackEntry* free_slot = nullptr;
    for (auto& entry : g_golden_sun_obj_track_table) {
        if (entry.used &&
            frame - entry.confirmed_frame > kGoldenSunObjTrackExpiryFrames) {
            entry.used = false;  // stale: free the slot for reuse.
        }
        if (!entry.used) {
            if (!free_slot) free_slot = &entry;
            continue;
        }
        const long dx = static_cast<long>(entry.screen_x) - x;
        const long dy = static_cast<long>(entry.screen_y) - y;
        const long dist2 = dx * dx + dy * dy;
        if (dist2 <= kGoldenSunObjTrackRadiusSq &&
            (best == nullptr || dist2 < best_dist2)) {
            best = &entry;
            best_dist2 = dist2;
        }
    }
    GoldenSunObjTrackEntry* target = best ? best : free_slot;
    if (!target) return;  // Table full and nothing nearby: drop silently.
    target->used = true;
    target->screen_x = x;
    target->screen_y = y;
    target->confirmed_frame = frame;
    target->attr0 = attr0;
    target->attr1 = attr1;
    target->attr2 = attr2;
    target->track_only_run = 0;
}

// Table-based recovery for a sprite the record path just declined. The raw
// OAM byte admits two ambiguous readings per axis (raw and raw-512 for X,
// raw and raw-256 for Y -- the same aliasing golden_sun_wide_obj_fallback_
// position resolves by viewport/band heuristics). Here the tie-break is
// evidence rather than a heuristic: if exactly one of the four raw/wrapped
// (x, y) combinations lands within the match radius of exactly one tracked,
// still-fresh entry, that entry's own remembered position is the answer --
// during an effect nothing has moved, so the last confirmed position IS the
// current one. Two qualifying entries (or candidates), or none, decline:
// "do not pick a nearest" (task spec) -- an untracked or ambiguous sprite is
// left to the raw-coordinate fallback below instead of a guess.
//
// Both wide providers call this ONE function (see golden_sun_wide_obj_attr_x_
// provider and golden_sun_wide_obj_attr_y_provider) so X and Y always agree
// on whether, and where, a sprite gets a tracked position -- the same
// reason golden_sun_wide_obj_fallback_position is itself shared. Memoised per
// OAM entry per frame so the (up to 64-entry, up to four-candidate) scan runs
// at most once per object per frame despite being called once per scanline.
// The memo key is frame-only, unlike golden_sun_obj_provider_provenance's
// frame+generation key: a mid-frame table update (from some other object's
// confirmation, later in the same frame) could in principle let a later
// scanline of this same object succeed where an earlier one declined, but a
// stale memo can only cause an extra decline, never a wrong placement, which
// is the conservative side of this trade-off.
bool golden_sun_obj_track_lookup(int oam_index, std::uint16_t attr0,
                                 std::uint16_t attr1, int* out_x, int* out_y) {
    if (!golden_sun_expanded_obj_view_active() || !out_x || !out_y ||
        oam_index < 0 ||
        static_cast<std::size_t>(oam_index) >=
            gsr::widescreen::kGoldenSunOamShadowSlotCount)
        return false;
    struct Memo {
        std::uint64_t frame = UINT64_MAX;
        bool has_result = false;
        int x = 0;
        int y = 0;
    };
    static std::array<Memo, gsr::widescreen::kGoldenSunOamShadowSlotCount>
        memo{};
    Memo& slot = memo[static_cast<std::size_t>(oam_index)];
    const std::uint64_t frame_now = runtime_current_frame();
    if (slot.frame == frame_now) {
        if (!slot.has_result) return false;
        *out_x = slot.x;
        *out_y = slot.y;
        return true;
    }
    slot.frame = frame_now;
    slot.has_result = false;

    const int raw_x = static_cast<int>(attr1 & 0x01FFu);
    const int raw_y = static_cast<int>(attr0 & 0x00FFu);
    const int x_candidates[2] = {raw_x, raw_x - 512};
    const int y_candidates[2] = {raw_y, raw_y - 256};

    // The remembered position decides WHICH reading is meant; the reading
    // itself is the answer.
    //
    // Returning the remembered position instead made moving NPCs jitter
    // (user-reported 2026-09-11): a character who is actually walking gets a
    // position that is one or two frames stale, so they snap backwards and
    // then catch up, every time the record path happens to decline. The raw
    // coordinate is not stale and not wrong -- it is only ambiguous. So memory
    // is used purely to rule out the wrong reading, and the surviving
    // candidate, which is this frame's true position with the wrap resolved,
    // is what we return. A frozen character gets the same answer either way;
    // a moving one now gets the correct one.
    //
    // Candidates are 512px (X) and 256px (Y) apart while the match radius is a
    // few pixels, so at most one candidate combination can fall near any one
    // entry -- two matches therefore always mean two different characters,
    // which is the ambiguity worth declining on.
    int match_x = 0;
    int match_y = 0;
    int match_count = 0;
    GoldenSunObjTrackEntry* matched = nullptr;
    for (int xi = 0; xi < 2 && match_count < 2; ++xi) {
        for (int yi = 0; yi < 2 && match_count < 2; ++yi) {
            const int cx = x_candidates[xi];
            const int cy = y_candidates[yi];
            for (auto& entry : g_golden_sun_obj_track_table) {
                if (!entry.used) continue;
                if (frame_now - entry.confirmed_frame >
                    kGoldenSunObjTrackExpiryFrames) continue;  // not recent
                if (entry.track_only_run >= kGoldenSunObjTrackOnlyRunLimit)
                    continue;  // carried too long without confirmation
                // The tracked character must at least be the same SHAPE of
                // sprite. Position alone is not identity: a shadow down in
                // the bottom stripe has a second reading up at the top of
                // the view, and if anyone happens to be standing there the
                // match is unique and confidently wrong -- that is the
                // detached shadow drawn over a woman at the top edge, seen
                // in the user's screenshots while Move was being cast,
                // 2026-09-11. Shape and size come from the OAM bits that do
                // NOT change as a character animates (unlike the tile index
                // in ATTR2, which changes every animation frame and would
                // make this test far too strict), so a shadow can never be
                // mistaken for a person.
                if (((entry.attr0 ^ attr0) & 0xC000u) != 0u ||
                    ((entry.attr1 ^ attr1) & 0xC000u) != 0u)
                    continue;
                const long dx = static_cast<long>(entry.screen_x) - cx;
                const long dy = static_cast<long>(entry.screen_y) - cy;
                if (dx * dx + dy * dy <= kGoldenSunObjTrackRadiusSq) {
                    match_x = cx;
                    match_y = cy;
                    matched = &entry;
                    if (++match_count >= 2) break;
                }
            }
        }
    }
    if (match_count != 1) return false;  // none, or ambiguous: decline.
    // Follow the character. Without this the entry stays pinned to wherever
    // the record path last spoke, so a character who keeps walking drifts
    // out of the match radius and is dropped a few pixels later -- which is
    // exactly the reported symptom: a child running circles in Vault stays
    // visible until it moves a little way into the bottom stripe, then
    // clips (user-reported with video, 2026-09-11). A tracker has to update
    // or it is only an anchor. The run counter above bounds how long a
    // character may be carried on this evidence alone.
    if (matched) {
        matched->screen_x = match_x;
        matched->screen_y = match_y;
        matched->confirmed_frame = frame_now;
        matched->attr0 = attr0;
        matched->attr1 = attr1;
        ++matched->track_only_run;
    }
    slot.has_result = true;
    slot.x = match_x;
    slot.y = match_y;
    *out_x = slot.x;
    *out_y = slot.y;
    return true;
}

int golden_sun_wide_obj_attr_x_provider(int oam_index,
                                        std::uint16_t attr0,
                                        std::uint16_t attr1,
                                        std::uint16_t attr2,
                                        int* out_x) {
    // Keep OBJ coordinates in the native composition while the side arena
    // layers are reconstructed.
    if (g_golden_sun_battle_backdrop_state.active) return 0;
    if (!golden_sun_expanded_obj_view_active() || !out_x || oam_index < 0 ||
        static_cast<std::size_t>(oam_index) >=
            g_golden_sun_obj_visible_provenance.size()) return 0;
    const int raw_x = static_cast<int>(attr1 & 0x01FFu);
    const int raw_y = static_cast<int>(attr0 & 0x00FFu);
    const auto* provenance = golden_sun_obj_provider_provenance(
        oam_index, attr0, attr1, attr2, raw_x, raw_y,
        /*tally_rejection=*/true, /*allow_track_confirm=*/true);
    if (!provenance) {
        // Record path declined (already tallied above). Step 2: consult our
        // own position table (see golden_sun_obj_track_lookup) before
        // falling back to raw-coordinate reconstruction.
        int track_x = 0, track_y = 0;
        if (golden_sun_obj_track_lookup(oam_index, attr0, attr1, &track_x,
                                        &track_y)) {
            // Gated like the other obj_* counters: the providers run once
            // per object per scanline, so an ungated increment counts ~160x
            // per object per frame (measured 2,732 "per frame" in session
            // 20260911_191138, against 128 sprites).
            if (gba::g_ws_obj_census_line) ++g_ws_obj_from_track_total;
            *out_x = track_x;
            return 1;
        }
        // Step 3: try to reconstruct a position from the raw attributes
        // before giving up.
        int fallback_x = 0, fallback_y = 0;
        if (golden_sun_wide_obj_fallback_position(attr0, attr1, &fallback_x,
                                                  &fallback_y)) {
            *out_x = fallback_x;
            return 1;
        }
        return 0;
    }
    *out_x = provenance->logical_x;
    return 1;
}

int golden_sun_wide_obj_attr_y_provider(int oam_index,
                                        std::uint16_t attr0,
                                        std::uint16_t attr1,
                                        std::uint16_t attr2,
                                        int* out_y) {
    const std::uint32_t diag_pc = 0u;
    const int raw_y = static_cast<int>(attr0 & 0x00FFu);
    if (g_golden_sun_battle_backdrop_state.active) {
        g_golden_sun_obj_y_edge_alias_state = {};
        return 0;
    }
    if (!golden_sun_expanded_obj_view_active()) {
        g_golden_sun_obj_y_edge_alias_state = {};
        trace_golden_sun_obj_y_attempt(diag_pc, "scene-disabled",
                                       attr0 & 0x00FFu, 0u, false, oam_index);
        return 0;
    }
    const auto record_obj = [&](GoldenSunObjYOutcome outcome) {
        record_golden_sun_obj_y_outcome(oam_index, raw_y, outcome);
    };
    if (!out_y) {
        record_obj(GoldenSunObjYOutcome::OutputUnavailable);
        trace_golden_sun_obj_y_attempt(diag_pc, "output-unavailable",
                                       attr0 & 0x00FFu, 0u, false, oam_index);
        return 0;
    }
    if (oam_index < 0 || static_cast<std::size_t>(oam_index) >=
                              g_golden_sun_obj_visible_provenance.size()) {
        trace_golden_sun_obj_y_attempt(diag_pc, "slot-rejected",
                                       attr0 & 0x00FFu, 0u, false, oam_index);
        return 0;
    }
    auto& visible_provenance = g_golden_sun_obj_visible_provenance[
        static_cast<std::size_t>(oam_index)];
    const int raw_x = static_cast<int>(attr1 & 0x01FFu);
    // tally_rejection=false: the X provider above already attributes this
    // object's reject reason for this frame; Y deliberately does not tally
    // so the same object isn't counted twice.
    const auto* selected_provenance = golden_sun_obj_provider_provenance(
        oam_index, attr0, attr1, attr2, raw_x, raw_y,
        /*tally_rejection=*/false, /*allow_track_confirm=*/true);
    // Keep the visible record as the diagnostic baseline when no candidate
    // is usable; acceptance below is controlled by selected_provenance.
    const auto& provenance = selected_provenance ? *selected_provenance
                                                 : visible_provenance;
    const bool alias_candidate =
        gsr::widescreen::golden_sun_obj_y_alias_candidate(
            raw_y, provenance.logical_y);
    if (alias_candidate) {
        gba::vram_trace::OamAttr0Provenance attr_provenance{};
        if (gba::vram_trace::get_oam_attr0_provenance(
            static_cast<std::size_t>(oam_index), &attr_provenance)) {
            // Generation is diagnostic state attached to the DMA-latched
            // record. Do not mutate a pending candidate while rendering it.
            // selected_provenance is now found by identity rather than by
            // slot (see golden_sun_obj_provider_provenance), so it may
            // point at a different OAM slot's visible record than
            // oam_index; check array membership instead of comparing
            // against this slot's own record.
            const auto* visible_begin =
                g_golden_sun_obj_visible_provenance.data();
            const auto* visible_end =
                visible_begin + g_golden_sun_obj_visible_provenance.size();
            if (selected_provenance >= visible_begin &&
                selected_provenance < visible_end) {
                const std::size_t visible_idx = static_cast<std::size_t>(
                    selected_provenance - visible_begin);
                g_golden_sun_obj_visible_provenance[visible_idx]
                    .writer_generation = attr_provenance.generation;
            }
        }
    }
    const bool provenance_epoch_matches =
        provenance.auth_epoch == g_golden_sun_field_auth_epoch;
    const bool provenance_raw_matches =
        golden_sun_obj_oam_truncated(provenance.logical_y, 8) == raw_y;
    const bool provenance_identity_matches =
        gsr::widescreen::golden_sun_obj_provenance_attrs_match(
            provenance.oam_identity_valid, provenance.expected_attr0,
            provenance.expected_attr1, provenance.expected_attr2,
            attr0, attr1, attr2);
    const bool signed_provenance_matches = selected_provenance != nullptr;
    const std::uint32_t expected_target =
        gsr::widescreen::kGoldenSunOamShadowStart +
        static_cast<std::uint32_t>(oam_index) *
            gsr::widescreen::kGoldenSunOamShadowSlotBytes;
    const bool affine = (attr0 & 0x0100u) != 0u;
    const bool sprite_disabled = !affine && (attr0 & 0x0200u) != 0u;
    const bool sprite_empty = attr0 == 0u && attr1 == 0u && attr2 == 0u;
    const bool sprite_dormant = attr0 == 0x00C0u;
    if (oam_index >= 0 && static_cast<std::size_t>(oam_index) <
                              g_golden_sun_obj_y_edge_alias_state.size()) {
        const auto sample = gsr::widescreen::GoldenSunObjYEdgeAliasSample{
            true, !(sprite_disabled || sprite_empty || sprite_dormant),
            signed_provenance_matches, oam_index, expected_target,
            g_golden_sun_field_auth_epoch, runtime_current_frame(), raw_y,
            raw_y >= 160 ? raw_y - 256 : raw_y, attr0, attr1, attr2};
        auto& alias_state = g_golden_sun_obj_y_edge_alias_state[
            static_cast<std::size_t>(oam_index)];
        const auto alias_step =
            gsr::widescreen::golden_sun_obj_y_edge_alias_step(
                alias_state, sample);
        alias_state = alias_step.state;
        if (alias_step.activated) {
            *out_y = -97;
            trace_golden_sun_obj_y_edge_alias(
                oam_index, expected_target, runtime_current_frame(),
                g_golden_sun_field_auth_epoch, attr0, attr1, attr2);
            return 1;
        }
    }
    const bool in_transition_band = raw_y >= 159 && raw_y <= 199;
    const bool diagnostic_transition =
        golden_sun_wide_diagnostics_enabled() && in_transition_band;
    const bool recorder_render_check =
        gsr::obj_recorder_enabled() && !in_transition_band;
    if (diagnostic_transition || recorder_render_check) {
        const bool renderer_provenance_rejected = selected_provenance == nullptr;
        const auto classified_reason = classify_golden_sun_obj_y_transition(
            provenance, runtime_current_frame(), g_golden_sun_field_auth_epoch,
            expected_target, raw_y, attr0, attr1, attr2);
        // The classifier is Y-focused; a null selected candidate can also
        // mean the provider rejected X. Never label that rejection accepted.
        const auto transition_reason = renderer_provenance_rejected &&
                classified_reason == GoldenSunObjYTransitionReason::Accepted
            ? GoldenSunObjYTransitionReason::NoProvenance : classified_reason;
        const bool recorder_out_of_band_rejection = recorder_render_check &&
            !(sprite_disabled || sprite_empty || sprite_dormant) &&
            renderer_provenance_rejected &&
            transition_reason != GoldenSunObjYTransitionReason::Accepted;
        if (diagnostic_transition || recorder_out_of_band_rejection) {
            const int canonical_y = raw_y >= 160 ? raw_y - 256 : raw_y;
            const auto resolution = signed_provenance_matches
                ? GoldenSunObjYTransitionResolution::Signed
                : GoldenSunObjYTransitionResolution::Canonical;
            record_golden_sun_obj_y_transition(
                oam_index, raw_y, canonical_y,
                signed_provenance_matches ? provenance.logical_y : canonical_y,
                resolution, transition_reason, provenance, attr0, attr1, attr2);
        }
    }
    switch (gsr::widescreen::golden_sun_obj_y_resolution(
        signed_provenance_matches)) {
    case gsr::widescreen::GoldenSunObjYResolution::SignedProvenance:
        *out_y = provenance.logical_y;
        record_obj(GoldenSunObjYOutcome::AcceptedProvenance);
        trace_golden_sun_obj_y_jump(
            oam_index, raw_y, provenance.logical_y, provenance,
            attr0, attr1, attr2);
        trace_golden_sun_obj_y_alias(
            provenance.logical_y < 0 ? "accepted-negative" :
                                      "accepted-positive",
            oam_index, raw_y, provenance.logical_y,
            provenance, attr0, attr1, attr2, false);
        return 1;
    case gsr::widescreen::GoldenSunObjYResolution::Canonical:
        record_obj(GoldenSunObjYOutcome::ProvenanceMissing);
        if (provenance.valid && provenance.y_valid &&
            gsr::widescreen::golden_sun_obj_y_alias_candidate(
                raw_y, provenance.logical_y)) {
            const char* reason = !provenance_epoch_matches
                ? "canonical-provenance-stale-epoch"
                : !provenance_raw_matches
                ? "canonical-provenance-y-mismatch"
                : !provenance_identity_matches
                ? "canonical-provenance-oam-mismatch"
                : "canonical-provenance-rejected";
            trace_golden_sun_obj_y_alias(
                reason, oam_index, raw_y, provenance.logical_y, provenance,
                attr0, attr1, attr2, true);
        }
        // Record path declined (selected_provenance is null here). Step 2:
        // consult our own position table (mirrors the X provider above --
        // same shared golden_sun_obj_track_lookup, so X and Y always agree
        // on the outcome; not tallied here, matching how the X provider
        // alone tallies obj_reject_totals).
        {
            int track_x = 0, track_y = 0;
            if (golden_sun_obj_track_lookup(oam_index, attr0, attr1,
                                            &track_x, &track_y)) {
                *out_y = track_y;
                return 1;
            }
        }
        // Step 3: try to reconstruct a position from the raw attributes
        // before giving up.
        {
            int fallback_x = 0, fallback_y = 0;
            if (golden_sun_wide_obj_fallback_position(
                    attr0, attr1, &fallback_x, &fallback_y)) {
                *out_y = fallback_y;
                return 1;
            }
        }
        return 0;
    }
    // Keep compilers that do not treat enum switches as exhaustive happy.
    record_obj(GoldenSunObjYOutcome::ProvenanceMissing);
    return 0;
}

struct GoldenSunCullTrace {
    std::uint32_t pc;
    // `calls` is the raw routed-PC count. It is intentionally recorded before
    // the strict 360x240 policy guard so inactive modes remain visible.
    std::uint64_t calls = 0;
    std::uint64_t bypasses = 0;
    std::uint64_t inactive_rejects = 0;
    std::int32_t min_coord = INT32_MAX;
    std::int32_t max_coord = INT32_MIN;
};

std::array<GoldenSunCullTrace, 10> g_golden_sun_cull_trace{{
    {0x0800B27Eu}, {0x0800B324u}, {0x0800B328u},
    {0x0800B3D2u}, {0x0800B3DCu}, {0x0800B3E6u}, {0x0800B3ECu},
    {0x0800C6FAu}, {0x0800C702u}, {0x0800C708u},
}};

std::uint64_t g_golden_sun_cull_last_report_frame = UINT64_MAX;

void report_golden_sun_cull_trace(std::uint64_t frame,
                                  const char* reason) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    // Emit every reviewed branch, including zeroes. A zero is evidence about
    // the raw route rather than an artifact of the strict policy guard.
    for (const auto& stat : g_golden_sun_cull_trace) {
        const std::int32_t min_coord = stat.calls != 0u ? stat.min_coord : 0;
        const std::int32_t max_coord = stat.calls != 0u ? stat.max_coord : 0;
        std::fprintf(
            stderr,
            "[wide-branch] frame=%llu auth_epoch=%llu reason=%s "
            "pc=0x%08x calls=%llu bypasses=%llu inactive_rejects=%llu "
            "coord=%d..%d\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(g_golden_sun_field_auth_epoch),
            reason, stat.pc,
            static_cast<unsigned long long>(stat.calls),
            static_cast<unsigned long long>(stat.bypasses),
            static_cast<unsigned long long>(stat.inactive_rejects), min_coord,
            max_coord);
    }
}

void begin_golden_sun_field_auth_epoch() {
    if (golden_sun_wide_diagnostics_enabled()) {
        report_golden_sun_cull_trace(runtime_current_frame(),
                                     "auth-epoch-end");
        report_golden_sun_field_provider_trace(runtime_current_frame(),
                                               "auth-epoch-end");
        report_golden_sun_palace_margin_diagnostics(runtime_current_frame(),
                                                    "auth-epoch-end");
        report_golden_sun_obj_b328_diagnostics(runtime_current_frame(),
                                               "auth-epoch-end");
        report_golden_sun_obj_y_transition_diagnostics(
            runtime_current_frame(), "auth-epoch-end");
        report_golden_sun_obj_y_steady_state_diagnostics(runtime_current_frame(),
                                                         "auth-epoch-end");
        report_golden_sun_field_map_id_attribution(runtime_current_frame(),
                                                   "auth-epoch-end");
        report_golden_sun_field_producers("auth-epoch-end");
        report_golden_sun_obj_record_census("auth-epoch-end");
        report_golden_sun_obj_record_values("auth-epoch-end");
        report_golden_sun_obj_commit_order("auth-epoch-end");
    }
    clear_golden_sun_obj_y_provenance();
    g_golden_sun_obj_y_provenance_logs_in_epoch = 0;
    reset_golden_sun_obj_y_alias_diagnostics();
    reset_golden_sun_field_producers();
    reset_golden_sun_obj_record_census();
    reset_golden_sun_obj_record_value_events();
    reset_golden_sun_obj_commit_order();
    g_golden_sun_field_provider_trace = {};
    g_golden_sun_palace_margin_diagnostics.reset();
    g_golden_sun_obj_y_slot_diagnostics = {};
    g_golden_sun_field_map_id_attribution = {};
    g_golden_sun_field_map_id_attribution_overflow = {};
    for (auto& stat : g_golden_sun_cull_trace) {
        const std::uint32_t pc = stat.pc;
        stat = {};
        stat.pc = pc;
    }
    ++g_golden_sun_field_auth_epoch;
    g_golden_sun_palace_table_authorized = false;
    g_golden_sun_palace_table_invalidated = false;
    g_golden_sun_palace_table_auth_attempted = false;
    reset_golden_sun_palace_active_region();
    g_golden_sun_field_epoch_map_writes = 0;
    g_golden_sun_field_epoch_raw_writes = 0;
    g_golden_sun_field_epoch_map_dma_writes = 0;
    g_golden_sun_field_epoch_raw_dma_writes = 0;
    g_golden_sun_field_table_cpu_logs_in_epoch = 0;
    g_golden_sun_field_table_dma_logs_in_epoch = 0;
    const bool vram_rearmed = gba::vram_trace::rearm_bounded_window();
    // OAM ATTR0 provenance is window-local just like the VRAM writer trace;
    // do not let a prior authenticated scene contaminate the next handoff.
    gba::vram_trace::reset_oam_trace_window();
    if (golden_sun_wide_diagnostics_enabled()) {
        std::fprintf(stderr,
                     "[wide-auth-epoch] frame=%llu auth_epoch=%llu "
                     "vram_trace_rearmed=%u\n",
                     static_cast<unsigned long long>(runtime_current_frame()),
                     static_cast<unsigned long long>(
                         g_golden_sun_field_auth_epoch),
                     vram_rearmed ? 1u : 0u);
        // CRASH-03 provenance: re-CRC the pool-LDM stack window at every
        // auth-epoch boundary so a stale-since-load window is visible in
        // session logs without retaining guest bytes.
        runtime_note_pool_ldm_epoch_crc();
    }
}

void maybe_report_golden_sun_cull_trace() {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    const std::uint64_t frame = runtime_current_frame();
    // This callback runs once per scanline.  The object-scene bit can change
    // between scanlines as DISPCNT transitions, so using it as an immediate
    // report trigger floods stderr during map transitions and stalls the
    // game thread.  Cull evidence is diagnostic only; sample it periodically.
    if (g_golden_sun_cull_last_report_frame == UINT64_MAX) {
        g_golden_sun_cull_last_report_frame = frame;
        return;
    }
    if (frame < g_golden_sun_cull_last_report_frame + 120u) return;
    report_golden_sun_cull_trace(frame, "periodic");
    g_golden_sun_cull_last_report_frame = frame;
}

void report_golden_sun_cull_trace_at_exit() {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    report_golden_sun_cull_trace(runtime_current_frame(), "exit");
}

void record_golden_sun_cull_trace(std::uint32_t pc) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    const int site = gsr::widescreen::golden_sun_viewport_branch_site_index(pc);
    if (site < 0 || static_cast<std::size_t>(site) >=
                         g_golden_sun_cull_trace.size()) {
        return;
    }
    GoldenSunCullTrace& stat = g_golden_sun_cull_trace[
        static_cast<std::size_t>(site)];
    ++stat.calls;
    std::int32_t coord = 0;
    if (pc == 0x0800B324u) coord = static_cast<std::int32_t>(g_cpu.R[4]);
    else if (pc == 0x0800B27Eu || pc == 0x0800B328u)
        coord = static_cast<std::int32_t>(g_cpu.R[6]);
    else if (pc == 0x0800B3D2u || pc == 0x0800B3DCu ||
             pc == 0x0800B3E6u || pc == 0x0800B3ECu)
        coord = static_cast<std::int32_t>(g_cpu.R[3]);
    else if (pc == 0x0800C6FAu)
        coord = static_cast<std::int32_t>(g_cpu.R[3]);
    else if (pc == 0x0800C702u || pc == 0x0800C708u)
        coord = static_cast<std::int32_t>(g_cpu.R[2]);
    stat.min_coord = std::min(stat.min_coord, coord);
    stat.max_coord = std::max(stat.max_coord, coord);
}

void record_golden_sun_cull_inactive_reject(std::uint32_t pc) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    const int site = gsr::widescreen::golden_sun_viewport_branch_site_index(pc);
    if (site < 0 || static_cast<std::size_t>(site) >=
                         g_golden_sun_cull_trace.size()) {
        return;
    }
    ++g_golden_sun_cull_trace[static_cast<std::size_t>(site)].inactive_rejects;
}

void record_golden_sun_cull_bypass(std::uint32_t pc) {
    if (!golden_sun_wide_diagnostics_enabled()) return;
    const int site = gsr::widescreen::golden_sun_viewport_branch_site_index(pc);
    if (site < 0 || static_cast<std::size_t>(site) >=
                         g_golden_sun_cull_trace.size()) {
        return;
    }
    ++g_golden_sun_cull_trace[static_cast<std::size_t>(site)].bypasses;
}

int golden_sun_wide_conditional_branch(std::uint32_t instruction_pc,
                                       std::uint32_t original_decision,
                                       std::uint32_t* out_decision) {
    record_golden_sun_cull_trace(instruction_pc);
    const std::uint32_t width = gsr::widescreen::kNativeWidth +
        g_golden_sun_wide_extra_left + g_golden_sun_wide_extra_right;
    const std::uint32_t height = gsr::widescreen::kNativeHeight +
        g_golden_sun_wide_extra_top + g_golden_sun_wide_extra_bottom;
    std::int32_t compared_operand = 0;
    if (instruction_pc == 0x0800B324u) {
        // Generated disassembly: B322 is `cmp r4,#239`; B324 consumes r4.
        compared_operand = static_cast<std::int32_t>(g_cpu.R[4]);
    } else if (instruction_pc == 0x0800B27Eu ||
               instruction_pc == 0x0800B328u) {
        // Generated disassembly: B27C/B326 are `cmp r6,#159`; both consume r6.
        compared_operand = static_cast<std::int32_t>(g_cpu.R[6]);
    } else if (instruction_pc == 0x0800B3D2u ||
               instruction_pc == 0x0800B3DCu ||
               instruction_pc == 0x0800B3E6u ||
               instruction_pc == 0x0800B3ECu ||
               instruction_pc == 0x0800C6FAu) {
        // Func_b388 and C6FA consume signed r3 at their reviewed branches.
        // C6FA's policy converts the preserved bit pattern back to unsigned.
        compared_operand = static_cast<std::int32_t>(g_cpu.R[3]);
    } else if (instruction_pc == 0x0800C702u ||
               instruction_pc == 0x0800C708u) {
        // Func_c62c's lower/upper Y routes consume signed r2.
        compared_operand = static_cast<std::int32_t>(g_cpu.R[2]);
    }
    GoldenSunObjB328ParentClassification b328_classification =
        GoldenSunObjB328ParentClassification::NoParent;
    unsigned b328_mismatch_flags = 0u;
    if (instruction_pc == 0x0800B328u &&
        golden_sun_expanded_obj_view_active() && compared_operand >= 160 &&
        compared_operand <= 199) {
        const std::uint64_t frame = runtime_current_frame();
        const std::uint32_t call_depth = runtime_call_stack_depth();
        const std::uint32_t call_return_pc =
            golden_sun_obj_call_return_pc(call_depth);
        const auto classification = classify_golden_sun_b328_parent(
            g_cpu.R[7], frame, call_depth, call_return_pc);
        b328_classification = classification.classification;
        b328_mismatch_flags = classification.mismatch_flags;
        record_golden_sun_b328_classification(
            compared_operand, classification, g_cpu.R[7], frame, call_depth,
            call_return_pc);
        if (b328_classification == GoldenSunObjB328ParentClassification::NoParent) {
            record_golden_sun_b328_parentless_token(
                frame, g_cpu.R[7], compared_operand, call_depth,
                call_return_pc);
        }
    }
    bool overridden = g_ws_active &&
        gsr::widescreen::golden_sun_expanded_viewport_branch_override(
            instruction_pc, original_decision, width, height,
            g_golden_sun_wide_extra_left, g_golden_sun_wide_extra_right,
            g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom,
            compared_operand, out_decision);
    if (!overridden && instruction_pc == 0x0800B328u) {
        const std::uint64_t frame = runtime_current_frame();
        const std::uint32_t call_depth = runtime_call_stack_depth();
        const std::uint32_t call_return_pc =
            golden_sun_obj_call_return_pc(call_depth);
        const GoldenSunObjB27EParentRoute* parent =
            find_golden_sun_current_b27e_parent(
                g_cpu.R[7], frame, call_depth, call_return_pc);
        if (parent) {
            gsr::widescreen::GoldenSunObjB328ParentMatch match{};
            match.valid = parent->valid;
            match.staging_address = parent->staging_address;
            match.frame = parent->frame;
            match.call_depth = parent->call_depth;
            match.call_return_pc = parent->call_return_pc;
            match.original_decision = parent->original_decision;
            match.final_decision = parent->final_decision;
            match.overridden = parent->overridden;
            overridden =
                gsr::widescreen::golden_sun_b328_parent_override(
                    golden_sun_expanded_obj_view_active(), original_decision,
                    compared_operand, g_cpu.R[7], frame, call_depth,
                    call_return_pc, match, out_decision);
        }
        if (!overridden && golden_sun_expanded_obj_view_active() &&
            original_decision == 1u && compared_operand >= 160 &&
            compared_operand < 200 &&
            golden_sun_obj_b328_writer_authorized(g_cpu.R[7], frame)) {
            *out_decision = 0u;
            overridden = true;
        }
        // A sprite entering the bottom margin (never on screen last frame,
        // so never authorized above) -- the oars as the ship's deck scrolls
        // (gpu_rewind_0092: parked at 199 until their real row was <= 159).
        // Only when its box cannot wrap to the console's top row.
        std::uint16_t attr0 = 0, attr1 = 0, attr2 = 0;
        if (!overridden && golden_sun_expanded_obj_view_active() &&
            original_decision == 1u && compared_operand >= 160 &&
            compared_operand < 200 &&
            read_golden_sun_obj_staging_attrs(g_cpu.R[7], &attr0, &attr1,
                                              &attr2) &&
            gsr::widescreen::golden_sun_obj_bottom_margin_cannot_wrap(
                compared_operand, attr0, attr1)) {
            *out_decision = 0u;
            overridden = true;
        }
    }
    const std::uint32_t final_decision =
        overridden ? *out_decision : original_decision;
    // World-map sprite decisions near the right edge, one line each (VRAM
    // map write trace on, map number 2, bounded). Towns vanish there at an
    // OAM x of 235..244 although every widened x route admits them, so this
    // names the site and value that actually drops them (FACTS.md
    // 2026-09-24). C6FA's operand is 16.16 fixed point.
    if (golden_sun_wide_diagnostics_enabled() &&
        bus_read_u16(0x02000408u) == 2u &&
        (instruction_pc == 0x0800B324u || instruction_pc == 0x0800B3D2u ||
         instruction_pc == 0x0800B3DCu || instruction_pc == 0x0800C6FAu)) {
        static unsigned logged = 0;
        const std::int32_t px = instruction_pc == 0x0800C6FAu
            ? static_cast<std::int32_t>(
                  static_cast<std::uint32_t>(compared_operand) >> 16)
            : compared_operand;
        if (px >= 200 && px <= 420 && logged < 4000u) {
            ++logged;
            std::fprintf(stderr,
                         "[wide-branch-worldmap] frame=%llu pc=0x%08X "
                         "operand=%d px=%d original=%u final=%u\n",
                         static_cast<unsigned long long>(runtime_current_frame()),
                         instruction_pc, compared_operand, px,
                         original_decision, final_decision);
        }
    }
    record_golden_sun_signed_y_cull(
        instruction_pc, compared_operand, original_decision, final_decision,
        overridden);
    if (instruction_pc == 0x0800B27Eu || instruction_pc == 0x0800B328u) {
        trace_golden_sun_obj_y_cull_decision(
            instruction_pc, original_decision, final_decision, overridden,
            compared_operand);
    }
    if (instruction_pc == 0x0800B328u) {
        if (gsr::widescreen::golden_sun_obj_y_branch_writes_oam(
                instruction_pc, original_decision, overridden,
                final_decision)) {
            record_golden_sun_b328_accepted_candidate(
                compared_operand, b328_classification, g_cpu.R[7],
                runtime_current_frame(), runtime_call_stack_depth(),
                golden_sun_obj_call_return_pc(runtime_call_stack_depth()));
        } else {
            record_golden_sun_b328_rejected_candidate(
                compared_operand, b328_classification, b328_mismatch_flags,
                g_cpu.R[7], runtime_current_frame(),
                runtime_call_stack_depth(),
                golden_sun_obj_call_return_pc(runtime_call_stack_depth()));
        }
    }
    if (golden_sun_expanded_obj_view_active() &&
        instruction_pc == 0x0800B27Eu) {
        record_golden_sun_b27e_parent_route(
            original_decision, overridden, final_decision, compared_operand);
    }
    // B324/B328 are the final staging-record route. B27E is an earlier
    // decision point and does not itself write the record, so provenance must
    // not be attached there.
    const bool expanded_obj_view = golden_sun_expanded_obj_view_active();
    if (expanded_obj_view && instruction_pc == 0x0800B324u &&
        ((!overridden && !original_decision) ||
         (overridden && *out_decision == 0u))) {
        record_golden_sun_obj_staging(
            instruction_pc, true, compared_operand);
    }
    if (expanded_obj_view && instruction_pc == 0x0800B328u &&
        gsr::widescreen::golden_sun_obj_y_branch_writes_oam(
            instruction_pc, original_decision, overridden,
            overridden ? *out_decision : original_decision)) {
        record_golden_sun_obj_staging(
            instruction_pc, false, compared_operand);
    }
    // A rejected final Y branch means no staging record is written. Retire
    // the X candidate captured at B324 so a later OAM slot cannot inherit it.
    if (expanded_obj_view && instruction_pc == 0x0800B328u &&
        !gsr::widescreen::golden_sun_obj_y_branch_writes_oam(
            instruction_pc, original_decision, overridden,
            overridden ? *out_decision : original_decision)) {
        clear_golden_sun_obj_staging(g_cpu.R[7]);
    }
    if (!overridden) {
        if (instruction_pc == 0x0800B27Eu || instruction_pc == 0x0800B328u) {
            trace_golden_sun_obj_y_attempt(
                instruction_pc, g_ws_active ? "branch-rejected" : "scene-disabled",
                compared_operand < 0 ? 0u : static_cast<std::uint32_t>(compared_operand),
                0u, false, -1);
        }
        record_golden_sun_cull_inactive_reject(instruction_pc);
        return 0;
    }
    record_golden_sun_cull_bypass(instruction_pc);
    return 1;
}

void record_golden_sun_obj_ewram_write(std::uint32_t address,
                                       std::uint32_t size) {
    if (!gsr::obj_recorder_enabled() || size == 0u) return;
    const std::uint64_t write_end = static_cast<std::uint64_t>(address) + size;
    const auto record = [&](std::uint32_t start, std::uint32_t end,
                            const char* region) {
        // `end` is an inclusive measured address. A store may extend past it,
        // but only its overlap with the measured interval is in scope.
        if (write_end <= start || address > end) return false;
        gsr::ObjEwramWriteSample sample;
        sample.frame = runtime_current_frame();
        sample.epoch = g_golden_sun_field_auth_epoch;
        sample.region = region;
        sample.writer_pc = runtime_current_pc();
        sample.address = address;
        sample.offset = address - start;
        sample.size = size;
        gsr::obj_recorder_note_ewram_write(sample);
        return true;
    };
    if (record(kGoldenSunObjSourceRegionAStart,
               kGoldenSunObjSourceRegionAEnd, "region_a")) return;
    record(kGoldenSunObjSourceRegionBStart, kGoldenSunObjSourceRegionBEnd,
           "region_b");
}

bool golden_sun_obj_boulder_field_in_span(std::uint32_t source,
                                          std::uint32_t offset) {
    return source >= kGoldenSunObjSourceRegionBStart &&
        source <= kGoldenSunObjSourceRegionBEnd &&
        offset <= kGoldenSunObjSourceRegionBEnd - source &&
        4u <= kGoldenSunObjSourceRegionBEnd - source - offset + 1u;
}

void fill_golden_sun_obj_boulder_registers(
    gsr::ObjBoulderTraceSample* sample) {
    if (!sample) return;
    sample->r0 = g_cpu.R[0];
    sample->r1 = g_cpu.R[1];
    sample->r2 = g_cpu.R[2];
    sample->r3 = g_cpu.R[3];
    sample->r5 = g_cpu.R[5];
    sample->r6 = g_cpu.R[6];
    sample->r7 = g_cpu.R[7];
    sample->r9 = g_cpu.R[9];
    sample->r10 = g_cpu.R[10];
    sample->r11 = g_cpu.R[11];
}

void fill_golden_sun_obj_boulder_source_fields(
    gsr::ObjBoulderTraceSample* sample, std::uint32_t source,
    std::uint32_t pending_offset = 0u, std::uint32_t pending_value = 0u) {
    if (!sample) return;
    if (source >= kGoldenSunObjSourceRegionBStart &&
        source <= kGoldenSunObjSourceRegionBEnd) {
        sample->source = source;
        sample->source_offset = source - kGoldenSunObjSourceRegionBStart;
    }
    if (!golden_sun_obj_boulder_field_in_span(source, 0x0Cu) ||
        !golden_sun_obj_boulder_field_in_span(source, 0x14u)) return;
    sample->field_a_value = bus_read_u32(source + 0x0Cu);
    sample->field_b_value = bus_read_u32(source + 0x14u);
    if (pending_offset == 0x0Cu) sample->field_a_value = pending_value;
    if (pending_offset == 0x14u) sample->field_b_value = pending_value;
    sample->candidate_fields_valid = true;
}

// The writer census measured these exact instructions as the Region B
// candidate-field stores. The generated code shows 0x0809496E stores R1 and
// 0x08094970 stores R0, then the post-call result is stored at 0x08094980.
// Capture each exact source operand before the fast-path store, together with
// the rest of the calculation registers.
void record_golden_sun_obj_boulder_field_write(std::uint32_t address,
                                               std::uint32_t size) {
    if (!gsr::obj_recorder_enabled() || size != 4u) return;
    const std::uint32_t pc = runtime_current_pc();
    const std::uint32_t field_offset = pc == 0x0809496Eu ? 0x0Cu :
        pc == 0x08094970u ? 0x14u :
        pc == 0x08094980u ? 0x10u : 0u;
    if (field_offset == 0u || address < kGoldenSunObjSourceRegionBStart ||
        address > kGoldenSunObjSourceRegionBEnd) return;
    const std::uint32_t relative = address - kGoldenSunObjSourceRegionBStart;
    if (relative < field_offset ||
        (relative - field_offset) % 0x20u != 0u) return;
    const std::uint32_t source = address - field_offset;
    if (!golden_sun_obj_boulder_field_in_span(source, 0x0Cu) ||
        !golden_sun_obj_boulder_field_in_span(source, 0x14u)) return;
    // At 0x08094980 the generated code stores the result of the preceding
    // 0x0809497C call through the same R7 source base. Keep this boundary
    // tied to that measured base rather than accepting an address-shaped
    // match from an unrelated store.
    if (pc == 0x08094980u && g_cpu.R[7] != source) return;

    gsr::ObjBoulderTraceSample sample;
    sample.event = pc == 0x08094980u ? "calc-result-write" : "field-write";
    sample.source_state = "writer-store";
    sample.outcome = "not-committed";
    sample.frame = runtime_current_frame();
    sample.epoch = g_golden_sun_field_auth_epoch;
    sample.writer_pc = pc;
    fill_golden_sun_obj_boulder_registers(&sample);
    // The source operands are fixed by the measured generated instructions
    // above, not inferred from the stored value.
    sample.store_value = pc == 0x0809496Eu ? g_cpu.R[1] : g_cpu.R[0];
    fill_golden_sun_obj_boulder_source_fields(
        &sample, source, field_offset, sample.store_value);
    gsr::obj_recorder_note_boulder_trace(sample);
}

// This is the measured Region B producer's function entry. Its R7 base is
// the source table that the two field writers walk; recording the entry state
// gives the next capture a calculation boundary before either candidate field
// is written.
void record_golden_sun_obj_boulder_calc_entry(std::uint32_t entry_pc) {
    if (!gsr::obj_recorder_enabled() || entry_pc != 0x08094928u) return;
    const std::uint32_t source = g_cpu.R[7];
    if (!golden_sun_obj_boulder_field_in_span(source, 0x0Cu) ||
        !golden_sun_obj_boulder_field_in_span(source, 0x14u)) return;
    gsr::ObjBoulderTraceSample sample;
    sample.event = "calc-entry";
    sample.source_state = "producer-entry";
    sample.outcome = "not-committed";
    sample.frame = runtime_current_frame();
    sample.epoch = g_golden_sun_field_auth_epoch;
    sample.writer_pc = entry_pc;
    fill_golden_sun_obj_boulder_registers(&sample);
    fill_golden_sun_obj_boulder_source_fields(&sample, source);
    gsr::obj_recorder_note_boulder_trace(sample);
}

void golden_sun_wide_ewram_write_observer(std::uint32_t address,
                                          std::uint32_t size) {
    // DMA has descriptor-level provenance and must not be misreported as a
    // CPU store or establish authored-cell identity one copied unit at a time.
    if (gba::vram_trace::dma_active()) return;
    record_golden_sun_obj_ewram_write(address, size);
    record_golden_sun_obj_boulder_field_write(address, size);
    invalidate_golden_sun_palace_table_if_overlapping(address, size);
    // A loaded savestate can contain a populated table before any post-load
    // CPU store repopulates the bitmap; the provider stays fail-closed until
    // current-epoch CPU ownership is observed.
    g_golden_sun_field_authored.mark_write(address, size);
    record_golden_sun_field_table_write(address, size);
    // Chained rather than replacing the above: the room-load EWRAM write
    // recorder (GSR_MAP_RECORD) needs the same every-store feed to recover
    // the room-bounds struct offsets. No-ops in one branch when that
    // recorder is disabled.
    gsr::map_recorder_on_ewram_write(address, size);
}

// True when any COMMITTED-phase consumer of a fast IWRAM store below can act on
// [address, address + size). Each test copies the consumer's own condition:
//  - trace_oam_shadow_write_committed: golden_sun_oam_shadow_write_range (raw
//    address, no mirror normalisation), installed as its range predicate
//  - note_golden_sun_obj_record_write / _value: raw address vs the census
//    window (the value consumer's window is a subset of the write consumer's)
//  - runtime_note_pool_ldm_bus_write: mirror-normalised address vs the pool
//    window; it still checks its own armed flag, so it is only reached here
inline bool golden_sun_fast_iwram_store_has_consumer(std::uint32_t address,
                                                     std::uint32_t size) {
    if (size == 0u) return false;
    if (golden_sun_oam_shadow_write_range(address, size)) return true;
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    if (last > kGoldenSunObjRecordCensusStart &&
        first < kGoldenSunObjRecordCensusEnd) return true;
    const std::uint64_t physical = (address >> 24) == 0x03u
        ? 0x03000000u + (address & 0x00007FFFu)
        : address;
    return physical < RUNTIME_POOL_LDM_WINDOW_END &&
           physical + size > RUNTIME_POOL_LDM_WINDOW_BASE;
}

// Fill the inline gate g_runtime_fast_iwram_watch (runtime_arm.h): one bit per
// 64-byte IWRAM block, set when golden_sun_fast_iwram_store_has_consumer
// accepts any byte of it, so the observer is only called for stores it can act
// on. Every debug store trace needs every store, so it sets all blocks. The
// observer keeps the precise per-store check, so the gate only has to be a
// superset. A mirrored address reaches its block through `addr & 0x7FFF` and
// is still judged by the observer's own raw-address predicates.
void arm_golden_sun_fast_iwram_watch(bool enabled) {
    for (auto& word : g_runtime_fast_iwram_watch) word = 0u;
    if (!enabled) return;
    if (gba::vram_trace::iwram_store_trace_enabled()) {
        for (auto& word : g_runtime_fast_iwram_watch) word = ~0ull;
        return;
    }
    for (std::uint32_t off = 0; off < 0x8000u; ++off) {
        if (golden_sun_fast_iwram_store_has_consumer(0x03000000u + off, 1u)) {
            g_runtime_fast_iwram_watch[off >> 12] |=
                1ull << ((off >> 6) & 63u);
        }
    }
}

void golden_sun_fast_iwram_write_observer(std::uint32_t address,
                                          std::uint32_t size,
                                          std::uint32_t phase) {
    // Both debug traces read the environment once and never change, so the
    // answer is cached. With them off, the START phase has no consumer and
    // the COMMITTED phase is skipped unless a consumer's range is touched.
    static const bool store_traces_on =
        gba::vram_trace::iwram_store_trace_enabled();
    if (!store_traces_on) {
        if (phase == RUNTIME_FAST_IWRAM_WRITE_START) return;
        if (!golden_sun_fast_iwram_store_has_consumer(address, size)) return;
    }
    if (phase == RUNTIME_FAST_IWRAM_WRITE_START) {
        gba::vram_trace::trace_oam_shadow_write(g_cpu.R[15], address, size);
        return;
    }
    gba::vram_trace::trace_oam_shadow_write_committed(
        g_cpu.R[15], address, size);
    // Fast generated IWRAM stores are already committed at this seam. Feed
    // the CRASH-03 observer as a post-write bus-equivalent event so partial
    // stores can be reconstructed from the live word.
    note_golden_sun_obj_record_write(g_cpu.R[15], address, size);
    note_golden_sun_obj_record_value(g_cpu.R[15], address, size);
    runtime_note_pool_ldm_bus_write(address, size);
}

// Is this OAM slot one of Golden Sun's parked dummies? Called once per OBJ per
// rendered frame by the expanded renderer (gba_ppu.h, g_ws_obj_park_provider).
// Pure: it reads only the attributes handed to it, touches no guest memory and
// changes nothing. The sentinel and the measurement behind it are in
// src/widescreen_policy.h.
extern "C" int golden_sun_obj_park_provider(int /*oam_index*/,
                                            std::uint16_t attr0,
                                            std::uint16_t attr1,
                                            std::uint16_t /*attr2*/) {
    return gsr::widescreen::golden_sun_obj_is_parked(attr0, attr1) ? 1 : 0;
}

int golden_sun_thumb_alu_immediate(std::uint32_t pc, std::uint32_t original,
                                   std::uint32_t* out_value);
int golden_sun_thumb_literal(std::uint32_t pc, std::uint32_t original,
                             std::uint32_t* out_value);
int golden_sun_conditional_branch(std::uint32_t pc, std::uint32_t original,
                                  std::uint32_t* out_decision);

void install_golden_sun_widescreen(std::uint32_t extra_left,
                                   std::uint32_t extra_right,
                                   std::uint32_t extra_top,
                                   std::uint32_t extra_bottom) {
    // run_game resets all game-owned PPU hooks before startup. This callback
    // is reached only after an expanded view has been authorized and applied.
    g_golden_sun_wide_extra_left = extra_left;
    g_golden_sun_wide_extra_right = extra_right;
    g_golden_sun_wide_extra_top = extra_top;
    g_golden_sun_wide_extra_bottom = extra_bottom;
    g_golden_sun_mode0_field = false;
    g_golden_sun_mode0_split_scroll = false;
    g_golden_sun_mode0_split_scroll_frame.reset();
    g_golden_sun_expanded_obj_scene = false;
    clear_golden_sun_obj_y_provenance();
    reset_golden_sun_func1dc8_writer_diagnostics();
    g_golden_sun_obj_y_provenance_logs_in_epoch = 0;
    reset_golden_sun_obj_y_alias_diagnostics();
    g_golden_sun_wide_line_io_valid = false;
    g_golden_sun_wide_line_dispcnt = 0;
    g_golden_sun_field_authored.reset();
    g_golden_sun_field_auth_scene = GoldenSunFieldAuthScene::None;
    g_golden_sun_wide_field_map_census.clear();
    g_golden_sun_wide_field_map_trace_frame = UINT64_MAX;
    g_golden_sun_field_auth_epoch = 0;
    g_golden_sun_palace_table_authorized = false;
    g_golden_sun_palace_table_invalidated = false;
    g_golden_sun_palace_table_auth_attempted = false;
    reset_golden_sun_palace_active_region();
    g_golden_sun_field_map_writes = {};
    g_golden_sun_field_raw_writes = {};
    g_golden_sun_field_map_dma_writes = {};
    g_golden_sun_field_raw_dma_writes = {};
    g_golden_sun_field_provider_trace = {};
    g_golden_sun_field_map_id_attribution = {};
    g_golden_sun_field_map_id_attribution_overflow = {};
    reset_golden_sun_field_producers();
    reset_golden_sun_obj_record_census();
    reset_golden_sun_obj_record_value_events();
    reset_golden_sun_obj_commit_order();
    g_golden_sun_field_epoch_map_writes = 0;
    g_golden_sun_field_epoch_raw_writes = 0;
    g_golden_sun_field_epoch_map_dma_writes = 0;
    g_golden_sun_field_epoch_raw_dma_writes = 0;
    g_golden_sun_field_table_cpu_logs_in_epoch = 0;
    g_golden_sun_field_table_dma_logs_in_epoch = 0;
    gba::vram_trace::reset_oam_trace_window();
    g_golden_sun_wide_policy_seen = false;
    g_golden_sun_wide_policy_last_flags = gsr::widescreen::kPillarboxAll;
    g_golden_sun_wide_policy_last_reason =
        GoldenSunWidePolicyReason::UnsupportedMode;
    g_golden_sun_wide_policy_transition_count = 0;
    g_golden_sun_wide_policy_logged_transitions = 0;
    g_golden_sun_wide_policy_sample_count = 0;
    g_golden_sun_wide_policy_sample_frame = UINT64_MAX;
    g_golden_sun_wide_policy_sample_end_frame = UINT64_MAX;
    g_golden_sun_wide_policy_sample_transition = 0;
    g_golden_sun_wide_policy_samples_in_window = 0;
    g_golden_sun_margin_diagnostic_callbacks = 0;
    g_golden_sun_margin_diagnostic_logged = 0;
    g_golden_sun_margin_diagnostic_last_log_frame = UINT64_MAX;
    g_golden_sun_margin_diagnostic_total = {};
    g_golden_sun_margin_diagnostic_last = {};
    static bool cull_report_armed = false;
    if (!cull_report_armed) {
        cull_report_armed = true;
        std::atexit(report_golden_sun_cull_trace_at_exit);
    }
    static bool wide_report_armed = false;
    if (!wide_report_armed) {
        wide_report_armed = true;
        std::atexit(report_golden_sun_wide_diagnostics_at_exit);
    }
    // The margin policy is a survivor of the old widescreen implementation:
    // it classifies each raster line and pillarboxes (blacks) the margins
    // whenever it cannot vouch for what would be drawn there. That made sense
    // when margin content was invented per pixel. With the room buffer it is
    // actively wrong -- it blacked Goma Cave Entrance out entirely while
    // towns, which its heuristics happen to accept, drew fine. The buffer
    // blanks per sample when it genuinely has no answer, which is the same
    // protection without the guesswork.
    // NOT nullptr: the PPU reads a null policy as "use the legacy
    // pillarbox globals" (margin_is_pillarboxed's use_legacy_pillarbox),
    // so removing the policy switches an older blanking path ON. Install a
    // policy that pillarboxes nothing instead.
    gba::g_ws_margin_policy = gsr::room_buffer_rendering()
        ? golden_sun_room_buffer_margin_policy
        : golden_sun_wide_margin_policy_callback;
    gba::g_ws_margin_diagnostics = golden_sun_wide_diagnostics_enabled()
        ? golden_sun_wide_margin_diagnostics_callback : nullptr;
    gba::g_ws_tilemap_provider = golden_sun_wide_tilemap_provider;
    gba::g_ws_bg_x_provider = golden_sun_wide_bg_x_provider;
    // Armed per row by golden_sun_update_battle_backdrop; the mask is limited
    // to battle BG layers, so the field path never pays for this hook.
    gba::g_ws_bg_sample_provider = golden_sun_battle_bg_sample_provider;
    gba::g_ws_bg_sample_provider_layers = 0u;
    gba::g_ws_bg_sample_provider_ignore_window_layers = 0u;
    gba::g_ws_defer_native_rows = 0;
    // The rich hooks consume signed logical coordinates captured before the
    // guest truncates them into OAM. Missing or stale visible provenance
    // always leaves canonical GBA wrapping in control.
    gba::g_ws_obj_park_provider = golden_sun_obj_park_provider;
    gba::g_ws_obj_attr_x_provider = golden_sun_wide_obj_attr_x_provider;
    gba::g_ws_obj_x_provider = nullptr;
    gba::g_ws_obj_y_provider = nullptr;
    gba::g_ws_obj_attr_y_provider = golden_sun_wide_obj_attr_y_provider;
    // Authored ownership is required by the provider in every expanded run;
    // diagnostics only controls logging, not the ownership contract.
    gba::g_ws_ewram_write_observer = golden_sun_wide_ewram_write_observer;
    g_runtime_fast_ewram_write_observer = golden_sun_wide_ewram_write_observer;
    // Enhanced Options uses the same committed F0 seam as the diagnostics.
    // Arm the generated fast-IWRAM path for every mode that consumes the
    // authenticated object placement, otherwise F0 stores bypass the runner
    // observer entirely.
    const bool oam_shadow_observer_enabled =
        golden_sun_wide_diagnostics_enabled() ||
        golden_sun_experimental_fixes_enabled() ||
        gsr::obj_recorder_enabled();
    arm_golden_sun_fast_iwram_watch(oam_shadow_observer_enabled);
    g_runtime_fast_iwram_write_observer =
        oam_shadow_observer_enabled
            ? golden_sun_fast_iwram_write_observer : nullptr;
    gba::vram_trace::set_dma_descriptor_observer(
        golden_sun_wide_dma_descriptor_observer);
    gba::vram_trace::set_oam_shadow_write_range_predicate(
        oam_shadow_observer_enabled ? golden_sun_oam_shadow_write_range
                                    : nullptr);
    gba::vram_trace::set_oam_shadow_write_observer(
        oam_shadow_observer_enabled
            ? golden_sun_oam_shadow_write_observer : nullptr);
    gba::g_ws_bg_x_provider_layers = 0xFu; // BG0 + Mode0 field layers.
    g_runtime_thumb_alu_imm_override = golden_sun_thumb_alu_immediate;
    g_runtime_thumb_literal_override = golden_sun_thumb_literal;
    g_runtime_conditional_branch_override = golden_sun_conditional_branch;
}

// ---- Message-speed delay observer (GSR_TEXT_RECORD) ----------------------
//
// The single store that decides how fast dialogue appears. From the generated
// image, 0x08016E5C is `strh r3,[r6,#0x22]`. The literal at 0x08016E4C is
// 0x02000240, but 0x08016E50..0x08016E54 add 0x83 << 2 (0x20C) before the
// ldrb at 0x08016E58. The actual source index byte is therefore
// 0x0200044C; that byte indexes the table at 0x08073808, and r6 is the text
// context.
//
// Everything else about this is measured except the value itself: the probe
// in function_tracer.cpp samples the context at function entry, by which
// point the delay has already been spent and reads the same under every
// setting (518 samples each, two sessions, no difference). Catching the store
// is the only way to see what each setting actually writes. Logged, never
// altered -- what to do with the value is a separate decision that needs this
// number first.
constexpr std::uint32_t kTextDelayStorePc = 0x08016E5Cu;
constexpr std::uint32_t kTextBudgetBasePc = 0x08016920u;
constexpr std::uint32_t kTextBudgetOverridePc = 0x08016942u;
constexpr std::uint32_t kTextBudgetDecrementPc = 0x08016F00u;
constexpr std::uint32_t kMessageSpeedIndexBaseAddress = 0x02000240u;
constexpr std::uint32_t kMessageSpeedIndexOffset = 0x0000020Cu;
constexpr std::uint32_t kMessageSpeedIndexAddress =
    kMessageSpeedIndexBaseAddress + kMessageSpeedIndexOffset;

bool text_delay_logging_enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("GSR_TEXT_RECORD");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return enabled;
}

void note_text_delay_store(std::uint32_t addr, std::uint32_t value) {
    std::uint32_t table_index = 0xFFFFu;
    std::uint32_t base_byte = 0xFFFFu;
    if (const gba::GbaBus* bus = gbarecomp::active_bus()) {
        if (const std::uint8_t* ewram = bus->ewram_ptr()) {
            table_index = ewram[(kMessageSpeedIndexAddress -
                                 0x02000000u) & 0x0003FFFFu];
            base_byte = ewram[(kMessageSpeedIndexBaseAddress -
                                 0x02000000u) & 0x0003FFFFu];
        }
    }
    gsr::text_trace_delay_store(table_index, base_byte,
                                value & 0xFFFFu, addr);
}

void observe_text_delay_store(std::uint32_t pc, std::uint32_t addr,
                              std::uint32_t value, std::uint32_t width) {
    if (pc == kTextDelayStorePc && width == 2u)
        note_text_delay_store(addr, value);
    if (width == 4u &&
        (pc == kTextBudgetBasePc || pc == kTextBudgetOverridePc ||
         pc == kTextBudgetDecrementPc)) {
        gsr::text_trace_budget_store(pc, addr, value, g_cpu.R[6]);
    }
}

// Draw the field with the graphics card (GSR_GPU_FIELD, a launcher checkbox).
// Off by default and never part of the faithful run. Step 5 of
// docs/NATIVE_SCENE_RENDERER_PLAN.md: src/field_scene.cpp describes each
// frame, src/field_scene_renderer.cpp draws it through gbarecomp's
// game-agnostic gpu_surface.cpp, and this pair of hooks connects both to the
// running game. Declining is always whole-frame (FieldSceneRenderer::draw's
// own contract) -- battle, the overworld, and anything this renderer does
// not yet know how to draw still comes from the emulated hardware path
// exactly as it does today.
bool gpu_field_enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("GSR_GPU_FIELD");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return enabled;
}

// Draw ONLY with the graphics card (GSR_GPU_FIELD_ONLY, the launcher's
// sub-checkbox). Two effects, both aimed at making what is still missing
// visible instead of invisible:
//
//   * the console's per-scanline compositor stops drawing altogether (it
//     still records each row's registers, which the capture below reads);
//   * a frame the card refuses is painted flat magenta rather than quietly
//     handed back to that compositor.
//
// With the fallback in place a refused frame looks like a correct frame, so
// the refusals -- the only thing left to find -- cannot be seen at all. The
// launcher forces this off unless GSR_GPU_FIELD is on too.
bool gpu_field_only_enabled() {
    static const bool enabled = [] {
        if (!gpu_field_enabled()) return false;
        const char* e = std::getenv("GSR_GPU_FIELD_ONLY");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return enabled;
}

// Filled by gpu_field_capture_hook() at VBlank start -- the same boundary
// the canonical framebuffer latches at (runtime_bus_bridge.cpp's
// latch_native_scene_state) -- and read once, later the same frame, by
// gpu_field_present_override(). Plain memory copies only; no GL call is made
// here, because this fires from deep inside guest instruction dispatch, not
// necessarily from wherever the host's GL context is current. The actual
// draw happens at present time instead, right beside the existing
// win.present() call it stands in for (see runtime.cpp's present_first).
// Game windows (menus, dialogue, settings) borrow BG0's character block for
// their font and frame pieces. The room's layers can share that block
// (BG3CNT 0x0503 and BG0CNT 0x0400 both use block 0 in gpu_rewind_0025), and
// on hardware a menu covers the screen, so the borrowed tiles never show
// under the room; in the expanded margins they showed as scraps of menu text.
// While no window is open this keeps a copy of the background graphics;
// while one is open the margins read the copy instead. FACTS.md 2026-09-24.
//
// Widened 2026-09-26. The copy was BG0's first 16 KB block only; menus also
// write the rest of the background area. Djinn and Status screens
// (logs/gpu_frame_0077/0078): the ground layer's margin tiles 517..1015 sit
// in block 1, where the menu's second text bank lives (window block +0xEA2
// set), and the tree/cliff layers read block 2, which the copy never held --
// pink ground and noisy cliffs in the margins. So the copy is now the whole
// 64 KB background area, whatever the layers' character bases are.
//
// It is also refreshed only after the window system has stood empty for
// kRoomCopySettleFrames in a row. A single empty frame used to refresh it,
// and the menu frees and re-takes its text tiles between screens, so one
// such frame would copy the menu's own graphics in as the "room".
//
// The colours too (2026-09-26, logs/gpu_frame_0079/0080): menus load their
// own palettes over some of the field's background banks -- bank 0 held
// pure red/green/blue/white on the Status screen -- so room tiles in the
// margins came out pink and blue with their shapes intact. The 256
// background colours are saved and restored with the graphics.
std::vector<std::uint8_t> g_room_char_copy;
std::vector<std::uint16_t> g_room_palette_copy;
constexpr std::size_t kBgPaletteEntries = 256;
int g_room_copy_closed_frames = 0;
constexpr int kRoomCopySettleFrames = 30;
constexpr std::uint32_t kBgAreaBytes = 0x10000u;
extern std::atomic<int> g_settings_page;  // defined with the settings page

bool game_window_open() {
    using namespace gsr::settings_page;
    if (g_settings_page.load() == 2) return true;
    const std::uint32_t block = bus_read_u32(kWindowBlockSlot);
    if ((block >> 24) != 0x02u) return false;
    if (bus_read_u8(block + kTileBankFlagOffset) != 0u) return true;
    for (std::uint32_t i = 0; i < 256u; ++i) {
        if (bus_read_u8(block + kTileUsageOffset + i) != 0u) return true;
    }
    return false;
}

void build_room_vram(const std::vector<std::uint8_t>& vram,
                     const std::array<std::uint16_t, 512>& palette,
                     std::vector<std::uint8_t>* out,
                     std::vector<std::uint16_t>* out_palette) {
    out->clear();          // empty = the renderer's live copies already agree
    out_palette->clear();
    if (vram.size() < kBgAreaBytes) return;
    if (!game_window_open()) {
        if (g_room_copy_closed_frames < kRoomCopySettleFrames) {
            ++g_room_copy_closed_frames;
        } else {
            g_room_char_copy.assign(vram.begin(), vram.begin() + kBgAreaBytes);
            g_room_palette_copy.assign(palette.begin(),
                                       palette.begin() + kBgPaletteEntries);
        }
        return;
    }
    g_room_copy_closed_frames = 0;
    if (g_room_char_copy.size() != kBgAreaBytes ||
        g_room_palette_copy.size() != kBgPaletteEntries)
        return;
    *out = vram;
    std::copy(g_room_char_copy.begin(), g_room_char_copy.end(), out->begin());
    out_palette->assign(palette.begin(), palette.end());
    std::copy(g_room_palette_copy.begin(), g_room_palette_copy.end(),
              out_palette->begin());
}

struct GpuFieldCapture {
    bool valid = false;
    gsr::CapturedEffectFrame effects;
    gsr::FieldScene scene;
    std::vector<std::uint8_t> vram;   // bus->vram_ptr(), 96 KB
    // The same, except that while a game window is open BG0's character
    // block holds what it held before the window opened (see
    // build_room_vram). FieldSceneRenderer::upload_room_vram; room tiles
    // in the margins read it.
    std::vector<std::uint8_t> room_vram;
    // Likewise the palette: the field's background colours restored while a
    // window is open, else empty.
    std::vector<std::uint16_t> room_palette;
    // bus->ewram_ptr(), 256 KB: the room rect/camera/id-grid/atlas tables
    // FieldSceneRenderer::upload_room reads (addresses in room_buffer.h),
    // taken from the live guest, not the offline recorded snapshots the
    // check tool uses.
    std::vector<std::uint8_t> ewram;
    // bus->iwram_ptr(), 32 KB. Not read by the renderer at all -- this is for
    // the F12 dump alone, and it is here because the spell effects cannot be
    // read any other way. Golden Sun composes an effect in a 16 KB buffer on
    // its own IWRAM heap and RELOCATES the routine that stamps particles into
    // that buffer to just behind it, so the routine exists only at runtime:
    // its address is never a ROM literal and the disassembly has no symbol for
    // it (FACTS.md, 2026-09-18). A write trace can say where it wrote but
    // never what instructions it is. Capturing IWRAM captures the routine
    // itself, to be disassembled offline.
    std::vector<std::uint8_t> iwram;
    // The cartridge, for WorldMapSource: the world map's pieces are unpacked
    // from it (world_map_source.h). Constant for the session.
    const std::uint8_t* rom = nullptr;
    std::size_t rom_bytes = 0;
    // Raw register/object/palette memory, for gpu_field_present_override()'s
    // F12 dump only (see its header comment). `scene` already carries a
    // decoded summary of IO/OAM/PAL, but a one-frame dump hands the replay
    // tool the exact memory the hardware compositor reads, not a re-encoding
    // of it, so these are separate full copies rather than derived from
    // `scene` at dump time.
    std::vector<std::uint8_t> io;     // bus->io().raw(), 0x400 bytes
    std::vector<std::uint8_t> oam;    // bus->oam_ptr(), 1024 bytes
    std::vector<std::uint8_t> pal;    // bus->pal_ptr(), 1024 bytes
    // game_window_open() at this frame's capture (world-map margin hold).
    bool window_open = false;
};
GpuFieldCapture g_gpu_field_capture;

// ---- World-map margin hold ------------------------------------------------
//
// Opening the menu on the world map switches the screen from the world map's
// mode 2 to mode 0 text layers with the same map blocks (BG3 map block 8,
// BG2 map block 10; logs/gpu_frame_0081.bin, 2026-09-26: DISPCNT 0x1F40,
// BG2CNT 0xAA0E, BG3CNT 0xA80A). The widened margins are only sourced from
// the world map in mode 2, so they went black for as long as the menu was
// open. The camera cannot move while a menu is open, so the margins of the
// last world-map frame before it are still the right picture: keep them,
// and paint them back around the live native picture on menu frames.
std::vector<std::uint8_t> g_world_margin_rgb;
std::uint32_t g_world_margin_w = 0;
std::uint32_t g_world_margin_h = 0;
// True when this frame's margins were drawn from the decoded, checked world
// map (set where upload_world_map is called). A mode 2 frame without it has
// scrambled margins, and holding one kept that garbage on screen for the
// whole menu (logs/gpu_frame_0101.bin, 2026-09-30).
bool g_world_map_source_ok = false;
// The background palette (BGR555) when the margins were saved, and for each
// distinct saved margin colour the palette index it was drawn from, so the
// held margins can follow a palette effect started after the save: the
// world map's Psynergy tint turns the whole palette red (average 18,4,4
// against 10,18,20; logs/gpu_frame_0102.bin, 2026-09-30) while the held
// margins stayed green.
std::array<std::uint16_t, 256> g_world_margin_pal{};
std::unordered_map<std::uint32_t, std::uint8_t> g_world_margin_index;

void world_margin_rgb_of(std::uint16_t c, int out[3]) {
    out[0] = (c & 31) * 255 / 31;
    out[1] = ((c >> 5) & 31) * 255 / 31;
    out[2] = ((c >> 10) & 31) * 255 / 31;
}

// Nearest saved-palette index for every distinct colour in the saved
// margins, found once per save with a new palette.
void world_margin_index_colours(std::uint32_t width, std::uint32_t height) {
    g_world_margin_index.clear();
    int pal_rgb[256][3];
    for (int i = 0; i < 256; ++i)
        world_margin_rgb_of(g_world_margin_pal[i], pal_rgb[i]);
    const std::uint8_t* p = g_world_margin_rgb.data();
    const std::size_t n = std::size_t{width} * height;
    for (std::size_t i = 0; i < n; ++i, p += 3) {
        const std::uint32_t key = (std::uint32_t{p[0]} << 16) |
                                  (std::uint32_t{p[1]} << 8) | p[2];
        if (g_world_margin_index.count(key)) continue;
        int best = 0, best_d = 1 << 30;
        for (int k = 0; k < 256; ++k) {
            const int dr = p[0] - pal_rgb[k][0];
            const int dg = p[1] - pal_rgb[k][1];
            const int db = p[2] - pal_rgb[k][2];
            const int d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best_d = d; best = k; }
        }
        g_world_margin_index[key] = static_cast<std::uint8_t>(best);
    }
}

bool world_map_blocks(const std::vector<std::uint8_t>& io) {
    if (io.size() < 0x10) return false;
    return world_map_blocks_raw(io.data());
}

// The colour the console draws across row `y` from its four text layers,
// when all 240 pixels come out the same (the battle flash's band, see
// world_map_margin_hold). False for anything else, and for a row with
// windows or alpha blending, which this does not model. Objects are not
// drawn. `regs` is the row's register copy (FieldScene::row_io).
bool text_row_single_colour(const std::uint8_t* regs, int y,
                            const std::vector<std::uint8_t>& vram,
                            const std::vector<std::uint8_t>& pal,
                            std::uint8_t rgb_out[3]) {
    auto reg = [&](int o) { return unsigned(regs[o] | (regs[o + 1] << 8)); };
    const unsigned dispcnt = reg(0x00);
    if ((dispcnt & 7u) != 0u || (dispcnt & 0xE000u) != 0u) return false;
    if (vram.size() < 0x10000u || pal.size() < 512u) return false;
    const unsigned bldcnt = reg(0x50);
    const unsigned effect = (bldcnt >> 6) & 3u;
    if (effect == 1u) return false;
    const unsigned evy = std::min(16u, reg(0x54) & 31u);
    int first = -1;
    for (int x = 0; x < 240; ++x) {
        int best_prio = 4, target = 5;  // 5 = backdrop
        unsigned index = 0;
        for (int bg = 0; bg < 4; ++bg) {
            if (!((dispcnt >> (8 + bg)) & 1u)) continue;
            const unsigned cnt = reg(0x08 + 2 * bg);
            const int prio = static_cast<int>(cnt & 3u);
            if (prio >= best_prio) continue;
            const unsigned chars = ((cnt >> 2) & 3u) * 0x4000u;
            const unsigned map = ((cnt >> 8) & 31u) * 0x800u;
            const int w = (cnt & 0x4000u) ? 512 : 256;
            const int h = (cnt & 0x8000u) ? 512 : 256;
            const int tx = (x + static_cast<int>(reg(0x10 + 4 * bg) & 0x1FFu)) & (w - 1);
            const int ty = (y + static_cast<int>(reg(0x12 + 4 * bg) & 0x1FFu)) & (h - 1);
            const unsigned at = map +
                (unsigned(tx >> 8) + unsigned(ty >> 8) * unsigned(w >> 8)) * 0x800u +
                unsigned(((ty & 255) >> 3) * 32 + ((tx & 255) >> 3)) * 2u;
            if (at + 1u >= vram.size()) continue;
            const unsigned e = vram[at] | (vram[at + 1] << 8);
            const int px = (e & 0x400u) ? 7 - (tx & 7) : (tx & 7);
            const int py = (e & 0x800u) ? 7 - (ty & 7) : (ty & 7);
            unsigned i = 0;
            if (cnt & 0x80u) {
                const unsigned a = chars + (e & 0x3FFu) * 64u + unsigned(py * 8 + px);
                if (a >= 0x10000u) continue;
                i = vram[a];
            } else {
                const unsigned a = chars + (e & 0x3FFu) * 32u + unsigned(py * 4 + px / 2);
                if (a >= 0x10000u) continue;
                i = (px & 1) ? (vram[a] >> 4) : (vram[a] & 15u);
                if (i) i += (e >> 12) * 16u;
            }
            if (!i) continue;
            best_prio = prio;
            target = bg;
            index = i;
        }
        unsigned c = pal[index * 2] | (pal[index * 2 + 1] << 8);
        if ((effect == 2u || effect == 3u) && ((bldcnt >> target) & 1u)) {
            unsigned out = 0;
            for (int k = 0; k < 3; ++k) {
                const unsigned v = (c >> (5 * k)) & 31u;
                const unsigned nv = effect == 2u ? v + (31u - v) * evy / 16u
                                                 : v - v * evy / 16u;
                out |= nv << (5 * k);
            }
            c = out;
        }
        if (first < 0) first = static_cast<int>(c);
        else if (static_cast<int>(c) != first) return false;
    }
    for (int k = 0; k < 3; ++k)
        rgb_out[k] = static_cast<std::uint8_t>(((first >> (5 * k)) & 31) * 255 / 31);
    return true;
}

void world_map_margin_hold(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height) {
    const std::vector<std::uint8_t>& io = g_gpu_field_capture.io;
    const unsigned mode = io.empty() ? 0xFFu : (io[0] & 7u);
    const bool world = world_map_blocks(io);
    const bool native_size = width == 240 && height == 160;
    if (!world) {
        g_world_margin_rgb.clear();  // anywhere else: the saved view is stale
        return;
    }
    if (mode == 2u) {
        // The battle flash: rows switched to mode 0 for a band of the
        // game's solid fill layer (BG1, tile 0x7F, palette 15), every other
        // frame, growing from the middle (gpu_rewind_0087, frames
        // 1684-1696). The graphics card draws a frame in one mode, so those
        // rows came out as world map; paint them, across the margins too,
        // in the one colour the console draws there.
        const gsr::FieldScene& sc = g_gpu_field_capture.scene;
        const std::uint32_t band_y0 = (height - 160u) / 2u;
        bool mixed = false;
        for (int y = 0; y < gsr::FieldScene::kRows; ++y) {
            if (!sc.row_io_valid[y] || (sc.row_io[y][0] & 7u) == 2u) continue;
            mixed = true;
            std::uint8_t colour[3];
            if (!text_row_single_colour(sc.row_io[y].data(), y,
                                        g_gpu_field_capture.vram,
                                        g_gpu_field_capture.pal, colour))
                continue;
            std::uint8_t* row = rgb + (std::size_t{band_y0} + y) * width * 3u;
            for (std::uint32_t x = 0; x < width; ++x)
                std::memcpy(row + std::size_t{x} * 3u, colour, 3);
        }
        if (mixed) return;  // a transition, not a view to hold
        // Save only a settled world-map frame: no menu yet, and no screen
        // fade under way (BLDCNT brightness effect with a non-zero BLDY),
        // or the "held" margins would be a darkened copy.
        const unsigned bldcnt = io[0x50] | (static_cast<unsigned>(io[0x51]) << 8);
        const bool fading = ((bldcnt >> 6) & 3u) >= 2u && (io[0x54] & 0x1Fu) != 0u;
        if (!native_size && !fading && !g_gpu_field_capture.window_open &&
            g_world_map_source_ok &&
            g_gpu_field_capture.pal.size() >= 512) {
            g_world_margin_rgb.assign(rgb, rgb + std::size_t{width} * height * 3u);
            g_world_margin_w = width;
            g_world_margin_h = height;
            const auto& pal = g_gpu_field_capture.pal;
            for (int i = 0; i < 256; ++i)
                g_world_margin_pal[i] = static_cast<std::uint16_t>(
                    pal[i * 2] | (pal[i * 2 + 1] << 8));
            // Saved every settled frame; the colour index is only needed if
            // a palette effect follows, so it is built lazily on the first
            // menu frame whose palette differs (see below).
            g_world_margin_index.clear();
        }
        return;
    }
    if (mode != 0u) return;
    // Mode 0 on the world map's blocks: the menu's view of the world map.
    if (native_size) return;
    const std::uint32_t x0 = (width - 240u) / 2u;
    const std::uint32_t y0 = (height - 160u) / 2u;
    // A flat picture -- the battle flash fading from white to black, the
    // game's fill layer over the whole screen (gpu_rewind_0087, frames
    // 1698-1714) -- is no menu over the map: the margins take its colour.
    {
        std::uint8_t flat[3];
        std::memcpy(flat, rgb + (std::size_t{y0} * width + x0) * 3u, 3);
        bool is_flat = true;
        for (std::uint32_t y = y0; y < y0 + 160u && is_flat; ++y) {
            const std::uint8_t* p = rgb + (std::size_t{y} * width + x0) * 3u;
            for (std::uint32_t x = 0; x < 240u; ++x, p += 3)
                if (p[0] != flat[0] || p[1] != flat[1] || p[2] != flat[2]) {
                    is_flat = false;
                    break;
                }
        }
        if (is_flat) {
            for (std::uint32_t y = 0; y < height; ++y) {
                const bool native_row = y >= y0 && y < y0 + 160u;
                std::uint8_t* p = rgb + std::size_t{y} * width * 3u;
                for (std::uint32_t x = 0; x < width; ++x, p += 3) {
                    if (native_row && x == x0) { x += 239u; p += 239u * 3u; continue; }
                    std::memcpy(p, flat, 3);
                }
            }
            return;
        }
    }
    // No settled world-map frame to hold, as when a Djinn event switches to
    // mode 0 straight away (logs/gpu_frame_0083.bin, Flint, 2026-09-26): the
    // margins would show the world map's rotation-layer data read as text
    // layers, which is scrambled tiles. Black instead (Jimmy prefers black
    // margins to garbage).
    if (g_world_margin_w != width || g_world_margin_h != height ||
        g_world_margin_rgb.empty()) {
        for (std::uint32_t y = 0; y < height; ++y) {
            std::uint8_t* dst = rgb + std::size_t{y} * width * 3u;
            if (y < y0 || y >= y0 + 160u) {
                std::memset(dst, 0, std::size_t{width} * 3u);
                continue;
            }
            std::memset(dst, 0, std::size_t{x0} * 3u);
            std::memset(dst + std::size_t{x0 + 240u} * 3u, 0,
                        std::size_t{width - x0 - 240u} * 3u);
        }
        return;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        const bool native_row = y >= y0 && y < y0 + 160u;
        std::uint8_t* dst = rgb + std::size_t{y} * width * 3u;
        const std::uint8_t* src =
            g_world_margin_rgb.data() + std::size_t{y} * width * 3u;
        if (!native_row) {
            std::memcpy(dst, src, std::size_t{width} * 3u);
            continue;
        }
        std::memcpy(dst, src, std::size_t{x0} * 3u);
        const std::uint32_t right = x0 + 240u;
        std::memcpy(dst + std::size_t{right} * 3u, src + std::size_t{right} * 3u,
                    std::size_t{width - right} * 3u);
    }
    // A palette effect started after the save (the Psynergy tint, a fade):
    // move every held margin colour by the change of the palette entry it
    // was drawn from, so the margins take the same tint as the picture.
    // Only a whole-palette effect, though: menus load their own colours into
    // a few banks (Status 0, 1 and 14; Djinn 0, 1, 4-7 and 14), and
    // following those turned the held margins into noise
    // (logs/gpu_rewind_0062/0063, 2026-10-01). The Psynergy tint changed
    // every bank but 15, the windows' own (logs/gpu_frame_0102.bin).
    const auto& pal = g_gpu_field_capture.pal;
    if (pal.size() < 512) return;
    int delta[256][3];
    std::array<bool, 16> bank_used{}, bank_changed{};
    for (int i = 0; i < 256; ++i) {
        const std::uint16_t live =
            static_cast<std::uint16_t>(pal[i * 2] | (pal[i * 2 + 1] << 8));
        int a[3], b[3];
        world_margin_rgb_of(g_world_margin_pal[i], a);
        world_margin_rgb_of(live, b);
        for (int k = 0; k < 3; ++k) delta[i][k] = b[k] - a[k];
        if (g_world_margin_pal[i] != 0u) bank_used[i / 16] = true;
        if (live != g_world_margin_pal[i]) bank_changed[i / 16] = true;
    }
    bool whole_palette = false;
    for (int bank = 0; bank < 15; ++bank) {
        if (!bank_used[bank]) continue;
        if (!bank_changed[bank]) return;  // a menu's colours: keep the margins
        whole_palette = true;
    }
    if (!whole_palette) return;
    if (g_world_margin_index.empty()) world_margin_index_colours(width, height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const bool native_row = y >= y0 && y < y0 + 160u;
        std::uint8_t* row = rgb + std::size_t{y} * width * 3u;
        for (std::uint32_t x = 0; x < width; ++x) {
            if (native_row && x == x0) { x = x0 + 239u; continue; }
            std::uint8_t* p = row + std::size_t{x} * 3u;
            const std::uint32_t key = (std::uint32_t{p[0]} << 16) |
                                      (std::uint32_t{p[1]} << 8) | p[2];
            const auto it = g_world_margin_index.find(key);
            if (it == g_world_margin_index.end()) continue;
            for (int k = 0; k < 3; ++k)
                p[k] = static_cast<std::uint8_t>(
                    std::clamp(p[k] + delta[it->second][k], 0, 255));
        }
    }
}

void native_page_frame_end(bool at_capture);
void native_page_patch_capture(std::vector<std::uint8_t>& vram,
                               std::vector<std::uint8_t>& oam,
                               gsr::FieldScene& scene);
void auto_capture_check_sprites(const gsr::FieldScene& scene,
                                const std::uint8_t* oam);

void gpu_field_capture_hook() {
    // Page two of Settings, before anything copies this frame (see there).
    native_page_frame_end(true);
    if (!gpu_field_enabled()) return;
    // Snapshot the latest completed effect copy, including fading history.
    g_gpu_field_capture.effects = gsr::effect_capture_take();
    gba::GbaBus* bus = gbarecomp::active_bus();
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!bus || !ppu) return;

    // "Only with the graphics card": stop the console compositor drawing.
    // Done here rather than at start-up because this is the first place that
    // holds the PPU, and it costs one bool test per frame afterwards. The
    // per-row register recording in render_scanline is NOT affected -- the
    // capture below depends on it.
    if (gpu_field_only_enabled()) {
        static bool applied = false;
        if (!applied) {
            applied = true;
            ppu->set_native_raster_enabled(false);
            std::fprintf(stderr,
                "[gsr] console compositor off: the graphics card draws every "
                "frame, refused frames go magenta\n");
        }
    }

    // The registers this frame was drawn with. The end-of-frame read can
    // already hold the next frame's: the battle flash on the world map
    // alternates a mode 2 frame with a white band drawn in mode 0 and a
    // plain mode 2 frame, and on the plain ones DISPCNT already said mode 0
    // at VBlank, so the whole frame was drawn as mode 0, solid white
    // (gpu_rewind_0087, frames 1683-1697). When every row agrees on a
    // display mode the end-of-frame read does not, the rows are the truth:
    // take the first row's registers.
    std::vector<std::uint8_t> frame_io(bus->io().raw(),
                                       bus->io().raw() + gba::GbaIo::kIoSize);
    {
        const std::uint8_t* rows = ppu->latched_native_line_io();
        const bool* rows_valid = ppu->latched_native_line_io_valid();
        int first = -1;
        bool agree = true;
        for (int y = 0; rows && rows_valid && y < gsr::FieldScene::kRows; ++y) {
            if (!rows_valid[y]) continue;
            const std::uint8_t* row = rows + std::size_t(y) * gsr::FieldScene::kLineIoBytes;
            if (first < 0) first = y;
            else if ((row[0] & 7u) != (rows[std::size_t(first) * gsr::FieldScene::kLineIoBytes] & 7u))
                agree = false;
        }
        if (first >= 0 && agree) {
            const std::uint8_t* row = rows + std::size_t(first) * gsr::FieldScene::kLineIoBytes;
            if ((row[0] & 7u) != (frame_io[0] & 7u)) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    std::fprintf(stderr,
                        "[gsr] frame drawn in display mode %u while the "
                        "end-of-frame registers say %u: using the rows' "
                        "registers (frame %llu)\n", row[0] & 7u,
                        frame_io[0] & 7u,
                        static_cast<unsigned long long>(ppu->frame_count()));
                }
                std::memcpy(frame_io.data(), row, gsr::FieldScene::kLineIoBytes);
            }
        }
    }

    gsr::FieldScene scene{};
    gsr::field_scene_capture(
        &scene, ppu->frame_count(), frame_io.data(), bus->oam_ptr(),
        bus->pal_ptr(), ppu->latched_native_line_io(),
        ppu->latched_native_line_io_valid(),
        ppu->latched_native_affine_line_refs(),
        ppu->latched_native_affine_line_ref_valid());
    if (!scene.valid || !scene.supported) {
        // Leaves any previous capture in place, but that is moot: a frame
        // this returns false for is never presented anyway, since
        // gpu_field_present_override() below checks `valid` again and
        // g_gpu_field_capture.scene.frame no longer matches.
        g_gpu_field_capture.valid = false;
        return;
    }
    // Before native_page_patch_capture hides page one's icons on purpose.
    auto_capture_check_sprites(scene, bus->oam_ptr());
    g_gpu_field_capture.scene = scene;
    g_gpu_field_capture.vram.assign(bus->vram_ptr(), bus->vram_ptr() + 96 * 1024);
    build_room_vram(g_gpu_field_capture.vram, scene.palette,
                    &g_gpu_field_capture.room_vram,
                    &g_gpu_field_capture.room_palette);
    g_gpu_field_capture.ewram.assign(bus->ewram_ptr(),
                                     bus->ewram_ptr() + 256 * 1024);
    g_gpu_field_capture.iwram.assign(bus->iwram_ptr(),
                                     bus->iwram_ptr() + 32 * 1024);
    g_gpu_field_capture.rom = bus->rom_ptr();
    g_gpu_field_capture.rom_bytes = bus->rom_size();
    std::uint32_t effect_canvas = 0;
    std::memcpy(&effect_canvas, g_gpu_field_capture.iwram.data() + 0x1EF0, 4);
    if (g_gpu_field_capture.effects.canvas != effect_canvas)
        g_gpu_field_capture.effects = {};
    g_gpu_field_capture.io = frame_io;
    g_gpu_field_capture.oam.assign(bus->oam_ptr(), bus->oam_ptr() + 1024);
    // Settings page two where the repaint above could not reach (see there).
    native_page_patch_capture(g_gpu_field_capture.vram, g_gpu_field_capture.oam,
                              g_gpu_field_capture.scene);
    g_gpu_field_capture.pal.assign(bus->pal_ptr(), bus->pal_ptr() + 1024);
    g_gpu_field_capture.window_open = game_window_open();
    g_gpu_field_capture.valid = true;
}

// F12: dump the just-drawn GPU field frame to logs/gpu_frame_<NNNN>.bin,
// once per press. F9 was the first choice and was wrong: host_window.cpp
// binds F2..F10 to the nine save-state slots (plain = load, Shift = save),
// so pressing it loaded slot 8 out from under the player mid-battle. F11
// and F12 are the only function keys that nothing else claims -- F1 is the
// rebind menu, and the rest are slots. This exists because tools/scene_renderer_check.cpp's recorded
// corpus holds only one whole-frame register read per snapshot, while the
// console re-reads registers every scanline -- a per-scanline fault can only
// be told apart from a whole-frame one by capturing what the console itself
// saw, live, row by row. Deliberately not a launcher checkbox: it is a
// one-shot action, not a setting (see the project's "debug options belong in
// the launcher" rule, which is about persistent toggles, not a keypress that
// does one thing once). The poll below is a single GetAsyncKeyState call per
// drawn frame, so it costs nothing until actually pressed.
//
// File layout matches tools/scene_renderer_check.cpp's GSRSNAP1 reader
// exactly (map_recorder.cpp's write_snapshot_bin header comment), so that
// tool's load_snapshot can read this format too once it also accepts this
// magic: an 88-byte header (magic, then zero, then section count as u32 at
// offset 12, frame number as u64 at offset 16, the rest zero), then that many
// 24-byte directory entries (8-byte name, u64 offset, u64 length), then the
// section bytes themselves.
// Declared here rather than by including <windows.h>. This translation unit
// is eleven thousand lines of game code, SDL and generated headers, and it
// leans on std::min/std::max throughout; pulling the Win32 headers in at the
// top of it to reach one function is a way to lose a twenty-minute link to an
// include-order collision. user32 is already linked (gbarecomp/CMakeLists.txt
// links it PUBLIC), so the symbol resolves with nothing else added.
#if defined(_WIN32)
extern "C" short __stdcall GetAsyncKeyState(int virtual_key);
constexpr int kVirtualKeyDump = 0x7B;  // VK_F12
bool dump_key_down() {
    return (GetAsyncKeyState(kVirtualKeyDump) & 0x8000) != 0;
}
#else
// Linux: SDL's keyboard state, declared here for the same reason (SDL2's
// own header is not included in this file). SDL_SCANCODE_F12 is 69.
extern "C" const unsigned char* SDL_GetKeyboardState(int* numkeys);
bool dump_key_down() {
    const unsigned char* keys = SDL_GetKeyboardState(nullptr);
    return keys && keys[69] != 0;
}
#endif

// The GSRGPUF1 bytes of the current capture (layout above), shared by the
// single F12 dump and the rewind ring below.
std::string gpu_field_dump_bytes() {
    const gsr::FieldScene& scene = g_gpu_field_capture.scene;
    constexpr std::size_t kLineIoBytes = gsr::FieldScene::kLineIoBytes;
    constexpr int kRows = gsr::FieldScene::kRows;

    // LINEIO: row y's own registers where the console latched one, else the
    // frame's end-of-frame register file -- substituted here so the replay
    // tool never has to know the difference (field_scene.h's own contract
    // for row_io/row_io_valid/io_bytes).
    std::vector<std::uint8_t> line_io(kLineIoBytes * static_cast<std::size_t>(kRows));
    for (int y = 0; y < kRows; ++y) {
        const std::uint8_t* src = scene.row_io_valid[y]
            ? scene.row_io[y].data() : scene.io_bytes.data();
        std::memcpy(line_io.data() + static_cast<std::size_t>(y) * kLineIoBytes,
                   src, kLineIoBytes);
    }

    // AFFREF: row y's own latched BG2/BG3 reference point where the console
    // captured one, else the frame-level reference walked by PB/PD the same
    // way field_scene_renderer.cpp's own per-row affine upload does, so a
    // row this scene never latched still gets the value the renderer itself
    // would use for it.
    std::vector<std::int32_t> affine_refs(static_cast<std::size_t>(kRows) * 4u);
    for (int y = 0; y < kRows; ++y) {
        std::int32_t* r = affine_refs.data() + static_cast<std::size_t>(y) * 4u;
        if (scene.row_affine_valid[y]) {
            r[0] = scene.row_affine[y][0];
            r[1] = scene.row_affine[y][1];
            r[2] = scene.row_affine[y][2];
            r[3] = scene.row_affine[y][3];
        } else {
            for (int k = 0; k < 2; ++k) {
                const gsr::SceneLayer& layer = scene.layers[2 + k];
                r[k * 2 + 0] = layer.affine_ref_x + y * layer.affine_pb;
                r[k * 2 + 1] = layer.affine_ref_y + y * layer.affine_pd;
            }
        }
    }

    struct Section {
        const char* name;
        const std::uint8_t* data;
        std::uint64_t length;
    };
    const Section sections[] = {
        {"SCENE",  reinterpret_cast<const std::uint8_t*>(&scene), sizeof(scene)},
        {"IO",     g_gpu_field_capture.io.data(), g_gpu_field_capture.io.size()},
        {"OAM",    g_gpu_field_capture.oam.data(), g_gpu_field_capture.oam.size()},
        {"PAL",    g_gpu_field_capture.pal.data(), g_gpu_field_capture.pal.size()},
        {"VRAM",   g_gpu_field_capture.vram.data(), g_gpu_field_capture.vram.size()},
        {"EWRAM",  g_gpu_field_capture.ewram.data(), g_gpu_field_capture.ewram.size()},
        {"IWRAM",  g_gpu_field_capture.iwram.data(), g_gpu_field_capture.iwram.size()},
        {"LINEIO", line_io.data(), line_io.size()},
        {"AFFREF", reinterpret_cast<const std::uint8_t*>(affine_refs.data()),
                   affine_refs.size() * sizeof(std::int32_t)},
        {"FXSTAMP", reinterpret_cast<const std::uint8_t*>(
                        g_gpu_field_capture.effects.stamps.data()),
                    g_gpu_field_capture.effects.stamps.size() * sizeof(gsr::EffectSpark)},
        {"FXART", g_gpu_field_capture.effects.artwork.data(),
                  g_gpu_field_capture.effects.artwork.size()},
    };
    constexpr std::size_t kSectionCount = sizeof(sections) / sizeof(sections[0]);

    auto put_u32 = [](std::string& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    };
    auto put_u64 = [](std::string& out, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    };

    // Zero-initialized already covers "rest zero", including offset 8-11
    // (GSRSNAP1's version field, unused here).
    std::string header(88, '\0');
    std::memcpy(&header[0], "GSRGPUF1", 8);
    // offset 12: section count
    {
        std::string cnt;
        put_u32(cnt, static_cast<std::uint32_t>(kSectionCount));
        std::memcpy(&header[12], cnt.data(), 4);
    }
    // offset 16: frame number
    {
        std::string frm;
        put_u64(frm, scene.frame);
        std::memcpy(&header[16], frm.data(), 8);
    }

    std::string table;
    std::string data;
    std::uint64_t data_off = 88 + kSectionCount * 24u;
    for (const Section& s : sections) {
        std::string name_field(8, '\0');
        std::memcpy(&name_field[0], s.name,
                   std::min(std::strlen(s.name), std::size_t{8}));
        table += name_field;
        put_u64(table, data_off);
        put_u64(table, s.length);
        if (s.length) data.append(reinterpret_cast<const char*>(s.data), s.length);
        data_off += s.length;
    }

    std::string out;
    out.reserve(header.size() + table.size() + data.size());
    out += header;
    out += table;
    out += data;
    return out;
}

bool frame_rewind_enabled();

void gpu_field_write_dump_if_requested(const std::uint8_t* rgb,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       const gsr::FieldSceneRenderer& renderer) {
    // With rewind on, F12 belongs to frame_rewind_record() instead.
    if (frame_rewind_enabled()) return;
    static bool was_down = false;
    const bool down = dump_key_down();
    const bool pressed = down && !was_down;
    was_down = down;
    if (!pressed) return;

    std::error_code ec;
    std::filesystem::create_directories("logs", ec);

    std::string path;
    for (unsigned n = 1; n <= 9999; ++n) {
        char candidate[64];
        std::snprintf(candidate, sizeof candidate, "logs/gpu_frame_%04u.bin", n);
        if (!std::filesystem::exists(candidate, ec)) {
            path = candidate;
            break;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr,
            "[gsr] GPU field dump: logs/gpu_frame_0001..9999 all exist, not "
            "writing another\n");
        return;
    }

    renderer.log_effect_state();

    const std::string bytes = gpu_field_dump_bytes();
    const gsr::FieldScene& scene = g_gpu_field_capture.scene;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[gsr] GPU field dump: could not open %s\n",
                     path.c_str());
        return;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "[gsr] GPU field frame dumped to %s\n", path.c_str());

    // Save the exact RGB buffer handed to presentation, not a later replay.
    // PPM preserves its dimensions and pixels without another render/readback.
    const std::string image_path = path + ".live.ppm";
    std::FILE* image = std::fopen(image_path.c_str(), "wb");
    if (!image) {
        std::fprintf(stderr, "[gsr] GPU field dump: could not open %s\n",
                     image_path.c_str());
        return;
    }
    const bool header_ok =
        std::fprintf(image, "P6\n%u %u\n255\n", width, height) > 0;
    const std::size_t image_bytes = static_cast<std::size_t>(width) * height * 3u;
    const bool pixels_ok = std::fwrite(rgb, 1, image_bytes, image) == image_bytes;
    const bool close_ok = std::fclose(image) == 0;
    std::fprintf(stderr, "[gsr] GPU field live image %s: %s (%ux%u, frame %llu)\n",
                 header_ok && pixels_ok && close_ok ? "saved" : "write failed",
                 image_path.c_str(), width, height,
                 static_cast<unsigned long long>(scene.frame));
}

// ---- Rewind F12 (launcher "Rewind F12", GSR_FRAME_REWIND) ---------------
//
// A one-frame flicker is gone before F12 lands, so with this on every
// presented frame is kept in a ring: the exact picture shown (after the
// settings page and battle ">>" are painted) and, when the graphics card had
// a capture, that frame's GSRGPUF1 bytes for tools/scene_renderer_check.
// F12 writes the whole ring, oldest first, to logs/gpu_rewind_NNNN/ with an
// index.csv. About 0.7 MB a frame; 120 frames is 2 s at 60 frames a second
// (a starting size, not a measured one).
bool auto_capture_enabled();

bool frame_rewind_enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("GSR_FRAME_REWIND");
        return (e != nullptr && e[0] != '\0' && e[0] != '0') ||
               auto_capture_enabled();
    }();
    return enabled;
}

struct RewindFrame {
    unsigned long long guest_frame = 0;
    unsigned long long scene_frame = 0;
    bool gpu_drawn = false;
    bool battle_2x = false;
    std::uint32_t width = 0, height = 0;
    std::string scene;               // empty when there was no capture
    std::vector<std::uint8_t> rgb;
};
constexpr std::size_t kRewindFrames = 120;
std::vector<RewindFrame> g_rewind;
// Guest frame until which "BUG REPORT SAVED" shows (paint_bug_report_notice).
unsigned long long g_bug_report_notice_until = 0;
// The notice's text: F12's, or the auto-capture's.
const char* g_bug_report_notice_text = "BUG REPORT SAVED";
std::size_t g_rewind_next = 0;
std::size_t g_rewind_count = 0;

// `note` (the auto-capture's findings) goes to auto_capture.txt beside the
// frames; F12 passes none.
void frame_rewind_write(const std::string& note = {}) {
    std::error_code ec;
    std::string dir;
    for (unsigned n = 1; n <= 9999; ++n) {
        char candidate[64];
        std::snprintf(candidate, sizeof candidate, "logs/gpu_rewind_%04u", n);
        if (!std::filesystem::exists(candidate, ec)) {
            dir = candidate;
            break;
        }
    }
    if (dir.empty() || !std::filesystem::create_directories(dir, ec)) {
        std::fprintf(stderr, "[gsr] rewind: could not create a logs/gpu_rewind_ "
                             "directory\n");
        return;
    }
    std::FILE* index = std::fopen((dir + "/index.csv").c_str(), "wb");
    if (index)
        std::fprintf(index, "order,guest_frame,scene_frame,gpu_drawn,"
                            "battle_2x,has_scene,file\n");
    std::size_t written = 0;
    for (std::size_t i = 0; i < g_rewind_count; ++i) {
        const RewindFrame& fr = g_rewind[(g_rewind_next + kRewindFrames -
                                          g_rewind_count + i) % kRewindFrames];
        char name[64];
        std::snprintf(name, sizeof name, "frame_%03zu.bin", i);
        const std::string path = dir + "/" + name;
        if (!fr.scene.empty()) {
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(fr.scene.data(), 1, fr.scene.size(), f);
                std::fclose(f);
            }
        }
        if (std::FILE* f = std::fopen((path + ".live.ppm").c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", fr.width, fr.height);
            std::fwrite(fr.rgb.data(), 1, fr.rgb.size(), f);
            std::fclose(f);
            ++written;
        }
        if (index)
            std::fprintf(index, "%zu,%llu,%llu,%d,%d,%d,%s\n", i,
                         fr.guest_frame, fr.scene_frame, fr.gpu_drawn ? 1 : 0,
                         fr.battle_2x ? 1 : 0, fr.scene.empty() ? 0 : 1, name);
    }
    if (index) std::fclose(index);
    if (!note.empty()) {
        if (std::FILE* f = std::fopen((dir + "/auto_capture.txt").c_str(), "wb")) {
            std::fwrite(note.data(), 1, note.size(), f);
            std::fclose(f);
        }
    }
    std::fprintf(stderr, "[gsr] rewind: %zu frames written to %s\n", written,
                 dir.c_str());
    // Three seconds of confirmation for the player (a release has this on
    // for bug reports; the launcher packs the folder when the game closes).
    g_bug_report_notice_until = runtime_current_frame() + 180u;
    g_bug_report_notice_text = note.empty() ? "BUG REPORT SAVED" : "GLITCH SAVED";
}

// ---- Auto-capture glitches (launcher "Auto-capture glitches",
// GSR_AUTO_CAPTURE) ------------------------------------------------------------
//
// Saves the rewind ring by itself when one of three checks fires, each
// modelled on a glitch that was only caught by luck with F12:
//   blink   -- part of the picture changes for one or two frames, then is
//              exactly as before (settings page two's page-one flash, 2
//              frames, gpu_rewind_0059; the "< Wide >" tips, 1 frame,
//              bug_report_20261001_114130);
//   sprite  -- the handheld would show a sprite on screen, but we draw it
//              nowhere (the bouncing party member placed at y = 252,
//              gpu_rewind_0061);
//   jump    -- a sprite drawn on the far side of the view for a frame or
//              two while the handheld moved it smoothly (Vale at the top
//              edge for one frame, gpu_rewind_0065);
//   refused -- the graphics card refused a frame in "only with the graphics
//              card" mode, which paints it magenta.
// Each check starts a capture only on the first frame of a run of firings.
// The frames are written kAutoCaptureAfter frames later so the capture also
// shows what followed; findings from that wait go into the same note.
bool auto_capture_enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("GSR_AUTO_CAPTURE");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return enabled;
}

enum class AutoCaptureCheck { Blink, Sprite, Jump, Refused, Count };

struct AutoCaptureState {
    unsigned long long write_at = 0;    // guest frame; 0 = nothing pending
    unsigned long long last_write = 0;
    unsigned saved = 0;
    std::string note;
    std::array<unsigned long long,
               static_cast<std::size_t>(AutoCaptureCheck::Count)>
        last_fired{};
};
AutoCaptureState g_auto_capture;
// Disk budget, not measured: a capture is about 85 MB.
constexpr unsigned kAutoCaptureMax = 10;
constexpr unsigned long long kAutoCaptureGap = 300;   // frames between captures
constexpr unsigned long long kAutoCaptureAfter = 30;  // frames kept after

void auto_capture_trigger(AutoCaptureCheck check, const std::string& detail) {
    if (!auto_capture_enabled()) return;
    static const char* const kNames[] = {"blink", "sprite", "jump", "refused"};
    const char* name = kNames[static_cast<std::size_t>(check)];
    const unsigned long long now = runtime_current_frame();
    unsigned long long& last = g_auto_capture.last_fired[static_cast<std::size_t>(check)];
    const bool run_start = last == 0 || now > last + 1u;
    last = now;
    char head[96];
    std::snprintf(head, sizeof head, "frame %llu %s: ", now, name);
    if (g_auto_capture.write_at != 0) {
        if (g_auto_capture.note.size() < 8000u)
            g_auto_capture.note += head + detail + "\n";
        return;
    }
    if (!run_start || g_auto_capture.saved >= kAutoCaptureMax) return;
    if (g_auto_capture.last_write != 0 &&
        now < g_auto_capture.last_write + kAutoCaptureGap) return;
    g_auto_capture.write_at = now + kAutoCaptureAfter;
    g_auto_capture.note = head + detail + "\n";
    std::fprintf(stderr, "[auto-capture] %s%s\n", head, detail.c_str());
}

// Called after each frame is added to the ring.
void auto_capture_write_if_due() {
    if (g_auto_capture.write_at == 0 ||
        runtime_current_frame() < g_auto_capture.write_at) return;
    g_auto_capture.write_at = 0;
    g_auto_capture.last_write = runtime_current_frame();
    ++g_auto_capture.saved;
    frame_rewind_write(g_auto_capture.note);
    std::fprintf(stderr, "[auto-capture] saved %u of %u\n", g_auto_capture.saved,
                 kAutoCaptureMax);
}

// Blink: an 8x8 block of the shown picture that was the same for two
// frames, then different for one or two, then back to exactly the same for
// two. Needing it steady before and after keeps animations that change
// every frame (screen shakes) from counting. Only in display mode 0 (field
// and menus) and only where no sprite was in the last six frames: replayed
// over the saved captures, every real blink (gpu_rewind_0059, both bug
// reports of 2026-10-01) was background text in mode 0, while sprite
// animation (the cursor hand, the world-map ship) and battle sparks
// (mode 1, gpu_rewind_0060) fired otherwise.
// Not while a game window opens or closes within the six frames: the game
// draws one or two in-between pictures then, and the handheld shows them
// too (Settings, Status, Djinn, a Yes/No prompt: gpu_rewind_0062-0064,
// 0068). And not for a faint change, 32 or less in every colour: a lamp's
// glow flickers by 8 (gpu_rewind_0069, 0072), every real blink by 255.
constexpr int kBlinkFaint = 32;
struct BlinkHistory {
    std::uint32_t width = 0, height = 0;
    std::array<std::vector<std::uint64_t>, 6> hashes;  // oldest .. newest
    std::array<std::vector<std::uint8_t>, 6> sprite;   // block under a sprite
    std::array<unsigned long long, 6> frames{};
    std::array<bool, 6> window{};                      // a game window open
    std::size_t count = 0;
};
BlinkHistory g_blink;

void auto_capture_check_blink(const std::uint8_t* rgb, std::uint32_t width,
                              std::uint32_t height) {
    if (!auto_capture_enabled()) return;
    const std::uint32_t bw = (width + 7u) / 8u, bh = (height + 7u) / 8u;
    if (width != g_blink.width || height != g_blink.height) {
        g_blink = BlinkHistory{};
        g_blink.width = width;
        g_blink.height = height;
    }
    std::rotate(g_blink.hashes.begin(), g_blink.hashes.begin() + 1,
                g_blink.hashes.end());
    std::rotate(g_blink.sprite.begin(), g_blink.sprite.begin() + 1,
                g_blink.sprite.end());
    std::rotate(g_blink.frames.begin(), g_blink.frames.begin() + 1,
                g_blink.frames.end());
    std::rotate(g_blink.window.begin(), g_blink.window.begin() + 1,
                g_blink.window.end());
    g_blink.window.back() = game_window_open_this_frame();
    std::vector<std::uint8_t>& covered = g_blink.sprite.back();
    covered.assign(std::size_t{bw} * bh, 0u);
    const bool scene_ok = g_gpu_field_capture.valid;
    const gsr::FieldScene& scene = g_gpu_field_capture.scene;
    if (scene_ok) {
        const int ox = static_cast<int>(g_golden_sun_wide_extra_left);
        const int oy = static_cast<int>(g_golden_sun_wide_extra_top);
        for (const gsr::SceneObject& o : scene.objects) {
            if (!o.present || o.window) continue;
            const int k = o.affine && o.double_size ? 2 : 1;
            const int x0 = std::max(0, o.x + ox), y0 = std::max(0, o.y + oy);
            const int x1 = std::min(static_cast<int>(width) - 1, o.x + ox + k * o.width - 1);
            const int y1 = std::min(static_cast<int>(height) - 1, o.y + oy + k * o.height - 1);
            for (int by = y0 / 8; by <= y1 / 8 && y0 <= y1; ++by)
                for (int bx = x0 / 8; bx <= x1 / 8 && x0 <= x1; ++bx)
                    covered[std::size_t(by) * bw + bx] = 1u;
        }
    }
    std::vector<std::uint64_t>& now = g_blink.hashes.back();
    now.assign(std::size_t{bw} * bh, 1469598103934665603ull);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = rgb + std::size_t{y} * width * 3u;
        std::uint64_t* block = now.data() + std::size_t{y / 8u} * bw;
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint64_t& h = block[x / 8u];
            for (int k = 0; k < 3; ++k) {
                h ^= row[x * 3u + k];
                h *= 1099511628211ull;
            }
        }
    }
    g_blink.frames.back() = runtime_current_frame();
    if (g_blink.count < g_blink.hashes.size()) ++g_blink.count;
    if (g_blink.count < g_blink.hashes.size()) return;
    if (!scene_ok || scene.video_mode != 0) return;
    // Six consecutive guest frames: under fast-forward only every few are
    // shown, and a screen shake or scrolling looks like a blink (a 1-pixel
    // shake every third frame, gpu_rewind_0075; the room's edge, 0076).
    for (std::size_t k = 1; k < g_blink.frames.size(); ++k)
        if (g_blink.frames[k] != g_blink.frames[k - 1] + 1u) return;
    if (std::count(g_blink.window.begin(), g_blink.window.end(),
                   g_blink.window.front()) != std::ptrdiff_t(g_blink.window.size()))
        return;
    // The pictures behind h[3] and h[4]: frame_rewind_record has just added
    // this frame to the ring, so h[5] is its newest entry.
    auto ring_rgb = [](std::size_t back) -> const RewindFrame& {
        return g_rewind[(g_rewind_next + kRewindFrames - 1 - back) % kRewindFrames];
    };
    const RewindFrame& odd = ring_rgb(2);
    const RewindFrame& steady = ring_rgb(1);
    if (odd.width != width || odd.height != height ||
        steady.width != width || steady.height != height) return;

    const auto& h = g_blink.hashes;
    const auto& s = g_blink.sprite;
    std::uint32_t x0 = bw, y0 = bh, x1 = 0, y1 = 0, blocks = 0;
    int length = 0;
    for (std::size_t b = 0; b < now.size(); ++b) {
        if (s[0][b] | s[1][b] | s[2][b] | s[3][b] | s[4][b] | s[5][b]) continue;
        int blip = 0;
        // One frame (h[3]) between two steady pairs.
        if (h[1][b] == h[2][b] && h[2][b] == h[4][b] && h[4][b] == h[5][b] &&
            h[3][b] != h[2][b])
            blip = 1;
        // Two frames (h[2], h[3]) between two steady pairs.
        else if (h[0][b] == h[1][b] && h[1][b] == h[4][b] &&
                 h[4][b] == h[5][b] && h[2][b] != h[1][b] &&
                 h[3][b] != h[1][b])
            blip = 2;
        if (!blip) continue;
        const std::uint32_t px = static_cast<std::uint32_t>(b % bw) * 8u;
        const std::uint32_t py = static_cast<std::uint32_t>(b / bw) * 8u;
        int change = 0;
        for (std::uint32_t y = py; y < std::min(py + 8u, height); ++y) {
            const std::size_t row = (std::size_t{y} * width + px) * 3u;
            const std::size_t n = std::size_t{std::min(px + 8u, width) - px} * 3u;
            for (std::size_t k = 0; k < n; ++k)
                change = std::max(change, std::abs(int(odd.rgb[row + k]) -
                                                   int(steady.rgb[row + k])));
        }
        if (change <= kBlinkFaint) continue;
        length = std::max(length, blip);
        ++blocks;
        const std::uint32_t bx = static_cast<std::uint32_t>(b % bw);
        const std::uint32_t by = static_cast<std::uint32_t>(b / bw);
        x0 = std::min(x0, bx); y0 = std::min(y0, by);
        x1 = std::max(x1, bx); y1 = std::max(y1, by);
    }
    if (!blocks) return;
    char detail[192];
    std::snprintf(detail, sizeof detail,
                  "%u block(s) of 8x8 in x %u-%u, y %u-%u of the %ux%u picture "
                  "changed for %d frame(s) (guest frames %llu-%llu) and came "
                  "back exactly",
                  blocks, x0 * 8u, x1 * 8u + 7u, y0 * 8u, y1 * 8u + 7u,
                  width, height, length,
                  g_blink.frames[length == 1 ? 3 : 2], g_blink.frames[3]);
    auto_capture_trigger(AutoCaptureCheck::Blink, detail);
}

// Sprite: every object the handheld would show inside its 240x160 screen
// (its hardware position, Y read as 8-bit wrapping and X as 9-bit signed)
// must be drawn somewhere in our view. Run on the captured scene before
// settings page two hides page one's icons on purpose.
// Two exceptions, from the 2026-10-01 run where all five firings were the
// handheld's own wrap: the middle of the sprite's picture (half its width
// and height) must be on the handheld's screen, since a box that only
// grazes the edge shows its empty border (gpu_rewind_0066, 0067, 0070,
// 0071); and a sprite placed by the game's own coordinates, track or actor
// table, with no menu open, is where the game put it -- the handheld draws
// a town that left the bottom at the top instead (gpu_rewind_0065 frame
// 2431), and a Kalay sprite 130 pixels above the view at its bottom
// (gpu_rewind_0074). The menu bug this check was made for still fires
// (gpu_rewind_0061-0063).
void auto_capture_check_sprites(const gsr::FieldScene& scene,
                                const std::uint8_t* oam) {
    if (!auto_capture_enabled() || !oam) return;
    const int view_left = -static_cast<int>(g_golden_sun_wide_extra_left);
    const int view_top = -static_cast<int>(g_golden_sun_wide_extra_top);
    const int view_right = 240 + static_cast<int>(g_golden_sun_wide_extra_right);
    const int view_bottom = 160 + static_cast<int>(g_golden_sun_wide_extra_bottom);
    const bool menu_open = game_window_open_this_frame();
    for (int i = 0; i < gsr::FieldScene::kObjects; ++i) {
        const gsr::SceneObject& o = scene.objects[i];
        if (!o.present || o.window) continue;
        const unsigned a0 = oam[i * 8] | (oam[i * 8 + 1] << 8);
        const unsigned a1 = oam[i * 8 + 2] | (oam[i * 8 + 3] << 8);
        const int raw_y = static_cast<int>(a0 & 0xFFu);
        const int raw_x = static_cast<int>(a1 & 0x1FFu);
        const int hw_y = raw_y >= 160 ? raw_y - 256 : raw_y;
        const int hw_x = raw_x >= 256 ? raw_x - 512 : raw_x;
        const int w = o.affine && o.double_size ? o.width * 2 : o.width;
        const int h = o.affine && o.double_size ? o.height * 2 : o.height;
        const int mid_x = hw_x + w / 2, mid_y = hw_y + h / 2;
        if (mid_x - o.width / 4 >= 240 || mid_x + o.width / 4 <= 0 ||
            mid_y - o.height / 4 >= 160 || mid_y + o.height / 4 <= 0)
            continue;  // the handheld shows at most its empty border
        if (o.trusted && !menu_open) continue;
        // The renderer keeps these inside the console's screen
        // (field_scene_renderer.cpp, u_clip_native).
        const bool native_only =
            o.parked || (menu_open && scene.video_mode == 0 && o.priority == 0);
        const int left = native_only ? 0 : view_left;
        const int top = native_only ? 0 : view_top;
        const int right = native_only ? 240 : view_right;
        const int bottom = native_only ? 160 : view_bottom;
        if (o.x < right && o.x + w > left && o.y < bottom && o.y + h > top)
            continue;
        char detail[192];
        std::snprintf(detail, sizeof detail,
                      "slot %d (%dx%d, tile 0x%03X) is on the handheld's screen "
                      "at (%d, %d) but drawn at (%d, %d), outside our view",
                      i, w, h, o.tile, hw_x, hw_y, o.x, o.y);
        auto_capture_trigger(AutoCaptureCheck::Sprite, detail);
    }
}

// Jump: a sprite at one place, then 128 or more pixels away vertically (256
// horizontally) for one or two shown frames, then back within 24 pixels of
// where it was -- while its hardware coordinates moved smoothly. That is our
// reading of the console's wrapped coordinates flipping sides, not the game
// moving it. Same size, shape and tile in every frame, so a slot handed to
// another sprite does not count. Replayed over every saved capture
// (gpu_rewind_0001-0072, both bug reports of 2026-10-01) it fired once:
// Vale at the top edge for one frame, gpu_rewind_0065 frame 2428.
struct JumpObject {
    bool on = false;
    int x = 0, y = 0, raw_x = 0, raw_y = 0, w = 0, h = 0;
    unsigned tile = 0;
    bool affine = false, double_size = false;
    bool same_sprite(const JumpObject& o) const {
        return on && o.on && w == o.w && h == o.h && tile == o.tile &&
               affine == o.affine && double_size == o.double_size;
    }
};
struct JumpHistory {
    // Shown frames, oldest .. newest.
    std::array<std::array<JumpObject, gsr::FieldScene::kObjects>, 4> frames;
    std::size_t count = 0;
    std::uint64_t scene_frame = UINT64_MAX;
};
JumpHistory g_jump;
constexpr int kJumpNear = 24;

void auto_capture_check_jumps() {
    if (!auto_capture_enabled()) return;
    if (!g_gpu_field_capture.valid || g_gpu_field_capture.oam.size() < 1024u) {
        g_jump.count = 0;
        return;
    }
    const gsr::FieldScene& scene = g_gpu_field_capture.scene;
    if (scene.frame == g_jump.scene_frame) return;  // the same picture again
    g_jump.scene_frame = scene.frame;
    std::rotate(g_jump.frames.begin(), g_jump.frames.begin() + 1,
                g_jump.frames.end());
    const std::uint8_t* oam = g_gpu_field_capture.oam.data();
    for (int i = 0; i < gsr::FieldScene::kObjects; ++i) {
        const gsr::SceneObject& o = scene.objects[i];
        JumpObject& j = g_jump.frames.back()[i];
        j = {};
        if (!o.present || o.window || o.parked) continue;
        j.on = true;
        j.x = o.x;
        j.y = o.y;
        j.raw_y = oam[i * 8];
        j.raw_x = (oam[i * 8 + 2] | (oam[i * 8 + 3] << 8)) & 0x1FF;
        j.w = o.width;
        j.h = o.height;
        j.tile = o.tile;
        j.affine = o.affine;
        j.double_size = o.double_size;
    }
    if (g_jump.count < g_jump.frames.size()) ++g_jump.count;
    auto wrapped = [](int d, int range) {
        return ((d + range / 2) % range + range) % range - range / 2;
    };
    auto in_view = [](const JumpObject& o) {
        const int k = o.affine && o.double_size ? 2 : 1;
        return o.x < 240 + static_cast<int>(g_golden_sun_wide_extra_right) &&
               o.x + k * o.w > -static_cast<int>(g_golden_sun_wide_extra_left) &&
               o.y < 160 + static_cast<int>(g_golden_sun_wide_extra_bottom) &&
               o.y + k * o.h > -static_cast<int>(g_golden_sun_wide_extra_top);
    };
    for (std::size_t span = 1; span <= 2; ++span) {
        if (g_jump.count < span + 2) continue;
        const auto& first = g_jump.frames[g_jump.frames.size() - span - 2];
        const auto& last = g_jump.frames.back();
        for (int i = 0; i < gsr::FieldScene::kObjects; ++i) {
            const JumpObject& a = first[i];
            const JumpObject& c = last[i];
            if (!a.same_sprite(c) || std::abs(a.x - c.x) > kJumpNear ||
                std::abs(a.y - c.y) > kJumpNear) continue;
            const int mx = (a.x + c.x) / 2, my = (a.y + c.y) / 2;
            bool jumped = true;
            bool seen = in_view(a) || in_view(c);
            for (std::size_t m = g_jump.frames.size() - span - 1;
                 m < g_jump.frames.size() - 1 && jumped; ++m) {
                const JumpObject& b = g_jump.frames[m][i];
                jumped = a.same_sprite(b) &&
                    (std::abs(b.y - my) >= 128 || std::abs(b.x - mx) >= 256) &&
                    std::abs(wrapped(b.raw_y - a.raw_y, 256)) <= 2 * kJumpNear &&
                    std::abs(wrapped(b.raw_x - a.raw_x, 512)) <= 2 * kJumpNear;
                seen = seen || in_view(b);
            }
            // Both places outside our view: nothing to see (a shadow at
            // (356, 191) read as (353, -65) for two frames, gpu_rewind_0082).
            if (!jumped || !seen) continue;
            const JumpObject& b = g_jump.frames[g_jump.frames.size() - span - 1][i];
            char detail[224];
            std::snprintf(detail, sizeof detail,
                          "slot %d (%dx%d, tile 0x%03X) was at (%d, %d), drawn "
                          "at (%d, %d) for %zu shown frame(s), then back at "
                          "(%d, %d); the handheld moved it smoothly",
                          i, a.w, a.h, a.tile, a.x, a.y, b.x, b.y, span, c.x, c.y);
            auto_capture_trigger(AutoCaptureCheck::Jump, detail);
        }
    }
}

// "BUG REPORT SAVED" at the top right, in a small built-in font (the game's
// menu font is only read once a menu has opened), white on a dark box.
void paint_bug_report_notice(std::uint8_t* rgb, std::uint32_t width,
                             std::uint32_t height) {
    struct Glyph { char c; const char* rows[7]; };
    static const Glyph kGlyphs[] = {
        {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
        {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
        {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
        {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
        {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
        {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
        {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
        {'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}},
        {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
        {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
        {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
        {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
        {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
        {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
        {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
        {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    };
    const char* const kText = g_bug_report_notice_text;
    const int text_w = static_cast<int>(std::strlen(kText)) * 6 - 1;
    const int box_w = text_w + 8, box_h = 7 + 8;
    const int x0 = static_cast<int>(width) - box_w - 4, y0 = 4;
    auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
            y >= static_cast<int>(height)) return;
        std::uint8_t* p = rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
        p[0] = r; p[1] = g; p[2] = b;
    };
    for (int y = 0; y < box_h; ++y)
        for (int x = 0; x < box_w; ++x) {
            const bool edge = x == 0 || y == 0 || x == box_w - 1 || y == box_h - 1;
            if (edge) put(x0 + x, y0 + y, 200, 160, 255);
            else put(x0 + x, y0 + y, 24, 14, 40);
        }
    int cx = x0 + 4;
    for (const char* s = kText; *s; ++s, cx += 6) {
        for (const Glyph& g : kGlyphs) {
            if (g.c != *s) continue;
            for (int ry = 0; ry < 7; ++ry)
                for (int rx = 0; rx < 5; ++rx)
                    if (g.rows[ry][rx] == '#') put(cx + rx, y0 + 4 + ry, 255, 255, 255);
        }
    }
}

// Called with the final picture of every presented frame.
void frame_rewind_record(const std::uint8_t* rgb, std::uint32_t width,
                         std::uint32_t height, bool gpu_drawn,
                         bool battle_2x) {
    if (!frame_rewind_enabled()) return;
    if (g_rewind.empty()) {
        g_rewind.resize(kRewindFrames);
        std::fprintf(stderr, "[gsr] rewind: keeping the last %zu frames; F12 "
                             "saves them\n", kRewindFrames);
    }
    RewindFrame& fr = g_rewind[g_rewind_next];
    fr.guest_frame = runtime_current_frame();
    fr.gpu_drawn = gpu_drawn;
    fr.battle_2x = battle_2x;
    fr.width = width;
    fr.height = height;
    fr.rgb.assign(rgb, rgb + static_cast<std::size_t>(width) * height * 3u);
    if (g_gpu_field_capture.valid) {
        fr.scene = gpu_field_dump_bytes();
        fr.scene_frame = g_gpu_field_capture.scene.frame;
    } else {
        fr.scene.clear();
        fr.scene_frame = 0;
    }
    g_rewind_next = (g_rewind_next + 1) % kRewindFrames;
    g_rewind_count = std::min(g_rewind_count + 1, kRewindFrames);

    auto_capture_check_blink(rgb, width, height);
    auto_capture_check_jumps();
    auto_capture_write_if_due();

    static bool was_down = false;
    const bool down = dump_key_down();
    const bool pressed = down && !was_down;
    was_down = down;
    if (pressed) frame_rewind_write();
}

// gbarecomp::set_frame_present_override_hook's callback. Draws the frame
// gpu_field_capture_hook() just captured and reads the finished picture back
// into `rgb` -- a full GPU round trip every drawn frame, accepted for this
// first run so SDL's own presentation, the overlay and the rest of the
// present path stay untouched (docs/NATIVE_SCENE_RENDERER_PLAN.md step 5).
// Returns false (leaving `rgb` exactly as the emulated hardware path already
// filled it) whenever the toggle is off, nothing usable was captured this
// frame, the graphics card cannot run this path at all, or the renderer
// declines this particular frame.
// Paint the whole frame flat magenta -- a colour Golden Sun's palettes do
// not produce, so it can never be mistaken for a drawing fault. Returns true
// because the frame HAS been filled: the point is that the emulated picture
// underneath is not shown.
bool gpu_field_paint_refused(std::uint8_t* rgb, std::uint32_t width,
                             std::uint32_t height, const char* reason) {
    const std::size_t pixels =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < pixels; ++i) {
        rgb[i * 3 + 0] = 0xFF;
        rgb[i * 3 + 1] = 0x00;
        rgb[i * 3 + 2] = 0xFF;
    }
    // Every refusal is counted, and the tally is printed each time the count
    // doubles -- 1, 2, 4, 8, 16 ... One line per reason hides a fault that
    // fires on one frame in a thousand; a line per frame drowns the log.
    static const char* last = nullptr;
    static unsigned long long count = 0;
    if (reason != last) { last = reason; count = 0; }
    ++count;
    // Magenta only shows when the console compositor is off.
    if (gpu_field_only_enabled())
        auto_capture_trigger(AutoCaptureCheck::Refused,
                             reason ? reason : "no reason given");
    if ((count & (count - 1)) == 0) {
        std::fprintf(stderr,
            "[gsr] GPU field refused %llu frame(s) in a row: %s\n",
            static_cast<unsigned long long>(count),
            reason ? reason : "no reason given");
    }
    return true;
}

// Better Field Psy (settings page two), defined with the other options below.
extern std::atomic<int> g_field_psynergy_fast;

bool gpu_field_present_override(std::uint8_t* rgb, std::uint32_t width,
                                std::uint32_t height) {
    if (!gpu_field_enabled()) return false;
    if (!g_gpu_field_capture.valid) {
        if (gpu_field_only_enabled())
            return gpu_field_paint_refused(rgb, width, height,
                                           "nothing was captured this frame");
        return false;
    }

    // Lazily created the first time this is actually needed, from present
    // time, where the window's GL context is the one already current --
    // never from gpu_field_capture_hook() above. gpu_surface.h attaches to
    // that existing context rather than creating one of its own.
    static gbarecomp::GpuSurface surface;
    static gsr::FieldSceneRenderer renderer;
    static bool init_attempted = false;
    static bool init_ok = false;
    if (!init_attempted) {
        init_attempted = true;
        init_ok = surface.init() && renderer.init(&surface);
        if (init_ok) {
            // Room-sourced backgrounds always come on with this toggle
            // (there is no separate control for it); see field_scene.h's
            // header comment on why BG0, the lighting layer, never uses it
            // regardless.
            renderer.set_room_source_enabled(true);
            // Capture spell stamp calls only when the launcher enables it.
            const char* effects = std::getenv("GSR_HOST_EFFECTS");
            const bool host_effects =
                effects != nullptr && effects[0] != '\0' && effects[0] != '0';
            renderer.set_effect_renderer_enabled(host_effects);
            gsr::effect_capture_set_enabled(host_effects);
        } else {
            std::fprintf(stderr,
                "[gsr] GPU field renderer unavailable (%s); staying on the "
                "emulated hardware path\n",
                !surface.ready() ? surface.failure() : renderer.failure());
        }
    }
    if (!init_ok) {
        if (gpu_field_only_enabled())
            return gpu_field_paint_refused(rgb, width, height,
                                           "the renderer could not start");
        return false;
    }

    if (!renderer.set_output_size(static_cast<int>(width),
                                  static_cast<int>(height))) {
        if (gpu_field_only_enabled())
            return gpu_field_paint_refused(rgb, width, height,
                                           "the output size was refused");
        return false;
    }
    renderer.upload_vram(g_gpu_field_capture.vram.data(),
                         g_gpu_field_capture.vram.size());
    if (!g_gpu_field_capture.room_vram.empty())
        renderer.upload_room_vram(g_gpu_field_capture.room_vram.data(),
                                  g_gpu_field_capture.room_vram.size());
    renderer.upload_palette(g_gpu_field_capture.scene.palette.data(),
                            g_gpu_field_capture.scene.palette.size());
    if (!g_gpu_field_capture.room_palette.empty())
        renderer.upload_room_palette(g_gpu_field_capture.room_palette.data(),
                                     g_gpu_field_capture.room_palette.size());
    renderer.upload_room(g_gpu_field_capture.ewram.data(),
                         g_gpu_field_capture.ewram.size());
    // The world map beyond the game's own 512-pixel ring, for the margins
    // (FACTS.md, 2026-09-25). Checked against the pieces the game loaded
    // every frame; off whenever that check fails or this is not the world map.
    {
        static gsr::WorldMapSource world_map;
        const auto& cap = g_gpu_field_capture;
        if (world_map.update(cap.ewram.data(), cap.ewram.size(),
                             cap.iwram.data(), cap.iwram.size(),
                             cap.rom, cap.rom_bytes)) {
            g_world_map_source_ok = true;
            renderer.upload_world_map(world_map.tiles().data(),
                                      world_map.generation());
        } else {
            g_world_map_source_ok = false;
            renderer.upload_world_map(nullptr, 0);
        }
    }
    // The effect layer is the affine background in a battle; its canvas base
    // and size come from its own BGCNT, never a remembered address, because
    // the game moves both between spells (FACTS.md, 2026-09-18).
    {
        // Which layer carries the spell effect this frame: a battle's affine,
        // non-wrapping BG2, read from the scene's own registers rather than a
        // remembered address, because the game moves it between spells.
        const gsr::FieldScene& sc = g_gpu_field_capture.scene;

        int effect_layer = -1;
        if (sc.video_mode == 1) {
            const gsr::SceneLayer& bg2 = sc.layers[2];
            // The stretched canvas (non-wrapping BG2), else a layer whose map
            // shows the canvas as a tile block: a wrapping BG2 (Ray,
            // gpu_rewind_0046) or BG1 (hit sparks, gpu_rewind_0047).
            if (bg2.enabled && bg2.affine && !bg2.wraps)
                effect_layer = 2;
            else
                effect_layer = gsr::FieldSceneRenderer::find_effect_canvas_layer(
                    g_gpu_field_capture.vram.data(),
                    g_gpu_field_capture.vram.size(), sc);
        }
        // Clear last frame's overlay before submitting this frame's sparks.
        renderer.set_effect_layer(effect_layer);

        // Add captured stamps only beyond the game's canvas; its original
        // image stays intact. CPU stamp counts do not prove GPU visibility.
        if (gsr::effect_capture_enabled()) {
            const auto& captured = g_gpu_field_capture.effects;
            const auto& asked = captured.stamps;
            gsr::move_probe_on_sparks(sc.frame, effect_layer, asked,
                                      effect_layer >= 0 ? &sc.layers[effect_layer] : nullptr);
            if (effect_layer >= 0) {
                // With no sparks this still reads the canvas's tint fill.
                if (!asked.empty())
                    renderer.upload_effect_sparks(
                        asked.data(), static_cast<int>(asked.size()),
                        captured.artwork.data(), captured.artwork.size(),
                        effect_layer);
                renderer.analyse_effect_canvas(g_gpu_field_capture.vram.data(),
                                               g_gpu_field_capture.vram.size(),
                                               sc.layers[effect_layer]);
            }
            gsr::earth_surge_on_present(renderer, sc.frame, effect_layer, asked,
                                        g_gpu_field_capture.oam.data(),
                                        g_gpu_field_capture.oam.size());
            // Report once, on the first frame that actually captured
            // something -- reporting on the first battle frame printed all
            // zeros because no stamp call had happened yet
            // (session_20260918_201759). A run that captures nothing at all
            // still gets a report from the frame counter below.
            static int battle_frames = 0;
            if (effect_layer >= 0) ++battle_frames;
            static bool reported = false;
            if (!reported && (!asked.empty() || battle_frames > 600)) {
                reported = true;
                char line[256];
                gsr::effect_capture_report(line, sizeof line);
                std::fprintf(stderr, "[gsr] host effects: %s\n", line);
            }
        }
    }
    renderer.set_menu_open(g_gpu_field_capture.window_open);
    renderer.set_reveal_full(g_field_psynergy_fast.load() != 0);
    if (!renderer.draw(g_gpu_field_capture.scene)) {
        // Say once why the card declined. Without this the log cannot tell a
        // path that never ran from one that ran and handed every frame back.
        static const char* reported = nullptr;
        const char* reason = renderer.declined_reason();
        if (reason && reason != reported) {
            reported = reason;
            std::fprintf(stderr, "[gsr] GPU field declined a frame: %s\n",
                         reason);
        }
        if (gpu_field_only_enabled())
            return gpu_field_paint_refused(rgb, width, height, reason);
        return false;
    }
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[gsr] GPU field renderer drawing at %ux%u\n",
                     width, height);
    }

    surface.read_texture_rgb(renderer.output_texture(),
                             static_cast<int>(width), static_cast<int>(height),
                             rgb);
    // A decode the console's own picture disagrees with was not drawn, so
    // it must not be held either (the pause menu's first frame,
    // logs/gpu_rewind_0058 frame 14).
    g_world_map_source_ok = g_world_map_source_ok && renderer.world_map_used();
    world_map_margin_hold(rgb, width, height);

    // Polled only once the frame is safely drawn and read back, so a dump
    // never races the draw it is describing.
    gpu_field_write_dump_if_requested(rgb, width, height, renderer);
    return true;
}

// True only while the player has Message speed on its fourth choice, so the
// in-game option still chooses between Slow, Normal, Fast and this.
bool message_speed_is_instant() {
    if (const gba::GbaBus* bus = gbarecomp::active_bus()) {
        if (const std::uint8_t* ewram = bus->ewram_ptr()) {
            return ewram[(gsr::text_speed_cheat::kMessageSpeedAddress -
                          0x02000000u) & 0x0003FFFFu] ==
                   gsr::text_speed_cheat::kMessageSpeedInstant;
        }
    }
    return false;
}

// ---- Settings page two (src/settings_page.h) ----------------------------
//
// Written on the emulation thread by the settings loop's key-word loads and
// read at present time, so everything the painter needs is copied here first:
// it never reads guest memory itself.
struct SettingsPageFont {
    std::uint8_t tiles[256 * 32] = {};
    std::uint8_t widths[256 - 0x20] = {};
};
SettingsPageFont g_settings_font;
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
// Window x | y << 8 | width << 16 | height << 24, in tiles.
std::atomic<std::uint32_t> g_settings_window{0};
std::atomic<std::uint32_t> g_settings_help_window{0};
std::atomic<int> g_settings_speed{2};
std::atomic<int> g_settings_view_mode{0};
std::atomic<int> g_settings_no_slowdown{0};
// Encounters: Normal / Half / Off, loaded from kOptionsFile at startup and
// saved there on each change.
std::atomic<int> g_encounter_rate{0};
bool g_encounter_skip_next = false;
// Battle speed-up (src/battle_speed.h), toggled with Select in battle; saved
// with the other options.
std::atomic<bool> g_battle_speed_on{false};
// Better Field Psy: Off / On (src/battle_speed.h), saved with the other
// options.
std::atomic<int> g_field_psynergy_fast{0};
// Cheat menu multipliers (src/cheat_menu.h), step indices, saved with the
// other options.
std::atomic<int> g_cheat_exp_step{gsr::cheat_menu::kNormalStep};
std::atomic<int> g_cheat_coin_step{gsr::cheat_menu::kNormalStep};
std::atomic<int> g_cheat_drop_step{gsr::cheat_menu::kNormalStep};
std::string g_options_path = gsr::settings_page::kOptionsFile;

void load_game_options(const char* argv0) {
    using namespace gsr::settings_page;
    if (argv0 && *argv0) {
        const std::filesystem::path exe(argv0);
        if (exe.has_parent_path())
            g_options_path = (exe.parent_path() / kOptionsFile).string();
    }
    FILE* f = std::fopen(g_options_path.c_str(), "r");
    if (!f) return;
    char line[128];
    const std::size_t encounter_length = std::strlen(kEncounterKey);
    const char* const battle_key = gsr::battle_speed::kOptionsKey;
    const std::size_t battle_length = std::strlen(battle_key);
    const std::size_t field_psy_length = std::strlen(kFieldPsynergyKey);
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, kEncounterKey, encounter_length) == 0) {
            const int value = std::atoi(line + encounter_length);
            if (value >= kEncounterNormal && value < kEncounterRateCount)
                g_encounter_rate.store(value);
        } else if (std::strncmp(line, battle_key, battle_length) == 0) {
            g_battle_speed_on.store(std::atoi(line + battle_length) != 0);
        } else if (std::strncmp(line, kFieldPsynergyKey, field_psy_length) == 0) {
            g_field_psynergy_fast.store(
                std::atoi(line + field_psy_length) != 0 ? 1 : 0);
        } else {
            using namespace gsr::cheat_menu;
            const std::pair<const char*, std::atomic<int>*> steps[] = {
                {kExpKey, &g_cheat_exp_step},
                {kCoinKey, &g_cheat_coin_step},
                {kDropKey, &g_cheat_drop_step}};
            for (const auto& [key, step] : steps) {
                const std::size_t n = std::strlen(key);
                if (std::strncmp(line, key, n) != 0) continue;
                const int value = std::atoi(line + n);
                const int count = step == &g_cheat_drop_step ? kDropStepCount
                                                             : kStepCount;
                if (value >= 0 && value < count) step->store(value);
            }
        }
    }
    std::fclose(f);
}

void save_game_options() {
    FILE* f = std::fopen(g_options_path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "[gsr] could not write %s\n",
                     g_options_path.c_str());
        return;
    }
    std::fprintf(f, "%s%d\n", gsr::settings_page::kEncounterKey,
                 g_encounter_rate.load());
    std::fprintf(f, "%s%d\n", gsr::battle_speed::kOptionsKey,
                 g_battle_speed_on.load() ? 1 : 0);
    std::fprintf(f, "%s%d\n", gsr::settings_page::kFieldPsynergyKey,
                 g_field_psynergy_fast.load() ? 1 : 0);
    std::fprintf(f, "%s%d\n", gsr::cheat_menu::kExpKey,
                 g_cheat_exp_step.load());
    std::fprintf(f, "%s%d\n", gsr::cheat_menu::kCoinKey,
                 g_cheat_coin_step.load());
    std::fprintf(f, "%s%d\n", gsr::cheat_menu::kDropKey,
                 g_cheat_drop_step.load());
    std::fclose(f);
}
// The game's cursor row (r9), which is also the page-two row.
std::atomic<int> g_settings_row{0};

// Emulation thread only: copies the menu font out of the ROM once.
void load_menu_font() {
    using namespace gsr::settings_page;
    if (g_settings_font_loaded.load(std::memory_order_acquire)) return;
    const std::uint32_t font = bus_read_u32(kAssetTable + kFontAsset * 4u);
    for (std::uint32_t i = 0; i < sizeof g_settings_font.tiles; ++i)
        g_settings_font.tiles[i] = bus_read_u8(font + i);
    for (std::uint32_t i = 0; i < sizeof g_settings_font.widths; ++i)
        g_settings_font.widths[i] = bus_read_u8(kGlyphWidthTable + i);
    g_settings_font_loaded.store(true, std::memory_order_release);
}

void note_settings_loop_frame() {
    using namespace gsr::settings_page;
    load_menu_font();
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
        // Two modes: either direction toggles. The host applies it on its
        // next pump and reports it back through runtime_get_view_mode.
        const int mode = runtime_get_view_mode() == 0 ? 1 : 0;
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
    *out_value = kZeroWord;
    return 1;
}

// True while the settings loop is running and not closing: the page
// marker is drawn on either page, page two's contents only on page two.
bool settings_overlay_visible() {
    return g_settings_open.load() && !g_settings_closing.load() &&
           g_settings_font_loaded.load(std::memory_order_acquire);
}

void paint_settings_window(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height, std::uint32_t window,
                           bool settings, bool page_two, std::uint8_t* letter,
                           std::uint8_t* shadow);

// Page one: only the page marker on the help line. Page two: covers the
// settings window's interior (keeping its first column, where the game's
// cursor hand sits) and the help window's, and draws page two. All in the
// game's own font.
void paint_settings_page(std::uint8_t* rgb, std::uint32_t width,
                         std::uint32_t height) {
    const bool page_two = g_settings_page.load() == 2;
    std::uint8_t letter[3] = {0xFF, 0xFF, 0xFF};
    std::uint8_t shadow[3] = {0x00, 0x00, 0x00};
    paint_settings_window(rgb, width, height, g_settings_window.load(), true,
                          page_two, letter, shadow);
    paint_settings_window(rgb, width, height, g_settings_help_window.load(),
                          false, page_two, letter, shadow);
}

// One window: the settings window (page two's row) or the help window above.
void paint_settings_window(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height, std::uint32_t window,
                           bool settings, bool page_two, std::uint8_t* letter,
                           std::uint8_t* shadow) {
    using namespace gsr::settings_page;
    const int wx = static_cast<int>(window & 0xFFu);
    const int wy = static_cast<int>((window >> 8) & 0xFFu);
    const int ww = static_cast<int>((window >> 16) & 0xFFu);
    const int wh = static_cast<int>(window >> 24);
    const int ox = static_cast<int>(g_golden_sun_wide_extra_left);
    const int oy = static_cast<int>(g_golden_sun_wide_extra_top);
    auto pixel = [&](int x, int y) -> std::uint8_t* {
        x += ox;
        y += oy;
        if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
            y >= static_cast<int>(height)) return nullptr;
        return rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
    };
    const int left = (wx + 1) * 8;
    const int top = (wy + 1) * 8;
    const int right = (wx + ww - 1) * 8;
    const int bottom = (wy + wh - 1) * 8;
    // The settings window keeps its first column, where the cursor hand is.
    const int fill_left = settings ? left + 8 : left;
    if (right <= fill_left || bottom <= top) return;

    auto glyph = [&](unsigned char code, int gx, int gy) {
        const std::uint8_t b =
            g_settings_font.tiles[code * 32u + gy * 4 + gx / 2];
        return static_cast<std::uint8_t>((b >> (4 * (gx & 1))) & 0xFu);
    };
    // Letter and shadow colours from page one's first label letter, read
    // before anything is painted over it; the help window reuses them.
    bool have_letter = !settings, have_shadow = !settings;
    for (int gy = 0; gy < 8 && settings; ++gy) {
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
    if (!settings) return;

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
    const int view_mode = g_settings_view_mode.load() == 0 ? 0 : 1;
    int encounters = g_encounter_rate.load();
    if (encounters < kEncounterNormal || encounters >= kEncounterRateCount)
        encounters = kEncounterNormal;
    const int no_slowdown = g_settings_no_slowdown.load() ? 1 : 0;
    const int field_psynergy = g_field_psynergy_fast.load() ? 1 : 0;
    const int speed = walk_speed_index(g_settings_speed.load());
    const char* const words[kRowCount] = {kWalkingSpeedWords[speed],
                                          kEncounterWords[encounters],
                                          kScreenWords[view_mode],
                                          kOnOffWords[no_slowdown],
                                          kFieldPsynergyWords[field_psynergy]};
    const int cursor_row = g_settings_row.load();
    for (int row = 0; row < kRowCount; ++row) {
        const int y = kRowLineY[row];
        draw(kRowLabels[row], kLabelX, y);
        const std::string value = row == cursor_row
            ? std::string("< ") + words[row] + " >" : std::string(words[row]);
        draw(value, kValueCentreX - text_width(value) / 2, y);
    }
    return canvas;
}

// Captures where the game had drawn page one's text over page two.
unsigned g_native_page_capture_logs = 0;

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
void native_page_frame_end(bool at_capture = false) {
    using namespace gsr::settings_page;
    const bool open = g_settings_open.load() && !g_settings_closing.load() &&
                      g_settings_font_loaded.load(std::memory_order_acquire);
    // Why page two was not kept, while it should show (bounded): the fix
    // above still let page one through in a player build
    // (bug_report_20260930_212742, rewind 2 frames 22-23), and the capture
    // there shows the game's page-one text untouched.
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
    // (bug_report_20261001_100121, frames 1033/1072/1129); that used to skip
    // the text repaint above too. Sprites wait for the next frame.
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

// ---- Battle speed-up (src/battle_speed.h) ------------------------------
bool g_battle_toggle_held = false;
bool g_battle_field_toggle_held = false;
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
// A cast is in progress on exactly the frames in which the game entered
// Func_96f50 or Func_96c80. `active` is whether the last completed frame saw
// either entry: no hold, no debounce. There is no per-frame hook, so the
// frame is closed by the first function entry of the next frame or by the
// frame wait's mask load, whichever comes first. Emulation thread only.
struct FieldPsynergyState {
    unsigned long long frame = 0;
    bool started = false;
    bool seen = false;    // an entry was seen in `frame`
    bool active = false;  // an entry was seen in the completed frame before
    bool running = false; // the field path is holding 2x (for the log)
};
FieldPsynergyState g_field_psy;

void field_psynergy_roll_frame() {
    FieldPsynergyState& s = g_field_psy;
    const unsigned long long frame = runtime_current_frame();
    if (s.started && frame == s.frame) return;
    // A gap of more than one frame (or a savestate load) had no entries.
    s.active = s.started && frame == s.frame + 1 && s.seen;
    s.started = true;
    s.frame = frame;
    s.seen = false;
}

void field_psynergy_on_entry(std::uint32_t entry_pc) {
    field_psynergy_roll_frame();
    if (entry_pc == gsr::battle_speed::kFieldPsynergyEntryA ||
        entry_pc == gsr::battle_speed::kFieldPsynergyEntryB)
        g_field_psy.seen = true;
}

// ---- Battle BG1 write recorder (launcher "Record battle BG1 writes") ----
//
// Battle speed 2x loses some BG1 updates: at battle entry a tile with black
// lines stays on BG1, after a hit only part of BG1's picture arrives
// (gpu_frame_0065..0069, 2026-09-23). This records, per guest frame in
// battle, which code writes BG1's tiles, BG1's map and the BG palette (CPU
// stores and DMA transfers, from the engine's video write observer), with
// the 2x state and which frame of a 2x pair it was. Payload-free: PCs,
// addresses and counts only. One CSV per session in logs/, at most
// kBattleBg1MaxRows rows (a size cap, not a tuned value).
struct BattleBg1Key {
    std::uint32_t pc, source;
    int channel;
    int kind;  // 0 BG1 tiles, 1 BG1 map, 2 BG palette
    bool operator<(const BattleBg1Key& o) const {
        return std::tie(pc, source, channel, kind) <
               std::tie(o.pc, o.source, o.channel, o.kind);
    }
};
struct BattleBg1Totals {
    std::uint32_t writes = 0, bytes = 0, first = 0xFFFFFFFFu, last = 0;
};
constexpr std::size_t kBattleBg1MaxRows = 500000;
std::FILE* g_bg1_file = nullptr;
std::size_t g_bg1_rows = 0;
unsigned long long g_bg1_frame = ~0ull;
bool g_bg1_half = false;
std::map<BattleBg1Key, BattleBg1Totals> g_bg1_frame_writes;

bool battle_bg1_record_enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("GSR_BATTLE_BG1_RECORD");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return enabled;
}

void battle_bg1_flush_frame() {
    if (!g_bg1_file || g_bg1_frame == ~0ull || g_bg1_frame_writes.empty()) {
        g_bg1_frame_writes.clear();
        return;
    }
    static const char* const kKinds[3] = {"bg1_tiles", "bg1_map", "bg_palette"};
    for (const auto& [key, total] : g_bg1_frame_writes) {
        if (g_bg1_rows >= kBattleBg1MaxRows) break;
        std::fprintf(g_bg1_file,
                     "%llu,%d,%d,%s,0x%08X,%d,0x%08X,%u,%u,0x%08X,0x%08X\n",
                     g_bg1_frame, g_battle_speed_on.load() ? 1 : 0,
                     g_bg1_half ? 1 : 0, kKinds[key.kind], key.pc,
                     key.channel, key.source, total.writes, total.bytes,
                     total.first, total.last);
        ++g_bg1_rows;
    }
    g_bg1_frame_writes.clear();
}

void battle_bg1_video_write(std::uint32_t pc, std::uint32_t address,
                            std::uint32_t bytes, std::uint32_t source,
                            int channel) {
    if (bus_read_u32(gsr::battle_speed::kBattleHeapSlot) == 0u) return;
    int kind = -1;
    if (address >= 0x05000000u && address < 0x05000200u) {
        kind = 2;
    } else if (address >= 0x06000000u && address < 0x06010000u) {
        const std::uint16_t cnt = bus_read_u16(0x0400000Au);
        const std::uint32_t chars = 0x06000000u + ((cnt >> 2) & 3u) * 0x4000u;
        const std::uint32_t chars_end =
            std::min(chars + ((cnt & 0x80u) ? 0x10000u : 0x8000u), 0x06010000u);
        const std::uint32_t map = 0x06000000u + ((cnt >> 8) & 31u) * 0x800u;
        static constexpr std::uint32_t kMapBytes[4] = {0x800u, 0x1000u, 0x1000u,
                                                       0x2000u};
        const std::uint32_t map_end = map + kMapBytes[(cnt >> 14) & 3u];
        const std::uint32_t end = address + (bytes ? bytes : 1u);
        if (address < map_end && end > map) kind = 1;
        else if (address < chars_end && end > chars) kind = 0;
    }
    if (kind < 0) return;
    const unsigned long long frame = runtime_current_frame();
    if (frame != g_bg1_frame) {
        battle_bg1_flush_frame();
        g_bg1_frame = frame;
    }
    g_bg1_half = g_battle_skip_this_frame;
    auto& total = g_bg1_frame_writes[{pc, source, channel, kind}];
    ++total.writes;
    total.bytes += bytes;
    total.first = std::min(total.first, address);
    total.last = std::max(total.last, address + bytes);
}

void battle_bg1_record_init() {
    if (!battle_bg1_record_enabled()) return;
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    for (unsigned n = 1; n <= 9999 && !g_bg1_file; ++n) {
        char path[64];
        std::snprintf(path, sizeof path, "logs/battle_bg1_%04u.csv", n);
        if (std::filesystem::exists(path, ec)) continue;
        g_bg1_file = std::fopen(path, "wb");
        if (g_bg1_file)
            std::fprintf(stderr, "[gsr] battle BG1 recorder: %s\n", path);
    }
    if (!g_bg1_file) return;
    std::fprintf(g_bg1_file, "frame,battle_2x,second_of_pair,kind,pc,"
                             "dma_channel,dma_source,writes,bytes,first,end\n");
    gba::vram_trace::set_video_write_observer(battle_bg1_video_write);
    std::atexit([] {
        battle_bg1_flush_frame();
        if (g_bg1_file) std::fclose(g_bg1_file);
        g_bg1_file = nullptr;
    });
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
// Last frame the out-of-battle toggle's notice shows.
std::atomic<unsigned long long> g_battle_speed_notice_until{0};
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

// True while battle work paced by the display is running (the entry, a hit
// fade); 2x then runs one pass per VBlank (src/battle_speed.h).
bool battle_speed_display_paced() {
    using namespace gsr::battle_speed;
    if (bus_read_u32(kVCountHandlerSlot) == kEntryVCountHandler) return true;
    static std::uint32_t logged_mode = ~0u, logged_param = ~0u;
    const std::uint32_t block = bus_read_u32(kEffectCanvasBlockSlot);
    std::uint32_t mode = ~0u, param = ~0u;
    if (block >= 0x02000000u &&
        block < 0x02040000u - kEffectCanvasParamOffset - 4u) {
        mode = bus_read_u32(block + kEffectCanvasModeOffset);
        param = bus_read_u32(block + kEffectCanvasParamOffset);
    }
    static unsigned long long fade_start = 0;
    // A spell's canvas passes through a drawing mode (1 or 3) before its
    // fade; a regular attack's never does. Reset when the block goes away.
    static bool spell_canvas = false;
    if (mode == ~0u) spell_canvas = false;
    if (mode == kEffectCanvasSpellModeA || mode == kEffectCanvasSpellModeB)
        spell_canvas = true;
    const bool halving =
        mode == kEffectCanvasFadeMode && param == kEffectCanvasHalveParam;
    const unsigned long long frame = runtime_current_frame();
    if (halving && (mode != logged_mode || param != logged_param))
        fade_start = frame;
    const bool paced = halving && !spell_canvas &&
                       frame - fade_start < kMaxPacedFadeFrames;
    if (mode != logged_mode || param != logged_param) {
        logged_mode = mode;
        logged_param = param;
        if (mode == ~0u) {
            std::fprintf(stderr, "[battle-speed] effect canvas none frame=%llu\n",
                         static_cast<unsigned long long>(runtime_current_frame()));
        } else {
            std::fprintf(stderr,
                         "[battle-speed] effect canvas mode=%u param=%u "
                         "paced=%d frame=%llu\n",
                         mode, param, paced ? 1 : 0,
                         static_cast<unsigned long long>(runtime_current_frame()));
        }
    }
    return paced;
}

// The wait's mask load, once per frame routine pass: reads Select, and on
// every other battle frame keeps the VBlank flag and arms the Halt drop.
int battle_speed_wait_mask(std::uint32_t* out_value) {
    using namespace gsr::battle_speed;
    const bool in_battle = bus_read_u32(kBattleHeapSlot) != 0u;
    const std::uint16_t held = static_cast<std::uint16_t>(
        ~bus_read_u16(kKeyInput) & 0x03FFu);
    const bool toggle = (held & kKeySelect) != 0u;
    if (in_battle && toggle && !g_battle_toggle_held) {
        g_battle_speed_on.store(!g_battle_speed_on.load());
        save_game_options();
    }
    g_battle_toggle_held = toggle;
    const bool field_toggle =
        (held & kFieldToggleKeys) == kFieldToggleKeys;
    if (!in_battle && field_toggle && !g_battle_field_toggle_held) {
        g_battle_speed_on.store(!g_battle_speed_on.load());
        save_game_options();
        load_menu_font();
        g_battle_speed_notice_until.store(runtime_current_frame() +
                                          kNoticeFrames);
    }
    g_battle_field_toggle_held = field_toggle;
    field_psynergy_roll_frame();
    const bool battle_2x = in_battle && g_battle_speed_on.load();
    // A field Psynergy cast at Fast: the same 2x, outside battle only.
    const bool field_2x = !in_battle && g_field_psynergy_fast.load() != 0 &&
                          g_field_psy.active;
    if (field_2x != g_field_psy.running) {
        g_field_psy.running = field_2x;
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
    const unsigned long long frame = runtime_current_frame();
    if (frame <= g_battle_speed_notice_until.load()) return true;
    return g_battle_speed_on.load() &&
           frame <= g_battle_speed_seen_frame.load() + 1u;
}

// ">>" (or "1x" for the off notice) in the menu font, white with a dark
// shadow, at the native top left.
void paint_battle_speed_indicator(std::uint8_t* rgb, std::uint32_t width,
                                  std::uint32_t height) {
    using namespace gsr::battle_speed;
    const char* const text =
        g_battle_speed_on.load() ? kIndicator : kIndicatorOff;
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

// ---- Cheat menu (src/cheat_menu.h) ------------------------------------------
// Emulation thread only: the hotkey, the key filter, the pause poll and the
// paused overlay all run from the run loop's input pump and present. The
// steps are atomics because the reward hooks read them from generated code
// on the same thread; nothing else touches them. The steps themselves are
// defined with the other saved options (g_cheat_exp_step and friends).
bool g_cheat_menu_open = false;
bool g_cheat_menu_requested_pause = false;
int g_cheat_menu_row = 0;
std::uint16_t g_cheat_menu_held = 0;
bool g_cheat_menu_swallow = false;
std::uint16_t g_cheat_window_palette[16] = {};
bool g_cheat_window_palette_known = false;

// New-game Hard Mode prompt (hard_mode.h): the cheat menu's window and key
// capture, opened from the entry to Func_77f40 instead of the hotkey.
bool g_hard_prompt_open = false;
int g_hard_prompt_row = 0;
std::uint16_t g_hard_prompt_held = 0;

void hard_mode_prompt_open() {
    load_menu_font();
    g_hard_prompt_open = true;
    g_hard_prompt_row = 0;
    g_hard_prompt_held = 0x3FFu;
}

void hard_mode_prompt_close(bool hard) {
    using namespace gsr::hard_mode;
    const std::uint8_t value = hard ? kHardModeValue : 0u;
    bus_write_u8(kSavedFlag, value);
    bus_write_u8(kAutoSleepFlag, value);
    g_hard_prompt_open = false;
    g_cheat_menu_swallow = true;
}

void cheat_menu_toggle() {
    if (g_hard_prompt_open) return;
    load_menu_font();
    g_cheat_menu_open = !g_cheat_menu_open;
    // Keys already held stay with whoever had them: nothing reaches the
    // menu until it is pressed again, and nothing reaches the game until
    // every key is let go after closing.
    g_cheat_menu_held = 0x3FFu;
    if (g_cheat_menu_open) {
        g_cheat_menu_row = 0;
    } else {
        g_cheat_menu_swallow = true;
        save_game_options();
    }
}

bool cheat_menu_pause_poll(bool* out_paused) {
    const bool want = g_cheat_menu_open || g_hard_prompt_open;
    if (want == g_cheat_menu_requested_pause) return false;
    g_cheat_menu_requested_pause = want;
    *out_paused = want;
    return true;
}

std::atomic<int>* cheat_menu_step(int row) {
    using namespace gsr::cheat_menu;
    if (row == kExpRow) return &g_cheat_exp_step;
    if (row == kCoinRow) return &g_cheat_coin_step;
    if (row == kDropRow) return &g_cheat_drop_step;
    return nullptr;
}

void cheat_menu_change(int row, int direction) {
    using namespace gsr::cheat_menu;
    if (row == kInfiniteHpRow) {
        runtime_set_infinite_hp(runtime_get_infinite_hp() ? 0 : 1);
    } else if (row == kInfinitePpRow) {
        runtime_set_infinite_pp(runtime_get_infinite_pp() ? 0 : 1);
    } else if (std::atomic<int>* step = cheat_menu_step(row)) {
        const int last = (row == kDropRow ? kDropStepCount : kStepCount) - 1;
        step->store(std::clamp(step->load() + direction, 0, last));
    }
}

std::uint16_t cheat_menu_keyinput_filter(std::uint16_t keyinput) {
    using gsr::settings_page::kKeyA;
    using gsr::settings_page::kKeyB;
    using gsr::settings_page::kKeyDown;
    using gsr::settings_page::kKeyLeft;
    using gsr::settings_page::kKeyRight;
    using gsr::settings_page::kKeyStart;
    using gsr::settings_page::kKeyUp;
    const std::uint16_t pressed = static_cast<std::uint16_t>(~keyinput & 0x3FFu);
    if (g_hard_prompt_open) {
        const std::uint16_t down =
            static_cast<std::uint16_t>(pressed & ~g_hard_prompt_held);
        g_hard_prompt_held = pressed;
        if (down & (kKeyUp | kKeyDown | kKeyLeft | kKeyRight))
            g_hard_prompt_row = 1 - g_hard_prompt_row;
        if (down & kKeyA) hard_mode_prompt_close(g_hard_prompt_row == 1);
        else if (down & kKeyB) hard_mode_prompt_close(false);
        return 0x3FFu;
    }
    if (!g_cheat_menu_open) {
        if (g_cheat_menu_swallow) {
            if (pressed != 0) return 0x3FFu;
            g_cheat_menu_swallow = false;
        }
        return keyinput;
    }
    const std::uint16_t down =
        static_cast<std::uint16_t>(pressed & ~g_cheat_menu_held);
    g_cheat_menu_held = pressed;
    constexpr int rows = gsr::cheat_menu::kRowCount;
    if (down & kKeyUp) g_cheat_menu_row = (g_cheat_menu_row + rows - 1) % rows;
    if (down & kKeyDown) g_cheat_menu_row = (g_cheat_menu_row + 1) % rows;
    if (down & kKeyLeft) cheat_menu_change(g_cheat_menu_row, -1);
    if (down & kKeyRight) cheat_menu_change(g_cheat_menu_row, +1);
    if ((down & kKeyA) && g_cheat_menu_row <= gsr::cheat_menu::kInfinitePpRow)
        cheat_menu_change(g_cheat_menu_row, +1);
    if (down & (kKeyB | kKeyStart)) cheat_menu_toggle();
    return 0x3FFu;
}

// The window, drawn over the paused picture in the game's own pieces and
// colours (src/cheat_menu.h).
// `content(draw_text, text_width, inner_x, inner_y)` paints inside it; shared
// by the cheat menu and the Hard Mode prompt.
template <typename Content>
void menu_window_paint(std::uint8_t* rgb, std::uint32_t width,
                       std::uint32_t height, Content content) {
    using namespace gsr::cheat_menu;
    if (!g_settings_font_loaded.load(std::memory_order_acquire)) return;
    // The live window palette when bank 15 holds it, else the last one
    // seen, else the measured default.
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
    auto put = [&](int x, int y, std::uint8_t index) {
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
    auto pixel_of = [&](unsigned char code, int gx, int gy) {
        const std::uint8_t b = g_settings_font.tiles[code * 32u + gy * 4 + gx / 2];
        return static_cast<std::uint8_t>((b >> (4 * (gx & 1))) & 0xFu);
    };
    for (int ty = 0; ty < kWindowRows; ++ty) {
        for (int tx = 0; tx < kWindowTiles; ++tx) {
            const bool top = ty == 0, bottom = ty == kWindowRows - 1;
            const bool left = tx == 0, right = tx == kWindowTiles - 1;
            const unsigned char code =
                top ? (left ? kTopLeft : right ? kTopRight : kTop)
                : bottom ? (left ? kBottomLeft : right ? kBottomRight : kBottom)
                : left ? kLeft : right ? kRight : kFill;
            for (int gy = 0; gy < 8; ++gy)
                for (int gx = 0; gx < 8; ++gx)
                    put((kWindowTileX + tx) * 8 + gx,
                        (kWindowTileY + ty) * 8 + gy, pixel_of(code, gx, gy));
        }
    }
    const int inner_x = (kWindowTileX + 1) * 8;
    const int inner_y = (kWindowTileY + 1) * 8;
    auto text_width = [&](const std::string& text) {
        int total = 0;
        for (const char c : text) {
            const auto code = static_cast<unsigned char>(c);
            if (code >= 0x20u) total += g_settings_font.widths[code - 0x20u];
        }
        return total;
    };
    // Letter and shadow pixels only; the window's own inside shows around
    // them, as it does behind the game's text.
    auto draw_text = [&](const std::string& text, int x, int y) {
        for (const char c : text) {
            const auto code = static_cast<unsigned char>(c);
            if (code < 0x20u) continue;
            for (int gy = 0; gy < 8; ++gy) {
                for (int gx = 0; gx < 8; ++gx) {
                    const std::uint8_t v = pixel_of(code, gx, gy);
                    if (v == gsr::settings_page::kGlyphLetter ||
                        v == gsr::settings_page::kGlyphShadow)
                        put(x + gx, y + gy, v);
                }
            }
            x += g_settings_font.widths[code - 0x20u];
        }
    };
    content(draw_text, text_width, inner_x, inner_y);
}

void cheat_menu_paint(std::uint8_t* rgb, std::uint32_t width,
                      std::uint32_t height) {
    using namespace gsr::cheat_menu;
    if (g_hard_prompt_open) {
        namespace hm = gsr::hard_mode;
        menu_window_paint(rgb, width, height,
                          [](auto& draw_text, auto& text_width, int inner_x,
                             int inner_y) {
            draw_text(hm::kPromptTitle, inner_x + (kWindowTiles - 2) * 4 -
                          text_width(hm::kPromptTitle) / 2, inner_y);
            for (int i = 0; i < 3; ++i)
                draw_text(hm::kPromptLines[i], inner_x + kLabelX,
                          inner_y + 14 + i * 12);
            for (int row = 0; row < 2; ++row) {
                std::string label = hm::kPromptRows[row];
                if (row == g_hard_prompt_row) label = "< " + label + " >";
                draw_text(label,
                          inner_x + kValueCentreX - text_width(label) / 2,
                          inner_y + 56 + row * kRowPitch);
            }
        });
        return;
    }
    if (!g_cheat_menu_open) return;
    menu_window_paint(rgb, width, height,
                      [](auto& draw_text, auto& text_width, int inner_x,
                         int inner_y) {
        draw_text(kTitle, inner_x + (kWindowTiles - 2) * 4 -
                              text_width(kTitle) / 2, inner_y);
        for (int row = 0; row < kRowCount; ++row) {
            const int y = inner_y + kRowLineY0 + row * kRowPitch;
            draw_text(kRowLabels[row], inner_x + kLabelX, y);
            std::string value;
            if (row == kInfiniteHpRow)
                value = runtime_get_infinite_hp() ? "On" : "Off";
            else if (row == kInfinitePpRow)
                value = runtime_get_infinite_pp() ? "On" : "Off";
            else
                value = kStepWords[cheat_menu_step(row)->load()];
            if (row == g_cheat_menu_row) value = "< " + value + " >";
            draw_text(value, inner_x + kValueCentreX - text_width(value) / 2, y);
        }
    });
}

// What one tally store adds, scaled by its multiplier. Returns -1 when `pc`
// is not one of the three stores.
int cheat_reward_write(std::uint32_t pc, std::uint32_t addr,
                       std::uint32_t requested, std::uint32_t width,
                       std::uint32_t* out_value) {
    using namespace gsr::cheat_menu;
    if (pc != kCoinBonusStorePc && pc != kCoinStorePc && pc != kExpStorePc)
        return -1;
    const int step = (pc == kExpStorePc ? g_cheat_exp_step
                                        : g_cheat_coin_step).load();
    if (!out_value || width != 4u || step == kNormalStep) return 0;
    const std::uint32_t before = bus_read_u32(addr);
    const std::uint64_t added = static_cast<std::uint32_t>(requested - before);
    const std::uint64_t scaled = std::min<std::uint64_t>(
        added * static_cast<std::uint64_t>(kStepHalves[step]) / 2u,
        0xFFFFFFFFull - before);
    *out_value = before + static_cast<std::uint32_t>(scaled);
    return 1;
}

// The drop roll's BLE (taken = no drop), decided on the scaled threshold.
int cheat_drop_branch(std::uint32_t pc, std::uint32_t original,
                      std::uint32_t* out_decision) {
    using namespace gsr::cheat_menu;
    if (pc != kDropBranchPc) return -1;
    const int step = g_cheat_drop_step.load();
    if (step == kNormalStep) return 0;
    const std::int64_t threshold =
        static_cast<std::int64_t>(static_cast<std::int32_t>(g_cpu.R[5])) *
        kStepHalves[step] / 2;
    const std::int64_t roll = static_cast<std::int32_t>(g_cpu.R[0]);
    *out_decision = threshold <= roll ? 1u : 0u;
    return *out_decision != original ? 1 : 0;
}

// Page two and the battle ">>" go over whatever picture was going to be
// shown, the GPU renderer's or the emulated hardware's.
bool golden_sun_present_override(std::uint8_t* rgb, std::uint32_t width,
                                 std::uint32_t height) {
    const bool drawn = gpu_field_present_override(rgb, width, height);
    const bool settings = settings_overlay_visible();
    const bool battle = battle_speed_indicator_visible();
    if (settings) paint_settings_page(rgb, width, height);
    if (battle) paint_battle_speed_indicator(rgb, width, height);
    frame_rewind_record(rgb, width, height, drawn, battle);
    // After the recording, so the notice is not in the saved frames.
    const bool notice = runtime_current_frame() <= g_bug_report_notice_until;
    if (notice) paint_bug_report_notice(rgb, width, height);
    return drawn || settings || battle || notice;
}

// Instant text is the settings screen's fourth Message-speed choice, always
// offered; the launcher's old GSR_INSTANT_TEXT checkbox no longer gates it.
// Only the reviewed sites in config/usa/main.toml reach these callbacks.
// The Message-speed caption the runner writes, set when the settings screen
// is about to draw it and emptied once written or by the next text drawn.
std::string g_speed_caption;
std::uint32_t g_speed_caption_id = 0;
std::size_t g_speed_caption_pos = 0;
bool g_speed_caption_armed = false;

std::uint32_t menu_text_width(const std::string& text) {
    std::uint32_t width = 0;
    for (const char c : text) {
        const auto code = static_cast<unsigned char>(c);
        if (code >= 0x20u)
            width += bus_read_u8(gsr::text_speed_cheat::kGlyphWidthTable +
                                 code - 0x20u);
    }
    return width;
}
// Message-speed icons still to be uploaded blank, armed by this row's icon
// table loads on the settings screen.
std::uint32_t g_blank_icons_pending = 0;
// The icon IDs of the table that armed it (Message speed's or Hard Mode's).
std::uint32_t g_blank_icon_first = 0;
std::uint32_t g_blank_icon_last = 0;

// Page two's help line, printed by the game: while page two shows, the help
// text it decodes (0xC15 + row, 0x0801E74C) is replaced character by
// character with the row's help, then the 0 that ends it.
int g_help_pos = -1;
std::uint32_t g_help_id = 0;

bool settings_help_character(std::uint32_t original, std::uint32_t* out_value) {
    using namespace gsr::settings_page;
    const std::uint32_t text_id = bus_read_u32(
        g_cpu.R[13] + gsr::text_speed_cheat::kDecoderTextIdStackOffset);
    const bool help = g_settings_page.load() == 2 &&
                      text_id >= kHelpTextBase &&
                      text_id < kHelpTextBase + static_cast<std::uint32_t>(kRowCount);
    if (!help) {
        g_help_pos = -1;
        return false;
    }
    if (g_help_pos < 0 || text_id != g_help_id) {
        g_help_pos = 0;
        g_help_id = text_id;
    }
    const std::string text = kRowHelp[text_id - kHelpTextBase];
    std::uint32_t wanted = 0u;
    if (static_cast<std::size_t>(g_help_pos) < text.size())
        wanted = static_cast<unsigned char>(text[g_help_pos++]);
    else
        g_help_pos = -1;
    *out_value = original + (wanted - g_cpu.R[0]);
    return true;
}

// Hard Mode's label and help line, written the same way while the settings
// screen is open. Page two's help is tried first and keeps 0xC19 while it
// shows.
int g_hard_text_pos = -1;
std::uint32_t g_hard_text_id = 0;

bool hard_mode_text_character(std::uint32_t original,
                              std::uint32_t* out_value) {
    using namespace gsr::hard_mode;
    const std::uint32_t text_id = bus_read_u32(
        g_cpu.R[13] + gsr::text_speed_cheat::kDecoderTextIdStackOffset);
    const char* text = text_id == kLabelTextId ? kLabelText
                     : text_id == kHelpTextId  ? kHelpText
                                               : nullptr;
    if (!g_settings_screen_text || !text) {
        g_hard_text_pos = -1;
        return false;
    }
    if (g_hard_text_pos < 0 || text_id != g_hard_text_id) {
        g_hard_text_pos = 0;
        g_hard_text_id = text_id;
    }
    std::uint32_t wanted = 0u;
    if (text[g_hard_text_pos] != '\0')
        wanted = static_cast<unsigned char>(text[g_hard_text_pos++]);
    else
        g_hard_text_pos = -1;
    *out_value = original + (wanted - g_cpu.R[0]);
    return true;
}

// The world-map overlay (rom_77a7c8, OvlFunc_598 at 0x02008598) switches each
// town and cave on or off by a box around the camera's focus: x within
// -160..+160 and z within -300..+200 world units, the flag at object + 0x54.
// Towns vanished at the old right edge because of it (FACTS.md, 2026-09-25).
// In the expanded view the x bounds and the lower z bound grow; the sprite
// routines after it still trim to the screen. Operands are the stock values:
// the ldr at 0x020085D2 (-160 << 16) and the movs at 0x020085D6 (160) and
// 0x020085E6 (200), each shifted left 16 by the next instruction.
// Returns -1 for any other site.
int golden_sun_worldmap_range_box(std::uint32_t pc, std::uint32_t original,
                                  std::uint32_t* out_value) {
    std::uint32_t grown = 0;
    if (pc == 0x020085D2u && original == 0xFF600000u) {
        grown = static_cast<std::uint32_t>(
            -(160 + 2 * static_cast<std::int32_t>(
                            g_golden_sun_wide_extra_left)) * 65536);
    } else if (pc == 0x020085D6u && original == 160u) {
        grown = 160u + 2u * g_golden_sun_wide_extra_right;
    } else if (pc == 0x020085E6u && original == 200u) {
        grown = 200u + g_golden_sun_wide_extra_bottom;
    } else {
        return -1;
    }
    if (!golden_sun_expanded_obj_view_active()) return 0;
    *out_value = grown;
    return 1;
}

// Field camera clamp, Func_10230 (ROM, FACTS.md 2026-10-01). S =
// [0x03001E70]; the camera's top-left is the focus + (-120, -96), clamped to
// [S+0xEC, S+0xF4 - 240] in x and [S+0xF0, S+0xF8 - 160] in y (16.16; the
// 240 and 160 are the literals at 0x08010404 and 0x08010408). Those bounds
// keep the console's 240x160 inside the room; the expanded view needs the
// margins inside too, so each side is pulled in by up to the margin. A room
// narrower than the view gets half the spare width per side, which centres
// it. Jimmy, 2026-10-01: the camera should not scroll past the map's edge.
constexpr std::uint32_t kCameraStructPointer = 0x03001E70u;
constexpr std::uint32_t kCameraMinXBranchPc = 0x08010262u;  // bge, r7 vs r3
constexpr std::uint32_t kCameraMinXAddPc = 0x08010264u;     // adds r7,r3,#0
constexpr std::uint32_t kCameraMaxXLiteralPc = 0x0801026Cu; // -240 << 16
constexpr std::uint32_t kCameraMinYBranchPc = 0x0801027Eu;  // bge, r0 vs r3
constexpr std::uint32_t kCameraMinYAddPc = 0x08010280u;     // adds r0,r3,#0
constexpr std::uint32_t kCameraMaxYLiteralPc = 0x08010288u; // -160 << 16
// Func_10000 (0x08010000), the per-frame follow, clamps the same way with
// its own literals (0x0801021C/0x08010220): lo = [S+0xEC] + [S+4] in r0,
// hi = [S+0xF4] - [S+4] - 240 in r1, x in r7, then `cmp r7,r1; ble`;
// y: lo in lr, hi = [S+0xF8] - [S+8] - 160 in r3, y in r6. Its minimum is a
// register add, so the hook takes over the final step instead: the ble is
// forced through to `adds r7,r1,#imm` / `adds r6,r3,#imm` with imm chosen
// to land on clamp(value, lo + inset, hi). rewinds 0098/0099: camera at the
// stock limit, the Func_10230 hooks alone never ran while walking.
constexpr std::uint32_t kFollowMaxXLiteralPc = 0x0801004Au;  // -240 << 16
constexpr std::uint32_t kFollowMaxYLiteralPc = 0x08010062u;  // -160 << 16
constexpr std::uint32_t kFollowClampXBranchPc = 0x0801007Cu; // ble, r7 vs r1
constexpr std::uint32_t kFollowClampXAddPc = 0x0801007Eu;    // adds r7,r1,#0
constexpr std::uint32_t kFollowClampYBranchPc = 0x08010088u; // ble, r6 vs r3
constexpr std::uint32_t kFollowClampYAddPc = 0x0801008Au;    // adds r6,r3,#0

struct CameraInset {
    std::int32_t left = 0, right = 0, top = 0, bottom = 0;  // pixels
};

// The title screen runs on the field camera too (map number 1, bounds
// 0..512 in both axes): an inset there moved the logo 60 px left and 40 up
// and pulled hidden rows of its map into view (gpu_rewind_0100; every map 1
// capture, maprec "Title" included, is the title or its file menu).
constexpr std::uint32_t kCameraMapNumberAddr = 0x02000408u;

CameraInset golden_sun_camera_inset() {
    CameraInset inset;
    if (!golden_sun_expanded_obj_view_active()) return inset;
    if (bus_read_u16(kCameraMapNumberAddr) <= 1u) return inset;
    const std::uint32_t s = bus_read_u32(kCameraStructPointer);
    const auto word = [&](std::uint32_t off) {
        return static_cast<std::int32_t>(bus_read_u32(s + off));
    };
    const std::int32_t spare_x = ((word(0xF4) - word(0xEC)) >> 16) - 240;
    const std::int32_t spare_y = ((word(0xF8) - word(0xF0)) >> 16) - 160;
    const std::int32_t half_x = spare_x > 0 ? spare_x / 2 : 0;
    const std::int32_t half_y = spare_y > 0 ? spare_y / 2 : 0;
    inset.left = std::min(half_x, static_cast<std::int32_t>(
                                      g_golden_sun_wide_extra_left));
    inset.right = std::min(half_x, static_cast<std::int32_t>(
                                       g_golden_sun_wide_extra_right));
    inset.top = std::min(half_y, static_cast<std::int32_t>(
                                     g_golden_sun_wide_extra_top));
    inset.bottom = std::min(half_y, static_cast<std::int32_t>(
                                        g_golden_sun_wide_extra_bottom));
    return inset;
}

// Spell spark loops (FACTS.md, 2026-10-02). Ten spell routines share one
// shape: a particle record is moved, then bounds tests (x past the canvas,
// y < 0) jump straight to the instruction after the stamp call
// `bl 0x080072F4` (`bx r4`), so a spark that left the canvas keeps moving
// but is not drawn. Spark Plasma (0x080D4576) and Supernova (0x080D4BC4)
// were identified in play from the stack of rewinds 0103/0104; the other
// eight were found by the same shape over the whole ROM (396 calls to
// 0x080072F4, exactly these ten have a bounce `b` to the join followed by
// x and y-bound tests to it). The argument setup between a test and the
// call differs per spell (sizes, art tables, a divide helper), so instead
// of a recipe per spell the game's own skipped path is dry-run on a scratch
// copy of the registers: the remaining bounds tests are treated as passed,
// every write goes to a scratch overlay, and at the call the stamp's
// routine (r4), canvas (r0), art (r1), box (r2, r3) and size ([sp],
// [sp+4]) are handed to the host spark capture. The game's own state and
// decisions are never changed.
struct SkippedSparkLoop {
    std::uint32_t call;                  // bl 0x080072F4
    std::array<std::uint32_t, 3> tests;  // bounds branches to call + 4
};
constexpr SkippedSparkLoop kSkippedSparkLoops[] = {
    {0x080CE476u, {0x080CE438u, 0x080CE43Cu, 0u}},
    {0x080D4576u, {0x080D4532u, 0x080D4536u, 0x080D453Au}},  // Spark Plasma
    {0x080D4BC4u, {0x080D4B7Eu, 0x080D4B82u, 0u}},           // Supernova
    {0x080D51D8u, {0x080D5190u, 0x080D5196u, 0x080D519Au}},
    {0x080DB5C6u, {0x080DB592u, 0x080DB596u, 0u}},
    {0x080DE25Eu, {0x080DE22Au, 0x080DE22Eu, 0u}},
    {0x080E6C98u, {0x080E6C5Eu, 0x080E6C62u, 0u}},
    {0x080E98D2u, {0x080E988Eu, 0x080E9892u, 0u}},
    {0x080E9FEAu, {0x080E9FB6u, 0x080E9FBAu, 0u}},
    {0x080ECDDAu, {0x080ECD98u, 0x080ECD9Eu, 0x080ECDA2u}},
};

// Reads come from EWRAM, IWRAM and ROM only; writes land in a small
// overlay. Anything else ends the dry run, so a path that does more than
// set up a stamp is simply not drawn.
struct SparkDryRunBus final : armv4t::Bus {
    const std::uint8_t* ewram = nullptr;
    const std::uint8_t* iwram = nullptr;
    const std::uint8_t* rom = nullptr;
    std::size_t rom_bytes = 0;
    std::array<std::pair<std::uint32_t, std::uint8_t>, 64> written{};
    std::size_t count = 0;
    bool failed = false;

    std::uint8_t read8(std::uint32_t a) override {
        for (std::size_t i = count; i-- > 0;)
            if (written[i].first == a) return written[i].second;
        if (a >= 0x02000000u && a < 0x02040000u) return ewram[a - 0x02000000u];
        if (a >= 0x03000000u && a < 0x03008000u) return iwram[a - 0x03000000u];
        if (a >= 0x08000000u && a - 0x08000000u < rom_bytes)
            return rom[a - 0x08000000u];
        failed = true;
        return 0;
    }
    std::uint16_t read16(std::uint32_t a) override {
        a &= ~1u;
        return static_cast<std::uint16_t>(read8(a) | (read8(a + 1) << 8));
    }
    std::uint32_t read32(std::uint32_t a) override {
        a &= ~3u;
        return std::uint32_t(read16(a)) | (std::uint32_t(read16(a + 2)) << 16);
    }
    void write8(std::uint32_t a, std::uint8_t v) override {
        if (count == written.size()) { failed = true; return; }
        written[count++] = {a, v};
    }
    void write16(std::uint32_t a, std::uint16_t v) override {
        a &= ~1u;
        write8(a, static_cast<std::uint8_t>(v));
        write8(a + 1, static_cast<std::uint8_t>(v >> 8));
    }
    void write32(std::uint32_t a, std::uint32_t v) override {
        a &= ~3u;
        write16(a, static_cast<std::uint16_t>(v));
        write16(a + 2, static_cast<std::uint16_t>(v >> 16));
    }
};

void skipped_spark_capture(std::uint32_t pc, std::uint32_t original) {
    if (original == 0u || !gsr::effect_capture_enabled()) return;
    const SkippedSparkLoop* loop = nullptr;
    for (const auto& l : kSkippedSparkLoops)
        for (std::uint32_t t : l.tests)
            if (t != 0u && t == pc) loop = &l;
    if (!loop) return;
    auto* bus = gbarecomp::active_bus();
    if (!bus) return;
    SparkDryRunBus dry;
    dry.ewram = bus->ewram_ptr();
    dry.iwram = bus->iwram_ptr();
    dry.rom = bus->rom_ptr();
    dry.rom_bytes = bus->rom_size();
    armv4t::CPUState cpu{};
    gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
    cpu.R[15] = pc + 2u;  // the branch not taken
    cpu.cpsr.t = true;
    cpu.thumb = true;
    // The longest path (a divide helper in IWRAM) is a few hundred steps.
    for (int step = 0; step < 4096; ++step) {
        const std::uint32_t at = cpu.R[15];
        if (at == loop->call && cpu.thumb) {
            const std::uint32_t sp = cpu.R[13];
            const std::uint32_t width = dry.read32(sp);
            const std::uint32_t height = dry.read32(sp + 4u);
            if (dry.failed) return;
            gsr::effect_capture_skipped_spark(
                cpu.R[4], cpu.R[0], cpu.R[1],
                static_cast<std::int32_t>(cpu.R[2]),
                static_cast<std::int32_t>(cpu.R[3]), width, height,
                bus->ewram_ptr(), bus->iwram_ptr(), bus->rom_ptr(),
                bus->rom_size(), runtime_current_frame());
            return;
        }
        if (at == loop->call + 4u) return;
        if (cpu.thumb && std::find(loop->tests.begin(), loop->tests.end(), at) !=
                             loop->tests.end()) {
            cpu.R[15] = at + 2u;  // this test passes too
            continue;
        }
        const armv4t::Instr insn =
            cpu.thumb ? armv4t::ThumbDecoder::decode(dry.read16(at), at)
                      : armv4t::ArmDecoder::decode(dry.read32(at), at);
        if (dry.failed) return;
        const auto r = armv4t::Interpreter::step(cpu, dry, insn);
        if (dry.failed || (r != armv4t::Interpreter::Result::Normal &&
                           r != armv4t::Interpreter::Result::Branched))
            return;
    }
}

// Branch sites: the bge is taken (no clamp) when the camera is at or past
// the minimum; the minimum moves in by the inset. Returns -1 elsewhere.
int golden_sun_camera_clamp_branch(std::uint32_t pc, std::uint32_t original,
                                   std::uint32_t* out_decision) {
    if (pc == kFollowClampXBranchPc || pc == kFollowClampYBranchPc) {
        const CameraInset inset = golden_sun_camera_inset();
        const bool x = pc == kFollowClampXBranchPc;
        if ((x ? inset.left + inset.right : inset.top + inset.bottom) == 0)
            return 0;
        *out_decision = 0u;  // always through the adds below
        return original != 0u ? 1 : 0;
    }
    if (pc != kCameraMinXBranchPc && pc != kCameraMinYBranchPc) return -1;
    const CameraInset inset = golden_sun_camera_inset();
    const std::int32_t in = pc == kCameraMinXBranchPc ? inset.left : inset.top;
    if (in == 0) return 0;
    const std::int32_t value = static_cast<std::int32_t>(
        pc == kCameraMinXBranchPc ? g_cpu.R[7] : g_cpu.R[0]);
    const std::int32_t minimum =
        static_cast<std::int32_t>(g_cpu.R[3]) + in * 65536;
    *out_decision = value >= minimum ? 1u : 0u;
    return *out_decision != original ? 1 : 0;
}

// ALU and literal sites: the clamped minimum (r3 + inset) and the maximum's
// view size (240 or 160 plus the inset). Returns -1 elsewhere.
int golden_sun_camera_clamp_value(std::uint32_t pc, std::uint32_t original,
                                  std::uint32_t* out_value) {
    if ((pc == kFollowClampXAddPc || pc == kFollowClampYAddPc) &&
        original == 0u) {
        const CameraInset inset = golden_sun_camera_inset();
        const bool x = pc == kFollowClampXAddPc;
        if ((x ? inset.left + inset.right : inset.top + inset.bottom) == 0)
            return 0;
        const std::int32_t value = static_cast<std::int32_t>(
            x ? g_cpu.R[7] : g_cpu.R[6]);
        const std::int32_t lo = static_cast<std::int32_t>(
            x ? g_cpu.R[0] : g_cpu.R[14]) + (x ? inset.left : inset.top) * 65536;
        const std::int32_t hi = static_cast<std::int32_t>(
            x ? g_cpu.R[1] : g_cpu.R[3]);
        const std::int32_t want = std::min(std::max(value, lo), hi);
        *out_value = static_cast<std::uint32_t>(want - hi);
        return 1;
    }
    const bool min_x = pc == kCameraMinXAddPc && original == 0u;
    const bool min_y = pc == kCameraMinYAddPc && original == 0u;
    const bool max_x = (pc == kCameraMaxXLiteralPc ||
                        pc == kFollowMaxXLiteralPc) && original == 0xFF100000u;
    const bool max_y = (pc == kCameraMaxYLiteralPc ||
                        pc == kFollowMaxYLiteralPc) && original == 0xFF600000u;
    if (!min_x && !min_y && !max_x && !max_y) return -1;
    const CameraInset inset = golden_sun_camera_inset();
    const std::int32_t in = min_x ? inset.left : min_y ? inset.top
                          : max_x ? inset.right : inset.bottom;
    if (in == 0) return 0;
    *out_value = (min_x || min_y)
        ? static_cast<std::uint32_t>(in * 65536)
        : original - static_cast<std::uint32_t>(in * 65536);
    return 1;
}

int golden_sun_thumb_alu_immediate(std::uint32_t pc, std::uint32_t original,
                                   std::uint32_t* out_value) {
    using namespace gsr::text_speed_cheat;
    if (const int r = golden_sun_worldmap_range_box(pc, original, out_value);
        r >= 0) {
        return r;
    }
    if (const int r = golden_sun_camera_clamp_value(pc, original, out_value);
        r >= 0) {
        return r;
    }
    if (pc == kChoiceCountPc) {
        *out_value = kChoiceCountWithInstant;
        return original != kChoiceCountWithInstant;
    }
    if (pc == kCaptionClearXPc || pc == gsr::hard_mode::kCaptionClearXPc) {
        *out_value = kCaptionClearX;
        return 1;
    }
    if (pc == kCaptionDrawXPc || pc == gsr::hard_mode::kCaptionDrawXPc) {
        if (!g_speed_caption_armed) {
            *out_value = kCaptionClearX;
            return 1;
        }
        const std::uint32_t width = menu_text_width(g_speed_caption);
        *out_value = kCaptionCentreX - width / 2u;
        return 1;
    }
    if (pc == kIconSourcePc) {
        if (g_blank_icons_pending == 0u) return 0;
        const std::uint32_t icon = g_cpu.R[6];
        if (icon < g_blank_icon_first || icon > g_blank_icon_last) {
            g_blank_icons_pending = 0u;
            return 0;
        }
        --g_blank_icons_pending;
        // r2 = r5 + imm; choose imm so the upload source is the clear marker.
        *out_value = kBlankIconSource - g_cpu.R[5];
        return 1;
    }
    if (pc == kDecodedCharPc && settings_help_character(original, out_value))
        return 1;
    if (pc == kDecodedCharPc && hard_mode_text_character(original, out_value))
        return 1;
    if (pc != kDecodedCharPc || !g_speed_caption_armed) return 0;
    const std::uint32_t text_id =
        bus_read_u32(g_cpu.R[13] + kDecoderTextIdStackOffset);
    if (text_id != g_speed_caption_id) {
        g_speed_caption_armed = false;
        return 0;
    }
    // Every character comes from the caption, then the 0 that ends the
    // string; what the game decoded is discarded.
    std::uint32_t wanted = 0u;
    if (g_speed_caption_pos < g_speed_caption.size()) {
        wanted = static_cast<unsigned char>(
            g_speed_caption[g_speed_caption_pos++]);
    } else {
        g_speed_caption_armed = false;
    }
    *out_value = original + (wanted - g_cpu.R[0]);
    return 1;
}

int golden_sun_thumb_literal(std::uint32_t pc, std::uint32_t original,
                             std::uint32_t* out_value) {
    using namespace gsr::text_speed_cheat;
    if (const int r = golden_sun_worldmap_range_box(pc, original, out_value);
        r >= 0) {
        return r;
    }
    if (const int r = golden_sun_camera_clamp_value(pc, original, out_value);
        r >= 0) {
        return r;
    }
    if (pc == gsr::settings_page::kPressedKeysLiteralPc &&
        original == gsr::settings_page::kPressedKeys) {
        using namespace gsr::settings_page;
        note_settings_loop_frame();
        if (g_settings_page.load() == 2) {
            const std::uint32_t keys = bus_read_u32(kPressedKeys);
            if (keys & kKeySelect) {
                *out_value = kZeroWord;
                return 1;
            }
            // The screen closes this frame; stop covering it now.
            if (keys & (kKeyA | kKeyB | kKeyStart)) g_settings_page.store(1);
        }
        if (bus_read_u32(kPressedKeys) & (kKeyA | kKeyB | kKeyStart)) {
            // Closing (both pages): draw nothing over the closing window.
            g_settings_closing.store(true);
            g_settings_screen_text = false;
        }
        return 0;
    }
    if (pc == gsr::settings_page::kRepeatKeysLiteralPc &&
        original == gsr::settings_page::kRepeatKeys) {
        return settings_page_repeat_keys(out_value);
    }
    if (pc == gsr::battle_speed::kWaitMaskLiteralPc &&
        original == gsr::battle_speed::kWaitMask) {
        native_page_frame_end();
        return battle_speed_wait_mask(out_value);
    }
    if (pc == gsr::battle_speed::kRevealEndLiteralPc &&
        original == gsr::battle_speed::kRevealEndCommand) {
        // Better Field Psy: Reveal lasts to twice the distance.
        using namespace gsr::battle_speed;
        if (g_field_psynergy_fast.load() == 0) return 0;
        const std::uint32_t state = bus_read_u32(kRevealStatePointer);
        if (bus_read_u16(state + kRevealCountdownOffset) == 0) return 0;
        // The countdown is running, so the distance test sent us here and
        // r2 still holds the squared distance.
        if (g_cpu.R[2] >= kRevealBetterRangeSquared) return 0;
        *out_value = bus_read_u16(state + kRevealCommandOffset);
        return 1;
    }
    if (pc == gsr::settings_page::kEncounterBaseLiteralPc &&
        original == gsr::settings_page::kEncounterBase) {
        using namespace gsr::settings_page;
        const int rate = g_encounter_rate.load();
        if (rate == kEncounterDouble) {
            // r0 is this step's amount; the game adds it once more below.
            const std::uint32_t counter =
                kEncounterBase + kEncounterCounterOffset;
            bus_write_u32(counter, bus_read_u32(counter) + g_cpu.R[0]);
            return 0;
        }
        bool drop = rate == kEncounterOff;
        if (rate == kEncounterHalf) {
            drop = g_encounter_skip_next;
            g_encounter_skip_next = !g_encounter_skip_next;
        }
        if (!drop) return 0;
        *out_value = kZeroWord - kEncounterCounterOffset;
        return 1;
    }
    if (pc == kSetupIconTableLiteralPc || pc == kRedrawIconTableLiteralPc) {
        if (pc == kSetupIconTableLiteralPc) {
            g_settings_page.store(1);
            g_settings_open.store(false);
            g_settings_closing.store(false);
        }
        if (original == kMessageSpeedIconTable) {
            g_blank_icons_pending =
                pc == kSetupIconTableLiteralPc ? kSetupIconCount : 1u;
            g_blank_icon_first = kFirstMessageSpeedIcon;
            g_blank_icon_last = kLastMessageSpeedIcon;
        }
        return 0;
    }
    if ((pc == gsr::hard_mode::kSetupIconTableLiteralPc ||
         pc == gsr::hard_mode::kRedrawIconTableLiteralPc) &&
        original == gsr::hard_mode::kIconTable) {
        // Hard Mode's row is text only, like Message speed.
        g_blank_icons_pending =
            pc == gsr::hard_mode::kSetupIconTableLiteralPc
                ? gsr::hard_mode::kSetupIconCount : 1u;
        g_blank_icon_first = gsr::hard_mode::kFirstIcon;
        g_blank_icon_last = gsr::hard_mode::kLastIcon;
        return 0;
    }
    if (pc == gsr::hard_mode::kCaptionBaseLiteralPc) {
        // Arms the Off/On caption; the text ID itself is unchanged.
        namespace hm = gsr::hard_mode;
        const std::uint32_t value = g_cpu.R[2];
        g_speed_caption_armed = original == hm::kCaptionBase && value <= 1u;
        if (g_speed_caption_armed) {
            g_speed_caption = hm::kWords[value];
            if (g_cpu.R[9] == gsr::settings_page::kAutoSleepRow) {
                g_speed_caption = gsr::text_speed_cheat::kArrowLeft +
                                  g_speed_caption +
                                  gsr::text_speed_cheat::kArrowRight;
            }
            g_speed_caption_id = hm::kCaptionBase + value;
            g_speed_caption_pos = 0;
        }
        return 0;
    }
    if (pc == kCaptionBaseLiteralPc) {
        // Only arms the caption; the text ID itself is unchanged.
        const std::uint32_t value = g_cpu.R[2];
        g_speed_caption_armed = original == kCaptionBase && value <= 3u;
        if (g_speed_caption_armed) {
            const bool selected = g_cpu.R[9] == kMessageSpeedRow;
            g_speed_caption = kSpeedWords[value];
            if (selected)
                g_speed_caption = kArrowLeft + g_speed_caption + kArrowRight;
            g_speed_caption_id = kCaptionBase + value;
            g_speed_caption_pos = 0;
        }
        return 0;
    }
    const bool delay = pc == kDelayTableLiteralPc && original == kDelayTable;
    const bool battle =
        pc == kBattleTableLiteralPc && original == kBattleTable;
    if ((!delay && !battle) || !message_speed_is_instant()) return 0;
    *out_value = original - 1u;
    return 1;
}

// Func_94820, Func_94bbc and Func_94e7c (verified ROM) each test a sprite
// against the old screen before queueing it with Func_3dec: `x + 16` (or
// `x + 15`) unsigned against 255, then y against -32 and 159. World-map towns
// vanish at the old right edge on a route none of the Func_b168/b388/c62c
// hooks see (FACTS.md, 2026-09-24). In the expanded view the x test and the
// lower y test admit the margins; the upper y test is kept, because an 8-bit
// OAM y below the screen cannot be told from one above it. Returns -1 for any
// other PC.
int golden_sun_worldmap_sprite_branch(std::uint32_t pc, std::uint32_t original,
                                      std::uint32_t* out_decision) {
    enum Kind { kX, kYLower };
    std::int32_t operand = 0;
    Kind kind = kX;
    switch (pc) {
        case 0x080948BCu: operand = static_cast<std::int32_t>(g_cpu.R[3]); break;
        case 0x08094C56u: operand = static_cast<std::int32_t>(g_cpu.R[1]); break;
        case 0x08094F26u: operand = static_cast<std::int32_t>(g_cpu.R[3]); break;
        case 0x080948C4u: operand = static_cast<std::int32_t>(g_cpu.R[0]);
                          kind = kYLower; break;
        case 0x08094C5Eu: operand = static_cast<std::int32_t>(g_cpu.R[0]);
                          kind = kYLower; break;
        case 0x08094F2Eu: operand = static_cast<std::int32_t>(g_cpu.R[8]);
                          kind = kYLower; break;
        default: return -1;
    }
    if (original != 1u || !golden_sun_expanded_obj_view_active()) return 0;
    const std::int32_t slack = gsr::widescreen::kExpandedObjSlackX;
    bool admit = false;
    if (kind == kX) {
        const std::int32_t right = 255 + static_cast<std::int32_t>(
            g_golden_sun_wide_extra_right) + slack;
        const std::int32_t left = -static_cast<std::int32_t>(
            g_golden_sun_wide_extra_left) - slack;
        admit = (operand > 255 && operand <= right) ||
                (operand < 0 && operand >= left);
    } else {
        admit = operand < -32 &&
                operand >= -32 - static_cast<std::int32_t>(
                                      g_golden_sun_wide_extra_top);
    }
    if (!admit) return 0;
    static unsigned logged = 0;
    if (golden_sun_wide_diagnostics_enabled() && logged < 2000u) {
        ++logged;
        std::fprintf(stderr,
                     "[worldmap-sprite-branch] frame=%llu pc=0x%08X operand=%d\n",
                     static_cast<unsigned long long>(runtime_current_frame()),
                     pc, operand);
    }
    *out_decision = 0u;
    return 1;
}

// Every reviewed conditional branch in config/usa/main.toml. The widescreen
// sites act only in the expanded view; the Message-speed icon bounce acts in
// every view.
int golden_sun_conditional_branch(std::uint32_t pc, std::uint32_t original,
                                  std::uint32_t* out_decision) {
    using namespace gsr::text_speed_cheat;
    if (const int r = golden_sun_worldmap_sprite_branch(pc, original,
                                                        out_decision);
        r >= 0) {
        return r;
    }
    if (const int r = cheat_drop_branch(pc, original, out_decision); r >= 0)
        return r;
    skipped_spark_capture(pc, original);
    if (const int r = golden_sun_camera_clamp_branch(pc, original,
                                                     out_decision);
        r >= 0) {
        return r;
    }
    if (pc == gsr::hard_mode::kIdleBranchPc) {
        // Auto-Sleep's idle timer is gone: always skip the counter.
        *out_decision = 1u;
        return original != 1u;
    }
    if (pc == kIconBounceBranchPc) {
        if (g_cpu.R[6] != kMessageSpeedRow ||
            bus_read_u8(g_cpu.R[5] + kScreenMessageSpeedOffset) !=
                kMessageSpeedInstant) {
            return 0;
        }
        *out_decision = 1u;
        return original != 1u;
    }
    return golden_sun_wide_conditional_branch(pc, original, out_decision);
}

// One callback serves every measured Golden Sun write policy; each policy
// gates itself, and the runtime only records a transform when the value
// actually changes.
int golden_sun_write_override(std::uint32_t pc, std::uint32_t addr,
                              std::uint32_t requested, std::uint32_t width,
                              std::uint32_t* out_value) {
    if (const int r = cheat_reward_write(pc, addr, requested, width, out_value);
        r >= 0) {
        return r;
    }
    if (out_value && gsr::text_speed_cheat::matches(pc, width) &&
        message_speed_is_instant()) {
        *out_value = gsr::text_speed_cheat::kInstantBudget;
        return 1;
    }
    using namespace gsr::player_speed_cheat;
    if (!out_value || !matches(pc, addr, width)) return 0;
    const std::uint32_t before = bus_read_u32(addr);
    const std::uint32_t applied =
        transform(before, requested,
                  static_cast<std::uint32_t>(
                      runtime_get_mem_write_override_enabled()));
    static std::atomic<unsigned> trace_logged{0};
    if (trace_logged.exchange(1u, std::memory_order_relaxed) == 0u) {
        std::fprintf(
            stderr,
            "[cheat] PlayerWalkRun2x observed: pc=0x%08X addr=0x%08X "
            "target=0x%08X before=0x%08X requested=0x%08X "
            "applied=0x%08X delta=0x%08X\n",
            pc, addr, addr, before, requested, applied,
            applied - before);
    }
    *out_value = applied;
    return 1;
}

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

const auto kTransientCodeImages = std::to_array<TransientCodeImage>({
    {0x03002000u, 0x0300207Cu, 0x08002D5Cu,
     0,
     "a0e2414cc0e339ec6f61fbd387eb6325a8eabe84", "Func_2d5c",
     gsr_func2d5c_kDispatchTable, &gsr_func2d5c_kDispatchTableLen},
    {0x03002000u, 0x0300209Cu, 0x0800A37Cu,
     0,
     "77bec232b22b723f3c5f5e0fedd28680fde50048", "Func_a37c",
     gsr_funca37c_kDispatchTable, &gsr_funca37c_kDispatchTableLen},
    {0x03002000u, 0x03002140u, 0x08015430u,
     0,
     "d49cef2181ce6f9fee635daa0ba3b072d336109a", "Func_15430",
     nullptr, nullptr},
    {0x03002140u, 0x030021A0u, 0x08015570u,
     0,
     "20f9190c9b7aac90e091beff5f7e54a46ba833eb", "Func_15570",
     nullptr, nullptr},
    // Func_2544 at 0x03002000 and 0x03006000 used to be two entries here. The
    // row at 0x0300347c below stays, because it is the identity gate for the
    // PCs the main corpus dispatches statically; the routine itself is now a
    // position-independent image.
    {0x03002400u, 0x030024E0u, 0x08001DC8u,
     0,
     "c2e9ace3620fc779387d91100c97071ebf21640b", "Func_1dc8",
     nullptr, nullptr},
    {0x0300387Cu, 0x0300395Cu, 0x08001DC8u,
     0,
     "c2e9ace3620fc779387d91100c97071ebf21640b", "Func_1dc8_at_0300387c",
     nullptr, nullptr},
    {0x0300347Cu, 0x03003740u, 0x08002544u,
     0,
     "b8967c9f00835f22da02cce31595588bb05496e6", "Func_2544_at_0300347c",
     nullptr, nullptr},
    // GS-011: the allocator reuses 0x03006000 for a second, larger ROM image.
    // DMA3 watch observed src=0x08002808 cnt=315 words; goldensun.elf agrees
    // (Func_2808, size 0x4ec, arm). SHA-1 keeps the two images distinct.
    // Player report 2026-10-03: live 0x00 vs ROM 0x01 at offset 0, written by
    // its own output pointer one byte past the buffer while still running; see
    // FACTS.md "Linux player crash: Func_2808 overwrote its own first byte".
    // The first ARM instruction only runs on entry, so it is the volatile prefix.
    {0x03006000u, 0x030064ECu, 0x08002808u,
     0,
     "5fd23904086a4129fe3472dbf6658bb5e73f0363",
     "Func_2808_at_03006000",
     gsr_func2808_03006000_kDispatchTable,
     &gsr_func2808_03006000_kDispatchTableLen,
     4u},
    // Func_6abc's stack thunk used to be registered here at 0x03007df8 and
    // 0x03007bc0. Func_6878 installs it at whatever depth it was entered at,
    // so it is now a single position-independent image below.
    // Func_9bb8, Func_15430, Func_15570 and Func_158e8 used to occupy nine
    // entries here, one per (routine, base) pair a run happened to reach. They
    // are position-independent images now; the DMA3 census that motivated them
    // is preserved in docs/GS011_TRANSIENT_IMAGES.md.
    // Func_a418 sits immediately below the 0x0300347c staging slot, so the
    // flash driver's working area is a contiguous run of separately installed
    // routines. DMA3 watch: src=0x0800a418, 31 words; entry observed through
    // _call_via_r4 in ARM. goldensun.elf sizes Func_a418 at exactly 0x7c.
    {0x03003400u, 0x0300347Cu, 0x0800A418u,
     0,
     "d3261b6f7b57ba4e7da8e44cf555d009c7cc27d1",
     "Func_a418_at_03003400",
     gsr_funca418_03003400_kDispatchTable,
     &gsr_funca418_03003400_kDispatchTableLen},
    // 0x03003a84 is a reusable staging slot for the flash driver. Trace
    // #4699483 reprograms DMA3 SAD=0x08015e10 CNT=0x1f words over the
    // Func_15afc image below, and #4699487 enters it in ARM through
    // _call_via_r4. goldensun.elf sizes Func_15e10 at exactly 0x7c bytes.
    {0x03003A84u, 0x03003B00u, 0x08015E10u,
     0,
     "50cf8d2eaf6ba74b7ae74a97458d04214a00af57",
     "Func_15e10_at_03003a84",
     gsr_func15e10_03003a84_kDispatchTable,
     &gsr_func15e10_03003a84_kDispatchTableLen},
    // Func_15afc, DMA-copied into IWRAM. Trace #4697289..91 programs DMA3
    // SAD=0x08015afc DAD=0x03003a84 CNT=0x9e words, the descriptor at
    // 0x03001e50 records the same 0x03003a84..0x03003cfc bounds, and #4697295
    // enters it in ARM through _call_via_r3. goldensun.elf sizes Func_15afc at
    // exactly 0x278 bytes.
    {0x03003A84u, 0x03003CFCu, 0x08015AFCu,
     0,
     "f6ab01b56876f8d1b72a4bf5239bd5cbac620cb0",
     "Func_15afc_at_03003a84",
     gsr_func15afc_03003a84_kDispatchTable,
     &gsr_func15afc_03003a84_kDispatchTableLen},
    // Func_2cf4's four observed stack depths (0x03007ba4, 0x03007dbc,
    // 0x03007dc4, 0x03007dc8) used to be four entries here. Func_3a7c DMAs the
    // routine to whatever its SP happens to be, so that set is bounded only by
    // the call graph and enumerating it never converges. It is now a single
    // POSITION-INDEPENDENT image in kRelocatableCodeImages below.
    // Ring event #3295783 records the first post-boot write: ARM STMIA at
    // 0x03000800 installs the 24-byte template rooted at 0x030008D4. The
    // writer repeats its six words into four holes while preserving the fixed
    // instructions between them. This identity is valid after the loop exits
    // at 0x03000818 and only until the next writer pass or an IWRAM reset.
    {0x03000828u, 0x030008CCu, 0u, 0,
     "6e801af81bd285752b5efffccc815c7c9dc2cfc3",
     "Func_dc8_synth_template_030008d4",
     gsr_synth_dc8_030008d4_kDispatchTable,
     &gsr_synth_dc8_030008d4_kDispatchTableLen},
    // The strict-static path independently selected the adjacent 24-byte
    // template at 0x030008EC. Keep it as a separate whole-image identity.
    {0x03000828u, 0x030008CCu, 0u, 0,
     "e9437b0a1a071ea8a136fab4ce4c47db3bac2e2e",
     "Func_dc8_synth_template_030008ec",
     gsr_synth_dc8_030008ec_kDispatchTable,
     &gsr_synth_dc8_030008ec_kDispatchTableLen},
    // The third 24-byte template, at 0x03000904, was previously derived but
    // unobserved. A self-heal discovery run reached 0x03000828 holding image
    // SHA-1 d8d1bcfc..., which is exactly what expanding that template
    // produces, so it is now an observed identity like the other two.
    {0x03000828u, 0x030008CCu, 0u, 0,
     "d8d1bcfcf2734deaca85791720f6180f4da4fcca",
     "Func_dc8_synth_template_03000904",
     gsr_synth_dc8_03000904_kDispatchTable,
     &gsr_synth_dc8_03000904_kDispatchTableLen},
    // Func_dc8's second self-modifying slot: the loop 0x03000A58..0x03000A94
    // with four instruction pairs copied from table 0x030009C4 by
    // 0x03000A20..0x03000A50, one variant per (r3 & 3). main.toml makes
    // 0x03000A5C a runtime_code_entry (its placeholder pairs are declared
    // data), so the main corpus hands control here after 0x03000A58;
    // tools/build_synthesized_dc8_slot2_variant.py builds each variant.
    {0x03000A58u, 0x03000A98u, 0u, 0,
     "8838c9200853541f953378c905c676a13014d114",
     "Func_dc8_slot2_variant_0",
     gsr_synth_dc8b_0_kDispatchTable,
     &gsr_synth_dc8b_0_kDispatchTableLen},
    {0x03000A58u, 0x03000A98u, 0u, 0,
     "4cb4ad3be5ab068177c58c3f62870fd18ac5dfdd",
     "Func_dc8_slot2_variant_1",
     gsr_synth_dc8b_1_kDispatchTable,
     &gsr_synth_dc8b_1_kDispatchTableLen},
    {0x03000A58u, 0x03000A98u, 0u, 0,
     "7249708ac31e8526efb4bc143d0169a929693d3f",
     "Func_dc8_slot2_variant_2",
     gsr_synth_dc8b_2_kDispatchTable,
     &gsr_synth_dc8b_2_kDispatchTableLen},
    {0x03000A58u, 0x03000A98u, 0u, 0,
     "2c1d013129083ded6a064a10b4c097cd338dca0c",
     "Func_dc8_slot2_variant_3",
     gsr_synth_dc8b_3_kDispatchTable,
     &gsr_synth_dc8b_3_kDispatchTableLen},
    // main.toml's [[code_copy]] runtime_start=0x03000000 source_start=
    // 0x08000770 size=0x1400 DMA-installs a fixed ROM image that kDispatchTable
    // already has AOT entries for (gf_irq_handler_03000000 and friends). Page 0
    // of IWRAM (0x03000000..0x03000FFF) is registered for dirty-tracking by the
    // synth-template rows above, so that DMA copy itself sets the page-0 dirty
    // bit before any dispatch ever reaches it, discarding the AOT fn on first
    // touch.
    //
    // Func_dc8's third self-modifying slot: 0x080037D4 copies one of five
    // 0x98-byte blocks from ROM 0x08000404 over 0x03000BD8..0x03000C70; each
    // ends at 0x03000C70. main.toml makes 0x03000BD8 a runtime_code_entry.
    // tools/build_synthesized_dc8_slot3_variant.py builds each variant.
    {0x03000BD8u, 0x03000C70u, 0u, 0,
     "614ae6c0078fce180d995d21459e7179264856ed",
     "Func_dc8_slot3_variant_0",
     gsr_synth_dc8c_0_kDispatchTable,
     &gsr_synth_dc8c_0_kDispatchTableLen},
    {0x03000BD8u, 0x03000C70u, 0u, 0,
     "1abe269ad19ab8c04d9c2c8c0d799811cbd87992",
     "Func_dc8_slot3_variant_1",
     gsr_synth_dc8c_1_kDispatchTable,
     &gsr_synth_dc8c_1_kDispatchTableLen},
    {0x03000BD8u, 0x03000C70u, 0u, 0,
     "7eb473e88fd39397d5591fd6037ca57fec7ec982",
     "Func_dc8_slot3_variant_2",
     gsr_synth_dc8c_2_kDispatchTable,
     &gsr_synth_dc8c_2_kDispatchTableLen},
    {0x03000BD8u, 0x03000C70u, 0u, 0,
     "04b236dc124de2851e1ec84ebc7c27d57ac4b177",
     "Func_dc8_slot3_variant_3",
     gsr_synth_dc8c_3_kDispatchTable,
     &gsr_synth_dc8c_3_kDispatchTableLen},
    {0x03000BD8u, 0x03000C70u, 0u, 0,
     "11160f4c7a2d1447fae4250b86f410b69b5e448d",
     "Func_dc8_slot3_variant_4",
     gsr_synth_dc8c_4_kDispatchTable,
     &gsr_synth_dc8c_4_kDispatchTableLen},
    // The identity covers code only. Hashing whole halves also covered the
    // image's main.toml data ranges (the IRQ table 0x030000E0..0x03000118,
    // variables at 0x03000350.., inline words at 0x030008CC, 0x0300099C,
    // 0x03000B5C, buffers 0x03000DF8..0x03001388), which the game writes, so
    // every half failed and all of its code was compiled at runtime (76 of the
    // 89 runtime compiles in sessions 20260923_090349/_093557). The runs
    // below skip those ranges and the mixer Func_dc8's three self-modifying
    // slots (0x03000828..0x030008CC, 0x03000A58..0x03000A98,
    // 0x03000BD8..0x03000C70), each registered above with every variant its
    // writer can produce and made a dispatch boundary in main.toml, so no
    // AOT function runs a slot's bytes. AOT code calls AOT code directly, so
    // that boundary is what makes these runs safe; main() refuses a main
    // corpus generated without it. SHA-1s are from the pinned ROM at each
    // run's source address; each run is registered once per mode, as ELF
    // mapping symbols put both ARM and THUMB code in the image (e.g.
    // gf_tfunc_03000658, gf_afunc_03000088).
    {0x03000000u, 0x030000E0u, 0x08000770u, 0,
     "425e210c9524665da2b651e29039187452e03538",
     "main_copy_03000000_code_0000_arm", nullptr, nullptr},
    {0x03000000u, 0x030000E0u, 0x08000770u, 1,
     "425e210c9524665da2b651e29039187452e03538",
     "main_copy_03000000_code_0000_thumb", nullptr, nullptr},
    {0x03000118u, 0x03000350u, 0x08000888u, 0,
     "24be4da49c5bf5e6687b01c5d42e6e50b80a9b2c",
     "main_copy_03000000_code_0118_arm", nullptr, nullptr},
    {0x03000118u, 0x03000350u, 0x08000888u, 1,
     "24be4da49c5bf5e6687b01c5d42e6e50b80a9b2c",
     "main_copy_03000000_code_0118_thumb", nullptr, nullptr},
    {0x03000380u, 0x03000716u, 0x08000AF0u, 0,
     "6615b4c278a236c602a2573e1d734ce8a93f5b3a",
     "main_copy_03000000_code_0380_arm", nullptr, nullptr},
    {0x03000380u, 0x03000716u, 0x08000AF0u, 1,
     "6615b4c278a236c602a2573e1d734ce8a93f5b3a",
     "main_copy_03000000_code_0380_thumb", nullptr, nullptr},
    {0x0300071Cu, 0x03000828u, 0x08000E8Cu, 0,
     "d8b24aa5a6292596e6fcb757cc036ae655016845",
     "main_copy_03000000_code_071c_arm", nullptr, nullptr},
    {0x0300071Cu, 0x03000828u, 0x08000E8Cu, 1,
     "d8b24aa5a6292596e6fcb757cc036ae655016845",
     "main_copy_03000000_code_071c_thumb", nullptr, nullptr},
    {0x030008D4u, 0x0300099Cu, 0x08001044u, 0,
     "15701bb90db4b5c68dec0b4b3976a37639a06f70",
     "main_copy_03000000_code_08d4_arm", nullptr, nullptr},
    {0x030008D4u, 0x0300099Cu, 0x08001044u, 1,
     "15701bb90db4b5c68dec0b4b3976a37639a06f70",
     "main_copy_03000000_code_08d4_thumb", nullptr, nullptr},
    {0x030009A8u, 0x03000A58u, 0x08001118u, 0,
     "66c770afa4d055cdd754ab31bbb59bce0c19787b",
     "main_copy_03000000_code_09a8_arm", nullptr, nullptr},
    {0x030009A8u, 0x03000A58u, 0x08001118u, 1,
     "66c770afa4d055cdd754ab31bbb59bce0c19787b",
     "main_copy_03000000_code_09a8_thumb", nullptr, nullptr},
    {0x03000A98u, 0x03000B5Cu, 0x08001208u, 0,
     "b21981f20a35278add1fb2390af860ea504be0d4",
     "main_copy_03000000_code_0a98_arm", nullptr, nullptr},
    {0x03000A98u, 0x03000B5Cu, 0x08001208u, 1,
     "b21981f20a35278add1fb2390af860ea504be0d4",
     "main_copy_03000000_code_0a98_thumb", nullptr, nullptr},
    {0x03000B60u, 0x03000BD8u, 0x080012D0u, 0,
     "20471e8954c7f61e1f6d346db745689164320992",
     "main_copy_03000000_code_0b60_arm", nullptr, nullptr},
    {0x03000B60u, 0x03000BD8u, 0x080012D0u, 1,
     "20471e8954c7f61e1f6d346db745689164320992",
     "main_copy_03000000_code_0b60_thumb", nullptr, nullptr},
    {0x03000C70u, 0x03000DF8u, 0x080013E0u, 0,
     "a0530c6e4231247de55a8ae4c0737634fbc0480f",
     "main_copy_03000000_code_0c70_arm", nullptr, nullptr},
    {0x03000C70u, 0x03000DF8u, 0x080013E0u, 1,
     "a0530c6e4231247de55a8ae4c0737634fbc0480f",
     "main_copy_03000000_code_0c70_thumb", nullptr, nullptr},
    {0x03001388u, 0x03001400u, 0x08001AF8u, 0,
     "b4b275f498b4a12b1ba721a3832e6ffc6b065fd6",
     "main_copy_03000000_code_1388_arm", nullptr, nullptr},
    {0x03001388u, 0x03001400u, 0x08001AF8u, 1,
     "b4b275f498b4a12b1ba721a3832e6ffc6b065fd6",
     "main_copy_03000000_code_1388_thumb", nullptr, nullptr},
#if 0  // Superseded by the generated, complete overlay registry below.
    // TCP capture at frame 293, PC 0x0808c5ba matched the installed bytes
    // uniquely to the pinned rom_779188 overlay build. Its compressed ROM
    // stream has no linear source/runtime bias, so identity is an image SHA-1
    // rather than rom_start + offset.
    //
    // The extent is the overlay's `.text` section, NOT its whole decompressed
    // image. Both overlays carry a writable `.data` section immediately after
    // `.text` (rom_779188: 0x020085f8, 0x5a bytes, plus 0x54 of .bss;
    // rom_7795e8: 0x020094d4, 0x246 bytes). Hashing those bytes made the
    // identity depend on mutable game state: an input-driven run aborted with
    // "unknown transient code identity at 0x020081fc" whose live window
    // differed from pinned rom_7795e8 in exactly two bytes, at 0x020096b8 and
    // 0x020096bc — both inside `.data`. A code identity must cover code only.
    {0x02008000u, 0x020085F8u, 0u, 1,
     "dbead77b3bca167228968dda277639ecb7355e01", "overlay_rom_779188",
     gsr_overlay_rom_779188_kDispatchTable,
     &gsr_overlay_rom_779188_kDispatchTableLen},
    // A third overlay reaches the same slot once the demo-input driver enters
    // gameplay. Its live `.text` matched pinned rom_787e04 byte for byte, and
    // the abort PC 0x02008160 is that build's exact ELF OvlFunc_160 entry.
    {0x02008000u, 0x02009AB4u, 0u, 1,
     "6ee2f3a2a1370a8a42e7d0e25eb983f0ef0576fb", "overlay_rom_787e04",
     gsr_overlay_rom_787e04_kDispatchTable,
     &gsr_overlay_rom_787e04_kDispatchTableLen},
    // A fourth overlay, reached only by the 5,400-frame campaign track. The
    // 5,400-frame run aborted on an unknown identity at 0x02008104; the live
    // EWRAM window matched pinned rom_77dd1c byte for byte over the whole
    // 0x4000 dump, and 0x02008105 is that build's exact ELF OvlFunc_104 THUMB
    // entry (a $t mapping symbol sits at 0x02008104).
    //
    // Extent is `.text` only — 0x02008000 + 0x48bc — NOT the 0x57f8
    // decompressed image. .data starts at 0x0200c8bc (0xf3c bytes) and .bss at
    // 0x0200d7f8; hashing those would make the code identity depend on mutable
    // game state, which is exactly the defect the rom_7795e8 audit found.
    {0x02008000u, 0x0200C8BCu, 0u, 1,
     "b58745ad5c0dc6872de7e75d5bf527714cad7f1e", "overlay_rom_77dd1c",
     gsr_overlay_rom_77dd1c_kDispatchTable,
     &gsr_overlay_rom_77dd1c_kDispatchTableLen},
    // Fifth overlay, found in live play just past the Mt. Aleph boulder scene.
    // Registered .text-only (0x1bdc): .data begins at 0x02009bdc and the game
    // writes it, so including it would make the code identity depend on mutable
    // state - the same defect already fixed for rom_779188 and rom_7795e8.
    {0x02008000u, 0x02009BDCu, 0u, 1,
     "eff53a832d743ae378ec9d07ff54e2e43b4c383c", "overlay_rom_78603c",
     gsr_overlay_rom_78603c_kDispatchTable,
     &gsr_overlay_rom_78603c_kDispatchTableLen},
    // Sixth overlay: the plaza cutscene near the psynergy stone. Found in live
    // play 2026-08-08 as a FREEZE, not a miss — the interpreter bridge for
    // 0x02008BBC ran away ("exceeded 200000000 instructions without returning
    // to stop_pc=0x0808D88C"). Identified by dumping the live window with
    // GBARECOMP_TRANSIENT_DUMP and matching it against the pinned decompressed
    // overlay set: rom_784360, byte for byte, all 16084 bytes.
    // Registered .text-only (0x2854): overlay.elf puts .data at 0x0200a854 and
    // the game writes it, so including it would make the code identity depend
    // on mutable state — the same defect already fixed for rom_779188,
    // rom_7795e8 and rom_78603c.
    {0x02008000u, 0x0200A854u, 0u, 1,
     "ed3b6e11906e313a8a1aaefaa4e6aef4374c6d15", "overlay_rom_784360",
     gsr_overlay_rom_784360_kDispatchTable,
     &gsr_overlay_rom_784360_kDispatchTableLen},
    // The allocator later replaces the shared EWRAM slot with rom_7795e8.
    {0x02008000u, 0x020094D4u, 0u, 1,
     "c728ff67c9e34efa992768081689429f1bd826bc", "overlay_rom_7795e8",
     gsr_overlay_rom_7795e8_kDispatchTable,
     &gsr_overlay_rom_7795e8_kDispatchTableLen},
    // Seventh overlay: the post-fight cutscene after loading state 8. The first
    // unknown PC was 0x02008AA4. Five independently sized live-prefix SHA-1s
    // and all six self-healed block CRC32s uniquely match pinned rom_780898.
    // Its ELF confirms the observed THUMB entries and the 0x02008AAC interior
    // resume. Register `.text` only: writable `.data` begins at 0x0200E190.
    {0x02008000u, 0x0200E190u, 0u, 1,
     "ab81b675700afa375749790d84da3ae5ffafe9fa", "overlay_rom_780898",
     gsr_overlay_rom_780898_kDispatchTable,
     &gsr_overlay_rom_780898_kDispatchTableLen},
#endif
#define GSR_OVERLAY(id, start, end, sha1)                                  \
    {start, end, 0u, 1, sha1, "overlay_" #id,                             \
     gsr_overlay_##id##_kDispatchTable,                                    \
     &gsr_overlay_##id##_kDispatchTableLen},
#include "overlay-registry.inc"
#undef GSR_OVERLAY
});

// Identity validation is needed only after a write can have changed a page
// containing one of these reviewed fixed-address transient images.  Derive
// the masks from the registry so ordinary data traffic does not invalidate
// every cached image and no RAM address is guessed here.
void init_ram_code_page_masks() {
    g_ram_code_page_mask_ewram_lo = 0;
    g_ram_code_page_mask_ewram_hi = 0;
    g_ram_code_page_mask_iwram = 0;
    for (const auto& image : kTransientCodeImages) {
        if (image.end <= image.start) continue;
        const uint32_t last = image.end - 1u;
        const uint32_t region = image.start >> 24;
        if (region == 0x03u) {
            const uint32_t first_page = (image.start & 0x7FFFu) >> 12;
            const uint32_t last_page = (last & 0x7FFFu) >> 12;
            for (uint32_t page = first_page; page <= last_page; ++page)
                g_ram_code_page_mask_iwram |= 1u << page;
        } else if (region == 0x02u) {
            const uint32_t first_page = (image.start & 0x3FFFFu) >> 12;
            const uint32_t last_page = (last & 0x3FFFFu) >> 12;
            for (uint32_t page = first_page; page <= last_page; ++page) {
                if (page < 32u) g_ram_code_page_mask_ewram_lo |= 1ull << page;
                else g_ram_code_page_mask_ewram_hi |= 1ull << (page - 32u);
            }
        }
    }
}

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

// Func_b5138 self-relocates a jump table into the middle of its own extent:
// ROM 0x080b5138+0xcc..0xd4 (a $d literal pool) and 0x080b5138+0xe0..0x120
// (the placeholder jump table the copy's own relocation loop overwrites),
// both declared as [[data_range]] in
// config/usa/transient-func-b5138-relocatable.toml. Offsets are relative to
// the image origin 0x0300207c, which is also the base at runtime since a
// relocatable image's data ranges move with the copy.
constexpr gsr::ByteRange kFuncB5138ExcludedRanges[] = {
    {0x000000CCu, 0x000000D4u},
    {0x000000E0u, 0x00000120u},
};

const std::array<RelocatableCodeImage, 29> kRelocatableCodeImages{{
    // Func_2cf4, DMA-copied onto the stack by Func_3a7c. Observed at
    // 0x03007ba4, 0x03007dbc, 0x03007dc4 and 0x03007dc8; the extent, mode and
    // identity are the same at every one of them, and goldensun.elf bounds the
    // ROM source at 0x08002cf4 size 0x68 with ARM mapping symbols.
    {0x08002CF4u,
     0,
     "87b609455a1aa6cd4f6251a73f8e15f4b6bc9c05",
     "Func_2cf4_relocatable",
     &gsr_func2cf4_pic_kImageOrigin,
     &gsr_func2cf4_pic_kImageSize,
     gsr_func2cf4_pic_kDispatchTable,
     &gsr_func2cf4_pic_kDispatchTableLen},
    // Func_1dc8, copied into the flash driver's working area. Known at
    // 0x03002400 and 0x0300387c, and the campaign track reached a third base
    // 0x03003f9c (trace #9553280..82 programs DMA3 SAD=0x08001dc8
    // DAD=0x03003f9c CNT=0x84000038, and #9553285 exchanges to it in ARM
    // through _call_via_r6). goldensun.elf sizes Func_1dc8 at 0xe0 with ARM
    // mapping symbols; the DMA word count agrees exactly.
    //
    // ADDITIVE, not a replacement: the two fixed rows above for 0x03002400 and
    // 0x0300387c stay, because they are the identity gate for PCs the main
    // corpus dispatches statically. This image covers every other base.
    {0x08001DC8u,
     0,
     "c2e9ace3620fc779387d91100c97071ebf21640b",
     "Func_1dc8_relocatable",
     &gsr_func1dc8_pic_kImageOrigin,
     &gsr_func1dc8_pic_kImageSize,
     gsr_func1dc8_pic_kDispatchTable,
     &gsr_func1dc8_pic_kDispatchTableLen},
    // Func_6abc, the four-byte THUMB thunk Func_6ac0 writes into Func_6878's
    // active stack frame. Observed at 0x03007df8 and, one frame shallower,
    // 0x03007bc0. Four bytes is a weak identity on its own, which is why the
    // mode is checked too and why the resolver only ever tries bases derived
    // from the failing PC itself.
    {0x08006ABCu,
     1,
     "bbfc4c623d6e47e6d00a0c13ca4a9bbf58f6af36",
     "Func_6abc_relocatable",
     &gsr_func6abc_pic_kImageOrigin,
     &gsr_func6abc_pic_kImageSize,
     gsr_func6abc_pic_kDispatchTable,
     &gsr_func6abc_pic_kDispatchTableLen},
    // The flash driver's working-area routines. A DMA3 watch on 0x0300347c
    // across one campaign run counted 219 copies from 0x08009bb8, 12 from
    // 0x08015430, 4 from 0x08002544 and 1 from 0x080158e8, and the same
    // routines also appear at 0x03002000, 0x03002140, 0x030035bc, 0x03003b9c,
    // 0x03003cdc and 0x03006000. Routine and base vary independently, which is
    // exactly what per-(routine, base) registration could not keep up with.
    //
    // Every extent, mode and identity below is the one the fixed registrations
    // carried, re-derived from goldensun.elf STT_FUNC sizes by
    // tools/build_transient_image_config.py. All five SHA-1s matched what the
    // live-verified rows already used, which is an independent check that the
    // ELF extents and the observed DMA extents agree.
    {0x08009BB8u,
     0,
     "38da8bb7176c71ef1e86ce50346af1d72174529c",
     "Func_9bb8_relocatable",
     &gsr_func9bb8_pic_kImageOrigin,
     &gsr_func9bb8_pic_kImageSize,
     gsr_func9bb8_pic_kDispatchTable,
     &gsr_func9bb8_pic_kDispatchTableLen},
    {0x08015430u,
     0,
     "d49cef2181ce6f9fee635daa0ba3b072d336109a",
     "Func_15430_relocatable",
     &gsr_func15430_pic_kImageOrigin,
     &gsr_func15430_pic_kImageSize,
     gsr_func15430_pic_kDispatchTable,
     &gsr_func15430_pic_kDispatchTableLen},
    // Func_15570 is always installed contiguously after Func_15430, so the two
    // are separate images at a fixed stride rather than one larger image.
    {0x08015570u,
     0,
     "20f9190c9b7aac90e091beff5f7e54a46ba833eb",
     "Func_15570_relocatable",
     &gsr_func15570_pic_kImageOrigin,
     &gsr_func15570_pic_kImageSize,
     gsr_func15570_pic_kDispatchTable,
     &gsr_func15570_pic_kDispatchTableLen},
    {0x080158E8u,
     0,
     "03e57ceee902e248334bb546cd32716edf90fca3",
     "Func_158e8_relocatable",
     &gsr_func158e8_pic_kImageOrigin,
     &gsr_func158e8_pic_kImageSize,
     gsr_func158e8_pic_kDispatchTable,
     &gsr_func158e8_pic_kDispatchTableLen},
    // ADDITIVE at 0x0300347c: that fixed row stays, because it gates PCs the
    // main corpus dispatches statically. This image covers every other base.
    {0x08002544u,
     0,
     "b8967c9f00835f22da02cce31595588bb05496e6",
     "Func_2544_relocatable",
     &gsr_func2544_pic_kImageOrigin,
     &gsr_func2544_pic_kImageSize,
     gsr_func2544_pic_kDispatchTable,
     &gsr_func2544_pic_kDispatchTableLen},
    // A NEW pooled routine, found only by the 5,400-frame campaign run. It
    // aborted as an unknown identity at 0x0300387c, 208 bytes different from
    // the Func_1dc8 image that address had been hosting; a DMA3 watch on that
    // address records ch=3 src=0x08001b70 word=0/150 (150 words = 0x258 bytes)
    // as the last writer, the live first word 0xE92D40E2 matches ROM
    // 0x08001b70, and goldensun.elf sizes Func_1b70 at exactly 0x258 with an
    // $a at the entry. Registered position-independent from the start.
    //
    // This is the class relocation does NOT close: it removes the need to
    // enumerate bases, not the need to discover routines.
    // Session 20260923_114927 compiled 0x03007B64 at runtime; its bytes equal ROM 0x08006b84, goldensun.elf Func_6b84 (THUMB, 0x24 bytes, the last halfword a $d pad). A byte-copy loop, copied onto the stack.
    // config/usa/transient-func_6b84-relocatable.toml.
    {0x08006B84u,
     1,
     "bb05c02ca568f1e3ec93e421166ceb083b0a47de",
     "Func_6b84_relocatable",
     &gsr_func6b84_pic_kImageOrigin,
     &gsr_func6b84_pic_kImageSize,
     gsr_func6b84_pic_kDispatchTable,
     &gsr_func6b84_pic_kDispatchTableLen},
    // CORRECTS the earlier Func_7998 registration (2026-09-23): that image
    // started one ARM instruction late (ROM 0x08007998:0x18). Session
    // 20260923_202001 self-healed the same 28-byte piece (crc=0x66F864FE) at
    // nine stack bases, all equal to ROM 0x08007994:0x1C — smull, smlal,
    // ldm sp, smlal, lsl, orr, bx lr, no ELF symbol of its own, copied onto
    // the stack. The copier's own literal pool (ROM 0x080051C0) independently
    // confirms source 0x08007994 and size 0x1C (DMA3 CNT 0x84000007 = 7
    // words). config/usa/transient-func_7994-relocatable.toml.
    {0x08007994u,
     0,
     "1f75f819768ebe6da0a4200ec6d2f0f3ff29f663",
     "Func_7994_relocatable",
     &gsr_func7994_pic_kImageOrigin,
     &gsr_func7994_pic_kImageSize,
     gsr_func7994_pic_kDispatchTable,
     &gsr_func7994_pic_kDispatchTableLen},
    // Func_9e7c, copied to 0x03005AE0 (session 20260923_114927 compiled 18
    // pieces there at runtime, each byte-identical to ROM 0x08009e7c +
    // offset). goldensun.elf sizes it at 0x27c bytes of ARM with no $d.
    // config/usa/transient-func_9e7c-relocatable.toml.
    {0x08009E7Cu,
     0,
     "befbca336a97e12e686a9ce42c10793388f837c2",
     "Func_9e7c_relocatable",
     &gsr_func9e7c_pic_kImageOrigin,
     &gsr_func9e7c_pic_kImageSize,
     gsr_func9e7c_pic_kDispatchTable,
     &gsr_func9e7c_pic_kDispatchTableLen},
    // Func_a0f8, copied to 0x030057E0 (session 20260924_080129 compiled 2
    // pieces there at runtime, each byte-identical to ROM 0x0800a0f8 +
    // offset). goldensun.elf sizes it at 0x284 bytes of ARM with one
    // interior $d pool. config/usa/transient-func_a0f8-relocatable.toml.
    {0x0800A0F8u,
     0,
     "fc4539c4a9b4e13efc9022885c57dd23ea8fbb7a",
     "Func_a0f8_relocatable",
     &gsr_funca0f8_pic_kImageOrigin,
     &gsr_funca0f8_pic_kImageSize,
     gsr_funca0f8_pic_kDispatchTable,
     &gsr_funca0f8_pic_kDispatchTableLen},
    // Spell canvas "copy and fade": Func_54e4 DMA3-copies ROM 0x08001ea8,
    // 0x50 bytes (one ARM STT_FUNC entry, no interior $d) onto the stack.
    // CORRECTED 2026-09-23: was registered at 0x140 bytes, misreading the
    // wrapper's literal-pool word (a byte count the wrapper itself
    // right-shifts by 2 to get a DMA3 word count) as a word count. Session
    // 20260923_202001 content-matched two contiguous fragments spanning
    // offset 0 to the full 0x50-byte end (the second uniquely matching this
    // routine's own tail bytes, not Func_1ef8's/203c's/2098's) to runtime
    // base 0x03007c14. config/usa/transient-func_1ea8-relocatable.toml.
    {0x08001EA8u,
     0,
     "f2fe52c16d6b2287b19a632445629fe39d660ae5",
     "Func_1ea8_relocatable",
     &gsr_func1ea8_pic_kImageOrigin,
     &gsr_func1ea8_pic_kImageSize,
     gsr_func1ea8_pic_kDispatchTable,
     &gsr_func1ea8_pic_kDispatchTableLen},
    // Spell canvas "copy and halve": Func_5534 DMA3-copies ROM 0x08001ef8,
    // 0x40 bytes (one ARM STT_FUNC entry, no interior $d) onto the stack.
    // CORRECTED 2026-09-23: was registered at 0x100 bytes for the same
    // pool-word-shift reason as Func_1ea8 above. Session 20260923_171845
    // content-matched two contiguous fragments spanning offset 0 to the
    // full 0x40-byte end (the second uniquely matching this routine's own
    // tail bytes) to runtime base 0x03007c7c — exactly 0x03007c14 + 0x50,
    // the Func_1ea8 image's own confirmed instance placed right before it.
    // config/usa/transient-func_1ef8-relocatable.toml.
    {0x08001EF8u,
     0,
     "1967086826fcff37e022aac5bebc4e74b2a9978b",
     "Func_1ef8_relocatable",
     &gsr_func1ef8_pic_kImageOrigin,
     &gsr_func1ef8_pic_kImageSize,
     gsr_func1ef8_pic_kDispatchTable,
     &gsr_func1ef8_pic_kDispatchTableLen},
    // Func_1f38 (ROM 0x08001f38, 0x80 bytes, one ARM STT_FUNC entry, no
    // interior $d), DMA3-copied onto the stack by the wrapper at
    // 0x08005490 (same shape as 0x080054e4/0x08005534). Its pool at ROM
    // 0x080054D8 holds byte-count 0x80, DMA3 SAD reg 0x040000D4 and source
    // 0x08001F38. Session 20260923_171845 content-matched two contiguous
    // fragments spanning offset 0 to the full 0x80-byte end, both unique to
    // this routine, to runtime base 0x03007bcc. Found 2026-09-23 while
    // re-checking the Func_1ea8/Func_1ef8 wrapper pools.
    // config/usa/transient-func_1f38-relocatable.toml.
    {0x08001F38u,
     0,
     "ba870c47936e1e54df7cd9a7846110e663452a59",
     "Func_1f38_relocatable",
     &gsr_func1f38_pic_kImageOrigin,
     &gsr_func1f38_pic_kImageSize,
     gsr_func1f38_pic_kDispatchTable,
     &gsr_func1f38_pic_kDispatchTableLen},
    // Func_1fb8 (ROM 0x08001fb8, 0x84 bytes, one ARM STT_FUNC entry, no
    // interior $d), DMA3-copied onto the stack by the wrapper at
    // 0x0800543C (same shape as 0x080054e4/0x08005534). Its pool at ROM
    // 0x08005484 holds byte-count 0x84, DMA3 SAD reg 0x040000D4 and source
    // 0x08001FB8. NOT directly confirmed at a runtime base: the only
    // session-log crc32 matches found are 4-8 byte fragments outside the
    // stack region every other image in this family was observed at, too
    // short to rule out coincidence. entry_pc is a plausible generation
    // origin only (placed right after the confirmed Func_1f38 instance).
    // config/usa/transient-func_1fb8-relocatable.toml.
    {0x08001FB8u,
     0,
     "5f53a347bca3fb9556047098c0ea8d5532dd4bb2",
     "Func_1fb8_relocatable",
     &gsr_func1fb8_pic_kImageOrigin,
     &gsr_func1fb8_pic_kImageSize,
     gsr_func1fb8_pic_kDispatchTable,
     &gsr_func1fb8_pic_kDispatchTableLen},
    // CORRECTS the earlier Func_6c3a registration (2026-09-23): that image
    // covered only a 16-byte interior slice (ROM 0x08006c3a..0x08006c4a) of
    // this larger copied unit. Session 20260923_202001 self-healed two
    // 22-byte THUMB pieces at offset 0 (crc=0x6241EAC1), directly equal to
    // ROM 0x08006c24:0x16. goldensun.elf sizes the copied unit as Func_6c24
    // (THUMB $t at 0x08006c24, $d at 0x08006c4a, size 0x44) followed by
    // Func_6c68. Two literal pools (ROM 0x08006c9c and 0x08007a98) hold the
    // pair (0x08006c25, 0x08006c69) — Func_6c24's and Func_6c68's own THUMB
    // entries — and Func_6c68 computes end-start=0x44 from them to size the
    // copy. config/usa/transient-func_6c24-relocatable.toml.
    {0x08006C24u,
     1,
     "94dd9ae3cdd074edfbf78693bc50dbffe0ca3e39",
     "Func_6c24_relocatable",
     &gsr_func6c24_pic_kImageOrigin,
     &gsr_func6c24_pic_kImageSize,
     gsr_func6c24_pic_kDispatchTable,
     &gsr_func6c24_pic_kDispatchTableLen},
    // Func_203c: DMA3-copied onto the stack by the wrapper at 0x0800562c
    // (same shape as 0x080054e4/0x08005534). Its pool at ROM 0x08005670
    // holds byte-count 0x5C, DMA3 SAD reg 0x040000D4 and source 0x0800203C.
    // ROM 0x0800203c..0x08002098 is one coherent ARM routine, no interior
    // $d. NOT directly confirmed at a runtime base: re-deriving the
    // Func_1ea8 image (2026-09-23) resolved this image's earlier
    // placeholder origin (0x03007c14) to a confirmed Func_1ea8 instance
    // instead, so this image now uses an unattached label address
    // (0x03007e00). config/usa/transient-func_203c-relocatable.toml.
    {0x0800203Cu,
     0,
     "edbf9a7eabc225926dd5c0ba22d1b928afab1b02",
     "Func_203c_relocatable",
     &gsr_func203c_pic_kImageOrigin,
     &gsr_func203c_pic_kImageSize,
     gsr_func203c_pic_kDispatchTable,
     &gsr_func203c_pic_kDispatchTableLen},
    // Func_2098 (ROM 0x08002098, 0x5c bytes, one ARM STT_FUNC entry, no
    // interior $d, immediately following Func_203c in ROM), DMA3-copied
    // onto the stack by the wrapper at 0x0800567C (same shape as
    // 0x080054e4/0x08005534/0x0800562c). Its pool at ROM 0x080056C0 holds
    // byte-count 0x5C, DMA3 SAD reg 0x040000D4 and source 0x08002098. NOT
    // directly confirmed at a runtime base: shares its first 20 bytes with
    // Func_1ea8/Func_1ef8/Func_203c, and every logged occurrence of that
    // shared prologue crc is now explained by a longer, uniquely-matching
    // fragment belonging to Func_1ea8 or Func_1ef8 except one
    // (0x03007ce0, session 20260923_202001), used here as a plausible
    // generation origin only. Found 2026-09-23 while re-checking the
    // Func_1ea8/Func_1ef8 wrapper pools.
    // config/usa/transient-func_2098-relocatable.toml.
    {0x08002098u,
     0,
     "92f07a6eb708e30d86f375fd841dac38bf6dc905",
     "Func_2098_relocatable",
     &gsr_func2098_pic_kImageOrigin,
     &gsr_func2098_pic_kImageSize,
     gsr_func2098_pic_kDispatchTable,
     &gsr_func2098_pic_kDispatchTableLen},
    {0x08001B70u,
     0,
     "7e06447b0578b9d6ea559eda377aafdd6faecdbe",
     "Func_1b70_relocatable",
     &gsr_func1b70_pic_kImageOrigin,
     &gsr_func1b70_pic_kImageSize,
     gsr_func1b70_pic_kDispatchTable,
     &gsr_func1b70_pic_kDispatchTableLen},
    // Func_15afc and Func_15e10, the last two flash-driver routines still
    // pinned to a single base. 0x03003a84 was treated as a fixed staging slot
    // until the 10,800-frame campaign run aborted at ARM 0x030044f4: a DMA3
    // watch records ch=3 dad=0x030044F4 src=0x08015AFC word=0/158 (158 words =
    // 0x278 bytes) with first word 0xE92D0060, an ARM STMDB sp! prologue, and
    // goldensun.elf sizes Func_15afc at exactly 0x278 with an $a at the entry -
    // DMA extent and ELF extent agree. The SHA-1 below is the ROM source bytes
    // at 0x08015afc over that extent and matches, byte for byte, the identity
    // the live-verified fixed row at 0x03003a84 already carried, which is an
    // independent check that this is the same routine at a new base.
    //
    // Func_15e10 is converted alongside it rather than after its own abort: it
    // shares the same staging slot, so the pool will relocate it for the same
    // reason. Conversion REMOVES an address assumption rather than adding one,
    // and the resolver hashes the full extent before dispatching, so a wrong
    // base aborts loudly instead of running.
    //
    // ADDITIVE, like Func_1dc8: the fixed 0x03003a84 rows above stay. They are
    // a redundant identity gate here rather than a necessary one, but a
    // redundant gate is safe and a missing gate is not; deleting them is a
    // separate change that wants its own verified run.
    {0x08015AFCu,
     0,
     "f6ab01b56876f8d1b72a4bf5239bd5cbac620cb0",
     "Func_15afc_relocatable",
     &gsr_func15afc_pic_kImageOrigin,
     &gsr_func15afc_pic_kImageSize,
     gsr_func15afc_pic_kDispatchTable,
     &gsr_func15afc_pic_kDispatchTableLen},
    {0x08015E10u,
     0,
     "50cf8d2eaf6ba74b7ae74a97458d04214a00af57",
     "Func_15e10_relocatable",
     &gsr_func15e10_pic_kImageOrigin,
     &gsr_func15e10_pic_kImageSize,
     gsr_func15e10_pic_kDispatchTable,
     &gsr_func15e10_pic_kDispatchTableLen},
    // Closing the flash-driver pool FAMILY instead of discovering it one abort
    // at a time. After Func_15afc and Func_15e10 were relocated, the campaign
    // run advanced and then aborted at 0x030044f4 AGAIN - the same base, a
    // different routine. A DMA3 watch on that address shows the slot cycling
    // between src=0x08015AFC (158 words), src=0x08015E10 (31 words) and finally
    // src=0x08015D74 (39 words). It is one pool, reused.
    //
    // In goldensun.elf the ARM STT_FUNCs in the contiguous run
    // 0x08015430..0x08015e10 are exactly: 15430, 15570, 155d0, 158e8, 15afc,
    // 15d74, 15e10. (The odd-valued symbols interleaved with them have bit 0
    // set and are THUMB, not pool members.) Five were already registered, so
    // Func_15d74 and Func_155d0 are the whole remainder - a bounded set, not an
    // open-ended one.
    //
    // Func_15d74 is directly attributed by the DMA watch above: 39 words = 156
    // bytes, and the ELF sizes it at exactly 156 with an $a at the entry.
    // Func_155d0 is registered on the Func_15e10 precedent, which the very
    // first run vindicated: an image that is never copied simply never
    // verifies, while a missing one aborts a run.
    {0x08015D74u,
     0,
     "4058793ba5db0477fc9c5c556edb91984dee3de0",
     "Func_15d74_relocatable",
     &gsr_func15d74_pic_kImageOrigin,
     &gsr_func15d74_pic_kImageSize,
     gsr_func15d74_pic_kDispatchTable,
     &gsr_func15d74_pic_kDispatchTableLen},
    {0x080155D0u,
     0,
     "4b3fe199338f1ecb1399cf9668839e72386bf7bc",
     "Func_155d0_relocatable",
     &gsr_func155d0_pic_kImageOrigin,
     &gsr_func155d0_pic_kImageSize,
     gsr_func155d0_pic_kDispatchTable,
     &gsr_func155d0_pic_kDispatchTableLen},
    // Func_9bb8 copied SHORT. The flash driver copies this one routine at two
    // different lengths from the same source: a DMA3 watch on 0x03003eec across
    // one campaign run records src=0x08009BB8 word=0/177 (0x2c4, the full ELF
    // STT_FUNC extent, registered above) AND word=0/121 (0x1e4).
    //
    // These are two IMAGES, not a wrong extent. goldensun.elf puts a trailing
    // $d literal pool at 0x08009d90 running to Func_9e7c at 0x08009e7c; the
    // short copy ends at 0x08009d9c, i.e. every instruction (code ends at
    // 0x08009d90) plus the three literal words it needs. A SHA-1 over the full
    // 0x2c4 extent covers 224 bytes the short copy never wrote, so it can only
    // ever match the long copy.
    //
    // GENERAL LESSON, recorded in docs/GS011_TRANSIENT_IMAGES.md: an image
    // identity must cover the bytes the WRITER wrote, not the bytes a symbol
    // table says the routine spans. This is the same defect class as hashing an
    // overlay's mutable .data section.
    {0x08009BB8u,
     0,
     "49496bc9fb68cacac07613ccb2ead86bc0583ffc",
     "Func_9bb8_short_relocatable",
     &gsr_func9bb8short_pic_kImageOrigin,
     &gsr_func9bb8short_pic_kImageSize,
     gsr_func9bb8short_pic_kDispatchTable,
     &gsr_func9bb8short_pic_kDispatchTableLen},
    // Func_9bb8, CODE-ONLY extent - the Mt. Aleph crash fix.
    //
    // The game bump-allocates an IWRAM block, DMAs this routine in, and then
    // PATCHES the copy: it writes runtime values into the routine's trailing
    // literal pool. A live dump from a replayed player session shows every one
    // of the 224 differing bytes at or after offset 0x1d8 - exactly where
    // goldensun.elf ends the code ($d at 0x08009d90). Bytes [0, 0x1d8) match
    // ROM byte for byte; the code is never modified.
    //
    // So the 0x2c4 and 0x1e4 identities above CANNOT match a live image, and
    // the runtime aborted on a perfectly healthy copy. Ordering matters: this
    // row must be tried, and it is the only one whose extent excludes bytes the
    // game writes.
    //
    // Third instance of one defect class: an identity must cover only bytes the
    // WRITER leaves alone - not an ELF extent, not a copy length.
    {0x08009BB8u,
     0,
     "36f6cde400bd706d9418bf212705dcfda0a02d94",
     "Func_9bb8_code_relocatable",
     &gsr_func9bb8code_pic_kImageOrigin,
     &gsr_func9bb8code_pic_kImageSize,
     gsr_func9bb8code_pic_kDispatchTable,
     &gsr_func9bb8code_pic_kDispatchTableLen},
    // Func_b5138, a transient allocator block. Trace events #55889096-105
    // (local/crash/terminal.log): allocator base 0x0300207c, next-free
    // 0x030022ac; DMA3SAD=0x080b5138, DMA3DAD=0x0300207c,
    // DMA3CNT=0x8400008c (0x230 bytes, matching the ELF STT_FUNC size
    // exactly); exchange cpsr=0x1f, T clear => ARM. See
    // config/usa/transient-func-b5138-relocatable.toml and
    // docs/GS011_TRANSIENT_IMAGES.md.
    //
    // The image self-relocates a jump table into its own extent (see
    // kFuncB5138ExcludedRanges above), so the whole-extent hash
    // "ec38094f7b1c2b14f7af17fdeb0072aa84d34ebe" stops matching once that
    // loop has run. The SHA-1 below is instead computed over the extent with
    // both declared data ranges removed: ROM 0x080b5138..0x080b5138+0x230,
    // concatenating [0,0xcc) + [0xd4,0xe0) + [0x120,0x230) (488 bytes), from
    // the hash-verified ROM.
    {0x080B5138u,
     0,
     "db6d5389162f3f8971ef3247ee7ed62596c5ed4d",
     "Func_b5138_relocatable",
     &gsr_funcb5138_pic_kImageOrigin,
     &gsr_funcb5138_pic_kImageSize,
     gsr_funcb5138_pic_kDispatchTable,
     &gsr_funcb5138_pic_kDispatchTableLen,
     kFuncB5138ExcludedRanges,
     2},
    // Func_f0024, the ending credits' decoder. Session 20261002_134927 hung
    // at the credits running it through the interpreter: "unknown transient
    // code identity at 0x0300347C", whose live prefix occurs once in the ROM,
    // at 0x080f0024. Same layout as Func_b5138 (same 0x230 extent, same two
    // $d runs, same self-relocated jump table), so it reuses
    // kFuncB5138ExcludedRanges; the SHA-1 is over [0,0xcc) + [0xd4,0xe0) +
    // [0x120,0x230) of ROM 0x080f0024. See
    // config/usa/transient-func-f0024-relocatable.toml.
    {0x080F0024u,
     0,
     "603f4e8cb1f2e0603244cadea88fc355448ebe86",
     "Func_f0024_relocatable",
     &gsr_funcf0024_pic_kImageOrigin,
     &gsr_funcf0024_pic_kImageSize,
     gsr_funcf0024_pic_kDispatchTable,
     &gsr_funcf0024_pic_kDispatchTableLen,
     kFuncB5138ExcludedRanges,
     2},
    // Func_a418, previously registered only fixed at 0x03003400 (see
    // kTransientCodeImages above). A savestate-frame-15982 run aborted with
    // "unknown transient code identity at 0x03002000"; all 16 fixed images and
    // the Func_15430 declared occupant of that slot failed identity (292
    // differing bytes). Searching the hash-verified ROM for the live 32-byte
    // prefix (`BICS r1,r1,#7; TSTNE r2,#7; BXEQ lr; PUSH {r5-r11}; MOV r12,r1;
    // LDM r0,{r4,r5}; ADD r0,r0,r1; LDM r0,{r6,r7}`) finds exactly one match, at
    // ROM 0x0800a418 - the very Func_a418 already registered at 0x03003400.
    // goldensun.elf sizes it at exactly 0x7c with a single $a mapping symbol
    // and no interior $d, matching the observed ARM entry (cpsr=0x1f, T
    // clear). Independent extent confirmation: rom_9000/src/rom_b798.s
    // (Func_bb20) DMA3s SAD=Func_a418, DAD=an allocator-returned buffer,
    // CNT=0x84000000|0x1f (enable, 32-bit, 31 words = 0x7c bytes) - agreeing
    // with the ELF size exactly - and the computed image SHA-1 matches the
    // 0x03003400 identity byte for byte. See
    // config/usa/transient-func-a418-relocatable.toml.
    {0x0800A418u,
     0,
     "d3261b6f7b57ba4e7da8e44cf555d009c7cc27d1",
     "Func_a418_relocatable",
     &gsr_funca418_pic_kImageOrigin,
     &gsr_funca418_pic_kImageSize,
     gsr_funca418_pic_kDispatchTable,
     &gsr_funca418_pic_kDispatchTableLen},
}};

struct GoldenSunFunc1dc8WriterCache {
    bool valid = false;
    bool image_verified = false;
    std::uint32_t base = 0;
    std::uint64_t generation = 0;
    std::array<unsigned int, 64> page_epochs{};
};
std::array<GoldenSunFunc1dc8WriterCache, 4>
    g_golden_sun_func1dc8_writer_cache{};

void reset_golden_sun_func1dc8_writer_diagnostics() {
    g_golden_sun_func1dc8_writer_cache.fill({});
    g_golden_sun_func1dc8_writer_recognized = 0;
    g_golden_sun_func1dc8_writer_identity_mismatch = 0;
    g_golden_sun_func1dc8_writer_unknown_variant = 0;
    g_golden_sun_func1dc8_writer_logs = 0;
    g_golden_sun_func1dc8_last_base = 0;
    g_golden_sun_func1dc8_last_generation = 0;
}

const char* golden_sun_func1dc8_writer_route_name(
    gsr::Func1dc8WriterRoute route) {
    switch (route) {
        case gsr::Func1dc8WriterRoute::D4: return "D4";
        case gsr::Func1dc8WriterRoute::EC: return "EC";
        case gsr::Func1dc8WriterRoute::F0: return "F0";
        case gsr::Func1dc8WriterRoute::Count: break;
    }
    return "unknown";
}

bool golden_sun_func1dc8_writer_pc(std::uint32_t pc,
                                   gsr::Func1dc8WriterRoute route) {
    const bool diagnostics = golden_sun_wide_diagnostics_enabled();
    // Preserve the previously proven fixed route when diagnostics are off;
    // identity work is strictly observational and must not affect gameplay.
    if (!diagnostics) {
        const std::uint32_t fixed =
            gsr::kFunc1dc8KnownFixedBase + gsr::writer_offset(route);
        return pc == fixed;
    }

    const std::uint32_t offset = gsr::writer_offset(route);
    if (offset == 0u || pc < offset) {
        ++g_golden_sun_func1dc8_writer_unknown_variant;
        return false;
    }
    const std::uint32_t base = pc - offset;
    if (base < 0x02000000u ||
        base + gsr::kFunc1dc8ImageSize > 0x04000000u ||
        (base & 3u) != 0u) {
        ++g_golden_sun_func1dc8_writer_unknown_variant;
        return false;
    }

    const RelocatableCodeImage* image = nullptr;
    for (const auto& candidate : kRelocatableCodeImages) {
        if (std::strcmp(candidate.name, "Func_1dc8_relocatable") == 0) {
            image = &candidate;
            break;
        }
    }
    if (!image) {
        ++g_golden_sun_func1dc8_writer_unknown_variant;
        return false;
    }

    auto& cache = g_golden_sun_func1dc8_writer_cache[
        static_cast<std::size_t>(route)];
    const bool current = cache.valid && cache.base == base &&
        gsr::ram_range_pages_current(
            base, base + *image->size, cache.page_epochs,
            ram_code_page_epoch);
    bool verified = false;
    if (current) {
        verified = cache.image_verified;
    } else {
        bool prefix_passed = false;
        ++g_func1dc8_writer_hashes;
        verified = relocatable_resident_at(*image, base, &prefix_passed);
        cache = {};
        cache.valid = true;
        cache.image_verified = verified;
        cache.base = base;
        cache.generation = g_ram_write_epoch;
        gsr::ram_range_register_mask(
            base, base + *image->size, &g_ram_code_page_mask_iwram,
            &g_ram_code_page_mask_ewram_lo, &g_ram_code_page_mask_ewram_hi);
        gsr::save_ram_range_page_epochs(
            base, base + *image->size, cache.page_epochs,
            ram_code_page_epoch);
    }
    const gsr::Func1dc8WriterIdentity identity{
        verified, gsr::kFunc1dc8ImageKey, base, cache.generation};
    const auto resolved = gsr::resolve_func1dc8_writer(
        pc, route, gsr::kFunc1dc8ImageKey, cache.generation, identity);
    if (!resolved.recognized) {
        if (resolved.identity_mismatch)
            ++g_golden_sun_func1dc8_writer_identity_mismatch;
        else
            ++g_golden_sun_func1dc8_writer_unknown_variant;
        return false;
    }
    ++g_golden_sun_func1dc8_writer_recognized;
    g_golden_sun_func1dc8_last_base = resolved.base;
    g_golden_sun_func1dc8_last_generation = cache.generation;
    if (g_golden_sun_func1dc8_writer_logs < 64u) {
        ++g_golden_sun_func1dc8_writer_logs;
        std::fprintf(stderr,
            "[wide-obj-writer] frame=%llu route=%s pc=0x%08x base=0x%08x "
            "image_key=0x%08x generation=%llu verified=1\n",
            static_cast<unsigned long long>(runtime_current_frame()),
            golden_sun_func1dc8_writer_route_name(route), pc, resolved.base,
            gsr::kFunc1dc8ImageKey,
            static_cast<unsigned long long>(cache.generation));
    }
    return true;
}

// Last base each relocatable image was verified at. A pooled routine is
// entered many times in a row at the same base, so trying it first keeps the
// common case to one hash instead of a scan over every entry offset. It is a
// hint only — the identity hash still has to pass.
struct RelocatableBaseHint {
    std::uint32_t base;
    bool valid;
};
std::array<RelocatableBaseHint, kRelocatableCodeImages.size()>
    g_relocatable_base_hint{};

struct RelocatableProfileImage {
    std::uint64_t visits = 0;
    std::uint64_t hint_attempts = 0;
    std::uint64_t hint_hits = 0;
    std::uint64_t scan_candidates = 0;
    std::uint64_t prefix_passes = 0;
    std::uint64_t hash_matches = 0;
    std::uint64_t wins = 0;
    std::uint64_t entry_wins = 0;
    std::uint64_t base_switches = 0;
    std::vector<std::uint32_t> bases;
};

struct RelocatableProfilePc {
    std::uint32_t pc = 0;
    std::uint64_t calls = 0;
    std::uint64_t candidates = 0;
    std::uint64_t wins = 0;
};

std::array<RelocatableProfileImage, kRelocatableCodeImages.size()>
    g_relocatable_profile_images{};
std::vector<RelocatableProfilePc> g_relocatable_profile_pcs;
std::uint64_t g_relocatable_profile_calls = 0;
std::uint64_t g_relocatable_profile_wins = 0;
std::uint64_t g_relocatable_profile_active_samples = 0;
std::uint64_t g_relocatable_profile_active_sum = 0;
std::uint32_t g_relocatable_profile_active_max = 0;

// GBARECOMP_RELOCATABLE_PROFILE, if explicitly set, wins outright. Otherwise
// falls back to the config UI's "Additional debug logging" toggle (default
// OFF) — see runtime_arm.h. First call happens from live dispatch, well
// after the config UI has loaded config.ini, so this reflects a saved
// preference from the very first relocatable dispatch of the session
// (cached after that, matching the existing once-per-run idiom).
bool relocatable_profile_enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GBARECOMP_RELOCATABLE_PROFILE");
        return env ? (env[0] != '\0' && env[0] != '0')
                   : (gsr_additional_debug_logging() != 0);
    }();
    return enabled;
}

RelocatableProfilePc& relocatable_profile_pc(std::uint32_t pc) {
    for (auto& entry : g_relocatable_profile_pcs) {
        if (entry.pc == pc) return entry;
    }
    g_relocatable_profile_pcs.push_back({pc});
    return g_relocatable_profile_pcs.back();
}

void report_relocatable_profile() {
    std::fprintf(stderr,
        "RELOC_PROFILE calls=%llu wins=%llu distinct_pcs=%zu "
        "active_samples=%llu active_average=%.2f active_max=%u\n",
        static_cast<unsigned long long>(g_relocatable_profile_calls),
        static_cast<unsigned long long>(g_relocatable_profile_wins),
        g_relocatable_profile_pcs.size(),
        static_cast<unsigned long long>(g_relocatable_profile_active_samples),
        g_relocatable_profile_active_samples == 0
            ? 0.0
            : static_cast<double>(g_relocatable_profile_active_sum) /
                  g_relocatable_profile_active_samples,
        g_relocatable_profile_active_max);
    for (std::size_t i = 0; i < kRelocatableCodeImages.size(); ++i) {
        const auto& image = kRelocatableCodeImages[i];
        const auto& stats = g_relocatable_profile_images[i];
        std::fprintf(stderr,
            "RELOC_IMAGE index=%zu name=%s visits=%llu hint_attempts=%llu "
            "hint_hits=%llu scan_candidates=%llu prefix_passes=%llu "
            "hash_matches=%llu wins=%llu entry_wins=%llu bases=%zu "
            "base_switches=%llu\n",
            i, image.name,
            static_cast<unsigned long long>(stats.visits),
            static_cast<unsigned long long>(stats.hint_attempts),
            static_cast<unsigned long long>(stats.hint_hits),
            static_cast<unsigned long long>(stats.scan_candidates),
            static_cast<unsigned long long>(stats.prefix_passes),
            static_cast<unsigned long long>(stats.hash_matches),
            static_cast<unsigned long long>(stats.wins),
            static_cast<unsigned long long>(stats.entry_wins),
            stats.bases.size(),
            static_cast<unsigned long long>(stats.base_switches));
        if (!stats.bases.empty()) {
            std::fprintf(stderr, "RELOC_BASES index=%zu", i);
            for (const std::uint32_t base : stats.bases) {
                std::fprintf(stderr, " 0x%08X", base);
            }
            std::fprintf(stderr, "\n");
        }
    }
    for (const auto& stats : g_relocatable_profile_pcs) {
        if (stats.wins != 0 || stats.candidates >= 1000) {
            std::fprintf(stderr,
                "RELOC_PC pc=0x%08X calls=%llu candidates=%llu wins=%llu\n",
                stats.pc,
                static_cast<unsigned long long>(stats.calls),
                static_cast<unsigned long long>(stats.candidates),
                static_cast<unsigned long long>(stats.wins));
        }
    }
}

// GBARECOMP_RELOCATABLE_LOG=1 reports each (image, base) pair the first time
// it is verified. The whole point of a position-independent image is that the
// base set is NOT enumerated ahead of time, so this is the only way to see
// which bases a run actually used — and the only honest way to claim the
// mechanism replaced a set of per-base registrations rather than skipping
// them. Off by default; one getenv at startup, no per-dispatch cost.
// Same env-wins/toggle-fallback precedence as relocatable_profile_enabled
// above.
bool relocatable_log_enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GBARECOMP_RELOCATABLE_LOG");
        return env ? (env[0] != '\0' && env[0] != '0')
                   : (gsr_additional_debug_logging() != 0);
    }();
    return enabled;
}

void (*lookup_entry(const DispatchEntry* table, unsigned len,
                    std::uint32_t addr, int thumb))(void) {
    std::size_t lo = 0;
    std::size_t hi = len;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const DispatchEntry& entry = table[mid];
        if (entry.addr < addr || (entry.addr == addr && entry.thumb < thumb)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < len) {
        const DispatchEntry& entry = table[lo];
        if (entry.addr == addr && entry.thumb == thumb) return entry.fn;
    }
    return nullptr;
}

std::string live_sha1(std::uint32_t start, std::uint32_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::uint32_t offset = 0; offset < size; ++offset) {
        bytes[offset] = bus_read_u8(start + offset);
    }
    return gba::sha1(bytes.data(), bytes.size()).hex();
}

// A single bus_fast_ram() lookup per byte re-does the same region/mask check
// and pointer add every time; for a whole-extent hash that adds up over a
// ~700-byte image. When [base, base+size) provably stays inside one fast-RAM
// backing array (no IWRAM/EWRAM/mirror-boundary crossing), resolve the
// pointer once and let the caller index it directly instead. Returns null
// (never unsafe to do so — the caller falls back to the per-byte slow path)
// whenever fast RAM isn't available or the range isn't provably contiguous.
const std::uint8_t* bus_fast_ram_contiguous(std::uint32_t base,
                                            std::uint32_t size) {
    if (size == 0) return nullptr;
    if ((base >> 24) != ((base + size - 1u) >> 24)) return nullptr;
    return bus_fast_ram(base);
}

// Would `image` be resident at `base`? Rejects the base outright when it does
// not lie in RAM or is misaligned for the image's mode, then screens on the
// first word before paying for a hash of the whole extent.
bool relocatable_resident_at(const RelocatableCodeImage& image,
                             std::uint32_t base, bool* prefix_passed) {
    *prefix_passed = false;
    const std::uint32_t size = *image.size;
    if (base < 0x02000000u || base + size > 0x04000000u) return false;
    if (base & (image.thumb ? 1u : 3u)) return false;
    if (image.rom_start != 0u &&
        bus_read_u32(base) != bus_read_u32(image.rom_start)) {
        return false;
    }
    *prefix_passed = true;
    ++g_relocatable_resident_hashes;
    g_relocatable_resident_hash_bytes += size;
    if (const std::uint8_t* fast = bus_fast_ram_contiguous(base, size)) {
        return gsr::sha1_excluding(
                   size,
                   [fast](std::uint32_t offset) { return fast[offset]; },
                   image.excluded_ranges, image.excluded_ranges_len) ==
               image.sha1;
    }
    return gsr::sha1_excluding(
               size,
               [base](std::uint32_t offset) { return bus_read_u8(base + offset); },
               image.excluded_ranges, image.excluded_ranges_len) == image.sha1;
}

// --- Cached relocatable identity (GS-cost: per-dispatch SHA-1 rehash) ----
//
// try_relocatable_dispatch's hint path calls relocatable_resident_at above
// on EVERY dispatch through a pooled routine, even though the hint means
// "this (image, base) pair already verified last time and nothing else has
// touched it." One cache entry per image, keyed on the base it was last
// verified at, lets a repeat dispatch skip the hash entirely while the pages
// the image occupies are provably unchanged — see relocatable_identity.h's
// "Page-epoch cache core" comment for why mask coverage has to be proven
// per-base here rather than computed once at startup like the fixed-address
// sibling's g_verified_identity_cache.
//
// Several entries per image, for matches and non-matches alike: the scan path
// (a dispatch at a base other than the hint's) ran 17,494 whole-image hashes
// (10.3 MB) over one slot-8 Ragnarok run, ~11% of the heaviest frames
// (FACTS.md). A non-match entry is only ever answered by the exact byte
// compare, never by page epochs.
struct RelocatableIdentityCacheEntry {
    bool valid = false;
    bool matched = false;
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    std::array<unsigned int, 64> page_epochs{};
    std::vector<std::uint8_t> snapshot;
};
constexpr std::size_t kRelocatableIdentityCacheWays = 4;
struct RelocatableIdentityCacheSet {
    std::array<RelocatableIdentityCacheEntry, kRelocatableIdentityCacheWays>
        ways{};
    std::size_t next = 0;  // round-robin replacement
};
std::array<RelocatableIdentityCacheSet, kRelocatableCodeImages.size()>
    g_relocatable_identity_cache{};
unsigned long long g_relocatable_identity_hashes = 0;
unsigned long long g_relocatable_identity_cache_hits = 0;
// Epoch/local-word check failed but the live bytes still equal the snapshot
// taken at the last matching SHA-1, so the hash was skipped.
unsigned long long g_relocatable_identity_snapshot_hits = 0;

unsigned int ram_code_page_epoch(std::uint32_t addr);

// True when every byte the identity hash covers (i.e. outside
// image.excluded_ranges, exactly as sha1_excluding skips them) equals the
// snapshot. Identical hashed bytes give an identical SHA-1, so a true result
// is the same conclusion the hash would reach for a previously matched image.
bool relocatable_live_matches_snapshot(const RelocatableCodeImage& image,
                                       std::uint32_t base, std::uint32_t size,
                                       const std::vector<std::uint8_t>& snapshot) {
    if (snapshot.size() != size) return false;
    const std::uint8_t* fast = bus_fast_ram_contiguous(base, size);
    if (fast && image.excluded_ranges_len == 0) {
        return std::memcmp(fast, snapshot.data(), size) == 0;
    }
    // With excluded ranges: memcmp over the spans between them. The per-byte
    // loop below tested every range for every byte and was 7% of the
    // heaviest Nereid frames (logs/session_20260930_190323.hostprof.txt).
    std::array<gsr::ByteRange, 16> excluded{};
    if (fast && image.excluded_ranges_len <= excluded.size()) {
        std::size_t count = 0;
        for (unsigned i = 0; i < image.excluded_ranges_len; ++i) {
            const std::uint32_t start = std::min(image.excluded_ranges[i].start, size);
            const std::uint32_t end = std::min(image.excluded_ranges[i].end, size);
            if (start < end) excluded[count++] = {start, end};
        }
        std::sort(excluded.begin(), excluded.begin() + count,
                  [](const gsr::ByteRange& a, const gsr::ByteRange& b) {
                      return a.start < b.start;
                  });
        std::uint32_t at = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (excluded[i].start > at &&
                std::memcmp(fast + at, snapshot.data() + at,
                            excluded[i].start - at) != 0) {
                return false;
            }
            at = std::max(at, excluded[i].end);
        }
        return at >= size ||
               std::memcmp(fast + at, snapshot.data() + at, size - at) == 0;
    }
    for (std::uint32_t offset = 0; offset < size; ++offset) {
        if (gsr::byte_range_excludes(offset, image.excluded_ranges,
                                     image.excluded_ranges_len)) {
            continue;
        }
        const std::uint8_t live =
            fast ? fast[offset]
                 : static_cast<std::uint8_t>(bus_read_u8(base + offset));
        if (live != snapshot[offset]) return false;
    }
    return true;
}

// Cache-aware sibling of relocatable_resident_at, used by both the hint path
// and the scan in try_relocatable_dispatch. Never concludes anything
// relocatable_resident_at would not: the page-epoch hit only ever answers
// `true` for an entry installed on a real match, the byte compare answers
// with the result the hash gave for those same bytes, and anything else falls
// through to the same byte-exact hash the uncached path runs, which is the
// sole authority over whether the image matches.
bool relocatable_resident_at_cached(std::size_t index,
                                    const RelocatableCodeImage& image,
                                    std::uint32_t base, std::uint32_t pc,
                                    bool* prefix_passed) {
    *prefix_passed = false;
    const std::uint32_t size = *image.size;
    if (base < 0x02000000u || base + size > 0x04000000u) return false;
    if (base & (image.thumb ? 1u : 3u)) return false;
    if (image.rom_start != 0u &&
        bus_read_u32(base) != bus_read_u32(image.rom_start)) {
        return false;
    }
    *prefix_passed = true;

    auto& set = g_relocatable_identity_cache[index];
    RelocatableIdentityCacheEntry* cache = nullptr;
    for (auto& way : set.ways) {
        if (way.valid && way.base == base && way.size == size) {
            cache = &way;
            break;
        }
    }
    if (cache && cache->matched &&
        gsr::ram_range_pages_current(base, base + size, cache->page_epochs,
                                     ram_code_page_epoch) &&
        gsr::identity_local_words_current(
            base, pc, cache->snapshot.data(), cache->size,
            image.excluded_ranges, image.excluded_ranges_len, bus_read_u32)) {
        ++g_relocatable_identity_cache_hits;
        return true;
    }

    // Epochs or local words say "maybe changed" (an unrelated write in a
    // shared page is common), or this is a remembered non-match. Exact byte
    // compare against the snapshot the hash judged before paying SHA-1.
    if (cache &&
        relocatable_live_matches_snapshot(image, base, size, cache->snapshot)) {
        if (cache->matched) {
            gsr::ram_range_register_mask(base, base + size,
                                         &g_ram_code_page_mask_iwram,
                                         &g_ram_code_page_mask_ewram_lo,
                                         &g_ram_code_page_mask_ewram_hi);
            gsr::save_ram_range_page_epochs(base, base + size,
                                            cache->page_epochs,
                                            ram_code_page_epoch);
        }
        ++g_relocatable_identity_snapshot_hits;
        return cache->matched;
    }

    ++g_relocatable_identity_hashes;
    const bool matched = relocatable_resident_at(image, base, prefix_passed);
    if (!cache) {
        cache = &set.ways[set.next];
        set.next = (set.next + 1u) % set.ways.size();
    }
    cache->valid = true;
    cache->matched = matched;
    cache->base = base;
    cache->size = size;
    cache->snapshot.resize(size);
    if (const std::uint8_t* fast = bus_fast_ram_contiguous(base, size)) {
        std::memcpy(cache->snapshot.data(), fast, size);
    } else {
        for (std::uint32_t offset = 0; offset < size; ++offset) {
            cache->snapshot[offset] = bus_read_u8(base + offset);
        }
    }
    if (matched) {
        gsr::ram_range_register_mask(base, base + size,
                                     &g_ram_code_page_mask_iwram,
                                     &g_ram_code_page_mask_ewram_lo,
                                     &g_ram_code_page_mask_ewram_hi);
        gsr::save_ram_range_page_epochs(base, base + size, cache->page_epochs,
                                        ram_code_page_epoch);
    }
    return matched;
}

void sample_known_active_relocatable_placements() {
    std::uint32_t active = 0;
    for (std::size_t index = 0; index < kRelocatableCodeImages.size(); ++index) {
        const auto& image = kRelocatableCodeImages[index];
        for (const std::uint32_t base : g_relocatable_profile_images[index].bases) {
            bool prefix_passed = false;
            if (relocatable_resident_at(image, base, &prefix_passed)) ++active;
        }
    }
    ++g_relocatable_profile_active_samples;
    g_relocatable_profile_active_sum += active;
    if (active > g_relocatable_profile_active_max) {
        g_relocatable_profile_active_max = active;
    }
}

struct RelocatableOpcodeCandidate {
    std::uint32_t opcode;
    std::uint16_t image_index;
    const DispatchEntry* entry;
};

const std::vector<RelocatableOpcodeCandidate>& relocatable_opcode_candidates(
    int thumb) {
    static const std::vector<RelocatableOpcodeCandidate> arm = [] {
        std::vector<RelocatableOpcodeCandidate> result;
        for (std::size_t index = 0; index < kRelocatableCodeImages.size();
             ++index) {
            const auto& image = kRelocatableCodeImages[index];
            if (image.thumb || image.rom_start == 0u) continue;
            const std::uint32_t origin = *image.origin;
            const std::uint32_t size = *image.size;
            for (unsigned i = 0; i < *image.dispatch_table_len; ++i) {
                const DispatchEntry& entry = image.dispatch_table[i];
                const std::uint32_t offset = entry.addr - origin;
                if (entry.thumb || offset >= size) continue;
                result.push_back({bus_read_u32(image.rom_start + offset),
                                  static_cast<std::uint16_t>(index), &entry});
            }
        }
        std::sort(result.begin(), result.end(), [](const auto& left,
                                                   const auto& right) {
            if (left.opcode != right.opcode) return left.opcode < right.opcode;
            if (left.image_index != right.image_index) {
                return left.image_index < right.image_index;
            }
            return left.entry->addr < right.entry->addr;
        });
        return result;
    }();
    static const std::vector<RelocatableOpcodeCandidate> thumb_candidates = [] {
        std::vector<RelocatableOpcodeCandidate> result;
        for (std::size_t index = 0; index < kRelocatableCodeImages.size();
             ++index) {
            const auto& image = kRelocatableCodeImages[index];
            if (!image.thumb || image.rom_start == 0u) continue;
            const std::uint32_t origin = *image.origin;
            const std::uint32_t size = *image.size;
            for (unsigned i = 0; i < *image.dispatch_table_len; ++i) {
                const DispatchEntry& entry = image.dispatch_table[i];
                const std::uint32_t offset = entry.addr - origin;
                if (!entry.thumb || offset >= size) continue;
                result.push_back({bus_read_u16(image.rom_start + offset),
                                  static_cast<std::uint16_t>(index), &entry});
            }
        }
        std::sort(result.begin(), result.end(), [](const auto& left,
                                                   const auto& right) {
            if (left.opcode != right.opcode) return left.opcode < right.opcode;
            if (left.image_index != right.image_index) {
                return left.image_index < right.image_index;
            }
            return left.entry->addr < right.entry->addr;
        });
        return result;
    }();
    return thumb ? thumb_candidates : arm;
}

RuntimeGuestFn try_relocatable_dispatch(std::uint32_t pc, int thumb) {
    const bool profiling = relocatable_profile_enabled();
    RelocatableProfilePc* pc_stats = nullptr;
    if (profiling) {
        if (g_relocatable_profile_calls++ == 0) {
            std::atexit(report_relocatable_profile);
        }
        pc_stats = &relocatable_profile_pc(pc);
        ++pc_stats->calls;
    }
    auto attempt = [&](std::size_t index, std::uint32_t base,
                       RuntimeGuestFn known_fn, bool from_hint) -> RuntimeGuestFn {
        const auto& image = kRelocatableCodeImages[index];
        auto& image_stats = g_relocatable_profile_images[index];
        const std::uint32_t origin = *image.origin;
        const std::uint32_t size = *image.size;
        if (profiling) {
            if (from_hint) ++image_stats.hint_attempts;
            else ++image_stats.scan_candidates;
            ++pc_stats->candidates;
        }
        const std::uint32_t offset = pc - base;
        if (offset >= size) return nullptr;
        RuntimeGuestFn fn = known_fn;
        if (fn == nullptr) {
            fn = lookup_entry(image.dispatch_table, *image.dispatch_table_len,
                              origin + offset, thumb);
        }
        if (fn == nullptr) return nullptr;
        bool prefix_passed = false;
        const bool resident = relocatable_resident_at_cached(
            index, image, base, pc, &prefix_passed);
        if (!resident) {
            if (profiling && prefix_passed) ++image_stats.prefix_passes;
            return nullptr;
        }
        if (profiling) {
            ++image_stats.prefix_passes;
            ++image_stats.hash_matches;
            ++image_stats.wins;
            if (from_hint) ++image_stats.hint_hits;
            if (offset == 0) ++image_stats.entry_wins;
            ++g_relocatable_profile_wins;
            ++pc_stats->wins;
            bool known_base = false;
            for (const std::uint32_t known : image_stats.bases) {
                if (known == base) known_base = true;
            }
            if (!known_base) image_stats.bases.push_back(base);
            if (g_relocatable_base_hint[index].valid &&
                g_relocatable_base_hint[index].base != base) {
                ++image_stats.base_switches;
            }
            if (!from_hint) sample_known_active_relocatable_placements();
        }
        if (relocatable_log_enabled() &&
            !(g_relocatable_base_hint[index].valid &&
              g_relocatable_base_hint[index].base == base)) {
            std::fprintf(stderr,
                "GoldenSunRecomp: relocatable %s verified at base "
                "0x%08X (entry pc=0x%08X offset=0x%X)\n",
                image.name, base, pc, offset);
        }
        g_relocatable_base_hint[index] = {base, true};
        g_runtime_image_base = base;
        return fn;
    };

    // Try every image's last verified base before doing any discovery work.
    // A later image's good hint must not sit behind an earlier image's scan.
    for (std::size_t index = 0; index < kRelocatableCodeImages.size();
         ++index) {
        const auto& image = kRelocatableCodeImages[index];
        if (thumb != image.thumb) continue;
        if (profiling) ++g_relocatable_profile_images[index].visits;
        const RelocatableBaseHint hint = g_relocatable_base_hint[index];
        if (hint.valid) {
            if (RuntimeGuestFn fn = attempt(index, hint.base, nullptr, true))
                return fn;
        }
    }

    // Whole-image identity can only match when the live instruction at `pc`
    // equals the immutable instruction at the same image offset. Indexing by
    // opcode turns the old all-entry scan into a small exact candidate set;
    // the full identity hash below remains the authority.
    const std::uint32_t live_opcode =
        thumb ? bus_read_u16(pc) : bus_read_u32(pc);
    const auto& candidates = relocatable_opcode_candidates(thumb);
    auto first = std::lower_bound(
        candidates.begin(), candidates.end(), live_opcode,
        [](const RelocatableOpcodeCandidate& candidate, std::uint32_t opcode) {
            return candidate.opcode < opcode;
        });
    auto last = std::upper_bound(
        first, candidates.end(), live_opcode,
        [](std::uint32_t opcode, const RelocatableOpcodeCandidate& candidate) {
            return opcode < candidate.opcode;
        });
    for (auto it = first; it != last; ++it) {
        const auto& image = kRelocatableCodeImages[it->image_index];
        const std::uint32_t offset = it->entry->addr - *image.origin;
        const std::uint32_t base = pc - offset;
        const RelocatableBaseHint hint =
            g_relocatable_base_hint[it->image_index];
        if (hint.valid && base == hint.base) continue;
        if (RuntimeGuestFn fn =
                attempt(it->image_index, base, it->entry->fn, false)) {
            return fn;
        }
    }
    // Images without immutable ROM backing cannot use the opcode index. Keep
    // the old exhaustive discovery path for them; identity hashing remains the
    // authority and current ROM-backed images pay no cost here.
    static const bool has_unbacked_image = [] {
        for (const auto& image : kRelocatableCodeImages) {
            if (image.rom_start == 0u) return true;
        }
        return false;
    }();
    if (has_unbacked_image) {
        for (std::size_t index = 0; index < kRelocatableCodeImages.size();
             ++index) {
            const auto& image = kRelocatableCodeImages[index];
            if (thumb != image.thumb || image.rom_start != 0u) continue;
            const std::uint32_t origin = *image.origin;
            const std::uint32_t size = *image.size;
            for (unsigned i = 0; i < *image.dispatch_table_len; ++i) {
                const DispatchEntry& entry = image.dispatch_table[i];
                const std::uint32_t offset = entry.addr - origin;
                if (entry.thumb != thumb || offset >= size) continue;
                const std::uint32_t base = pc - offset;
                const RelocatableBaseHint hint =
                    g_relocatable_base_hint[index];
                if (hint.valid && base == hint.base) continue;
                if (RuntimeGuestFn fn = attempt(index, base, entry.fn, false))
                    return fn;
            }
        }
    }
    return nullptr;
}

void (*lookup_variant(const TransientCodeImage& image,
                      std::uint32_t pc, int thumb))(void) {
    return lookup_entry(image.dispatch_table, *image.dispatch_table_len, pc,
                        thumb);
}

// Every abort below ends a strict-static run, so the ring of recent branch,
// dispatch and store events is the only remaining evidence about which writer
// installed the bytes at the failing PC. GBARECOMP_TRACE_DUMP_DEPTH tunes it.
void dump_recent_trace() {
    std::uint32_t depth = 160u;
    if (const char* env = std::getenv("GBARECOMP_TRACE_DUMP_DEPTH")) {
        const unsigned long parsed = std::strtoul(env, nullptr, 0);
        if (parsed != 0ul) depth = static_cast<std::uint32_t>(parsed);
    }
    runtime_trace_dump_recent(depth);
}

// --- RAM code the game assembles at runtime ------------------------------
//
// Not every RAM code image is a copy of something. Golden Sun's sprite
// blitter is ASSEMBLED at runtime: a ROM template is used as a skeleton and
// the generator patches immediates, fills branch displacements and omits
// instructions the current parameters do not need. The result is a coherent
// ARM routine that is byte-identical to no ROM range — the live image at
// 0x03006220 during the first scripted fight shares only its 3-word prologue
// with the ROM template at 0x080EDCC4, then diverges by construction (measured
// word alignment: 46 of 97 template words survive, the rest are patched or
// dropped).
//
// No static corpus can cover that: the generated variant depends on runtime
// parameters, so the identity registry legitimately has nothing to match. The
// registry aborting on it is a DEVELOPMENT guard that was right while every
// known RAM image was a copy, and wrong for generated code.
//
// Policy, per AGENTS.md rule 6:
//   * strict-static run  -> abort exactly as before. A strict-static claim
//                           must never absorb a dynamically executed PC, and
//                           the abort's trace ring is the evidence.
//   * ordinary run       -> log loudly once per PC, count it, and fall
//                           through to the ordinary dispatch path, which
//                           tries the healed-overlay tier and then bridges
//                           through the interpreter. Reported at exit so a
//                           run that used this path can never be quoted as
//                           fully static.
// The healed-overlay tier and the interpreter bridge, both from the runtime.
// Routing an unexplained RAM PC through these EXPLICITLY (rather than
// returning 0 and letting runtime_dispatch continue) is deliberate: the main
// dispatch table does contain RAM addresses from declared code copies, and a
// PC whose live bytes match no registered identity must never reach a static
// translation built from different bytes. Silently running the wrong
// translation is worse than the abort this replaces.
extern "C" int overlay_try_dispatch(std::uint32_t pc, int thumb);
extern "C" void runtime_dispatch_miss(std::uint32_t target_pc);

// Hard Mode: Func_77428 entered from Func_79460's enemy setup (r6 = unit)
// with the live flag on. It rebuilds the final stats from the base fields
// scaled here.
void hard_mode_scale_enemy() {
    using namespace gsr::hard_mode;
    if ((g_cpu.R[14] & ~1u) != kEnemySetupReturn ||
        bus_read_u8(kAutoSleepFlag) != kHardModeValue) {
        return;
    }
    const std::uint32_t unit = g_cpu.R[6];
    const auto scale = [unit](std::uint32_t offset, std::uint32_t num,
                              std::uint32_t den) {
        std::uint32_t value = bus_read_u16(unit + offset) * num / den;
        if (value > kStatMax) value = kStatMax;
        bus_write_u16(unit + offset, static_cast<std::uint16_t>(value));
    };
    scale(kBaseHpOffset, kHpNumerator, kHpDenominator);
    scale(kMaxHpOffset, kHpNumerator, kHpDenominator);
    scale(kCurrentHpOffset, kHpNumerator, kHpDenominator);
    scale(kBaseAttackOffset, kStatNumerator, kStatDenominator);
    scale(kBaseDefenceOffset, kStatNumerator, kStatDenominator);
}

// Settings screen translation (hard_mode.h): the screen knows 0/1 only.
std::uint8_t g_hard_mode_saved_byte = 0;
void hard_mode_settings_setup() {
    using namespace gsr::hard_mode;
    g_hard_mode_saved_byte = bus_read_u8(kSavedFlag);
    bus_write_u8(kSavedFlag, g_hard_mode_saved_byte == kHardModeValue ? 1u : 0u);
}
void hard_mode_settings_teardown() {
    using namespace gsr::hard_mode;
    if (g_cpu.R[5] != 0u) {
        bus_write_u8(kSavedFlag, g_hard_mode_saved_byte);
        return;
    }
    const std::uint8_t value =
        bus_read_u8(kSavedFlag) == 1u ? kHardModeValue : 0u;
    bus_write_u8(kSavedFlag, value);
    bus_write_u8(kAutoSleepFlag, value);
}

void golden_sun_function_entry_observer(std::uint32_t entry_pc) {
    if (entry_pc == gsr::hard_mode::kSettingsSetupEntryPc) {
        g_settings_screen_text = true;
        hard_mode_settings_setup();
    } else if (entry_pc == gsr::hard_mode::kSettingsTeardownEntryPc) {
        hard_mode_settings_teardown();
    } else if (entry_pc == gsr::hard_mode::kNewGameEntryPc) {
        hard_mode_prompt_open();
    } else if (entry_pc == gsr::hard_mode::kStatRecalcPc) {
        hard_mode_scale_enemy();
    }
    field_psynergy_on_entry(entry_pc);
    gsr::move_probe_on_entry(entry_pc);
    gsr::earth_surge_on_entry(entry_pc);
    golden_sun_obj_f0_entry_capture(entry_pc);
    golden_sun_obj_staging_handoff(entry_pc);
    record_golden_sun_obj_boulder_calc_entry(entry_pc);
    // Each of these costs one predictable branch when its own toggle is
    // disabled, and neither depends on the other being enabled -- see
    // map_recorder.h's header comment on why the two stay independent.
    gsr::function_tracer_on_entry(entry_pc);
    gsr::map_recorder_on_entry(entry_pc);
    gsr::map_recorder_on_function_args(entry_pc, g_cpu.R[0], g_cpu.R[1],
                                       g_cpu.R[2], g_cpu.R[3]);
    gsr::room_buffer_on_function_args(entry_pc, g_cpu.R[0], g_cpu.R[1],
                                      g_cpu.R[2]);
    gsr::room_buffer_on_entry(entry_pc);
    // Spell sparks, taken from the game's own stamp call rather than decoded
    // out of its memory (src/effect_capture.h). One comparison while off.
    if (gsr::effect_capture_enabled() && gsr::effect_capture_observes(entry_pc)) {
        if (auto* bus = gbarecomp::active_bus()) {
            gsr::effect_capture_on_entry(entry_pc, &g_cpu.R[0],
                bus->ewram_ptr(), bus->iwram_ptr(), bus->rom_ptr(),
                bus->rom_size(), runtime_current_frame());
        }
    }
}

bool strict_static_run() {
    static const bool strict = [] {
        const char* env = std::getenv("GBARECOMP_STRICT_STATIC");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();
    return strict;
}

std::vector<std::uint32_t> g_dynamic_ram_pcs;
unsigned long long g_dynamic_ram_dispatches = 0;

// A verified transient image is safe to reuse until one of its RAM pages
// changes. The global generation is retained for diagnostics, but using it as
// the dispatch-cache key made an unrelated overlay write flush every hot PC.
enum class VerifiedRamAction : std::uint8_t { Static, Native };
constexpr std::size_t kNoVerifiedIdentity =
    static_cast<std::size_t>(-1);
struct VerifiedRamCacheEntry {
    std::size_t identity_index = kNoVerifiedIdentity;
    VerifiedRamAction action = VerifiedRamAction::Static;
    void (*fn)(void) = nullptr;
};
std::unordered_map<std::uint64_t, VerifiedRamCacheEntry>
    g_verified_ram_cache;

// Hash each reviewed image once per RAM-code generation, rather than once
// per PC inside that image. A fight visits many interior PCs, so a PC-only
// cache still repeated the same SHA-1 over and over.
struct VerifiedIdentityCacheEntry {
    bool valid = false;
    bool matched = false;
    // True once this candidate has matched its pinned identity at least once.
    // Overlay .text is immutable; later page writes can therefore reject a
    // known candidate with a few sentinels instead of hashing an unrelated
    // overlay. Unknown candidates still take the conservative full hash path.
    bool known_snapshot = false;
    // A candidate can span several 4 KiB pages. Recheck the bytes only when
    // one of those page generations changes.
    std::array<unsigned int, 64> page_epochs{};
    std::array<std::uint32_t, 4> sparse_words{};
    std::vector<std::uint8_t> snapshot;
};
std::array<VerifiedIdentityCacheEntry, kTransientCodeImages.size()>
    g_verified_identity_cache{};
unsigned long long g_verified_identity_hashes = 0;
unsigned long long g_verified_identity_cache_hits = 0;
unsigned long long g_verified_identity_content_hits = 0;
unsigned long long g_verified_identity_invalidations = 0;
std::array<unsigned long long, kTransientCodeImages.size()>
    g_verified_identity_hash_counts{};
// Stamp byte-scan path (try_stamp_dispatch): entries into the scan, and SHA-1
// computations made inside it. Reported at exit with the counters above.
unsigned long long g_stamp_identity_scans = 0;
unsigned long long g_stamp_identity_hashes = 0;

std::uint64_t verified_ram_key(std::uint32_t pc, int thumb) {
    return (static_cast<std::uint64_t>(pc & ~1u) << 1) |
           static_cast<std::uint64_t>(thumb ? 1 : 0);
}

unsigned int ram_code_page_epoch(std::uint32_t addr) {
    const std::uint32_t region = addr >> 24;
    if (region == 0x03u)
        return g_ram_code_page_epoch_iwram[(addr & 0x00007FFFu) >> 12];
    if (region == 0x02u)
        return g_ram_code_page_epoch_ewram[(addr & 0x0003FFFFu) >> 12];
    return 0;
}

bool verified_identity_pages_current(std::size_t index) {
    if (index >= kTransientCodeImages.size()) return false;
    const auto& candidate = kTransientCodeImages[index];
    const auto& identity = g_verified_identity_cache[index];
    if (!identity.valid || candidate.end <= candidate.start) return false;
    const std::uint32_t region = candidate.start >> 24;
    const std::uint32_t mask = region == 0x03u ? 0x00007FFFu : 0x0003FFFFu;
    const std::uint32_t first = (candidate.start & mask) >> 12;
    const std::uint32_t last = ((candidate.end - 1u) & mask) >> 12;
    for (std::uint32_t page = first; page <= last && page < 64u; ++page) {
        const std::uint32_t page_addr =
            (candidate.start & ~mask) | (page << 12);
        if (identity.page_epochs[page] != ram_code_page_epoch(page_addr))
            return false;
    }
    return true;
}

void save_verified_identity_page_epochs(std::size_t index) {
    const auto& candidate = kTransientCodeImages[index];
    auto& identity = g_verified_identity_cache[index];
    identity.page_epochs.fill(0);
    if (candidate.end <= candidate.start) return;
    const std::uint32_t region = candidate.start >> 24;
    const std::uint32_t mask = region == 0x03u ? 0x00007FFFu : 0x0003FFFFu;
    const std::uint32_t first = (candidate.start & mask) >> 12;
    const std::uint32_t last = ((candidate.end - 1u) & mask) >> 12;
    for (std::uint32_t page = first; page <= last && page < 64u; ++page) {
        const std::uint32_t page_addr =
            (candidate.start & ~mask) | (page << 12);
        identity.page_epochs[page] = ram_code_page_epoch(page_addr);
    }
}

std::array<std::uint32_t, 4> identity_sparse_words(
    const TransientCodeImage& candidate,
    const std::vector<std::uint8_t>* image = nullptr) {
    std::array<std::uint32_t, 4> words{};
    const std::uint32_t size = candidate.end - candidate.start;
    std::array<std::uint32_t, 4> offsets{
        0u,
        size > 4u ? (size / 3u) & ~3u : 0u,
        size > 4u ? (2u * size / 3u) & ~3u : 0u,
        size > 4u ? (size - 4u) & ~3u : 0u,
    };
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        const std::uint32_t off = offsets[i];
        if (off + 4u > size) continue;
        if (image) {
            const auto& bytes = *image;
            words[i] = static_cast<std::uint32_t>(bytes[off]) |
                       (static_cast<std::uint32_t>(bytes[off + 1u]) << 8) |
                       (static_cast<std::uint32_t>(bytes[off + 2u]) << 16) |
                       (static_cast<std::uint32_t>(bytes[off + 3u]) << 24);
        } else {
            words[i] = bus_read_u32(candidate.start + off);
        }
    }
    return words;
}

// SHA-1 of a live image with the candidate's volatile prefix replaced by the
// ROM bytes, so a game-side overwrite of those bytes does not change identity.
std::string identity_image_sha1(const TransientCodeImage& candidate,
                                std::vector<std::uint8_t> image) {
    if (candidate.volatile_prefix != 0 && candidate.rom_start != 0) {
        const std::size_t n = std::min<std::size_t>(candidate.volatile_prefix,
                                                    image.size());
        for (std::size_t i = 0; i < n; ++i)
            image[i] = bus_read_u8(candidate.rom_start +
                                   static_cast<std::uint32_t>(i));
    }
    return gba::sha1(image.data(), image.size()).hex();
}

bool volatile_prefix_matches_rom(const TransientCodeImage& candidate) {
    if (candidate.volatile_prefix == 0 || candidate.rom_start == 0) return true;
    for (std::uint32_t i = 0; i < candidate.volatile_prefix; ++i) {
        if (bus_read_u8(candidate.start + i) !=
            bus_read_u8(candidate.rom_start + i))
            return false;
    }
    return true;
}

bool identity_sparse_matches(const TransientCodeImage& candidate,
                             const VerifiedIdentityCacheEntry& identity) {
    return identity_sparse_words(candidate) == identity.sparse_words;
}

// Compressed overlay .text is immutable while resident. The synthetic DC8
// templates use the same `rom_start == 0` marker but are deliberately rewritten
// by the guest's template writer, so a sparse mismatch can be a transient
// variant change and must not permanently reject a valid template identity.
bool identity_sparse_fast_reject_safe(const TransientCodeImage& candidate) {
    return candidate.rom_start == 0u && candidate.name != nullptr &&
           std::strncmp(candidate.name, "overlay_", 8) == 0;
}

// Revalidate the identity already attached to a hot PC before falling back to
// the full candidate scan. RAM page epochs are deliberately conservative: a
// data write elsewhere in the same page invalidates the PC cache too. In the
// common case the cached overlay is still resident, so checking that one image
// avoids hashing every other overlay that shares the staging slot.
bool refresh_verified_identity(std::size_t candidate_index) {
    if (candidate_index >= kTransientCodeImages.size()) return false;
    const auto& candidate = kTransientCodeImages[candidate_index];
    auto& identity = g_verified_identity_cache[candidate_index];
    if (!identity.valid) return false;
    if (verified_identity_pages_current(candidate_index))
        return identity.matched;

    ++g_verified_identity_invalidations;
    if (identity.known_snapshot && identity_sparse_fast_reject_safe(candidate) &&
        !identity_sparse_matches(candidate, identity)) {
        identity.matched = false;
        save_verified_identity_page_epochs(candidate_index);
        return false;
    }

    bool changed = identity.snapshot.size() != candidate.end - candidate.start;
    if (!changed) {
        for (std::uint32_t offset = 0;
             offset < candidate.end - candidate.start; ++offset) {
            if (bus_read_u8(candidate.start + offset) !=
                identity.snapshot[offset]) {
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        ++g_verified_identity_hashes;
        ++g_verified_identity_hash_counts[candidate_index];
        std::vector<std::uint8_t> image(candidate.end - candidate.start);
        for (std::uint32_t offset = 0; offset < image.size(); ++offset)
            image[offset] = bus_read_u8(candidate.start + offset);
        identity.matched = identity_image_sha1(candidate, image) == candidate.sha1;
        identity.snapshot = std::move(image);
        identity.known_snapshot = identity.matched && candidate.rom_start == 0u;
        if (identity.known_snapshot)
            identity.sparse_words = identity_sparse_words(
                candidate, &identity.snapshot);
    } else {
        ++g_verified_identity_content_hits;
    }
    save_verified_identity_page_epochs(candidate_index);
    return identity.matched;
}

// DMA and a few BIOS routines write RAM directly rather than through the
// generated bus helpers.  Those stores cannot advance the page-generation
// counters above, so a cached RAM identity also checks the two words at the
// current PC before dispatching native code.  This is cheap (two RAM reads)
// and catches the important case where a shared overlay slot is replaced while
// its page number stays unchanged.
bool verified_identity_local_words_current(std::size_t candidate_index,
                                           std::uint32_t pc) {
    if (candidate_index >= kTransientCodeImages.size()) return false;
    const auto& candidate = kTransientCodeImages[candidate_index];
    const auto& identity = g_verified_identity_cache[candidate_index];
    if (!identity.valid || !identity.matched || pc < candidate.start ||
        pc >= candidate.end || identity.snapshot.empty()) return false;
    const std::uint32_t size = candidate.end - candidate.start;
    const std::uint32_t offset = (pc - candidate.start) & ~3u;
    bool checked = false;
    for (unsigned word = 0; word < 2u; ++word) {
        const std::uint32_t off = offset + word * 4u;
        if (off >= size || off >= identity.snapshot.size()) break;
        const std::uint32_t available =
            std::min<std::uint32_t>(4u, size - off);
        if (available == 4u && off + 4u <= identity.snapshot.size()) {
            const std::uint32_t live = bus_read_u32(candidate.start + off);
            const auto& b = identity.snapshot;
            const std::uint32_t saved = static_cast<std::uint32_t>(b[off]) |
                                         (static_cast<std::uint32_t>(b[off + 1u]) << 8) |
                                         (static_cast<std::uint32_t>(b[off + 2u]) << 16) |
                                         (static_cast<std::uint32_t>(b[off + 3u]) << 24);
            if (live != saved) return false;
        } else {
            for (std::uint32_t byte = 0; byte < available; ++byte) {
                if (bus_read_u8(candidate.start + off + byte) !=
                    identity.snapshot[off + byte]) return false;
            }
        }
        checked = true;
    }
    return checked;
}

// A PC already reported as dynamic. Generated code is hot — the blitter runs
// per sprite — so everything below this point must stay silent after the
// first report, which is also why the expensive candidate-by-candidate
// diagnostic only runs the first time a PC is seen.
bool dynamic_ram_pc_reported(std::uint32_t pc) {
    for (std::uint32_t seen : g_dynamic_ram_pcs) {
        if (seen == pc) return true;
    }
    return false;
}

// A resolved dynamic-RAM path has already performed its interpreter/fallback
// work. Returning this sentinel lets runtime_dispatch stop without falling
// through to static tables or reporting a second miss.
void verified_ram_dispatch_noop() {}

// The resolver returns a native function for runtime_dispatch to tail-transfer
// into. Keep the active-entry marker alive until the generated callee's guest
// return path retires it; a C++ RAII guard cannot span that tail transfer.
void verified_ram_mark_active(std::uint32_t pc, int thumb);

// ---- Particle stamp routines (docs/features/STATIC_RAM_CODE.md, group D) --
//
// Func_ed408 builds an ARM routine into a heap block; every possible output
// is pre-compiled into gsr_stamps_*, packed at gsr_stamps_kImageOrigin. The
// routines are position-independent, so a live one runs from the image with
// g_runtime_image_base = live base - its offset in the image. A live routine
// is found by walking back from the PC to one of the builder's opening
// sequences and matching the bytes from there by length and SHA-1; a
// verified placement is reused until a write touches its pages.
struct StampPlacement {
    std::uint32_t base = 0;
    const GsrStampVariant* variant = nullptr;
    std::array<unsigned int, 64> page_epochs{};
    // The verified bytes. The heap page also holds data the game writes
    // every frame; after such a write the placement is rechecked by a direct
    // comparison with these, not a new hash.
    std::vector<std::uint8_t> bytes;
};
std::array<StampPlacement, 8> g_stamp_placements{};
std::size_t g_stamp_next_slot = 0;
// Other routines open with the same instruction. A base that matched no
// variant is remembered until a write touches its pages, so their dispatches
// do not repeat the hashing.
struct StampMiss {
    std::uint32_t base = 0;
    std::uint32_t end = 0;
    std::array<unsigned int, 64> page_epochs{};
    std::vector<std::uint8_t> bytes;  // as for StampPlacement::bytes
};
std::array<StampMiss, 16> g_stamp_misses{};
std::size_t g_stamp_next_miss = 0;

struct StampRegistry {
    std::uint32_t max_length = 0;
    std::vector<std::uint32_t> lengths;  // distinct, longest first
    std::unordered_map<std::string, const GsrStampVariant*> by_identity;
};

const StampRegistry& stamp_registry() {
    static const StampRegistry registry = [] {
        StampRegistry built;
        for (unsigned i = 0; i < gsr_stamps_kVariantCount; ++i) {
            const GsrStampVariant& v = gsr_stamps_kVariants[i];
            built.max_length = std::max(built.max_length, v.length);
            if (std::find(built.lengths.begin(), built.lengths.end(),
                          v.length) == built.lengths.end())
                built.lengths.push_back(v.length);
            built.by_identity[std::to_string(v.length) + ":" + v.sha1] = &v;
        }
        std::sort(built.lengths.rbegin(), built.lengths.rend());
        return built;
    }();
    return registry;
}

RuntimeGuestFn stamp_entry(const StampPlacement& placement, std::uint32_t pc) {
    const std::uint32_t image_offset =
        placement.variant->offset + (pc - placement.base);
    RuntimeGuestFn fn = lookup_entry(gsr_stamps_kDispatchTable,
                                     gsr_stamps_kDispatchTableLen,
                                     gsr_stamps_kImageOrigin + image_offset, 0);
    if (fn) g_runtime_image_base = placement.base - placement.variant->offset;
    return fn;
}

RuntimeGuestFn try_stamp_dispatch(std::uint32_t pc, int thumb) {
    if (thumb || (pc & 3u) != 0u) return nullptr;
    const std::uint32_t region = pc >> 24;
    if (region != 0x02u && region != 0x03u) return nullptr;
    // The boot copy (main.toml code_copy 0x03000000..0x03001400) is never a
    // stamp heap block.
    if (pc < 0x03001400u && region == 0x03u) return nullptr;
    const std::uint32_t region_start = region << 24;

    for (StampPlacement& placement : g_stamp_placements) {
        if (!placement.variant || pc < placement.base ||
            pc >= placement.base + placement.variant->length)
            continue;
        const std::uint32_t end = placement.base + placement.variant->length;
        if (!gsr::ram_range_pages_current(placement.base, end,
                                          placement.page_epochs,
                                          ram_code_page_epoch)) {
            bool same = true;
            for (std::uint32_t i = 0; same && i < placement.bytes.size();
                 i += 4u) {
                std::uint32_t expected = 0;
                std::memcpy(&expected, placement.bytes.data() + i, 4);
                same = bus_read_u32(placement.base + i) == expected;
            }
            if (!same) continue;
            gsr::save_ram_range_page_epochs(placement.base, end,
                                            placement.page_epochs,
                                            ram_code_page_epoch);
        }
        if (RuntimeGuestFn fn = stamp_entry(placement, pc)) return fn;
    }

    const StampRegistry& registry = stamp_registry();
    if (registry.lengths.empty()) return nullptr;
    ++g_stamp_identity_scans;
    // Every variant opens with STMFD sp!, {r5-r11, lr} (0xE92D4FE0), and no
    // variant holds that word anywhere else.
    constexpr std::uint32_t kStampOpening = 0xE92D4FE0u;
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t distance = 0; distance < registry.max_length;
         distance += 4u) {
        if (pc - region_start < distance) break;
        const std::uint32_t base = pc - distance;
        if (bus_read_u32(base) != kStampOpening) continue;
        StampMiss* known_miss = nullptr;
        for (StampMiss& miss : g_stamp_misses) {
            if (miss.base != base || miss.end <= base) continue;
            if (gsr::ram_range_pages_current(base, miss.end, miss.page_epochs,
                                             ram_code_page_epoch))
                return nullptr;
            known_miss = &miss;
        }
        bytes.resize(registry.max_length);
        for (std::uint32_t i = 0; i < registry.max_length; ++i)
            bytes[i] = bus_read_u8(base + i);
        if (known_miss && known_miss->bytes == bytes) {
            gsr::save_ram_range_page_epochs(base, known_miss->end,
                                            known_miss->page_epochs,
                                            ram_code_page_epoch);
            return nullptr;
        }
        for (const std::uint32_t length : registry.lengths) {
            if (base + length <= pc) break;  // longest first: none reach pc
            ++g_stamp_identity_hashes;
            const std::string key = std::to_string(length) + ":" +
                                    gba::sha1(bytes.data(), length).hex();
            const auto found = registry.by_identity.find(key);
            if (found == registry.by_identity.end()) continue;
            StampPlacement& slot = g_stamp_placements[g_stamp_next_slot];
            g_stamp_next_slot = (g_stamp_next_slot + 1u) %
                                g_stamp_placements.size();
            slot.base = base;
            slot.variant = found->second;
            slot.bytes.assign(bytes.begin(), bytes.begin() + length);
            gsr::ram_range_register_mask(
                base, base + length, &g_ram_code_page_mask_iwram,
                &g_ram_code_page_mask_ewram_lo,
                &g_ram_code_page_mask_ewram_hi);
            gsr::save_ram_range_page_epochs(base, base + length,
                                            slot.page_epochs,
                                            ram_code_page_epoch);
            static unsigned logged = 0;
            if (logged < 32u) {
                ++logged;
                std::fprintf(stderr,
                    "[stamp] pre-built routine %u at 0x%08X (%u bytes, pc "
                    "0x%08X)\n",
                    static_cast<unsigned>(found->second - gsr_stamps_kVariants),
                    base, length, pc);
            }
            return stamp_entry(slot, pc);
        }
        // The opening never occurs inside a stamp (checked over all
        // variants), so this is the routine's start: another routine with the
        // same opening, or a stamp variant this build lacks.
        StampMiss& miss = g_stamp_misses[g_stamp_next_miss];
        g_stamp_next_miss = (g_stamp_next_miss + 1u) % g_stamp_misses.size();
        miss.base = base;
        miss.end = base + registry.max_length;
        miss.bytes = bytes;
        gsr::ram_range_register_mask(
            base, miss.end, &g_ram_code_page_mask_iwram,
            &g_ram_code_page_mask_ewram_lo, &g_ram_code_page_mask_ewram_hi);
        gsr::save_ram_range_page_epochs(base, miss.end, miss.page_epochs,
                                        ram_code_page_epoch);
        static unsigned unknown_logged = 0;
        if (unknown_logged < 8u) {
            ++unknown_logged;
            std::fprintf(stderr,
                "[stamp] routine at 0x%08X (pc 0x%08X) opens like a stamp "
                "but matches no pre-built variant\n", base, pc);
        }
        return nullptr;
    }
    return nullptr;
}

// Run an unexplained RAM PC: resolve healed native first, else bridge through
// the interpreter (which also enqueues the heal keyed by live bytes' CRC).
RuntimeGuestFn dispatch_dynamic_ram(std::uint32_t pc, int thumb) {
    if (RuntimeGuestFn fn = try_stamp_dispatch(pc, thumb)) {
        verified_ram_mark_active(pc, thumb);
        return fn;
    }
    // The builder reuses two measured IWRAM staging slots. The older
    // 0x030057e0..0x03005a64 image is just as generated as the newer
    // 0x03006000..0x03006500 image; leaving it on the interpreter path makes
    // every pool tail-jump pay the full bridge cost while its native shard is
    // already queued.
    const bool generated_blitter = !thumb &&
        ((pc >= 0x030057E0u && pc < 0x03005A64u) ||
         (pc >= 0x03006000u && pc < 0x03006500u));
    if (RuntimeGuestFn fn = gbarecomp::overlay_resolve(pc, thumb)) {
        verified_ram_mark_active(pc, thumb);
        return fn;
    }
    if (generated_blitter) {
        const auto outcome = gbarecomp::overlay_request_compile(pc, thumb != 0);
        if (outcome != gbarecomp::OverlayRequestOutcome::Failed) {
            // This wait STOPS THE GAME. It is the only place the frame loop
            // can stand still for seconds, and session_20260913_195033 caught
            // it doing exactly that: frame 653242 spent 8.86 s inside the
            // guest with one compile outstanding, while the same shard
            // compiles standalone in 0.49 s. Where the rest of that time goes
            // is not yet known, so measure the wait itself -- unconditionally,
            // like [ram-compile] beside it, because the data has to come from
            // an ordinary play session.
            //
            // The wait lasts while the compile is still queued or running,
            // in 10 s slices up to kMaxWaitSlices. A fixed 10 s aborted
            // session_20260923_093557 with 0x0300614C queued behind three
            // other shards; it healed moments after the abort. Only a
            // compile known to have failed ends the wait early.
            constexpr int kMaxWaitSlices = 12;
            const auto wait_t0 = std::chrono::steady_clock::now();
            RuntimeGuestFn fn = nullptr;
            for (int slice = 0; slice < kMaxWaitSlices && !fn; ++slice) {
                fn = gbarecomp::overlay_wait_resolve(pc, thumb != 0, 10000u);
                if (!fn && gbarecomp::overlay_request_compile(
                               pc, thumb != 0) ==
                               gbarecomp::OverlayRequestOutcome::Failed) {
                    break;
                }
            }
            const auto wait_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - wait_t0).count();
            // A resolved-on-the-first-try wait is the normal case and says
            // nothing; only a wait the player can feel is reported.
            if (wait_ms >= 50) {
                std::fprintf(stderr,
                    "[ram-compile-wait] pc=0x%08X %s blocked the game "
                    "thread for %lld ms, resolved=%d\n",
                    pc, thumb ? "thumb" : "arm",
                    static_cast<long long>(wait_ms), fn != nullptr ? 1 : 0);
            }
            if (fn) {
                verified_ram_mark_active(pc, thumb);
                return fn;
            }
        }
        std::fprintf(stderr,
            "GoldenSunRecomp: generated blitter 0x%08X failed to heal "
            "(compile failed, or still pending after 120 s); refusing "
            "unsafe interpreter bridge.\n",
            pc);
        std::abort();
    }
    runtime_dispatch_miss(pc | (thumb ? 1u : 0u));
    return &verified_ram_dispatch_noop;
}

// Fast path for a PC we have already explained: count it and run it without
// printing anything.
bool dynamic_ram_pc_repeat(std::uint32_t pc) {
    if (strict_static_run()) return false;
    if (!dynamic_ram_pc_reported(pc)) return false;
    ++g_dynamic_ram_dispatches;
    return true;
}

void report_dynamic_ram_summary() {
    if (g_dynamic_ram_pcs.empty()) return;
    std::fprintf(stderr,
        "GoldenSunRecomp: dynamic_ram_pcs=%zu dynamic_ram_dispatches=%llu — "
        "RAM code with no registered identity ran through the interpreter/heal "
        "tier. This run is NOT fully static.\n",
        g_dynamic_ram_pcs.size(), g_dynamic_ram_dispatches);
    for (std::uint32_t pc : g_dynamic_ram_pcs) {
        std::fprintf(stderr, "  dynamic_ram_pc=0x%08X\n", pc);
    }
}

// Returns true when the caller should fall through to the ordinary dispatch
// path instead of aborting.
bool allow_dynamic_ram_pc(const char* reason, std::uint32_t pc, int thumb) {
    if (strict_static_run()) return false;
    ++g_dynamic_ram_dispatches;
    if (dynamic_ram_pc_reported(pc)) return true;
    if (g_dynamic_ram_pcs.empty()) std::atexit(report_dynamic_ram_summary);
    g_dynamic_ram_pcs.push_back(pc);
    std::fprintf(stderr,
        "GoldenSunRecomp: DYNAMIC RAM CODE at 0x%08X (%s) — %s. No registered "
        "identity explains these bytes; executing through the heal/interpreter "
        "tier. NOT fully static. A strict-static run aborts here instead.\n",
        pc, thumb ? "thumb" : "arm", reason);
    return true;
}

// Bounds nested verified-RAM native dispatch on this thread. A cached
// native entry that re-dispatches its own pc before returning (a
// generated-code bug, not ordinary nested guest calls) would otherwise
// recurse through runtime_dispatch -> verified_ram_dispatch -> fn()
// forever and blow the host stack. A fixed-capacity array plus a linear
// scan is used instead of a std::set: this path is hit ~100,000 times per
// fight, and per-dispatch hashing/allocation was measured to make that
// crawl (see the comment above verified_ram_dispatch).
constexpr std::size_t kVerifiedRamActiveCapacity = 64;
thread_local std::array<std::uint64_t, kVerifiedRamActiveCapacity>
    g_verified_ram_active_keys{};
thread_local std::array<std::uint32_t, kVerifiedRamActiveCapacity>
    g_verified_ram_active_call_depths{};
thread_local std::array<std::uint32_t, kVerifiedRamActiveCapacity>
    g_verified_ram_active_return_pcs{};
thread_local std::size_t g_verified_ram_active_depth = 0;
thread_local std::vector<std::uint32_t> g_verified_ram_self_dispatch_reported;
thread_local std::vector<std::uint32_t> g_verified_ram_depth_cap_reported;

bool verified_ram_pc_in(const std::vector<std::uint32_t>& reported,
                        std::uint32_t pc) {
    for (std::uint32_t seen : reported) {
        if (seen == pc) return true;
    }
    return false;
}

// Active verified-RAM entries are guest-call scoped, not C++-call scoped.
// runtime_dispatch tail-transfers to the returned function, so a local RAII
// guard would die before that function starts. The runtime return hook below
// retires these entries after generated BX-LR/cancel handling unwinds the
// guest call-return stack, preserving both cycle protection and tail calls.
struct VerifiedRamActiveGuard {
    bool self_dispatch = false;
    bool tail_redispatch = false;
    bool depth_exceeded = false;

    explicit VerifiedRamActiveGuard(std::uint64_t key) {
        for (std::size_t i = 0; i < g_verified_ram_active_depth; ++i) {
            if (g_verified_ram_active_keys[i] == key) {
                // Re-entering the same validated PC is ordinary guest control
                // flow, including MP2K's nested loop at 0x03000828. The guest
                // call stack preserves recursion semantics; keep this as a
                // native tail transfer. The prior crash came specifically
                // from overlay_try_dispatch's non-tail retry.
                tail_redispatch = true;
                return;
            }
        }
        if (g_verified_ram_active_depth >= kVerifiedRamActiveCapacity) {
            // A long top-level tail-dispatch chain can visit more than 64
            // distinct RAM entries without a guest return. Those entries are
            // not recursive host calls. Rotate the bounded diagnostic set;
            // never turn valid control flow into a multi-million-instruction
            // interpreter bridge merely because this set filled.
            g_verified_ram_active_depth = 0;
        }
    }
    VerifiedRamActiveGuard(const VerifiedRamActiveGuard&) = delete;
    VerifiedRamActiveGuard& operator=(const VerifiedRamActiveGuard&) = delete;
};

void verified_ram_dispatch_return_hook(std::uint32_t return_pc,
                                       std::uint32_t call_stack_depth) {
    // A guest return truncates the runtime call stack. Every marker created
    // below that new depth belongs to a callee that has now returned. Remove
    // by swap; ordering is only diagnostic/protection state, not guest state.
    for (std::size_t i = g_verified_ram_active_depth; i != 0;) {
        const std::size_t slot = i - 1u;
        const bool unwound =
            g_verified_ram_active_call_depths[slot] > call_stack_depth;
        const bool top_level_return =
            g_verified_ram_active_call_depths[slot] == 0u &&
            g_verified_ram_active_return_pcs[slot] == (return_pc & ~1u);
        if (!unwound && !top_level_return) {
            i = slot;
            continue;
        }
        --g_verified_ram_active_depth;
        if (slot != g_verified_ram_active_depth) {
            g_verified_ram_active_keys[slot] =
                g_verified_ram_active_keys[g_verified_ram_active_depth];
            g_verified_ram_active_call_depths[slot] =
                g_verified_ram_active_call_depths[g_verified_ram_active_depth];
            g_verified_ram_active_return_pcs[slot] =
                g_verified_ram_active_return_pcs[g_verified_ram_active_depth];
        }
        i = std::min(i, g_verified_ram_active_depth);
    }
}

void verified_ram_mark_active(std::uint32_t pc, int thumb) {
    if (g_verified_ram_active_depth >= kVerifiedRamActiveCapacity) return;
    const std::uint64_t key = verified_ram_key(pc, thumb);
    const std::uint32_t call_depth = runtime_call_stack_depth();
    g_verified_ram_active_keys[g_verified_ram_active_depth] = key;
    g_verified_ram_active_call_depths[g_verified_ram_active_depth] = call_depth;
    g_verified_ram_active_return_pcs[g_verified_ram_active_depth] =
        g_cpu.R[14] & ~1u;
    ++g_verified_ram_active_depth;
}

void verified_ram_dispatch_outer_boundary() {
    // The runner has regained control only after the current top-level guest
    // dispatch returned. This can be a scheduling yield/SWI rather than a
    // guest BX-LR, so no return hook is guaranteed to have fired. Nested
    // markers are retired by the call-return hook; clear the remaining
    // top-level markers here so the next outer resume is a fresh dispatch.
    if (runtime_call_stack_depth() == 0u)
        g_verified_ram_active_depth = 0;
}

RuntimeGuestFn verified_ram_dispatch(std::uint32_t pc, int thumb) {
    // A PC already classified as generated code short-circuits everything
    // below. This must come FIRST: the identity scan re-hashes the whole
    // containing candidate image (SHA-1 over ~1.2 KB) and then every
    // relocatable image at this base (~2.5 KB more) before it can conclude
    // what it already concluded the first time. Generated code is HOT — a
    // single fight dispatches these ~100,000 times — so paying that scan per
    const std::uint64_t key = verified_ram_key(pc, thumb);

    if (dynamic_ram_pc_repeat(pc)) return dispatch_dynamic_ram(pc, thumb);

    VerifiedRamActiveGuard active_guard(key);

    if (active_guard.self_dispatch) {
        // A verified-RAM native entry called back into runtime_dispatch for
        // the exact same pc/thumb it is currently executing, before
        // returning. Left alone this recurses through
        // runtime_dispatch -> verified_ram_dispatch -> fn() without bound
        // and overflows the host stack (this is the fix for that crash).
        // Evict the cache entry before using the interpreter fallback; do not
        // let the ordinary static/overlay path select this active body again.
        const auto self_cached = g_verified_ram_cache.find(key);
        if (!verified_ram_pc_in(g_verified_ram_self_dispatch_reported, pc)) {
            g_verified_ram_self_dispatch_reported.push_back(pc);
            std::fprintf(stderr,
                "GoldenSunRecomp: SELF-DISPATCH RECURSION at 0x%08X (%s) "
                "identity_index=%zu — a verified-RAM native entry "
                "re-dispatched its own pc before returning. Bridging this "
                "edge once through the interpreter instead of recursing "
                "without bound.\n",
                pc, thumb ? "thumb" : "arm",
                self_cached != g_verified_ram_cache.end()
                    ? self_cached->second.identity_index
                    : kNoVerifiedIdentity);
        }
        if (self_cached != g_verified_ram_cache.end())
            g_verified_ram_cache.erase(self_cached);
        // This dispatch was already handled by the active native entry.  A
        // null return would make runtime_dispatch continue into the static
        // table (and then Stage-2 on a miss), where a same-PC static entry can
        // re-enter the body that just declined.  Bridge the recursive edge
        // once through the interpreter and return a handled sentinel so the
        // dispatcher cannot select either lower tier.
        runtime_bridge_interpret(pc, thumb != 0, 0u, 0u);
        return &verified_ram_dispatch_noop;
    }
    if (active_guard.depth_exceeded) {
        // Not a same-pc self-dispatch, but this thread's nested
        // verified-RAM native dispatch depth ran past the cap — e.g. an
        // A->B->A cycle across distinct pcs. Refuse to recurse further and
        // bridge through the interpreter once. As with same-PC recursion,
        // returning null here would let runtime_dispatch select a static or
        // Stage-2 RAM body for the declined transfer.
        if (!verified_ram_pc_in(g_verified_ram_depth_cap_reported, pc)) {
            g_verified_ram_depth_cap_reported.push_back(pc);
            std::fprintf(stderr,
                "GoldenSunRecomp: VERIFIED-RAM DISPATCH DEPTH CAP (%zu) hit "
                "at pc=0x%08X (%s) — refusing to nest further and falling "
                "back to the interpreter.\n",
                kVerifiedRamActiveCapacity, pc, thumb ? "thumb" : "arm");
        }
        // runtime_dispatch_miss retries ready overlays first. At this point
        // that can re-enter an already-active body and bypass this depth cap.
        runtime_bridge_interpret(pc, thumb != 0, 0u, 0u);
        return &verified_ram_dispatch_noop;
    }

    const auto mark_active = [&] {
        if (!active_guard.tail_redispatch)
            verified_ram_mark_active(pc, thumb);
    };

    const auto cached = g_verified_ram_cache.find(key);
    if (cached != g_verified_ram_cache.end()) {
        const bool local_words_current =
            cached->second.identity_index < kTransientCodeImages.size() &&
            verified_identity_local_words_current(cached->second.identity_index,
                                                  pc);
        if (verified_identity_pages_current(cached->second.identity_index) &&
            local_words_current) {
            if (cached->second.action == VerifiedRamAction::Native &&
                cached->second.fn != nullptr) {
                mark_active();
                return cached->second.fn;
            }
            // A fixed identity owns the ordinary static dispatch entry.
            return nullptr;
        }
        // The page epoch may have changed because of an unrelated write in
        // the same RAM page. Revalidate the identity that already won for
        // this PC first; scanning all overlapping overlays would otherwise
        // pay several full SHA-1s before reaching the same winner.
        const std::size_t identity_index = cached->second.identity_index;
        if (identity_index < kTransientCodeImages.size()) {
            const auto& candidate = kTransientCodeImages[identity_index];
            bool identity_refreshed = false;
            if (!local_words_current) {
                // A DMA/direct store changed the code near this PC without
                // advancing the page epoch.  Force the full identity scan;
                // refresh_verified_identity intentionally trusts a current
                // epoch and therefore cannot be used for this case.
                auto& identity = g_verified_identity_cache[identity_index];
                identity.valid = false;
                identity.matched = false;
                identity.snapshot.clear();
            } else {
                identity_refreshed = refresh_verified_identity(identity_index);
            }
            if (pc >= candidate.start && pc < candidate.end &&
                thumb == candidate.thumb && identity_refreshed) {
                if (cached->second.action == VerifiedRamAction::Native &&
                    cached->second.fn != nullptr) {
                    mark_active();
                    return cached->second.fn;
                }
                return nullptr;
            }
        }
        g_verified_ram_cache.erase(cached);
    }

    const auto cache_verified = [&](VerifiedRamAction action,
                                    void (*fn)(void),
                                    std::size_t identity_index) {
        g_verified_ram_cache[key] = {identity_index, action, fn};
    };

    bool covered = false;
    bool mode_matched = false;
    // This used to be a local std::array. Proactive registration grows the
    // candidate set to ~100 images; several KB per recursively nested guest
    // call exhausted the Windows host stack. The runtime is single-threaded,
    // and a successful nested dispatch makes its caller return immediately,
    // so reusable thread-local diagnostics preserve behavior without growing
    // each host call frame.
    thread_local std::array<std::string, kTransientCodeImages.size()>
        observed_sha1{};
    thread_local std::vector<std::size_t> observed_indices;
    for (const std::size_t index : observed_indices) observed_sha1[index].clear();
    observed_indices.clear();
    for (std::size_t candidate_index = 0;
         candidate_index < kTransientCodeImages.size(); ++candidate_index) {
        const auto& candidate = kTransientCodeImages[candidate_index];
        if (pc < candidate.start || pc >= candidate.end) continue;
        covered = true;
        if (thumb != candidate.thumb) continue;
        mode_matched = true;

        auto& identity = g_verified_identity_cache[candidate_index];
        if (!identity.valid || !verified_identity_pages_current(candidate_index)) {
            if (identity.valid) ++g_verified_identity_invalidations;
            // Registered compressed overlays are immutable in their .text
            // extent. Once one has matched, four spread-out words are enough
            // to reject a different overlay image without paying SHA-1. A
            // sparse match remains conservative: it falls through to the
            // existing full comparison and hash.
            if (identity.valid && identity.known_snapshot &&
                identity_sparse_fast_reject_safe(candidate) &&
                !identity_sparse_matches(candidate, identity)) {
                identity.matched = false;
                save_verified_identity_page_epochs(candidate_index);
                continue;
            }
            bool changed = !identity.valid ||
                           identity.snapshot.size() != candidate.end - candidate.start;
            if (!changed) {
                for (std::uint32_t offset = 0;
                     offset < candidate.end - candidate.start; ++offset) {
                    if (bus_read_u8(candidate.start + offset) !=
                        identity.snapshot[offset]) {
                        changed = true;
                        break;
                    }
                }
            }
            if (changed) {
                ++g_verified_identity_hashes;
                ++g_verified_identity_hash_counts[candidate_index];
                std::vector<std::uint8_t> image(candidate.end - candidate.start);
                for (std::uint32_t offset = 0; offset < image.size(); ++offset)
                    image[offset] = bus_read_u8(candidate.start + offset);
                const std::string actual = identity_image_sha1(candidate, image);
                observed_sha1[candidate_index] = actual;
                observed_indices.push_back(candidate_index);
                identity.snapshot = std::move(image);
                identity.matched = (actual == candidate.sha1);
                identity.known_snapshot = identity.matched &&
                    candidate.rom_start == 0u;
                if (identity.known_snapshot)
                    identity.sparse_words = identity_sparse_words(
                        candidate, &identity.snapshot);
            } else {
                ++g_verified_identity_content_hits;
            }
            save_verified_identity_page_epochs(candidate_index);
            identity.valid = true;
        } else ++g_verified_identity_cache_hits;
        if (!identity.matched) continue;
        // An entry inside the volatile prefix is served only while the live
        // bytes still equal ROM, and is never cached.
        if (pc < candidate.start + candidate.volatile_prefix &&
            !volatile_prefix_matches_rom(candidate))
            continue;
        const bool cacheable = pc >= candidate.start + candidate.volatile_prefix;

        if (candidate.dispatch_table == nullptr) {
            // This identity owns the fixed AOT entry in kDispatchTable.
            if (cacheable)
                cache_verified(VerifiedRamAction::Static, nullptr,
                               candidate_index);
            return nullptr;
        }
        if (void (*fn)(void) = lookup_variant(candidate, pc, thumb)) {
            if (cacheable)
                cache_verified(VerifiedRamAction::Native, fn, candidate_index);
            mark_active();
            return fn;
        }
        if (dynamic_ram_pc_repeat(pc)) {
            return dispatch_dynamic_ram(pc, thumb);
        }
        std::fprintf(stderr,
            "GoldenSunRecomp: verified transient image %s has no AOT entry "
            "for 0x%08X\n",
            candidate.name, pc);
        if (allow_dynamic_ram_pc("verified image is short an AOT entry", pc,
                                 thumb)) {
            return dispatch_dynamic_ram(pc, thumb);
        }
        dump_recent_trace();
        std::abort();
    }

    // No fixed-address image explains this PC. Try the position-independent
    // ones, which are keyed by identity rather than by address: a pooled
    // routine at a base nobody has ever recorded still dispatches, provided
    // its live bytes hash to a registered image. This runs before every abort
    // path below, including the mode mismatch — a stale ARM registration can
    // overlap a THUMB thunk installed at the same stack address later.
    if (RuntimeGuestFn fn = try_relocatable_dispatch(pc, thumb)) {
        mark_active();
        return fn;
    }
    // Particle stamp routines are pre-built for every builder input.
    if (RuntimeGuestFn fn = try_stamp_dispatch(pc, thumb)) {
        mark_active();
        return fn;
    }

    if (!covered) return nullptr;
    if (dynamic_ram_pc_repeat(pc)) {
        return dispatch_dynamic_ram(pc, thumb);
    }
    if (!mode_matched) {
        std::fprintf(stderr,
            "GoldenSunRecomp: transient code mode mismatch at 0x%08X\n", pc);
        if (allow_dynamic_ram_pc("registered image is the other mode", pc,
                                 thumb)) {
            return dispatch_dynamic_ram(pc, thumb);
        }
        dump_recent_trace();
        std::abort();
    }
    std::fprintf(stderr,
        "GoldenSunRecomp: unknown transient code identity at 0x%08X\n", pc);
    if (pc >= 0x03000000u && pc < 0x03002300u) {
        // At most once per 600 frames: show what rewrote the IWRAM code area.
        static unsigned long long s_last_dump_frame = 0;
        const unsigned long long frame = runtime_current_frame() + 1ull;
        if (s_last_dump_frame == 0 || frame - s_last_dump_frame >= 600ull) {
            s_last_dump_frame = frame;
            runtime_iwram_code_write_dump("unknown-identity");
        }
    }
    // Always-on (not behind Additional debug logging), bounded report for the
    // fixed image that covers this PC: which bytes differ from the ROM copy,
    // and what wrote into its range. The first candidate that covers the PC in
    // this mode and has a linear ROM source is the one reported; the number of
    // covering candidates is printed so a second one is not hidden.
    {
        const TransientCodeImage* failing = nullptr;
        std::size_t failing_index = 0;
        unsigned covering = 0;
        for (std::size_t candidate_index = 0;
             candidate_index < kTransientCodeImages.size(); ++candidate_index) {
            const auto& candidate = kTransientCodeImages[candidate_index];
            if (pc < candidate.start || pc >= candidate.end) continue;
            ++covering;
            if (!failing && candidate.rom_start != 0 &&
                candidate.thumb == thumb) {
                failing = &candidate;
                failing_index = candidate_index;
            }
        }
        // Same 600-frame spacing as the write dump above, so a routine that
        // keeps self-healing cannot flood stderr.
        static unsigned long long s_last_candidate_frame = 0;
        const unsigned long long frame = runtime_current_frame() + 1ull;
        if (failing && (s_last_candidate_frame == 0 ||
                        frame - s_last_candidate_frame >= 600ull)) {
            s_last_candidate_frame = frame;
            const std::string actual = observed_sha1[failing_index].empty()
                ? live_sha1(failing->start, failing->end - failing->start)
                : observed_sha1[failing_index];
            std::fprintf(stderr,
                "  failing candidate=%s range=0x%08X..0x%08X covering=%u "
                "expected=%s actual=%s\n",
                failing->name, failing->start, failing->end, covering,
                failing->sha1, actual.c_str());
            unsigned differences = 0;
            for (std::uint32_t offset = 0;
                 offset < failing->end - failing->start; ++offset) {
                const std::uint8_t live = bus_read_u8(failing->start + offset);
                const std::uint8_t source =
                    bus_read_u8(failing->rom_start + offset);
                if (live == source) continue;
                if (differences < 16) {
                    std::fprintf(stderr,
                        "    offset=0x%X live=0x%02X rom=0x%02X\n",
                        offset, live, source);
                }
                ++differences;
            }
            std::fprintf(stderr, "    differing_bytes=%u\n", differences);
            std::fprintf(stderr, "    live_prefix=");
            for (std::uint32_t offset = 0; offset < 32; ++offset) {
                std::fprintf(stderr, "%02x",
                             bus_read_u8(failing->start + offset));
            }
            std::fprintf(stderr, "\n");
            runtime_iwram_code_write_dump_range(
                "unknown-identity-candidate", failing->start, failing->end);
            // Journal of DMA/CPU copies into, and dispatches to, the overlay
            // unpacker slot (see runtime_arm.cpp): says WHEN and FROM WHERE
            // each routine was copied over 0x03002000.
            if (pc >= 0x03002000u && pc < 0x030022C4u)
                runtime_unpacker_slot_dump("unknown-identity");
        }
    }
    // A compressed overlay has no linear ROM source to diff against, so the
    // byte-level report below can say nothing about it. Dumping the live
    // window instead lets the image be matched offline against the pinned
    // decompressed `orig.bin` set without a TCP capture session.
    if (const char* dump_path = std::getenv("GBARECOMP_TRANSIENT_DUMP")) {
        std::uint32_t window_start = pc & ~0xFFFu;
        std::uint32_t window_end = window_start + 0x4000u;
        for (const auto& candidate : kTransientCodeImages) {
            if (pc < candidate.start || pc >= candidate.end) continue;
            if (candidate.start < window_start) window_start = candidate.start;
            if (candidate.end > window_end) window_end = candidate.end;
        }
        if (std::FILE* dump = std::fopen(dump_path, "wb")) {
            for (std::uint32_t addr = window_start; addr < window_end; ++addr) {
                std::fputc(bus_read_u8(addr), dump);
            }
            std::fclose(dump);
            std::fprintf(stderr,
                "  live window 0x%08X..0x%08X written to %s\n",
                window_start, window_end, dump_path);
        } else {
            std::fprintf(stderr,
                "  could not open GBARECOMP_TRANSIENT_DUMP path %s\n",
                dump_path);
        }
    }
    // The candidate-by-candidate byte report is evidence for a strict-static
    // abort, but the shared EWRAM overlay slot currently has scores of
    // candidates. Printing all of them during ordinary self-heal caused a
    // 200-line synchronous stderr burst on the first unknown identity. Keep
    // normal play concise; diagnostics can opt back into the full report.
    // Controlled by the config UI's "Additional debug logging" toggle
    // (default OFF); a strict-static run always reports in full.
    const bool verbose_identity_report =
        strict_static_run() || gsr_additional_debug_logging() != 0;
    if (!verbose_identity_report && g_dynamic_ram_pcs.empty()) {
        std::fprintf(stderr,
            "  identity details suppressed for normal play (%zu fixed, %zu "
            "relocatable candidates); turn on Additional debug logging to "
            "dump\n",
            observed_indices.size(), kRelocatableCodeImages.size());
    }
    for (std::size_t candidate_index = 0;
         verbose_identity_report &&
         candidate_index < kTransientCodeImages.size(); ++candidate_index) {
        const auto& candidate = kTransientCodeImages[candidate_index];
        if (observed_sha1[candidate_index].empty()) continue;
        std::fprintf(stderr,
            "  candidate=%s range=0x%08X..0x%08X expected=%s actual=%s\n",
            candidate.name, candidate.start, candidate.end,
            candidate.sha1, observed_sha1[candidate_index].c_str());
        if (candidate.rom_start == 0) {
            std::fprintf(stderr,
                "    compressed image: no linear ROM byte comparison\n");
            continue;
        }
        unsigned differences = 0;
        for (std::uint32_t offset = 0;
             offset < candidate.end - candidate.start; ++offset) {
            const std::uint8_t live = bus_read_u8(candidate.start + offset);
            const std::uint8_t source = bus_read_u8(candidate.rom_start + offset);
            if (live == source) continue;
            if (differences < 8) {
                std::fprintf(stderr,
                    "    offset=0x%X live=0x%02X rom=0x%02X\n",
                    offset, live, source);
            }
            ++differences;
        }
        std::fprintf(stderr, "    differing_bytes=%u\n", differences);
        std::fprintf(stderr, "    live_prefix=");
        for (std::uint32_t offset = 0; offset < 32; ++offset) {
            std::fprintf(stderr, "%02x", bus_read_u8(candidate.start + offset));
        }
        std::fprintf(stderr, "\n");
    }
    // What the position-independent images saw. A relocatable routine has no
    // declared range to report, so report the base the PC would imply if this
    // PC were its entry — the overwhelmingly common case — and the identity
    // actually resident there.
    for (std::size_t image_index = 0;
         verbose_identity_report && image_index < kRelocatableCodeImages.size();
         ++image_index) {
        const auto& image = kRelocatableCodeImages[image_index];
        std::fprintf(stderr,
            "  relocatable=%s mode=%s size=0x%X expected=%s\n",
            image.name, image.thumb ? "thumb" : "arm", *image.size,
            image.sha1);
        if (pc >= 0x02000000u && pc + *image.size <= 0x04000000u) {
            std::fprintf(stderr, "    at_entry_base=0x%08X actual=%s\n", pc,
                         live_sha1(pc, *image.size).c_str());
        }
    }
    if (allow_dynamic_ram_pc("no registered identity matches the live bytes; "
                             "this is runtime-generated code",
                             pc, thumb)) {
        return dispatch_dynamic_ram(pc, thumb);
    }
    dump_recent_trace();
    std::abort();
}

// The static kDispatchTable already has a fixed AOT entry for this
// transient-RAM pc, but runtime_ram_code_range_dirty() found a write
// recorded somewhere in its containing 4 KiB page since boot/load — the
// dirty bit is page-granular, not per-pc. verified_ram_dispatch() above
// already ran for this exact pc/thumb earlier in the same runtime_dispatch
// call, so its cache entry (if any) is guaranteed fresh here. A `Static`
// entry means it independently confirmed, via the registered identity's own
// SHA-1, that the reviewed ROM-sourced bytes at this pc still match live
// memory — the write that dirtied the page landed elsewhere in the page, not
// in this pc's own bytes — so the AOT entry is safe to use as-is.
int verified_ram_identity_confirmed(std::uint32_t pc, int thumb) {
    const auto cached = g_verified_ram_cache.find(verified_ram_key(pc, thumb));
    return cached != g_verified_ram_cache.end() &&
           cached->second.action == VerifiedRamAction::Static;
}

void print_usage() {
    std::printf(
        "GoldenSunRecomp [--bios <path> | --no-bios] [--rom <path>] [game.toml]\n"
        "\n"
        "The Golden Sun USA/Europe ROM is required and SHA-1 gated. The default\n"
        "also needs a canonical GBA BIOS (SHA-1 gated) and runs the faithful\n"
        "recompiled BIOS path; --no-bios (or GBARECOMP_NO_BIOS=1) reads no BIOS\n"
        "and runs the runtime's own replacements for the BIOS services instead.\n"
        "\n"
        "Expected ROM SHA-1: %s\n",
        kRomSha1);
}

}  // namespace

// Cross-TU accessors for gsr::function_tracer (src/function_tracer.cpp).
// Deliberately outside the anonymous namespace above so they get ordinary
// external linkage; the preceding block's state is still reachable here via
// the unnamed-namespace using-directive. Only called from the tracer's
// per-frame sample boundary, never the hot path.
//
// kTransientCodeImages holds two different kinds of row: small IWRAM
// identity-gate stubs and the large EWRAM overlay banks named
// "overlay_rom_*". Only the latter are meaningful here -- reporting a stub
// in their place is worse than reporting nothing. Several banks can be
// matched at once, so this is an index-based iterator: call with
// i = 0, 1, 2, ... until it returns nullptr.
extern "C" const char* gsr_overlay_name_at(std::size_t i) {
    std::size_t seen = 0;
    for (std::size_t idx = 0; idx < kTransientCodeImages.size(); ++idx) {
        const char* name = kTransientCodeImages[idx].name;
        if (std::strncmp(name, "overlay_rom_", 12) != 0) continue;
        if (!g_verified_identity_cache[idx].valid ||
            !g_verified_identity_cache[idx].matched) {
            continue;
        }
        if (seen == i) return name;
        ++seen;
    }
    return nullptr;
}

// TODO-EVIDENCE: room identity is not currently readable.
//
// The GSR_MAP_RECORD diagnostic that provided it was lost on 2026-09-04 when
// uncommitted work was destroyed by a git checkout during recovery from a
// half-finished widescreen removal. What was measured survives in FACTS.md:
// the pointer at 0x03001E70 is always 0x02030CCC and is useless as an
// identity, while the four bounds values beside it took 23 distinct
// combinations that tracked real room changes.
//
// What did NOT survive is the exact layout -- the offsets and widths of those
// four bounds fields relative to the room struct. Guessing them would violate
// the evidence rule in AGENTS.md, so this returns false until the layout is
// re-established by watching the writes. The tracer already treats an
// unavailable identity as a normal case; room tagging is simply absent.
extern "C" bool gsr_map_record_identity(std::uint32_t* room_ptr,
                                        std::uint32_t* min_x,
                                        std::uint32_t* min_y,
                                        std::uint32_t* ext_x,
                                        std::uint32_t* ext_y) {
    (void)room_ptr; (void)min_x; (void)min_y; (void)ext_x; (void)ext_y;
    return false;
}

int main(int argc, char** argv) {
    // Must be the FIRST thing in main, before SDL init or any other
    // subsystem, so as much of the run as possible is covered. nullptr =
    // default to the directory this executable lives in.
    gbarecomp::crash_handler_install(nullptr);
    // F1 "Crash log": a crash report also gets the game's last instructions
    // (freezes get the same trail from the hang watchdog, hang_fp_tail.csv).
    gbarecomp::crash_handler_set_extra_writer([](const char* dir) -> const char* {
        if (runtime_fp_count() == 0) return nullptr;
        char path[300];
        std::snprintf(path, sizeof(path), "%s\\crash_trail.csv", dir);
        return runtime_fp_save_tail_csv(path, 20000) ? "crash_trail.csv"
                                                     : nullptr;
    });
    // The field atlas source is now part of the evidence-backed widescreen
    // policy. Keep the payload-free producer trace opt-in for future source
    // investigations; set GBARECOMP_VRAM_MAP_TRACE=1 when needed.
    gba::vram_trace::set_default_enabled(false);

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    load_game_options(argc > 0 ? argv[0] : nullptr);

    // The mixer's code-only identities are correct only when the main corpus
    // stops at Func_dc8's self-modifying slots (main.toml runtime_code_entry
    // 0x03000A5C and 0x03000BD8). An older corpus has AOT entries there with
    // the ROM's placeholder bytes baked in and would play wrong audio.
    for (unsigned i = 0; i < kDispatchTableLen; ++i) {
        const std::uint32_t addr = kDispatchTable[i].addr;
        if ((addr == 0x03000A5Cu || addr == 0x03000BD8u) &&
            kDispatchTable[i].thumb == 0) {
            std::fprintf(stderr,
                "GoldenSunRecomp: the main corpus predates the Func_dc8 slot "
                "boundaries (AOT entry at 0x%08X). Regenerate it: "
                "scripts/gs.ps1 -To recompile, then build.\n", addr);
            return 1;
        }
    }

    gbarecomp::RunOptions options;
    options.builtin_game_name = "Golden Sun";
    options.builtin_rom_sha1 = kRomSha1;
    // No Nintendo code runs by default (Jimmy, 2026-09-24): our own stand-ins
    // replace every BIOS service. `--bios <path>` in a GBARECOMP_LINK_BIOS
    // build is the comparison run.
    options.no_bios_by_default = true;
    options.function_entry_observer = golden_sun_function_entry_observer;
    gsr::function_tracer_init();
    // After the tracer: map_recorder_init() chains onto whatever the
    // tracer just installed in host_config_ui.h's single extra-draw slot
    // (see map_recorder.cpp) so both toggles' UIs can coexist.
    gsr::map_recorder_init();
    // Independent of the map recorder: its own env flag, its own session
    // directory, no shared state. See obj_recorder.h.
    gsr::obj_recorder_init();
    battle_bg1_record_init();
    // After the recorder, for the same extra-draw chaining reason: each of
    // these captures whatever the previous one installed.
    gsr::room_buffer_init();
    // gbarecomp keeps one savestate-load hook; the tracer's handler is called
    // from here with the settings overlay's.
    gbarecomp::set_savestate_load_hook([] {
        gsr::function_tracer_on_savestate_load();
        g_settings_open.store(false);
        g_settings_screen_text = false;
        g_hard_prompt_open = false;
        g_settings_page.store(1);
        native_page_end(false);
    });
    // Two fresh, single-purpose gbarecomp hooks (nothing else installs
    // either), so no chaining is needed: gpu_field_capture_hook copies this
    // frame's description out at VBlank start, gpu_field_present_override
    // draws it at present time. Both are cheap no-ops whenever
    // GSR_GPU_FIELD is off.
    gbarecomp::set_scene_capture_hook(gpu_field_capture_hook);
    gbarecomp::set_frame_present_override_hook(golden_sun_present_override);
    options.max_view_width = 360;
    options.max_view_height = 240;
    options.frame_interpolation_available = true;
    options.enhanced_timing_available = true;
    options.native_renderer_available = true;
    options.max_resize_view_width = 240;
    options.resize_driven_view = false;
    options.extended_view_init = install_golden_sun_widescreen;
    options.rom_patch = gsr::mods::patch_rom;
    options.thumb_alu_immediate_override = golden_sun_thumb_alu_immediate;
    options.thumb_literal_override = golden_sun_thumb_literal;
    options.swi_override = golden_sun_swi_override;
    options.conditional_branch_override = golden_sun_conditional_branch;
    options.game_menu_toggle = cheat_menu_toggle;
    options.keyinput_filter = cheat_menu_keyinput_filter;
    options.paused_overlay = cheat_menu_paint;
    options.pause_request_poll = cheat_menu_pause_poll;
    options.launcher_region = "USA/Europe";
    options.launcher_game_config = "game.toml";
    options.launcher_expose_widescreen = true;
    options.launcher_expose_adaptive_view = false;
    options.widescreen_view_width = 360;
    options.widescreen_view_height = 240;
    options.launcher_aspect_labels = kGoldenSunAspectLabels;
    options.launcher_aspect_view_widths = kGoldenSunAspectWidths;
    options.launcher_aspect_view_heights = kGoldenSunAspectHeights;
    options.launcher_num_aspects = 2;
    g_runtime_ram_dispatch_hook = verified_ram_dispatch;
    g_runtime_ram_identity_confirmed_hook = verified_ram_identity_confirmed;
    g_runtime_call_return_hook = verified_ram_dispatch_return_hook;
    g_runtime_guest_step_boundary_hook = verified_ram_dispatch_outer_boundary;
    g_runtime_mem_write_observer = text_delay_logging_enabled()
        ? observe_text_delay_store : nullptr;
    g_runtime_mem_write_override = golden_sun_write_override;
    init_ram_code_page_masks();
    g_verified_ram_cache.clear();
    g_verified_identity_cache = {};
    {
        std::size_t static_aot_ranges = 0;
        for (const auto& image : kTransientCodeImages) {
            if (image.dispatch_table == nullptr) ++static_aot_ranges;
        }
        std::fprintf(stderr,
            "GoldenSunRecomp: %zu transient-RAM range(s) registered to defer "
            "to the static AOT dispatch table once their identity matches "
            "(no heal/JIT needed).\n",
            static_aot_ranges);
    }
    g_relocatable_identity_cache = {};
    const int result = gbarecomp::run_game(argc, argv, options);
    // Printed once, after the run. The launcher's "Cost probe" test toggle sets
    // GBARECOMP_COST_PROBE (same test as runtime.cpp), which never sets the
    // RAM_CACHE_STATS variable, so either one turns the report on.
    const char* cost_probe_env = std::getenv("GBARECOMP_COST_PROBE");
    const bool cost_probe_on = cost_probe_env != nullptr &&
        !(cost_probe_env[0] == '0' && cost_probe_env[1] == '\0');
    if (std::getenv("GBARECOMP_RAM_CACHE_STATS") || cost_probe_on) {
        std::fprintf(stderr,
            "ram_cache_stats frames=%llu epoch=%llu hashes=%llu "
            "image_hits=%llu content_hits=%llu invalidations=%llu "
            "pc_entries=%zu\n",
            static_cast<unsigned long long>(runtime_current_frame()),
            g_ram_write_epoch, g_verified_identity_hashes,
            g_verified_identity_cache_hits, g_verified_identity_content_hits,
            g_verified_identity_invalidations,
            g_verified_ram_cache.size());
        std::fprintf(stderr,
            "stamp_identity_stats scans=%llu hashes=%llu\n",
            g_stamp_identity_scans, g_stamp_identity_hashes);
        if (std::getenv("GBARECOMP_RAM_CACHE_DETAIL") || cost_probe_on) {
            for (std::size_t i = 0; i < kTransientCodeImages.size(); ++i) {
                if (g_verified_identity_hash_counts[i] == 0) continue;
                std::fprintf(stderr, "  ram_cache_image=%s hashes=%llu matched=%d known=%d\n",
                    kTransientCodeImages[i].name,
                    g_verified_identity_hash_counts[i],
                    g_verified_identity_cache[i].matched ? 1 : 0,
                    g_verified_identity_cache[i].known_snapshot ? 1 : 0);
            }
        }
        std::fprintf(stderr,
            "relocatable_cache_stats hashes=%llu cache_hits=%llu "
            "identity_snapshot_hits=%llu resident_hashes=%llu "
            "resident_hash_bytes=%llu func1dc8_writer_checks=%llu\n",
            g_relocatable_identity_hashes, g_relocatable_identity_cache_hits,
            g_relocatable_identity_snapshot_hits,
            g_relocatable_resident_hashes,
            g_relocatable_resident_hash_bytes, g_func1dc8_writer_hashes);
    }
    gbarecomp::crash_handler_mark_clean_exit();
    return result;
}
