// The engine plug-in calls this runner's main directly.
#define SDL_MAIN_HANDLED

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
#include "parked_characters_tbs.h"
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
#include "treasure_randomizer_tbs.h"
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
#include "temporal_blend.h"
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
#include "../../shared_game/world_margin_live.h"

extern "C" unsigned g_ws_active;
extern "C" unsigned long long runtime_current_frame();
extern "C" void runtime_iwram_code_write_dump(const char* why);
extern "C" void runtime_iwram_code_write_dump_range(const char* why,
                                                    std::uint32_t lo,
                                                    std::uint32_t hi);
extern "C" void runtime_unpacker_slot_dump(const char* why);


namespace {
using namespace gsr_runner;

// View sizes and aspect rules are shared with The Lost Age in the engine's
// view_config.h (engine/src/runtime).

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

}  // namespace
namespace gsr_runner {
std::uint32_t g_golden_sun_wide_extra_left = 0;
}  // namespace gsr_runner
namespace {
std::uint32_t g_golden_sun_wide_extra_right = 0;
}  // namespace
namespace gsr_runner {
std::uint32_t g_golden_sun_wide_extra_top = 0;
}  // namespace gsr_runner
namespace {
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
bool golden_sun_func1dc8_d4_store_pc(std::uint32_t pc);
void remember_golden_sun_obj_b328_authorization(
    std::uint32_t record_base, std::uint16_t attr0, std::uint16_t attr1,
    std::uint16_t attr2);
// WIDE-01 body/shadow identity trace (see the full definition and comment
// near kGoldenSunObjCommitOrderLimit below); forward-declared so the EC/F0
// Func_1dc8 route observers above can call it.
enum class GoldenSunObjCommitOrderRoute : std::uint8_t { D4, EC, F0 };
// Record slots 0..5 at 0x03002000 count as sampled bodies on the EC and F0
// routes (case (b) in the WIDE-01 trace comments below).
constexpr std::uint32_t kGoldenSunObjCommitOrderBodySlotLimit = 6u;
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
}  // namespace
namespace gsr_runner {
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
}  // namespace gsr_runner
namespace {
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
    std::uint32_t entry_pc = GS_ROM(0x0800B328u);
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
// The cull decision stream reuses the correlation log bound but resets its
// dedupe window each guest frame (see trace_golden_sun_obj_y_cull_decision).
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
    route->pc = GS_ROM(0x0800B27Eu);
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
    // The bound is hardware (the array's shape), not how far the producer
    // audit reached: an audit-sized bound refused a trusted position to every
    // later actor, which clips a sprite standing in the expanded top margin.
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
    // so neither the expanded-view renderer nor the census should count it.
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
    // affine transform, so any off-screen test must still refuse it. Kept
    // as its own flag rather than an outcome so the two questions -- "can we
    // place it?" and "can we cull it?" -- are not answered by one bit.
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
// Shared by the expanded-view sprite placement (pending provenance) and the
// GSR_OBJ_RECORD census (obj_recorder.h): the census has to measure exactly the lookup the
// renderer would depend on. Read-only: it inspects guest memory and the
// per-frame F0 context table and changes neither.
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
    // Affine sprites are resolved like any other: Golden Sun scales sprites
    // constantly (walk squash, shadows), and refusing them left half the
    // sprites on the 8-bit byte, which wraps. `out.affine` carries the
    // restriction to any off-screen test instead.
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

}  // namespace
namespace gsr_runner {
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
    // WIDE-01 body/shadow identity trace (see note_golden_sun_obj_commit_order).
    // EC fires on every active object every frame (D4 only fires on a gated,
    // successful commit), and the ATTR-shaped bytes at this seam are still
    // mid-update from other writers; logging every EC entry flooded the cap
    // and stalled scene transitions. So only two bounded cases are admitted,
    // everything else is not recorded at all:
    //  (a) the shadow-candidate signature (tile=0, shape=1, size=0) from
    //      WIDE-01_NPC_IDENTITY.md (archived under Archive/2026-10-12/
    //      The Broken Seal/docs/OLD/issues/);
    //  (b) the first kGoldenSunObjCommitOrderBodySlotLimit record slots,
    //      computed from `staging` (a body event, so `staging` is the record
    //      base, no +0x0C offset). They give independent body-sprite ground
    //      truth for the scroll-transform check in that doc, which D4's own
    //      attrs cannot, since D4 reads them from the same record.
    // Slot is only known later, at F0, so this reports slot=-1. Signed logical
    // X/Y would need the D4 staging-provenance lookup; it is left invalid.
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
}  // namespace gsr_runner
namespace {

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
    // This function also feeds the F0 placement lookup below, so it must run
    // when diagnostics, the Enhanced Options fixes
    // (GBARECOMP_EXPERIMENTAL_FIXES) or the recorder is on -- not diagnostics
    // alone. All diagnostics-only work inside self-gates on
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
        // Capture the committed ATTR payload as the guest wrote it; diagnostics
        // must describe the guest write, not any later presentation-only change.
        link_golden_sun_b328_parentless_f0(
            writer_frame, writer_depth, writer_return_pc, writer_pc,
            slot_address, slot, bus_read_u16(slot_address),
            bus_read_u16(slot_address + 2u),
            bus_read_u16(slot_address + 4u));
    }
    // Placement lookup, shared by the GSR_OBJ_RECORD census and the Enhanced
    // Options sprite placement. The census has to run without the fixes being
    // on -- it measures, it must not change what is drawn -- so the gate is
    // either toggle.
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
        // renderer nor the census counts them.
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
    // WIDE-01 body/shadow identity trace; same two-case admission as the EC
    // route (see golden_sun_obj_f0_entry_capture). F0 fires on every committed
    // OAM-shadow write and ATTR0/ATTR1 carry the live raw Y/X, so an
    // unfiltered trace changes nearly every frame for every moving object.
    // Case (b) here is the first kGoldenSunObjCommitOrderBodySlotLimit
    // destination OAM slots. OAM slots are reused independently of record
    // slots, so they do not pair 1:1 with EC's record slots; only frames where
    // both admit the same object serve the cross-check, found empirically in
    // WIDE-01_NPC_IDENTITY.md. The source record is not resolved at this seam
    // (only EC captured it), so source=0. Signed logical X/Y is not available
    // either: the ATTR bytes are already truncated to hardware width.
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
        const bool f0_is_sampled_body_slot =
            static_cast<std::uint32_t>(slot) <
                kGoldenSunObjCommitOrderBodySlotLimit;
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
        (pc != GS_ROM(0x0800B3E6u) && pc != GS_ROM(0x0800B3ECu) &&
         pc != GS_ROM(0x0800C702u) && pc != GS_ROM(0x0800C708u))) return;
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
        pc == GS_ROM(0x0800B3E6u) ? 0u : pc == GS_ROM(0x0800B3ECu) ? 1u
        : pc == GS_ROM(0x0800C702u) ? 2u : 3u] = observation;
}

GoldenSunObjYCorrelation capture_golden_sun_obj_y_correlation(
    std::int32_t operand) {
    GoldenSunObjYCorrelation result{};
    if (!golden_sun_expanded_obj_view_active() || operand < 160 ||
        operand > 199) return result;
    result.valid = true;
    result.b328_pc = GS_ROM(0x0800B328u);
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
        GS_ROM(0x0800B3E6u), GS_ROM(0x0800B3ECu), GS_ROM(0x0800C702u), GS_ROM(0x0800C708u)};
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
    const bool object_y_route = branch_pc == GS_ROM(0x0800B27Eu) ||
        branch_pc == GS_ROM(0x0800B328u);
    if (!object_y_route) return;
    const auto policy_lower = static_cast<std::int32_t>(
        gsr::widescreen::kNativeHeight);
    const auto policy_upper = static_cast<std::int32_t>(
        gsr::widescreen::kNativeHeight + g_golden_sun_wide_extra_bottom);
    const bool in_policy_band = operand >= policy_lower && operand < policy_upper;
    const bool outside_policy_band = operand >= policy_upper;
    if (!golden_sun_wide_diagnostics_enabled() ||
        !golden_sun_expanded_obj_view_active() ||
        (!in_policy_band && !outside_policy_band)) return;
    const std::uint64_t frame = runtime_current_frame();
    // The dedupe window is per guest frame: a stationary object must not
    // consume the whole log budget before its later bottom crossing.
    if (g_golden_sun_obj_y_cull_decision_frame != frame) {
        g_golden_sun_obj_y_cull_decision_keys = {};
        g_golden_sun_obj_y_cull_decision_logs_in_frame = 0;
        g_golden_sun_obj_y_cull_decision_frame = frame;
    }
    const auto correlation = branch_pc == GS_ROM(0x0800B328u) && in_policy_band
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

}  // namespace
namespace gsr_runner {
// GBARECOMP_VRAM_MAP_TRACE enables this payload-free map and sprite
// investigation; it is off unless set.
bool golden_sun_wide_diagnostics_enabled() {
    static const bool enabled = [] {
        return gbarecomp::env_flag("GBARECOMP_VRAM_MAP_TRACE");
    }();
    return enabled;
}
}  // namespace gsr_runner
namespace {

// Enhanced Options: the launcher sets GBARECOMP_EXPERIMENTAL_FIXES to 1 on
// every launch (launcher/session_environment.h); unset or 0 leaves it off. It
// gates the expanded-sprite and shadow guards below.
bool golden_sun_experimental_fixes_enabled() {
    static const bool enabled = [] {
        return gbarecomp::env_flag("GBARECOMP_EXPERIMENTAL_FIXES");
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

// WIDE-01 entity-producer census: bounded IWRAM write census for the 0x38-stride
// object staging records at 0x03002000..0x030022E0. The commit identity rule
// admits the N=15 body beyond this range, but its producer writes have not
// been measured, so the census stops here. Diagnostics-only, deduped by
// (address, pc, size), capped per epoch, and reset at the same auth-epoch
// boundaries as the field producer trace above. Produces nothing unless
// widescreen diagnostics are enabled; never consulted by gameplay/render code.
constexpr std::uint32_t kGoldenSunObjRecordCensusStart = 0x03002000u;
constexpr std::uint32_t kGoldenSunObjRecordCensusEnd = 0x030022E0u;
// CRASH-03 pool-LDM probe's stack window [base,end), 16 words. It is passed to
// the engine probe as its layout at startup and also used by the fast-IWRAM
// write watch.
constexpr std::uint32_t kGoldenSunPoolLdmWindowBase = 0x03007E00u;
constexpr std::uint32_t kGoldenSunPoolLdmWindowEnd = 0x03007E40u;
// A 64-entry cap overflowed (Bilibin ~20005, Palace ~2912 write events; the
// overflow counts rejected writes, not distinct keys). The range is 0x2E0 = 736
// bytes. Worst measured case: one writer identity per byte (Palace's
// sequential fill); steady-state field has a handful of (pc,size) writers per
// byte. 4096 covers 736 bytes x ~5 identities with headroom, and the linear
// scan only runs for writes inside this window, never the rest of guest memory.
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
// committed write in the order the guest performs it within a frame, tagged
// with its route, so a body write and its shadow write for the same entity
// can be matched by adjacent sequence numbers whichever route produced them.
// Diagnostics-only; deduped against the preceding entry for the same (route,
// slot), so frames where that slot is unchanged are skipped. Capped, reset at
// the same auth-epoch boundaries as the rest of this census, and never
// consulted by gameplay/render code. See WIDE-01_NPC_IDENTITY.md (archived).
// (GoldenSunObjCommitOrderRoute is forward-declared near the top of this file.)
constexpr const char* golden_sun_obj_commit_order_route_name(
    GoldenSunObjCommitOrderRoute route) {
    switch (route) {
        case GoldenSunObjCommitOrderRoute::D4: return "D4";
        case GoldenSunObjCommitOrderRoute::EC: return "EC";
        case GoldenSunObjCommitOrderRoute::F0: return "F0";
    }
    return "?";
}
// 256 fit the D4 route alone but dropped events once EC/F0 were added; 512
// covers all three routes with headroom while staying well under the
// per-epoch volume that caused synchronous-fprintf transition stalls
// (WIDE-01_ENTITY_PRODUCER_FINDINGS.md, "Performance"; archived).
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
    // change). EC/F0 are restricted at their call sites to the shadow
    // signature or the first few body slots, but a moving object's raw ATTR0/ATTR1 (Y/X) still
    // changes almost every frame, which would flood the cap and stall scene
    // transitions. So EC/F0 dedupe coarsely on (route, dedupe_index) alone:
    // log only the first frame this object is seen in the epoch (or the first
    // frame after an absence). That still answers "does this route carry the
    // shadow signature, and where in write order"; the per-frame offset
    // tracking comes from D4's own logical X/Y.
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
            256u * 1024u, true,
            std::max(g_golden_sun_wide_extra_left, g_golden_sun_wide_extra_right),
            std::max(g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom));
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
    // position (low 5 scroll bits dropped). Raw scroll changes every frame
    // while walking and would exhaust the 64-entry bound before a later scene.
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

// Parallax capture: does each BG layer's scroll hold a constant offset/ratio
// to the gameplay layer within a room, readable from the hardware scroll
// registers alone? Written to wide_scroll_trace.csv at exit. Gated by
// GBARECOMP_VRAM_MAP_TRACE like the rest of this file. One row per authentic
// visible frame (the margin-policy callback fires once per raster line). A
// ring keeps the last kGoldenSunWideScrollTraceCap frames (~5.5 minutes at
// 60 fps); older rows are dropped.
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

// Record at most one row per frame. The map/raw CRCs use the same
// golden_sun_field_table_crc and EWRAM offsets as trace_golden_sun_field_map.
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

// Flush the ring once at exit (metadata only, no map bytes), the same way
// hang_trace.csv is written in engine/src/runtime/runtime_bus_bridge.cpp.
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

    // Alongside the renderer's own row dump (armed by the same setting,
    // g_ws_row_state_dump_mode): what this rule decided, once per frame.
    // Without it a capture shows what the game did but not what we did about it.
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
    // (black_nonuniform_window_margins). Measured in the Tret approach: the
    // drawn strip is exactly 240 columns wide across an 81-row band
    // (authentic rows 40..120), both margins black, which is that rule firing
    // for the light window the game runs there.
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
    // Terrain-only Mode0ScrollMismatch field (non-Palace split scroll): a
    // direct per-row classification, not a multi-frame stability gate. No
    // cross-layer offset is trusted, only the terrain layer's own live
    // register (see golden_sun_field_terrain_bg / golden_sun_wide_tilemap_provider).
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
    // The BG X-provider has no IO arguments, so publish this per-scanline
    // scene bit just before the PPU invokes it; invalid/transition frames
    // then cannot inherit Mode 0 cutoff behaviour.
    // Equal-scroll field/object authorization stays separate from the
    // complete-frame Palace split-scroll authorization: a split-scroll frame
    // must not inherit the equal-scroll object's culls.
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
    // golden_sun_field_terrain_bg. Already exclusive with the Palace path
    // (g_golden_sun_mode0_terrain_field is set only when
    // split_frame_authorized is false); the check is kept explicit.
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
                &boundary_metadata, nullptr, false, false, false,
                g_golden_sun_wide_extra_left, g_golden_sun_wide_extra_right,
                g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom);
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
    // read their own live raw scroll register; no cross-layer reconstruction.
    gsr::widescreen::GoldenSunFieldTilemapMetadata metadata;
    const bool resolved = gsr::widescreen::golden_sun_field_tilemap_entry(
                g_golden_sun_wide_line_dispcnt,
                g_golden_sun_wide_line_io.data(),
                g_golden_sun_wide_line_io.size(), bus->ewram_ptr(),
                256u * 1024u, bg, hw_x, screen_y, out_entry, &metadata,
                nullptr, false, split_scroll_palace, terrain_field,
                g_golden_sun_wide_extra_left, g_golden_sun_wide_extra_right,
                g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom);
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
    // widened right edge: raw X from 0x100 up to 240 plus the right margin.
    // Values outside that envelope keep the hardware signed decode, and
    // non-field scenes never opt into this hook.
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
// Scanning exactly one stride means a hit can never alias into the next
// actor's record, and the window still includes +0x0C.
//
// Encodings: unsigned/signed 16-bit, unsigned/signed 32-bit, and 16.16
// fixed point (the camera at 0x02030DB0 is 16.16, so other guest position
// fields may be too).
//
// Candidate kinds: the captured value (B324/B328) is a screen coordinate,
// while a guest actor record would normally hold a world coordinate.
// Comparing only against the raw screen value tests whether the record
// stores screen space verbatim. So each record pattern is also compared
// against the captured value with the camera's integer pixel position added
// and subtracted, covering both world-to-screen sign conventions. The camera
// (x then y) is read once per sample as room_buffer.cpp decodes it: the high
// 16 bits of each 32-bit field, read unsigned via bus_read_u16.
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
        gbarecomp::at_game_exit(report_golden_sun_obj_position_probe);
    }
    const int axis = x_axis ? 0 : 1;
    ++g_golden_sun_obj_position_probe_samples[axis];
    auto& tally = g_golden_sun_obj_position_probe_hits[axis];

    // Camera integer pixel part (see the decode note above the probe).
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
            // 16.16 fixed point, integer part only (arithmetic shift keeps
            // the sign).
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
        if (instruction_pc == GS_ROM(0x0800B328u) && logical_coordinate >= 160 &&
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

}  // namespace
namespace gsr_runner {
void golden_sun_obj_staging_handoff(
    std::uint32_t entry_pc, std::uint32_t staging_address_override,
    std::uint32_t destination_override) {
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
}  // namespace gsr_runner
namespace {

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
        gsr::widescreen::kNativeWidth + g_golden_sun_wide_extra_left +
            g_golden_sun_wide_extra_right,
        gsr::widescreen::kNativeHeight + g_golden_sun_wide_extra_top +
            g_golden_sun_wide_extra_bottom,
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

// Forward declaration: golden_sun_obj_provider_provenance below calls this
// to populate the persistent position table (defined further down, next to
// golden_sun_obj_track_lookup) when it authenticates a sprite.
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
// by an OAM wrap can share them. Slot keying guarded against this
// incidentally; identity does not, so an eligible match must additionally be
// UNIQUE within its store. Two or more equally-eligible records means the
// object cannot be told apart from another -- refuse rather than guess.
//
// tally_rejection: attribute this object's outcome to g_ws_obj_reject_totals.
// Both the X and Y wide providers call this for the same object on the same
// frame; only the X provider passes true, so each object is counted once.
// The bucket recorded is the VISIBLE slot's own reason (a by-slot diagnostic
// baseline), unless the identity search found a usable record (bucket "ok")
// or found two-or-more equally-eligible records in either store (the
// existing "identity" bucket, so signals.csv's column meanings stay put).
//
// Called once per object PER SCANLINE by both providers (up to 160 times a
// frame per on-screen object), so the identity search is memoised per OAM
// entry. The memo is keyed by frame AND by
// g_golden_sun_obj_provenance_generation: the visible store is replaced
// wholesale at the shadow->OAM handoff, but the PENDING store is written per
// slot by the commit path DURING a frame, so a frame-only key would hide a
// record committed after the first scanline until the next frame. A frame
// that commits heavily while the PPU is drawing invalidates and re-scans;
// that cost has not been measured.
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
    // Each branch bails at the first failing clause, labelled with which
    // clause it was. check_slot selects between the by-slot identity check
    // (kept only for the diagnostic baseline below) and the by-identity
    // search; every other clause is shared.
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
        // Feed the persistent position table: fires once per object per
        // frame (inside the memo recompute) when the record path just
        // authenticated a position. Only while the expanded view is active,
        // since that is the table's only consumer (golden_sun_obj_track_lookup
        // below). allow_track_confirm is false for the diagnostic accessor
        // below: it reads live OAM at a different point in the frame than the
        // renderer, and a measurement tool must never change what is drawn.
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
// On the world map the actor table decides from raw 128 up. A town ahead of
// the player has its top at y < 73 (dz 0) and, with the town box's upper z
// bound at -(300 + 2 x top margin), at y >= -123 (fit in
// golden_sun_worldmap_range_box): raw 133..255 when above the screen. One
// behind the player is below y 73. So raw 128..255 is "above" (raw - 256,
// -128..-1) or "below" (raw), and the side says which; raw < 128 is 0..127
// either way. Starting at 160 drew towns 377..449 units ahead (raw 139..158,
// really y -117..-98) in the bottom half of the screen (gpu_rewind_0025,
// 2026-10-14).
constexpr int kGoldenSunWorldSideFirstRawY = 128;

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
const std::uint32_t kGoldenSunActorContextPtr = gs::ram(0x03001EBCu);
constexpr std::uint32_t kGoldenSunActorTableOffset = 0x14u;
constexpr std::uint32_t kGoldenSunActorFocusOffset = 0x1E0u;
constexpr int kGoldenSunWorldActorFirst = 8;
constexpr int kGoldenSunWorldActorLast = 0x41;
constexpr std::uint32_t kGoldenSunActorSpriteOffset = 0x50u;
constexpr std::uint32_t kGoldenSunActorFlagOffset = 0x54u;
constexpr std::uint32_t kGoldenSunActorZOffset = 0x10u;
constexpr std::uint32_t kGoldenSunActorXOffset = 0x08u;

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
    std::int8_t side;    // +1 below the player, -1 above, 0 level
    std::int8_t side_x;  // +1 right of the player, -1 left, 0 level
};

// Which side of the player the world-map actor drawing this OAM entry
// (attr0/attr1 equal to its sprite record) is on: y +1 below / -1 above, x +1
// right / -1 left. 0 on an axis when this is not the world map, no drawn actor
// matches, the nearest ones disagree, or the distance on it is exactly 0. The
// table is scanned at most once per frame. X: towns the widened town box
// switches on just past the left edge come with raw X 256+, and both readings
// fell inside the fallback's slack; a town 237 units left (raw 328, really
// -184) was drawn in the right margin (gpu_rewind_0028/0029, 2026-10-14).
struct GoldenSunWorldSide {
    int y = 0;
    int x = 0;
};
GoldenSunWorldSide golden_sun_world_map_actor_side(std::uint16_t attr0,
                                                   std::uint16_t attr1) {
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
                const std::int32_t focus_x = static_cast<std::int32_t>(
                    bus_read_u32(focus + kGoldenSunActorXOffset));
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
                    const std::int64_t dx =
                        static_cast<std::int64_t>(static_cast<std::int32_t>(
                            bus_read_u32(actor + kGoldenSunActorXOffset))) -
                        focus_x;
                    if (dz == 0 && dx == 0) continue;
                    cache[cache_count++] = {
                        bus_read_u16(sprite + 4u), bus_read_u16(sprite + 6u),
                        static_cast<std::int8_t>(dz > 0 ? 1 : dz < 0 ? -1 : 0),
                        static_cast<std::int8_t>(dx > 0 ? 1 : dx < 0 ? -1 : 0)};
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
    int side = 0, side_x = 0;
    int best = INT_MAX;
    bool tie_disagrees = false, tie_disagrees_x = false;
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
            side_x = cache[i].side_x;
            tie_disagrees = tie_disagrees_x = false;
        } else if (distance == best) {
            if (cache[i].side != side) tie_disagrees = true;
            if (cache[i].side_x != side_x) tie_disagrees_x = true;
        }
    }
    GoldenSunWorldSide out;
    out.y = tie_disagrees ? 0 : side;
    out.x = tie_disagrees_x ? 0 : side_x;
    return out;
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
                                           int* out_x, int* out_y,
                                           bool* by_world_side = nullptr) {
    if (by_world_side) *by_world_side = false;
    if (!golden_sun_expanded_obj_view_active() || !out_x || !out_y)
        return false;
    const int raw_x = static_cast<int>(attr1 & 0x01FFu);
    const int raw_y = static_cast<int>(attr0 & 0x00FFu);
    if (gsr::field_obj_is_parked(attr0, attr1)) return false;
    const int min_x = -static_cast<int>(g_golden_sun_wide_extra_left) -
                       kGoldenSunObjFallbackSlackX;
    const int max_x = static_cast<int>(gsr::widescreen::kNativeWidth +
                       g_golden_sun_wide_extra_right) +
                       kGoldenSunObjFallbackSlackX;
    // X: exactly one candidate must land inside the viewport. Zero means the
    // sprite is nowhere near the view; two means the coordinate does not say
    // where it is. Both are a refusal.
    const bool near_x = raw_x >= min_x && raw_x <= max_x;
    const bool wrapped_x = (raw_x - 512) >= min_x && (raw_x - 512) <= max_x;
    // On the world map the actor table says which side each town is on (see
    // golden_sun_world_map_actor_side): asked for raw Y 128+ and raw X 256+,
    // the bands with two readings. Not while a game window is open (below).
    const GoldenSunWorldSide world =
        (raw_y >= kGoldenSunWorldSideFirstRawY || raw_x >= 256) &&
                !game_window_open_this_frame()
            ? golden_sun_world_map_actor_side(attr0, attr1)
            : GoldenSunWorldSide{};
    int resolved_x = 0;
    if (raw_x >= 256 && world.x != 0) {
        resolved_x = world.x > 0 ? raw_x : raw_x - 512;
    } else {
        if (near_x == wrapped_x) return false;
        resolved_x = near_x ? raw_x : raw_x - 512;
    }
    // Y: on the world map the actor table says which side the sprite is on,
    // for the whole ambiguous range (see golden_sun_world_map_actor_side).
    // Otherwise only the two unambiguous bands resolve; the aliased band
    // refuses. Not while a game window is open: a menu's own sprites are
    // not actors, and the selected party member bouncing to y = -4 (raw
    // 252) matched actor 16's 32x32 record and was sent below the screen
    // (logs/gpu_rewind_0061, frames 29-33, 2026-10-01).
    const int world_side = raw_y >= kGoldenSunWorldSideFirstRawY ? world.y : 0;
    if (world_side != 0) {
        *out_x = resolved_x;
        *out_y = world_side > 0 ? raw_y : raw_y - 256;
        if (by_world_side) *by_world_side = true;
        return true;
    }
    const int alias_last = std::min(255,
        kGoldenSunObjFallbackAliasLastRawY +
        static_cast<int>(g_golden_sun_wide_extra_bottom) -
        gsr::widescreen::kExpandedExtraY);
    if (raw_y >= kGoldenSunObjFallbackAliasFirstRawY &&
        raw_y <= alias_last) return false;
    const int resolved_y = raw_y > alias_last
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
//
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
    // Last-seen ATTR0/1/2. The lookup compares only the shape/size bits of
    // ATTR0/ATTR1; ATTR2 is stored but not consulted.
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
// frames (~5s at ~59.7Hz, the window map_recorder.cpp's flush_signal_log
// uses via kSignalFlushRows) is above the one measured floor yet short
// enough that a slot abandoned mid-play (character despawned, scrolled far
// off) frees up within a few seconds.
constexpr std::uint64_t kGoldenSunObjTrackExpiryFrames = 300;

// Record an authenticated position. Called from one place only --
// golden_sun_obj_provider_provenance's once-per-object-per-frame memo
// recompute -- so it runs at most once per object per frame even though both
// wide providers query that memo every scanline.
//
// Finds the tracked entry whose last position is nearest (x, y); if one is
// within the match radius, that is the same character seen again and its
// entry is refreshed in place. Otherwise the position belongs to a character
// not currently tracked (or tracked in a now-expired/reused slot), so a free
// slot is claimed. Expired entries (unconfirmed for longer than
// kGoldenSunObjTrackExpiryFrames) are freed while scanning; the scan runs on
// every authenticated object, and ordinary field play authenticates most of
// them (FACTS.md 2026-09-11), so an expired entry does not linger.
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
// never pick a nearest. An untracked or ambiguous sprite is left to the
// raw-coordinate fallback instead of a guess.
//
// Both wide providers call this ONE function (see golden_sun_wide_obj_attr_x_
// provider and golden_sun_wide_obj_attr_y_provider) so X and Y always agree
// on whether, and where, a sprite gets a tracked position -- the same
// reason golden_sun_wide_obj_fallback_position is itself shared. Memoised per
// OAM entry per frame so the scan (up to 64 entries, four candidates) runs
// at most once per object per frame despite being called once per scanline.
// The memo key is frame-only, unlike golden_sun_obj_provider_provenance's
// frame+generation key: a mid-frame table update from another object's
// confirmation could let a later scanline of this object succeed where an
// earlier one declined, but a stale memo only causes an extra decline, never
// a wrong placement.
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
    // a moving one gets the correct one.
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
    // Follow the character. Otherwise the entry stays pinned to wherever
    // the record path last spoke, a walking character drifts out of the
    // match radius, and is dropped a few pixels later (seen as a child
    // running circles in Vault staying visible until it moved a little way
    // into the bottom stripe, then clipping; 2026-09-11). The run counter
    // above bounds how long a character may be carried on this evidence alone.
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

// ---- Sprite placements (engine field/obj_placements.h) -------------------
// Placements from character builders Func_b168 and Func_b388 are recorded by
// 12-byte sprite record (attr0/1/2 at +4/+6/+8,
// read_golden_sun_obj_staging_attrs), and both providers ask these first
// (design shared with The Lost Age). Body: `cmp r4,#239; bgt` 0x0800B324 then `cmp r6,#159; bgt`
// 0x0800B328 not taken, record r7, x r4, y r6. Shadow: `cmp r6,#159; bgt`
// 0x0800B27E not taken, record r7 + 0xC, y r6, x = r5 - [sp+0x10] as the
// game stores next (0x0800B2A6..0x0800B2B6). ROM reading, FACTS.md
// 2026-10-15. Report 10 (characters above the top edge at 480x320): the
// fallback refuses raw Y 160..247, which a recorded placement answers.
const std::uint32_t kGoldenSunFieldStateSlot = gs::ram(0x03001EBCu);
constexpr std::uint32_t kGoldenSunMapNumber = 0x02000408u;
gbarecomp::ObjPlacements g_obj_placements;

void golden_sun_sync_obj_placements(gba::GbaBus& bus) {
    g_obj_placements.sync(runtime_current_frame(),
                          bus.read32(kGoldenSunFieldStateSlot),
                          bus.read32(gsr::battle_speed::kBattleHeapSlot),
                          bus.read16(kGoldenSunMapNumber));
}

// Func_b388 queues its projected sprite through Func_3dec after writing
// the OAM words. Its full position is still in the projection scratch and
// preserved registers: B54A..B56C subtract the half-box and add the scaled
// offsets. Capture that position in the same table as the field builder.
// F12 0051: the cave's true Y reaches 256; its byte alone becomes row 0.
// Func_94820's rain queues the same way (`bl Func_3dec` at 0x08094942).
const std::uint32_t kGoldenSunRainQueueReturn = gs::rom(0x08094946u);
}  // namespace
namespace gsr_runner {
void golden_sun_note_projected_obj_placement(std::uint32_t entry_pc) {
    if (entry_pc != GS_ROM(0x08003DECu) ||
        !golden_sun_expanded_obj_view_active()) return;
    const std::uint32_t caller = g_cpu.R[14] & ~1u;
    const bool projected_shadow = caller == GS_ROM(0x0800B64Eu) || caller == GS_ROM(0x0800B656u);
    if (caller != GS_ROM(0x0800B5C2u) && caller != GS_ROM(0x0800B5CCu) && !projected_shadow &&
        caller != kGoldenSunRainQueueReturn) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    golden_sun_sync_obj_placements(*bus);
    if (projected_shadow) {
        // Func_b388 projects the ground position separately from the body.
        // B5FC/B612/B614 write scratch.x - half_width and
        // scratch.y - (half_width >> 1) + 2; B64A/B652 queue record+0x0C.
        // F12 0057: without this route Flint's shadow at row 177 fell back
        // to the eight-bit reading -79, detached in the top margin.
        const std::uint32_t half_width = bus->read32(g_cpu.R[13] + 4u);
        const int x = static_cast<std::int32_t>(
            bus->read32(g_cpu.R[10]) - half_width);
        const int y = static_cast<std::int32_t>(
            bus->read32(g_cpu.R[10] + 4u) - (half_width >> 1) + 2u);
        g_obj_placements.note(*bus, runtime_current_frame(), g_cpu.R[0], x, y);
        return;
    }
    if (caller == kGoldenSunRainQueueReturn) {
        // Func_94820's rain drop (record r7 = r0). Its screen position is
        // gone from the registers by the queue call (0x080948DC masks x,
        // 0x080948EA reuses r0), so redo the game's own sums: world x and
        // z - height at +0x0C/+0x14/+0x10, camera x/y at [sp+8]/[sp+4],
        // each difference truncated toward zero (`bge; add 0xFFFF; asr
        // 16`), plus the life frame's offsets at r4 - 6 / r4 - 4 (r4 ends at
        // table entry + 6, 0x08094900). Checked against F12 0052: every
        // drop this predicts at row <= 159 is in the next frame's OAM there.
        const auto px = [](std::uint32_t v) {
            const std::int32_t s = static_cast<std::int32_t>(v);
            return (s < 0 ? s + 0xFFFF : s) >> 16;
        };
        const std::uint32_t r7 = g_cpu.R[7], sp = g_cpu.R[13];
        const std::uint32_t entry = g_cpu.R[4] - 6u;
        const int x = px(bus->read32(r7 + 0x0Cu) - bus->read32(sp + 8u)) +
            static_cast<std::int16_t>(bus->read16(entry));
        const int y = px(bus->read32(r7 + 0x14u) - bus->read32(r7 + 0x10u) -
                         bus->read32(sp + 4u)) +
            static_cast<std::int16_t>(bus->read16(entry + 2u));
        g_obj_placements.note(*bus, runtime_current_frame(), r7, x, y);
        return;
    }
    const int x = static_cast<std::int32_t>(
        bus->read32(g_cpu.R[10]) - g_cpu.R[11] + g_cpu.R[8]);
    const int y = static_cast<std::int32_t>(
        bus->read32(g_cpu.R[10] + 4u) -
        bus->read32(g_cpu.R[13] + 8u) + g_cpu.R[6]);
    g_obj_placements.note(*bus, runtime_current_frame(), g_cpu.R[0], x, y);
}
}  // namespace gsr_runner
namespace {

// At the builder's two Y tests, once the final decision is known; 0 (not
// taken) places the sprite.
void golden_sun_note_obj_placement(std::uint32_t pc) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !golden_sun_expanded_obj_view_active()) return;
    golden_sun_sync_obj_placements(*bus);
    const int y = static_cast<std::int32_t>(g_cpu.R[6]);
    if (pc == GS_ROM(0x0800B328u)) {
        g_obj_placements.note(*bus, runtime_current_frame(), g_cpu.R[7],
                              static_cast<std::int32_t>(g_cpu.R[4]), y);
    } else {
        const int x = static_cast<std::int32_t>(g_cpu.R[5]) -
                      static_cast<std::int32_t>(bus->read32(g_cpu.R[13] + 0x10u));
        g_obj_placements.note(*bus, runtime_current_frame(), g_cpu.R[7] + 0xCu,
                              x, y);
    }
}

// The recorded placement of this OAM entry, or null. With a game window open
// a menu's own sprites (priority 0) keep the older rules: The Lost Age's
// menus build them with the field's character builder, which sees them 256
// rows up (TLA F12 tla_rewind_0023).
const gbarecomp::ObjPlacements::Placement* golden_sun_obj_placement(
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return nullptr;
    if (((attr2 >> 10) & 3u) == 0u && game_window_open_this_frame())
        return nullptr;
    golden_sun_sync_obj_placements(*bus);
    return g_obj_placements.find(*bus, runtime_current_frame(), attr0, attr1,
                                 attr2);
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
    if (const auto* placed = golden_sun_obj_placement(attr0, attr1, attr2)) {
        *out_x = placed->x;
        return 1;
    }
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

// Slots whose Y the provider below took from the raw-attribute fallback (a
// guess) since the field capture last cleared it; read by the sprite edge
// continuity rule (src/obj_y_continuity.h).
std::array<bool, gsr::FieldScene::kObjects> g_obj_y_from_fallback{};
// Slots whose Y the world map's actor table decided (a town's side): not a
// guess, so the continuity rule leaves them alone.
std::array<bool, gsr::FieldScene::kObjects> g_obj_y_from_world_side{};

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
    if (const auto* placed = golden_sun_obj_placement(attr0, attr1, attr2)) {
        *out_y = gsr::widescreen::golden_sun_obj_y_shown_reading(
            placed->y, attr0, attr1, g_golden_sun_wide_extra_top,
            g_golden_sun_wide_extra_bottom,
            // Flint's negative world-event placement keeps its native
            // exception. A projected object below the view must not wrap
            // back to the top when its full Y passes 255 (F12 0051).
            placed->y < 0 && bus_read_u16(kGoldenSunMapNumber) == 2u);
        record_obj(GoldenSunObjYOutcome::AcceptedProvenance);
        return 1;
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
            // selected_provenance is found by identity, not by slot (see
            // golden_sun_obj_provider_provenance), so it may point at a
            // different OAM slot's visible record than oam_index; check array
            // membership instead of comparing against this slot's record.
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
        // Never hide what the handheld shows: Flint was recorded at -176
        // with OAM 80 (gpu_rewind_0012 of bug_report_20261005_094058).
        *out_y = gsr::widescreen::golden_sun_obj_y_shown_reading(
            provenance.logical_y, attr0, attr1,
            g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom,
            bus_read_u16(0x02000408u) == 2u);
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
        // consult our own position table, via the same
        // golden_sun_obj_track_lookup as the X provider so X and Y agree.
        {
            int track_x = 0, track_y = 0;
            if (golden_sun_obj_track_lookup(oam_index, attr0, attr1,
                                            &track_x, &track_y)) {
                *out_y = gsr::widescreen::golden_sun_obj_y_shown_reading(
                    track_y, attr0, attr1,
                    g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom,
                    bus_read_u16(0x02000408u) == 2u);
                return 1;
            }
        }
        // Step 3: try to reconstruct a position from the raw attributes
        // before giving up.
        {
            int fallback_x = 0, fallback_y = 0;
            bool by_world_side = false;
            if (golden_sun_wide_obj_fallback_position(
                    attr0, attr1, &fallback_x, &fallback_y, &by_world_side)) {
                // The world map's actor table is evidence, not a guess.
                if (static_cast<std::size_t>(oam_index) <
                    g_obj_y_from_fallback.size())
                    (by_world_side ? g_obj_y_from_world_side
                                   : g_obj_y_from_fallback)[
                        static_cast<std::size_t>(oam_index)] = true;
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
    // the expanded-view guard so inactive modes remain visible.
    std::uint64_t calls = 0;
    std::uint64_t bypasses = 0;
    std::uint64_t inactive_rejects = 0;
    std::int32_t min_coord = INT32_MAX;
    std::int32_t max_coord = INT32_MIN;
};

std::array<GoldenSunCullTrace, 10> g_golden_sun_cull_trace{{
    {gs::rom(0x0800B27Eu)}, {gs::rom(0x0800B324u)}, {gs::rom(0x0800B328u)},
    {gs::rom(0x0800B3D2u)}, {gs::rom(0x0800B3DCu)}, {gs::rom(0x0800B3E6u)}, {gs::rom(0x0800B3ECu)},
    {gs::rom(0x0800C6FAu)}, {gs::rom(0x0800C702u)}, {gs::rom(0x0800C708u)},
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
    if (pc == GS_ROM(0x0800B324u)) coord = static_cast<std::int32_t>(g_cpu.R[4]);
    else if (pc == GS_ROM(0x0800B27Eu) || pc == GS_ROM(0x0800B328u))
        coord = static_cast<std::int32_t>(g_cpu.R[6]);
    else if (pc == GS_ROM(0x0800B3D2u) || pc == GS_ROM(0x0800B3DCu) ||
             pc == GS_ROM(0x0800B3E6u) || pc == GS_ROM(0x0800B3ECu))
        coord = static_cast<std::int32_t>(g_cpu.R[3]);
    else if (pc == GS_ROM(0x0800C6FAu))
        coord = static_cast<std::int32_t>(g_cpu.R[3]);
    else if (pc == GS_ROM(0x0800C702u) || pc == GS_ROM(0x0800C708u))
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
    if (instruction_pc == GS_ROM(0x0800B324u)) {
        // Generated disassembly: B322 is `cmp r4,#239`; B324 consumes r4.
        compared_operand = static_cast<std::int32_t>(g_cpu.R[4]);
    } else if (instruction_pc == GS_ROM(0x0800B27Eu) ||
               instruction_pc == GS_ROM(0x0800B328u)) {
        // Generated disassembly: B27C/B326 are `cmp r6,#159`; both consume r6.
        compared_operand = static_cast<std::int32_t>(g_cpu.R[6]);
    } else if (instruction_pc == GS_ROM(0x0800B3D2u) ||
               instruction_pc == GS_ROM(0x0800B3DCu) ||
               instruction_pc == GS_ROM(0x0800B3E6u) ||
               instruction_pc == GS_ROM(0x0800B3ECu) ||
               instruction_pc == GS_ROM(0x0800C6FAu)) {
        // Func_b388 and C6FA consume signed r3 at their reviewed branches.
        // C6FA's policy converts the preserved bit pattern back to unsigned.
        compared_operand = static_cast<std::int32_t>(g_cpu.R[3]);
    } else if (instruction_pc == GS_ROM(0x0800C702u) ||
               instruction_pc == GS_ROM(0x0800C708u)) {
        // Func_c62c's lower/upper Y routes consume signed r2.
        compared_operand = static_cast<std::int32_t>(g_cpu.R[2]);
    }
    GoldenSunObjB328ParentClassification b328_classification =
        GoldenSunObjB328ParentClassification::NoParent;
    unsigned b328_mismatch_flags = 0u;
    if (instruction_pc == GS_ROM(0x0800B328u) &&
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
    if (!overridden && instruction_pc == GS_ROM(0x0800B328u)) {
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
                    call_return_pc, match, out_decision,
                    g_golden_sun_wide_extra_bottom);
        }
        // The view's own bottom margin, not Wide's 40 rows: at Extra Wide a
        // Lalivero chest 40 rows below the screen vanished at row 200
        // (gpu_rewind_0014 frame 15, back at 0015 frame 81; 2026-10-14).
        const std::int32_t margin_end = static_cast<std::int32_t>(
            gsr::widescreen::kNativeHeight + g_golden_sun_wide_extra_bottom);
        if (!overridden && golden_sun_expanded_obj_view_active() &&
            original_decision == 1u && compared_operand >= 160 &&
            compared_operand < margin_end &&
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
            compared_operand < margin_end &&
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
    if ((instruction_pc == GS_ROM(0x0800B27Eu) || instruction_pc == GS_ROM(0x0800B328u)) &&
        final_decision == 0u)
        golden_sun_note_obj_placement(instruction_pc);
    // World-map sprite decisions near the right edge, one line each (VRAM
    // map write trace on, map number 2, bounded). Towns vanish there at an
    // OAM x of 235..244 although every widened x route admits them, so this
    // names the site and value that actually drops them (FACTS.md
    // 2026-09-24). C6FA's operand is 16.16 fixed point.
    if (golden_sun_wide_diagnostics_enabled() &&
        bus_read_u16(0x02000408u) == 2u &&
        (instruction_pc == GS_ROM(0x0800B324u) || instruction_pc == GS_ROM(0x0800B3D2u) ||
         instruction_pc == GS_ROM(0x0800B3DCu) || instruction_pc == GS_ROM(0x0800C6FAu))) {
        static unsigned logged = 0;
        const std::int32_t px = instruction_pc == GS_ROM(0x0800C6FAu)
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
    if (instruction_pc == GS_ROM(0x0800B27Eu) || instruction_pc == GS_ROM(0x0800B328u)) {
        trace_golden_sun_obj_y_cull_decision(
            instruction_pc, original_decision, final_decision, overridden,
            compared_operand);
    }
    if (instruction_pc == GS_ROM(0x0800B328u)) {
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
        instruction_pc == GS_ROM(0x0800B27Eu)) {
        record_golden_sun_b27e_parent_route(
            original_decision, overridden, final_decision, compared_operand);
    }
    // B324/B328 are the final staging-record route. B27E is an earlier
    // decision point and does not itself write the record, so provenance must
    // not be attached there.
    const bool expanded_obj_view = golden_sun_expanded_obj_view_active();
    if (expanded_obj_view && instruction_pc == GS_ROM(0x0800B324u) &&
        ((!overridden && !original_decision) ||
         (overridden && *out_decision == 0u))) {
        record_golden_sun_obj_staging(
            instruction_pc, true, compared_operand);
    }
    if (expanded_obj_view && instruction_pc == GS_ROM(0x0800B328u) &&
        gsr::widescreen::golden_sun_obj_y_branch_writes_oam(
            instruction_pc, original_decision, overridden,
            overridden ? *out_decision : original_decision)) {
        record_golden_sun_obj_staging(
            instruction_pc, false, compared_operand);
    }
    // A rejected final Y branch means no staging record is written. Retire
    // the X candidate captured at B324 so a later OAM slot cannot inherit it.
    if (expanded_obj_view && instruction_pc == GS_ROM(0x0800B328u) &&
        !gsr::widescreen::golden_sun_obj_y_branch_writes_oam(
            instruction_pc, original_decision, overridden,
            overridden ? *out_decision : original_decision)) {
        clear_golden_sun_obj_staging(g_cpu.R[7]);
    }
    if (!overridden) {
        if (instruction_pc == GS_ROM(0x0800B27Eu) || instruction_pc == GS_ROM(0x0800B328u)) {
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
    const std::uint32_t field_offset = pc == GS_ROM(0x0809496Eu) ? 0x0Cu :
        pc == GS_ROM(0x08094970u) ? 0x14u :
        pc == GS_ROM(0x08094980u) ? 0x10u : 0u;
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
    if (pc == GS_ROM(0x08094980u) && g_cpu.R[7] != source) return;

    gsr::ObjBoulderTraceSample sample;
    sample.event = pc == GS_ROM(0x08094980u) ? "calc-result-write" : "field-write";
    sample.source_state = "writer-store";
    sample.outcome = "not-committed";
    sample.frame = runtime_current_frame();
    sample.epoch = g_golden_sun_field_auth_epoch;
    sample.writer_pc = pc;
    fill_golden_sun_obj_boulder_registers(&sample);
    // The source operands are fixed by the measured generated instructions
    // above, not inferred from the stored value.
    sample.store_value = pc == GS_ROM(0x0809496Eu) ? g_cpu.R[1] : g_cpu.R[0];
    fill_golden_sun_obj_boulder_source_fields(
        &sample, source, field_offset, sample.store_value);
    gsr::obj_recorder_note_boulder_trace(sample);
}

}  // namespace
namespace gsr_runner {
// This is the measured Region B producer's function entry. Its R7 base is
// the source table that the two field writers walk; recording the entry state
// gives the next capture a calculation boundary before either candidate field
// is written.
void record_golden_sun_obj_boulder_calc_entry(std::uint32_t entry_pc) {
    if (!gsr::obj_recorder_enabled() || entry_pc != GS_ROM(0x08094928u)) return;
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
}  // namespace gsr_runner
namespace {

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
    // The room-load EWRAM write recorder (GSR_MAP_RECORD) needs the same
    // every-store feed to recover the room-bounds struct offsets. One cheap
    // branch when the recorder is disabled.
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
    return physical < kGoldenSunPoolLdmWindowEnd &&
           physical + size > kGoldenSunPoolLdmWindowBase;
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
// changes nothing. The rule is the engine's field_obj_is_parked; the sentinel
// and the measurement behind it are in src/widescreen_policy.h.
extern "C" int golden_sun_obj_park_provider(int /*oam_index*/,
                                            std::uint16_t attr0,
                                            std::uint16_t attr1,
                                            std::uint16_t /*attr2*/) {
    return gsr::field_obj_is_parked(attr0, attr1) ? 1 : 0;
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
    // Geometry changes do not change the room or the identities of its
    // sprites. Keep their tracking; only the bounded Palace reach depends
    // on the extent of the newly visible margins.
    if (gba::g_ws_tilemap_provider == golden_sun_wide_tilemap_provider) {
        reset_golden_sun_palace_active_region();
        return;
    }
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
        gbarecomp::at_game_exit(report_golden_sun_cull_trace_at_exit);
    }
    static bool wide_report_armed = false;
    if (!wide_report_armed) {
        wide_report_armed = true;
        gbarecomp::at_game_exit(report_golden_sun_wide_diagnostics_at_exit);
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
    gba::g_ws_obj_tile_provider = gsr::custom_battle::tbs::sprite_tile;
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
    gba::g_ws_bg_x_provider_layers = 0xFu; // BG0-BG3 (mask of layers).
    g_runtime_thumb_alu_imm_override = golden_sun_thumb_alu_immediate;
    g_runtime_thumb_literal_override = golden_sun_thumb_literal;
    g_runtime_conditional_branch_override = golden_sun_conditional_branch;
}


// Draw the field with the graphics card (GSR_GPU_FIELD, a launcher checkbox).
// Off by default and never part of the faithful run. See
// docs/NATIVE_SCENE_RENDERER_PLAN.md: engine/src/field/field_scene.cpp
// describes each frame, field_scene_renderer.cpp draws it through the
// game-agnostic gpu_surface.cpp, and gpu_field_capture_hook /
// gpu_field_present_override connect both to the running game. Declining is
// always whole-frame (FieldSceneRenderer::draw's own contract); anything this
// renderer cannot draw comes from the emulated hardware path.
bool gpu_field_enabled() {
    static const bool enabled = [] {
        return gbarecomp::env_flag("GSR_GPU_FIELD");
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
        return gbarecomp::env_flag("GSR_GPU_FIELD_ONLY");
    }();
    return enabled;
}

// Game windows (menus, dialogue, settings) borrow BG0's character block for
// their font and frame pieces. The room's layers can share that block
// (BG3CNT 0x0503 and BG0CNT 0x0400 both use block 0 in gpu_rewind_0025), and
// on hardware a menu covers the screen, so the borrowed tiles never show
// under the room; in the expanded margins they showed as scraps of menu text.
// While no window is open this keeps a copy of the background graphics;
// while one is open the margins read the copy instead. FACTS.md 2026-09-24.
//
// The copy is the whole 64 KB background area, whatever the layers'
// character bases are: menus also write beyond BG0's first 16 KB block.
// Djinn and Status screens (logs/gpu_frame_0077/0078): the ground layer's
// margin tiles 517..1015 sit in block 1, where the menu's second text bank
// lives (window block +0xEA2 set), and the tree/cliff layers read block 2.
//
// It is refreshed only after the window system has stood empty for
// kRoomCopySettleFrames in a row; the menu frees and re-takes its text tiles
// between screens, so one empty frame would copy the menu's own graphics in
// as the "room".
//
// The colours too (2026-09-26, logs/gpu_frame_0079/0080): menus load their
// own palettes over some of the field's background banks -- bank 0 held
// pure red/green/blue/white on the Status screen -- so room tiles in the
// margins came out pink and blue with their shapes intact. The 256
// background colours are saved and restored with the graphics.
std::vector<std::uint8_t> g_room_char_copy;
std::vector<std::uint16_t> g_room_palette_copy;
std::vector<std::uint16_t> g_room_palette_previous;  // last frame's live colours
std::uint16_t g_room_palette_menu_banks = 0;  // banks 0-13 a menu loaded
constexpr std::size_t kBgPaletteEntries = 256;
int g_room_copy_closed_frames = 0;
constexpr int kRoomCopySettleFrames = 30;
constexpr std::uint32_t kBgAreaBytes = 0x10000u;

bool game_window_open() {
    using namespace gsr::settings_page;
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
    // Last frame's background colours, for the per-frame decision below.
    struct KeepPrevious {
        const std::array<std::uint16_t, 512>& live;
        ~KeepPrevious() {
            g_room_palette_previous.assign(live.begin(),
                                           live.begin() + kBgPaletteEntries);
        }
    } keep_previous{palette};
    if (!game_window_open()) {
        if (g_room_copy_closed_frames < kRoomCopySettleFrames) {
            ++g_room_copy_closed_frames;
        } else {
            g_room_char_copy.assign(vram.begin(), vram.begin() + kBgAreaBytes);
            g_room_palette_copy.assign(palette.begin(),
                                       palette.begin() + kBgPaletteEntries);
            g_room_palette_menu_banks = 0;
        }
        return;
    }
    g_room_copy_closed_frames = 0;
    if (g_room_char_copy.size() != kBgAreaBytes ||
        g_room_palette_copy.size() != kBgPaletteEntries)
        return;
    *out = vram;
    std::copy(g_room_char_copy.begin(), g_room_char_copy.end(), out->begin());
    // A screen-wide palette effect (the Vale storm's lightning flash, a fade,
    // a tint) changes most used banks of 0-13 in one frame (lightning: 211
    // entries in banks 0-13, not 14 or 15; gpu_rewind_0004 of
    // bug_report_20261005_094058; over all saved rewinds, frames changing 4+
    // banks changed 10 of 12 to all of them, a fade leaving a bank or two
    // unchanged by rounding). A menu changes only its own few banks in a
    // frame (Status 0, 1, 14; Djinn 0, 1, 4-7, 14; Item none). So the
    // decision is per frame and per bank: a frame that changes more than half
    // of the room's banks is an effect and the copy follows it; banks changed
    // in any other frame are the menu's and keep the copy's colours until the
    // copy is retaken. Comparing against the copy instead (the earlier rule)
    // failed when both happened: a lightning flash under the field menu, then
    // the Status screen -- every bank differed from the copy, so the menu's
    // banks 0 and 1 were taken as part of the flash (gpu_rewind_0087, FACTS
    // 2026-11-04). Banks 14 and 15 are the windows' own and always come from
    // the copy.
    if (g_room_palette_previous.size() == kBgPaletteEntries) {
        std::uint16_t changed = 0;
        int room_banks = 0, room_banks_changed = 0;
        for (int bank = 0; bank < 14; ++bank) {
            bool used = false, step = false;
            for (int i = bank * 16; i < bank * 16 + 16; ++i) {
                if (g_room_palette_copy[i] != 0u) used = true;
                if (palette[i] != g_room_palette_previous[i]) step = true;
            }
            if (!used || (g_room_palette_menu_banks & (1u << bank))) continue;
            ++room_banks;
            if (step) {
                ++room_banks_changed;
                changed = static_cast<std::uint16_t>(changed | (1u << bank));
            }
        }
        if (room_banks_changed * 2 > room_banks) {
            for (int bank = 0; bank < 14; ++bank) {
                if (!(changed & (1u << bank))) continue;
                std::copy(palette.begin() + bank * 16,
                          palette.begin() + bank * 16 + 16,
                          g_room_palette_copy.begin() + bank * 16);
            }
        } else {
            g_room_palette_menu_banks =
                static_cast<std::uint16_t>(g_room_palette_menu_banks | changed);
        }
    }
    out_palette->assign(palette.begin(), palette.end());
    std::copy(g_room_palette_copy.begin(), g_room_palette_copy.end(),
              out_palette->begin());
}

// Filled by gpu_field_capture_hook() at VBlank start -- the same boundary
// the canonical framebuffer latches at (GbaPpu::latch_native_scene_state,
// called from runtime_bus_bridge.cpp) -- and read once, later the same frame,
// by gpu_field_present_override(). Plain memory copies only; no GL call is
// made there, because the hook fires from deep inside guest instruction
// dispatch, not necessarily where the host's GL context is current. The draw
// happens at present time instead, beside the win.present() call it stands
// in for (see runtime.cpp's present_first).
gsr::GpuFieldCapture g_gpu_field_capture;
struct GpuPresentation {
    gsr::GpuFieldCapture capture;
    gbarecomp::HistoryFrame frame;
    bool world_source_ok = false;
};
gbarecomp::TextureReadbackQueue<GpuPresentation> g_gpu_readback;
gsr::GpuFieldCapture g_world_settled_capture;

bool draw_gpu_field_capture(gsr::FieldSceneRenderer& renderer,
                            const gsr::GpuFieldCapture& capture,
                            std::uint32_t width, std::uint32_t height);

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
gsr::HeldPaletteEffect g_world_palette_effect;

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
                           std::uint32_t height,
                           gsr::FieldSceneRenderer& renderer,
                           gbarecomp::GpuSurface& surface,
                           const gsr::GpuFieldCapture& capture, bool world_source_ok) {
    const std::vector<std::uint8_t>& io = capture.io;
    const unsigned mode = io.empty() ? 0xFFu : (io[0] & 7u);
    const bool world = world_map_blocks(io);
    const bool native_size = width == 240 && height == 160;
    if (!world) {
        g_world_margin_rgb.clear();  // anywhere else: the saved view is stale
        g_world_settled_capture = {};
        return;
    }
    if (mode == 2u) {
        // The battle flash: rows switched to mode 0 for a band of the
        // game's solid fill layer (BG1, tile 0x7F, palette 15), every other
        // frame, growing from the middle (gpu_rewind_0087, frames
        // 1684-1696). The graphics card draws a frame in one mode, so those
        // rows came out as world map; paint them, across the margins too,
        // in the one colour the console draws there.
        const gsr::FieldScene& sc = capture.scene;
        const std::uint32_t band_y0 = (height - 160u) / 2u;
        bool mixed = false;
        for (int y = 0; y < gsr::FieldScene::kRows; ++y) {
            if (!sc.row_io_valid[y] || (sc.row_io[y][0] & 7u) == 2u) continue;
            mixed = true;
            std::uint8_t colour[3];
            if (!text_row_single_colour(sc.row_io[y].data(), y,
                                        capture.vram,
                                        capture.pal, colour))
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
        if (!fading && !capture.window_open &&
            world_source_ok &&
            capture.pal.size() >= 512) {
            g_world_margin_rgb.assign(rgb, rgb + std::size_t{width} * height * 3u);
            g_world_margin_w = width;
            g_world_margin_h = height;
            g_world_settled_capture = capture;
            g_world_palette_effect.save(capture.pal);
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
    // The held margins were saved at another output size: redraw the settled
    // capture at this size and hold that instead.
    if ((g_world_margin_w != width || g_world_margin_h != height) &&
        g_world_settled_capture.valid &&
        draw_gpu_field_capture(renderer, g_world_settled_capture, width, height)) {
        std::vector<std::uint8_t> resized(std::size_t{width} * height * 3u);
        if (!surface.read_texture_rgb(renderer.output_texture(),
                                      static_cast<int>(width), static_cast<int>(height),
                                      resized.data())) return;
        g_world_margin_rgb.swap(resized);
        g_world_margin_w = width;
        g_world_margin_h = height;
        g_world_palette_effect.reset_indices();
    }
    // No settled world-map frame to hold, as when a Djinn event switches to
    // mode 0 straight away (logs/gpu_frame_0083.bin, Flint, 2026-09-26): the
    // margins would show the world map's rotation-layer data read as text
    // layers, which is scrambled tiles. Black instead.
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
    // LIVE margins (shared_game/world_margin_live.h): the flat picture only
    // covers the console's screen, so the margins draw the settled world map
    // through the main renderer every frame with this frame's sprites.
    static std::vector<std::uint8_t> live_margins;
    static gsr::GpuFieldCapture mix;
    const std::vector<std::uint8_t>* margin_src = &g_world_margin_rgb;
    if (gsr::live_world_margin_capture(g_world_settled_capture, capture, mix)) {
        live_margins.resize(std::size_t{width} * height * 3u);
        if (draw_gpu_field_capture(renderer, mix, width, height) &&
            surface.read_texture_rgb(renderer.output_texture(),
                                     static_cast<int>(width),
                                     static_cast<int>(height),
                                     live_margins.data()))
            margin_src = &live_margins;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        const bool native_row = y >= y0 && y < y0 + 160u;
        std::uint8_t* dst = rgb + std::size_t{y} * width * 3u;
        const std::uint8_t* src =
            margin_src->data() + std::size_t{y} * width * 3u;
        if (!native_row) {
            std::memcpy(dst, src, std::size_t{width} * 3u);
            continue;
        }
        std::memcpy(dst, src, std::size_t{x0} * 3u);
        const std::uint32_t right = x0 + 240u;
        std::memcpy(dst + std::size_t{right} * 3u, src + std::size_t{right} * 3u,
                    std::size_t{width - right} * 3u);
    }
    g_world_palette_effect.apply(rgb, width, height, *margin_src, capture.pal);
}


// Sprite edge continuity: see src/obj_y_continuity.h. Always on.
// Field frames in the expanded view only. Sprite recorder logs the first
// 400 changes; normal play must not fill the launcher's pipe with these
// per-sprite diagnostics (Djinn menu report 190853).
void apply_obj_y_continuity_if_enabled(gsr::FieldScene* scene,
                                       const std::uint8_t* oam) {
    static gsr::ObjYContinuity state;
    static unsigned reported = 0;
    // The world map too (2026-11-08, gpu_rewind_0096): its actor table
    // places towns, but a Psynergy's sparkles rising off the top (raw Y
    // 216..233, really -40..-23) have no actor and vanished at the old top
    // edge. Towns the table placed are not guesses and stay as they are.
    const bool field = scene->video_mode == 0 &&
                       !g_golden_sun_battle_backdrop_state.active &&
                       golden_sun_expanded_obj_view_active();
    bool guessed[gsr::FieldScene::kObjects];
    for (int i = 0; i < gsr::FieldScene::kObjects; ++i)
        guessed[i] = (!scene->objects[i].trusted ||
                      g_obj_y_from_fallback[static_cast<std::size_t>(i)]) &&
                     !g_obj_y_from_world_side[static_cast<std::size_t>(i)];
    gsr::apply_obj_y_continuity(
        scene, oam, guessed, field, &state,
        [&](const gsr::ObjYContinuityChange& c) {
            if (!gsr::obj_recorder_enabled() || reported >= 400) return;
            ++reported;
            const char* source =
                !scene->objects[c.slot].trusted ? "untrusted" : "fallback";
            if (c.confined)
                std::fprintf(stderr,
                    "[obj-y-continuity] frame %llu slot %d raw %d (%s): no "
                    "previous position, held in the console's window\n",
                    static_cast<unsigned long long>(scene->frame), c.slot,
                    c.raw_y, source);
            else
                std::fprintf(stderr,
                    "[obj-y-continuity] frame %llu slot %d raw %d (%s): y %d "
                    "-> %d, previous %d\n",
                    static_cast<unsigned long long>(scene->frame), c.slot,
                    c.raw_y, source, c.from_y, c.to_y, c.previous_y);
        }, g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom);
}

void gpu_field_capture_hook() {
    load_menu_font();
    if (const auto* bus = gbarecomp::active_bus())
        gsr::trackers::sample(bus->ewram_ptr(), bus->rom_ptr(), bus->rom_size());
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
    g_obj_y_from_fallback.fill(false);
    g_obj_y_from_world_side.fill(false);
    gsr::field_scene_capture(
        &scene, ppu->frame_count(), frame_io.data(), bus->oam_ptr(),
        bus->pal_ptr(), ppu->latched_native_line_io(),
        ppu->latched_native_line_io_valid(),
        ppu->latched_native_affine_line_refs(),
        ppu->latched_native_affine_line_ref_valid(),
        static_cast<int>(ppu->view_extra_top()), static_cast<int>(ppu->view_extra_bottom()));
    if (!scene.valid || !scene.supported) {
        // Invalidate, so gpu_field_present_override() declines this frame
        // instead of drawing the previous capture.
        g_gpu_field_capture.valid = false;
        return;
    }
    apply_obj_y_continuity_if_enabled(&scene, bus->oam_ptr());
    g_gpu_field_capture.scene = scene;
    g_gpu_field_capture.vram.assign(bus->vram_ptr(), bus->vram_ptr() +
        gba::GbaBus::kNativeVram + (bus->obj_extra() ? gba::GbaBus::kObjExtraVram : 0));
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
    std::memcpy(&effect_canvas, g_gpu_field_capture.iwram.data() + gs::ram(0x03001EF0u) - 0x03000000u, 4);
    if (g_gpu_field_capture.effects.canvas != effect_canvas)
        g_gpu_field_capture.effects = {};
    g_gpu_field_capture.io = frame_io;
    g_gpu_field_capture.oam.assign(bus->oam_ptr(), bus->oam_ptr() + 1024);
    g_gpu_field_capture.pal.assign(bus->pal_ptr(), bus->pal_ptr() + 1024);
    g_gpu_field_capture.window_open = game_window_open();
    g_gpu_field_capture.valid = true;
}

// ---- F12 history (engine frame_history.h, always on) --------------------
//
// A one-frame flicker is gone before F12 lands, so every presented frame is
// kept: the exact picture shown (after the settings page and battle ">>" are
// painted) and, when the graphics card had a capture, that frame's GSRGPUF1
// bytes for tools/scene_renderer_check. F12 writes the last 2 seconds to
// logs/gpu_rewind_NNNN/.
gbarecomp::FrameHistory g_history("gpu_rewind_");

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
    if ((count & (count - 1)) == 0) {
        std::fprintf(stderr,
            "[gsr] GPU field refused %llu frame(s) in a row: %s\n",
            static_cast<unsigned long long>(count),
            reason ? reason : "no reason given");
    }
    return true;
}

// Better Field Psy (settings page two), defined with the other options below.

bool draw_gpu_field_capture(gsr::FieldSceneRenderer& renderer,
                            const gsr::GpuFieldCapture& capture,
                            std::uint32_t width, std::uint32_t height) {
    if (!renderer.set_output_size(static_cast<int>(width),
                                  static_cast<int>(height))) return false;
    renderer.upload_vram(capture.vram.data(),
                         capture.vram.size());
    if (!capture.room_vram.empty())
        renderer.upload_room_vram(capture.room_vram.data(),
                                  capture.room_vram.size());
    renderer.upload_palette(capture.scene.palette.data(),
                            capture.scene.palette.size());
    if (!capture.room_palette.empty())
        renderer.upload_room_palette(capture.room_palette.data(),
                                     capture.room_palette.size());
    renderer.upload_room(capture.ewram.data(),
                         capture.ewram.size());
    // The world map beyond the game's own 512-pixel ring, for the margins
    // (FACTS.md, 2026-09-25). Checked against the pieces the game loaded
    // every frame; off whenever that check fails or this is not the world map.
    {
        static gsr::WorldMapSource world_map;
        const auto& cap = capture;
        if (world_map.update(cap.ewram.data(), cap.ewram.size(),
                             cap.iwram.data(), cap.iwram.size(),
                             cap.rom, cap.rom_bytes)) {
            g_world_map_source_ok = true;
            static std::uint32_t checked_generation = 0;
            renderer.upload_world_map(world_map.tiles().data(),
                                      world_map.generation());
            // A rebuild the screen does not agree with, when an earlier map
            // exists: keep the earlier one (FACTS.md 2026-10-07). Checked on
            // the first frame that can be compared (a mode 2 world frame).
            if (world_map.generation() != checked_generation) {
                const int agree =
                    renderer.world_map_agreement(cap.scene, nullptr);
                if (agree >= 0 &&
                    agree < gsr::FieldSceneRenderer::kRoomCheckMinAgreement &&
                    world_map.restore_previous()) {
                    renderer.upload_world_map(world_map.tiles().data(),
                                              world_map.generation());
                }
                if (agree >= 0) checked_generation = world_map.generation();
            }
        } else {
            g_world_map_source_ok = false;
            renderer.upload_world_map(nullptr, 0);
        }
    }
    // The effect layer in a battle: a non-wrapping affine BG2, or another
    // layer showing the canvas as a tile block. Its canvas base and size come
    // from the scene's own registers, never a remembered address, because the
    // game moves both between spells (FACTS.md, 2026-09-18).
    {
        const gsr::FieldScene& sc = capture.scene;

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
                    capture.vram.data(),
                    capture.vram.size(), sc);
        }
        // Clear last frame's overlay before submitting this frame's sparks.
        renderer.set_effect_layer(effect_layer);

        // Add captured stamps only beyond the game's canvas; its original
        // image stays intact. CPU stamp counts do not prove GPU visibility.
        if (gsr::effect_capture_enabled()) {
            const auto& captured = capture.effects;
            const auto& asked = captured.stamps;
            if (effect_layer >= 0) {
                // With no sparks this still reads the canvas's tint fill.
                if (!asked.empty())
                    renderer.upload_effect_sparks(
                        asked.data(), static_cast<int>(asked.size()),
                        captured.artwork.data(), captured.artwork.size(),
                        effect_layer);
                renderer.analyse_effect_canvas(capture.vram.data(),
                                               capture.vram.size(),
                                               sc, effect_layer);
            }
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
    renderer.set_menu_open(capture.window_open);
    // The title flare's halos alternate frame by frame (sprite_flicker.h).
    renderer.set_flicker_smoothing(gbarecomp::temporal_blend_weight_for_index(
        gbarecomp::flicker_reduction_index()));
    renderer.set_reveal_full(g_field_psynergy_fast.load() != 0);
    // Characters parked off the room are hidden; effects stay.
    renderer.set_character_objects(golden_sun_character_objects(
        capture.scene, capture.ewram.data(), capture.ewram.size(),
        capture.iwram.data(), capture.iwram.size(), gs::ram(0x03001E70u),
        gs::ram(0x03001EBCu)));
    // The storm's rain continues into every margin (rain_margins.h); any other
    // scene draws with no margin data, as before.
    gsr::SceneMargins margins;
    const bool rain = golden_sun_rain_margins(capture.scene, margins,
                                              capture.vram);
    renderer.set_scene_margins(rain ? &margins : nullptr);
    const bool drawn = renderer.draw(capture.scene);
    renderer.set_scene_margins(nullptr);
    return drawn;
}

// Submit the current scene, then receive the preceding queued picture with
// its owned scene and presentation metadata. Live guest state is untouched.
bool gpu_field_present_override(std::uint8_t* rgb, std::uint32_t width,
                                std::uint32_t height,
                                gbarecomp::HistoryFrame& frame, bool synchronous) {
    if (!gpu_field_enabled()) { g_gpu_readback.invalidate(); return false; }
    if (!g_gpu_field_capture.valid) {
        g_gpu_readback.invalidate();
        if (gpu_field_only_enabled())
            return gpu_field_paint_refused(rgb, width, height,
                                           "nothing was captured this frame");
        return false;
    }

    // Lazily created the first time this is actually needed, from present
    // time, where the window's GL context is the one already current --
    // never from gpu_field_capture_hook() above. gpu_surface.h attaches to
    // that existing context rather than creating one of its own.
    struct Context {
        gbarecomp::GpuSurface surface;
        gsr::FieldSceneRenderer renderer;
    };
    static std::unique_ptr<Context> context;
    static bool init_attempted = false;
    static bool init_ok = false;
    if (!context) {
        context = std::make_unique<Context>();
        gbarecomp::set_frame_present_shutdown_hook([] {
            g_gpu_readback.invalidate();
            context.reset();  // renderer, then PBOs/fences, before the GL window
            init_attempted = init_ok = false;
        });
    }
    auto& surface = context->surface;
    auto& renderer = context->renderer;
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
            const bool host_effects = gbarecomp::env_flag("GSR_HOST_EFFECTS");
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
        g_gpu_readback.invalidate();
        if (gpu_field_only_enabled())
            return gpu_field_paint_refused(rgb, width, height,
                                           "the renderer could not start");
        return false;
    }

    if (!renderer.set_output_size(static_cast<int>(width),
                                  static_cast<int>(height))) {
        g_gpu_readback.invalidate();
        if (gpu_field_only_enabled())
            return gpu_field_paint_refused(rgb, width, height,
                                           "the output size was refused");
        return false;
    }
    if (!draw_gpu_field_capture(renderer, g_gpu_field_capture, width, height)) {
        g_gpu_readback.invalidate();
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

    const bool battle = frame.battle_2x;
    const bool source_ok = g_world_map_source_ok && renderer.world_map_used();
    auto delivered = g_gpu_readback.present(surface, renderer.output_texture(),
        static_cast<int>(width), static_cast<int>(height), rgb,
        runtime_current_frame(),
        GpuPresentation{g_gpu_field_capture, std::move(frame), source_ok}, synchronous);
    if (!delivered) {
        g_gpu_readback.invalidate();
        frame = gbarecomp::FrameHistory::capture();
        frame.battle_2x = battle;
        return gpu_field_only_enabled()
            ? gpu_field_paint_refused(rgb, width, height, "GPU readback failed") : false;
    }
    world_map_margin_hold(rgb, width, height, renderer, surface,
                          delivered->capture, delivered->world_source_ok);
    frame = std::move(delivered->frame);
    frame.gpu_drawn = true;
    frame.scene = gsr::gpu_field_dump_bytes(delivered->capture);
    frame.scene_frame = delivered->capture.scene.frame;

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

}  // namespace
namespace gsr_runner {
// ---- Enemy HP bar (shared_game/enemy_hp_bar.h) ------------------------------
// The drawn name's width in the menu font, from its u16 text.
int enemy_hp_bar_name_width(std::uint32_t name) {
    load_menu_font();
    int text_width = 0;
    for (std::uint32_t at = name;; at += 2u) {
        const std::uint16_t code = bus_read_u16(at);
        if (code == 0) break;
        if (code >= 0x20u && code <= 0xFFu)
            text_width += g_settings_font.widths[code - 0x20u];
    }
    return text_width;
}
}  // namespace gsr_runner
namespace {

// The bar just above the enemy's name, in the game's window colours, with
// the HP in the menu font (letters and shadow only).
void paint_enemy_hp_bar(std::uint8_t* rgb, std::uint32_t width,
                        std::uint32_t height, const gsr::enemy_hp_bar::Bar& bar) {
    const std::uint16_t* palette = g_cheat_window_palette_known
        ? g_cheat_window_palette : gsr::cheat_menu::kDefaultWindowPalette;
    const int ox = static_cast<int>(g_golden_sun_wide_extra_left);
    const int oy = static_cast<int>(g_golden_sun_wide_extra_top);
    const auto put = [&](int x, int y, int r, int g, int b) {
        x += ox;
        y += oy;
        if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
            y >= static_cast<int>(height)) return;
        std::uint8_t* p = rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
        p[0] = static_cast<std::uint8_t>(r);
        p[1] = static_cast<std::uint8_t>(g);
        p[2] = static_cast<std::uint8_t>(b);
    };
    const auto draw_number = [&](const std::string& number, int bar_left, int top) {
        if (!g_settings_font_loaded.load(std::memory_order_acquire)) return;
        int text_width = 0;
        for (const char ch : number)
            text_width += g_settings_font.widths[static_cast<unsigned char>(ch) - 0x20u];
        const auto colour = [&](std::uint8_t index, int* r, int* g, int* b) {
            const std::uint16_t c = palette[index];
            *r = ((c & 31) << 3) | ((c & 31) >> 2);
            *g = (((c >> 5) & 31) << 3) | (((c >> 5) & 31) >> 2);
            *b = (((c >> 10) & 31) << 3) | (((c >> 10) & 31) >> 2);
        };
        int pen = bar_left + (gsr::enemy_hp_bar::kBarWidth - text_width) / 2;
        for (const char ch : number) {
            const auto code = static_cast<unsigned char>(ch);
            for (int gy = 0; gy < 8; ++gy) {
                for (int gx = 0; gx < 8; ++gx) {
                    const std::uint8_t b = g_settings_font.tiles[code * 32u + gy * 4 + gx / 2];
                    const std::uint8_t v = (b >> (4 * (gx & 1))) & 0xFu;
                    if (v != gsr::settings_page::kGlyphLetter &&
                        v != gsr::settings_page::kGlyphShadow) continue;
                    int r, g, bl;
                    colour(v, &r, &g, &bl);
                    put(pen + gx, top + gy, r, g, bl);
                }
            }
            pen += g_settings_font.widths[code - 0x20u];
        }
    };
    // Always red, whatever window colour the player picked.
    gsr::enemy_hp_bar::paint(bar, palette, gsr::cheat_menu::kDefaultWindowPalette[13],
                             put, draw_number);
}

// The battle ">>" go over whatever picture was
// going to be shown, the GPU renderer's or the emulated hardware's.
bool golden_sun_present_override(std::uint8_t* rgb, std::uint32_t width,
                                 std::uint32_t height) {
    auto frame = gbarecomp::FrameHistory::capture();
    frame.battle_2x = battle_speed_indicator_visible();
    const gba::GbaPpu* ppu = gbarecomp::active_ppu();
    const bool held = ppu && g_history.count() &&
        runtime_current_frame() >= g_history.back(0).guest_frame &&
        g_history.back(0).width == width && g_history.back(0).height == height &&
        bus_read_u32(gsr::battle_speed::kBattleHeapSlot) != 0u &&
        (gbarecomp::spell_setup_torn_frame(*ppu) ||
         battle_spell_position_pending(*ppu));
    // A torn spell picture and the Quality of Life menu form explicit
    // synchronous boundaries.
    bool drawn = gpu_field_present_override(rgb, width, height, frame, held || gsr::qol::menu.showing);
    if (frame.scene.empty() && g_gpu_field_capture.valid) {
        frame.scene = gsr::gpu_field_dump_bytes(g_gpu_field_capture);
        frame.scene_frame = g_gpu_field_capture.scene.frame;
    }
    if (held) {
        const auto& last = g_history.back(0);
        std::copy(last.rgb.begin(), last.rgb.end(), rgb);
        frame = last;
        frame.rgb.clear();
        frame.held_frame = true;
        drawn = true;
    }
    const bool battle = frame.battle_2x;
    if (battle) paint_battle_speed_indicator(rgb, width, height);
    g_history.record(rgb, width, height, std::move(frame));
    std::string unpacker_note;
    const auto report_notice = g_history.update_report(
        gsr::unpacker_guard_capture_due(&unpacker_note) ? &unpacker_note : nullptr,
        "auto_capture.txt");
    // After the recording, so the HP bar, the Custom Battle panel and the
    // notice are not in the saved frames.
    gsr::enemy_hp_bar::Bar enemy_bar;
    const bool hp_bar = gsr::enemy_hp_bar::bar(&enemy_bar);
    if (hp_bar) paint_enemy_hp_bar(rgb, width, height, enemy_bar);
    const auto panel = gsr::custom_battle::panel_view();
    if (panel.open) paint_custom_battle_panel(rgb, width, height, panel);
    // Nine save files: the file box's scrollbar and red Challenge files.
    const bool files = gsr::save_files::tbs::g_picker.open;
    if (files) {
        auto put = window_pixel_painter(rgb, width, height);
        auto pixel = native_pixel_at(rgb, width, height);
        gsr::save_files::tbs::paint(put, pixel);
    }
    const bool quality = gsr::qol::menu.showing;
    if (quality) {
        namespace qm = gsr::qol_menu;
        menu_window_paint(rgb, width, height, qm::kTileX, qm::kTileY, qm::kTiles, qm::kRows,
            [](auto& draw, auto& text_width, int x, int y, auto&) {
                gsr::qol_menu::paint(gsr::qol::menu, draw, text_width, x, y);
            });
        drawn = true;
    }
    const bool travel = gsr::retreat_travel::showing;
    if (travel) {
        namespace rt = gsr::retreat_travel;
        menu_window_paint(rgb, width, height, rt::kTileX, rt::kTileY, rt::kTiles, rt::kRows,
            [](auto& draw, auto& text_width, int x, int y, auto&) {
                gsr::retreat_travel::paint(draw, text_width, x, y);
            });
    }
    const bool radial = gsr::radial_psynergy::paint(rgb, width, height,
        [&](int tile_x, int tile_y, int tiles, int rows, auto content) {
            menu_window_paint(rgb, width, height, tile_x, tile_y, tiles, rows,
                [&](auto& draw, auto& text_width, int x, int y, auto&) {
                    content(draw, text_width, x, y);
                });
        });
    using ReportNotice = gbarecomp::FrameHistory::ReportNotice;
    const bool notice = report_notice != ReportNotice::None;
    if (notice) {
        gsr::paint_bug_report_notice(rgb, width, height, report_notice == ReportNotice::Automatic
            ? "CRASH CAUGHT REPORT SAVED" : "BUG REPORT SAVED");
    }
    // The new-game questions, over the running game.
    const bool question = g_hard_prompt_open;
    if (question) cheat_menu_paint(rgb, width, height);
    return drawn || battle || hp_bar || panel.open || files || travel || radial || notice || question;
}

// Instant text is the settings screen's fourth Message-speed choice.
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

}  // namespace
namespace gsr_runner {
// The text ID of the line the menu text decoder is drawing, taken where it
// starts the line (text_speed_cheat.h kLineStartPc). Emulation thread.
std::uint32_t g_decoder_text_id = 0;
}  // namespace gsr_runner
namespace {

// Hard Mode's label and help line, written the same way while the settings
// screen is open.
int g_hard_text_pos = -1;
std::uint32_t g_hard_text_id = 0;

bool hard_mode_text_character(std::uint32_t original,
                              std::uint32_t* out_value) {
    using namespace gsr::hard_mode;
    const std::uint32_t text_id = g_decoder_text_id;
    const char* english = text_id == kLabelTextId ? kLabelText
                        : text_id == kHelpTextId  ? kHelpText
                                                  : nullptr;
    if (!g_settings_screen_text || !english) {
        g_hard_text_pos = -1;
        return false;
    }
    if (g_hard_text_pos < 0 || text_id != g_hard_text_id) {
        g_hard_text_pos = 0;
        g_hard_text_id = text_id;
    }
    const std::string text = settings_text(english);
    std::uint32_t wanted = 0u;
    if (static_cast<std::size_t>(g_hard_text_pos) < text.size())
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
// In the expanded view the x bounds and both z bounds grow; the sprite
// routines after it still trim to the screen (Func_c62c's top test stops at
// y -(32 + top margin)). Operands are the stock values: the ldrs at
// 0x020085D2 (-160 << 16) and 0x020085E4 (-300 << 16) and the movs at
// 0x020085D6 (160) and 0x020085E6 (200), each shifted left 16 by the next
// instruction. The upper z bound: towns' top edge sits at y -73 at z -300
// and at -122 by z -460 (fit y = -476 + 455954 / (830 - z), 0.9 px rms over
// the towns in all saved rewinds, 2026-10-14); Extra Wide's 80-row margin needs
// -112 for a 32-px town, Wide's 40 needs -72, which the stock -300 just met.
// Returns -1 for any other site.
int golden_sun_worldmap_range_box(std::uint32_t pc, std::uint32_t original,
                                  std::uint32_t* out_value) {
    static const std::uint32_t kXLow = gs::ovl("rom_77a7c8", 0x020085D2u);
    static const std::uint32_t kZLow = gs::ovl("rom_77a7c8", 0x020085E4u);
    static const std::uint32_t kXHigh = gs::ovl("rom_77a7c8", 0x020085D6u);
    static const std::uint32_t kZHigh = gs::ovl("rom_77a7c8", 0x020085E6u);
    std::uint32_t grown = 0;
    if (pc == kXLow && original == 0xFF600000u) {
        grown = static_cast<std::uint32_t>(
            -(160 + 2 * static_cast<std::int32_t>(
                            g_golden_sun_wide_extra_left)) * 65536);
    } else if (pc == kZLow && original == 0xFED40000u) {
        grown = static_cast<std::uint32_t>(
            -(300 + 2 * static_cast<std::int32_t>(
                            g_golden_sun_wide_extra_top)) * 65536);
    } else if (pc == kXHigh && original == 160u) {
        grown = 160u + 2u * g_golden_sun_wide_extra_right;
    } else if (pc == kZHigh && original == 200u) {
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
// it. The camera must not scroll past the map's edge.
const std::uint32_t kCameraStructPointer = gs::ram(0x03001E70u);
const std::uint32_t kCameraMinXBranchPc = gs::rom(0x08010262u);  // bge, r7 vs r3
const std::uint32_t kCameraMinXAddPc = gs::rom(0x08010264u);     // adds r7,r3,#0
const std::uint32_t kCameraMaxXLiteralPc = gs::rom(0x0801026Cu); // -240 << 16
const std::uint32_t kCameraMinYBranchPc = gs::rom(0x0801027Eu);  // bge, r0 vs r3
const std::uint32_t kCameraMinYAddPc = gs::rom(0x08010280u);     // adds r0,r3,#0
const std::uint32_t kCameraMaxYLiteralPc = gs::rom(0x08010288u); // -160 << 16
// Func_10000 (0x08010000), the per-frame follow, clamps the same way with
// its own literals (0x0801021C/0x08010220): lo = [S+0xEC] + [S+4] in r0,
// hi = [S+0xF4] - [S+4] - 240 in r1, x in r7, then `cmp r7,r1; ble`;
// y: lo in lr, hi = [S+0xF8] - [S+8] - 160 in r3, y in r6. Its minimum is a
// register add, so the hook takes over the final step instead: the ble is
// forced through to `adds r7,r1,#imm` / `adds r6,r3,#imm` with imm chosen
// to land on clamp(value, lo + inset, hi). rewinds 0098/0099: camera at the
// stock limit, the Func_10230 hooks alone never ran while walking.
const std::uint32_t kFollowMaxXLiteralPc = gs::rom(0x0801004Au);  // -240 << 16
const std::uint32_t kFollowMaxYLiteralPc = gs::rom(0x08010062u);  // -160 << 16
const std::uint32_t kFollowClampXBranchPc = gs::rom(0x0801007Cu); // ble, r7 vs r1
const std::uint32_t kFollowClampXAddPc = gs::rom(0x0801007Eu);    // adds r7,r1,#0
const std::uint32_t kFollowClampYBranchPc = gs::rom(0x08010088u); // ble, r6 vs r3
const std::uint32_t kFollowClampYAddPc = gs::rom(0x0801008Au);    // adds r6,r3,#0

struct CameraInset {
    std::int32_t left = 0, right = 0, top = 0, bottom = 0;  // pixels
};

// The title screen runs on the field camera too (map number 1, bounds
// 0..512 in both axes): an inset there moved the logo 60 px left and 40 up
// and pulled hidden rows of its map into view (gpu_rewind_0100; every map 1
// capture, maprec "Title" included, is the title or its file menu).
// The live number is not enough: on the Continue screen it follows the
// highlighted save (1 -> 111, "Tolbi-bound Ship", gpu_rewind_0143 frame 96)
// while the title is still showing, as in The Lost Age (TLARecomp be0078e).
// So the number is taken when a room's camera is set up (Func_10230) and
// kept until the next room; a live 0 or 1 (boot, title) resets it.
constexpr std::uint32_t kCameraMapNumberAddr = 0x02000408u;
static std::uint16_t g_camera_room_map = 0;

}  // namespace
namespace gsr_runner {
// Func_10000 keeps each of the three field layers' scroll at [S+0x104 +
// 0x30 * n] (x, y; 16.16) and redraws only the strip it scrolls onto:
// column writer 0x0800FF54 when x's 8-pixel tile changes, row writer
// 0x0800FEC8 (16 cells, the ring's width) when y's 16-pixel row does
// (0x08010182 / 0x080101B2; tiles rounded with bias 0x7FFFF, literal
// 0x08010228). The field state is [0x03001E70] in EWRAM (0x02030CCC); the
// world map's is 0x03002000. Repaint after a jump: tile_ring_redraw.h.
gsr::TileRingRedraw g_tile_ring_redraw({kCameraStructPointer, 0x104u, 0x30u, gs::rom(0x0800FEC8u),
                                        kCameraMapNumberAddr, true});
}  // namespace gsr_runner
namespace {

CameraInset golden_sun_camera_inset() {
    CameraInset inset;
    if (!golden_sun_expanded_obj_view_active()) return inset;
    const std::uint16_t live_map = bus_read_u16(kCameraMapNumberAddr);
    if (live_map <= 1u) g_camera_room_map = live_map;
    if (g_camera_room_map <= 1u) return inset;
    const std::uint32_t s = bus_read_u32(kCameraStructPointer);
    const auto word = [&](std::uint32_t off) {
        return static_cast<std::int32_t>(bus_read_u32(s + off));
    };
    const std::int32_t spare_x = ((word(0xF4) - word(0xEC)) >> 16) - 240;
    const std::int32_t spare_y = ((word(0xF8) - word(0xF0)) >> 16) - 160;
    const auto x = gbarecomp::resolve_view_camera_insets(
        spare_x, g_golden_sun_wide_extra_left, g_golden_sun_wide_extra_right);
    const auto y = gbarecomp::resolve_view_camera_insets(
        spare_y, g_golden_sun_wide_extra_top, g_golden_sun_wide_extra_bottom);
    inset.left = static_cast<std::int32_t>(x.before);
    inset.right = static_cast<std::int32_t>(x.after);
    inset.top = static_cast<std::int32_t>(y.before);
    inset.bottom = static_cast<std::int32_t>(y.after);
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
const SkippedSparkLoop kSkippedSparkLoops[] = {
    {gs::rom(0x080CE476u), {gs::rom(0x080CE438u), gs::rom(0x080CE43Cu), 0u}},
    {gs::rom(0x080D4576u), {gs::rom(0x080D4532u), gs::rom(0x080D4536u), gs::rom(0x080D453Au)}},  // Spark Plasma
    {gs::rom(0x080D4BC4u), {gs::rom(0x080D4B7Eu), gs::rom(0x080D4B82u), 0u}},           // Supernova
    {gs::rom(0x080D51D8u), {gs::rom(0x080D5190u), gs::rom(0x080D5196u), gs::rom(0x080D519Au)}},
    {gs::rom(0x080DB5C6u), {gs::rom(0x080DB592u), gs::rom(0x080DB596u), 0u}},
    {gs::rom(0x080DE25Eu), {gs::rom(0x080DE22Au), gs::rom(0x080DE22Eu), 0u}},
    {gs::rom(0x080E6C98u), {gs::rom(0x080E6C5Eu), gs::rom(0x080E6C62u), 0u}},
    {gs::rom(0x080E98D2u), {gs::rom(0x080E988Eu), gs::rom(0x080E9892u), 0u}},
    {gs::rom(0x080E9FEAu), {gs::rom(0x080E9FB6u), gs::rom(0x080E9FBAu), 0u}},
    {gs::rom(0x080ECDDAu), {gs::rom(0x080ECD98u), gs::rom(0x080ECD9Eu), gs::rom(0x080ECDA2u)}},
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
    // Func_10230's first hooked site: a room's camera is being set up.
    if (pc == kCameraMinXBranchPc)
        g_camera_room_map = bus_read_u16(kCameraMapNumberAddr);
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

// Iris rows (FACTS.md 2026-10-07, "Iris close leaves a full-width strip").
// The window effects in Func_8f52c build each row's WIN0H from
// sqrt(R^2 << 16 - (3 * d * d) << s), d = row - centre, R <= 160. The camera
// inset can put the centre (the player) below the console screen; past
// 3 * d * d << s >= 2^31 the term wraps and far rows come out full width.
// At `adds r3,r5,#0` (r5 = d, then `muls r3,r5,r3`) such a row gets
// r3 = sign(d) * ceil(target / |d|), so the product lands just above
// target: past R's ellipse (always empty) and below the wrap. Rows that do
// not wrap are untouched.
struct IrisRowSite { std::uint32_t pc; unsigned shift; std::int64_t target; };
const IrisRowSite kIrisRowSites[] = {
    {gs::rom(0x0808FAF4u), 15u, 19500}, {gs::rom(0x0808FBCAu), 15u, 19500},
    {gs::rom(0x0808FCC6u), 15u, 19500}, {gs::rom(0x0808FDCAu), 14u, 39000},
};

int golden_sun_iris_row_value(std::uint32_t pc, std::uint32_t original,
                              std::uint32_t* out_value) {
    for (const IrisRowSite& site : kIrisRowSites) {
        if (pc != site.pc) continue;
        if (original != 0u || !golden_sun_expanded_obj_view_active()) return 0;
        const std::int64_t d = static_cast<std::int32_t>(g_cpu.R[5]);
        if ((3 * d * d) << site.shift < (std::int64_t{1} << 31)) return 0;
        const std::int64_t mag = d < 0 ? -d : d;
        const std::int64_t q = (site.target + mag - 1) / mag;
        *out_value = static_cast<std::uint32_t>((d < 0 ? -q : q) - d);
        return 1;
    }
    return -1;
}

int golden_sun_thumb_alu_immediate(std::uint32_t pc, std::uint32_t original,
                                   std::uint32_t* out_value) {
    if (gsr::intro_music::music.immediate(pc, g_cpu.R[5], out_value)) return 1;
    using namespace gsr::text_speed_cheat;
    if (const int r = golden_sun_worldmap_range_box(pc, original, out_value);
        r >= 0) {
        return r;
    }
    if (const int r = golden_sun_iris_row_value(pc, original, out_value);
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
    if (pc == kDecodedCharPc) {
        // Lines replaced a character at a time: the decoded character in r0
        // becomes `wanted`.
        std::uint32_t wanted = 0u;
        const auto put = [&] {
            *out_value = original + (wanted - g_cpu.R[0]);
            return 1;
        };
        // The Pause menu's Quality of Life row; "No autosaved data" on an
        // empty Autosave row (save_files_picker.h).
        if (gsr::qol::character(g_decoder_text_id, &wanted) ||
            gsr::save_files::autosave_character(g_decoder_text_id, &wanted))
            return put();
        if (gsr::custom_battle::text_character(g_decoder_text_id, original, out_value))
            return 1;
        // Random Weapons: a weapon's description names its new unleash.
        if (gsr::random_weapons::tbs::text_character(g_decoder_text_id, &wanted))
            return put();
    }
    if (pc == kDecodedCharPc && hard_mode_text_character(original, out_value))
        return 1;
    if (pc != kDecodedCharPc || !g_speed_caption_armed) return 0;
    const std::uint32_t text_id = g_decoder_text_id;
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

// Func_94820's rain respawns each drop at x/z = object [r6] + rand - 128
// (the -128 << 16 is this literal, 0x080949A4, added to both) and lands it
// at ground height Func_897c(0, x, z). In the Vale storm that height is 0
// while the party stands on raised ground, so the drops land 256 rows below
// the camera top (F12 0053: every drop's z - height - camera y in 128..383,
// x 69..323, the object at (196, 256) in all 120 frames): the wider view's
// top had no rain. In the expanded view the offset instead centres the
// rain on the view: a streak covers its landing spot - 160 .. + 4 (life
// table 0x0809EF84, frames 15..0), mid -78, so the landings centre 78 rows
// below the view's middle. x moves by the same amount (one literal); the
// side repeat in rain_margins.h fills x either way. Returns -1 for any
// other PC.
int golden_sun_rain_spawn_offset(std::uint32_t pc, std::uint32_t original,
                                 std::uint32_t* out_value) {
    if (pc != GS_ROM(0x08094958u)) return -1;
    if (!golden_sun_expanded_obj_view_active()) return 0;
    const std::uint32_t camera = bus_read_u32(kCameraStructPointer);
    const std::int32_t camera_y = static_cast<std::int32_t>(bus_read_u32(camera + 0xE8u));
    const std::int32_t centre_z = static_cast<std::int32_t>(bus_read_u32(g_cpu.R[6] + 8u));
    const std::int32_t view_middle = (160 + static_cast<std::int32_t>(g_golden_sun_wide_extra_bottom) -
                                      static_cast<std::int32_t>(g_golden_sun_wide_extra_top)) / 2;
    *out_value = original + static_cast<std::uint32_t>(
        camera_y + ((view_middle + 78) << 16) - centre_z);
    return 1;
}

int golden_sun_thumb_literal(std::uint32_t pc, std::uint32_t original,
                             std::uint32_t* out_value) {
    using namespace gsr::text_speed_cheat;
    if (const int r = golden_sun_rain_spawn_offset(pc, original, out_value); r >= 0)
        return r;
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
        if (bus_read_u32(kPressedKeys) & (kKeyA | kKeyB | kKeyStart)) {
            // Closing: the Difficulty row's text is ours no longer.
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
        seamless_frame_end();
        challenge_spare_for_lesson();
        gsr::challenge::frame();
        gsr::random_weapons::tbs::frame();
        gsr::treasure::tbs::frame();
        gsr::run_sheet::frame();
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
            g_settings_open.store(false);
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
            g_speed_caption = settings_text(gsr::difficulty_row::word());
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
            g_speed_caption = settings_text(kSpeedWords[value]);
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
// OAM y below the screen cannot be told from one above it. Func_94820's (the
// Vale storm's rain, F12 0050/0052) is the exception: its drops are placed
// by their full position (golden_sun_note_projected_obj_placement), so its
// upper test admits the bottom margin for a box that cannot wrap onto the
// console's top rows. Without it the storm had no rain below the screen:
// the drops spawn in a 256-pixel square centred 256..288 rows below the
// camera, and about two thirds of them were held back there. Returns -1 for
// any other PC.
int golden_sun_worldmap_sprite_branch(std::uint32_t pc, std::uint32_t original,
                                      std::uint32_t* out_decision) {
    enum Kind { kX, kYLower, kYUpper };
    std::int32_t operand = 0;
    Kind kind = kX;
    if (pc == GS_ROM(0x080948BCu)) {
        operand = static_cast<std::int32_t>(g_cpu.R[3]);
    } else if (pc == GS_ROM(0x08094C56u)) {
        operand = static_cast<std::int32_t>(g_cpu.R[1]);
    } else if (pc == GS_ROM(0x08094F26u)) {
        operand = static_cast<std::int32_t>(g_cpu.R[3]);
    } else if (pc == GS_ROM(0x080948C4u)) {
        operand = static_cast<std::int32_t>(g_cpu.R[0]);
        kind = kYLower;
    } else if (pc == GS_ROM(0x08094C5Eu)) {
        operand = static_cast<std::int32_t>(g_cpu.R[0]);
        kind = kYLower;
    } else if (pc == GS_ROM(0x08094F2Eu)) {
        operand = static_cast<std::int32_t>(g_cpu.R[8]);
        kind = kYLower;
    } else if (pc == GS_ROM(0x080948C8u)) {
        operand = static_cast<std::int32_t>(g_cpu.R[0]);
        kind = kYUpper;
    } else {
        return -1;
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
    } else if (kind == kYUpper) {
        // r4 is the life frame's table entry + 4 here (0x080948A2/B8); its
        // shape and size bytes are at +2 and +4 (stored at 0x08094902/32).
        const std::uint16_t shape = bus_read_u8(g_cpu.R[4] + 2u);
        const std::uint16_t size = bus_read_u8(g_cpu.R[4] + 4u);
        admit = operand < 160 + static_cast<std::int32_t>(
                                    g_golden_sun_wide_extra_bottom) &&
                gsr::widescreen::golden_sun_obj_bottom_margin_cannot_wrap(
                    operand, static_cast<std::uint16_t>(shape << 14),
                    static_cast<std::uint16_t>(size << 14));
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

// Sol Sanctum's cloud rows (overlay rom_78c76c, loaded at 0x02008000): the
// room code puts three rows of 32x8 cloud sprites at y = 80 - camera (the
// third at y + 8) and queues them only while `cmp r3,#0xAF; bhi` passes with
// r3 = y + 16, i.e. -16 <= y <= 159. In the expanded view the rows vanished
// while still inside the top margin and popped back in at y -16
// (logs/gpu_rewind_0141 frames 54-60). Admit the top band down to
// the rows' last visible line; the lower band is kept for the same 8-bit Y
// reason as the world-map sprites above. The site is checked against the
// instruction bytes, so another room's code at this address is untouched.
const std::uint32_t kSolSanctumCloudBranchPc = gs::ovl("rom_78c76c", 0x02008EECu);
int golden_sun_overlay_sprite_branch(std::uint32_t pc, std::uint32_t original,
                                     std::uint32_t* out_decision) {
    if (pc != kSolSanctumCloudBranchPc) return -1;
    if (original != 1u || !golden_sun_expanded_obj_view_active()) return 0;
    if (bus_read_u16(pc - 2u) != 0x2BAFu ||          // cmp r3, #0xAF
        (bus_read_u16(pc) & 0xFF00u) != 0xD800u)     // bhi
        return 0;
    const std::int32_t y = static_cast<std::int32_t>(g_cpu.R[3]) - 16;
    const std::int32_t top = -static_cast<std::int32_t>(
        g_golden_sun_wide_extra_top);
    if (y >= -16 || y + 16 <= top) return 0;
    *out_decision = 0u;
    return 1;
}

// Every reviewed conditional branch in config/usa/main.toml. The widescreen
// sites act only in the expanded view; the Message-speed icon bounce acts in
// every view.
int golden_sun_conditional_branch(std::uint32_t pc, std::uint32_t original,
                                  std::uint32_t* out_decision) {
    if (gsr::intro_music::music.branch(pc, g_cpu.R[5], out_decision)) return 1;
    using namespace gsr::text_speed_cheat;
    if (const int r = golden_sun_worldmap_sprite_branch(pc, original,
                                                        out_decision);
        r >= 0) {
        return r;
    }
    if (const int r = golden_sun_overlay_sprite_branch(pc, original,
                                                       out_decision);
        r >= 0) {
        return r;
    }
    if (const int r = cheat_drop_branch(pc, original, out_decision); r >= 0)
        return r;
    if (const int r = gsr::save_files::tbs::branch(pc, original, out_decision); r >= 0)
        return r;
    if (gsr::walk_through_walls::is_branch(pc))
        return gbarecomp::walk_through_walls::decide_branch(original, out_decision);
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
    gsr::intro_music::choices.header_write(pc, addr, requested, width);
    gsr::intro_music::music.header_write(pc, addr, requested, width);
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

}  // namespace

int main(int argc, char** argv) {
    // Must be the FIRST thing in main, before SDL init or any other
    // subsystem, so as much of the run as possible is covered. nullptr =
    // default to the directory this executable lives in.
    gbarecomp::crash_handler_install(nullptr);
    // Game RAM addresses the shared engine's probes, journals and host effect
    // capture need (engine/src/armv4t/game_ram_layout.h). Set before the guest
    // runs; the engine holds none of these itself.
    {
        GameRamLayout layout;
        // IWRAM allocation heap the game builds routines in.
        layout.iwram_heap_first = 0x03002000u;
        layout.iwram_heap_last = 0x030077FFu;
        // Overlay unpacker (Func_2544) slot, 0x03002000..0x030022C4, and the
        // game's IWRAM bump-allocator pointer ([0x03001E50] + 4, Func_4938):
        // where the next copied routine lands.
        layout.unpacker_slot = 0x03002000u;
        layout.unpacker_slot_len = 0x2C4u;
        layout.iwram_heap_ptr = gs::ram(0x03001E54u);
        // Measured transient RAM code image, half-open.
        layout.ram_image_start = 0x03005B0Cu;
        layout.ram_image_end = 0x03005D5Cu;
        // OAM shadow buffer, copied whole to OAM at the frame handoff.
        layout.oam_shadow_start = 0x0300347Cu;
        layout.oam_shadow_end = 0x0300387Cu;
        // CRASH-03 pool-LDM probe: the generated blitter pool's LDM at
        // 0x03006100 loads two words from the stack cells 0x03007E24/28; the
        // poison target is the neighbouring cell 0x03007E30; the tracked
        // stack window is 0x03007E00..0x03007E40 (16 words). The probe arms
        // on dispatch into either pool range.
        layout.pool_ldm_pc = 0x03006100u;
        layout.pool_ldm_slot0 = 0x03007E24u;
        layout.pool_ldm_slot1 = 0x03007E28u;
        layout.pool_ldm_poison_target = 0x03007E30u;
        layout.pool_ldm_window_base = kGoldenSunPoolLdmWindowBase;
        layout.pool_ldm_window_end = kGoldenSunPoolLdmWindowEnd;
        layout.pool_range0_lo = 0x030057E0u;
        layout.pool_range0_hi = 0x03005A64u;
        layout.pool_range1_lo = 0x03006000u;
        layout.pool_range1_hi = 0x03006500u;
        runtime_set_game_ram_layout(layout);
        // Party for the Infinite HP/PP cheats (USA/EU image): four slots at
        // 0x02000534, 0x14C bytes apart.
        runtime_set_party_layout(0x02000534u, 0x14Cu, 4u);
    }
    // F1 "Crash log": a crash report also gets the game's last instructions
    // (freezes get the same trail from the hang watchdog, hang_fp_tail.csv).
    // Every crash also writes crash_memory.bin (gsr_write_memory_snapshot,
    // zero cost during play).
    gsr_install_crash_snapshot_writer();
    gsr::unpacker_catch_install();
    gsr::unpacker_guard_install(GS_ROM(0x08002808u));  // Func_2808 (FACTS.md)
    // Func_2544, DMA'd to the same 0x03006000 (FACTS.md 2026-10-16)
    gsr::unpacker_guard_install_format0(GS_ROM(0x08002544u));
    // The payload-free producer trace stays opt-in
    // (GBARECOMP_VRAM_MAP_TRACE=1).
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
    options.builtin_rom_sha1 = gs::game_rom_sha1();
    gsr::custom_battle::tbs::install();
    // Target-list pointer: Spain/Italy use sp+0x14; USA/France/Germany use sp+0x1C
    // (FACTS.md language-support sections).
    gsr::enemy_hp_bar::install({.frame_routine_pc = GS_ROM(0x080030F8u),
                                .target_loop_return = GS_ROM(0x08026E1Cu),
                                .name_draw_pc = GS_ROM(0x08017AA4u),
                                .name_draw_return = GS_ROM(0x08026B8Cu),
                                .candidate_list_sp =
                                    (gs::rom_version() == gs::RomVersion::Spain ||
                                     gs::rom_version() == gs::RomVersion::Italy) ? 0x14u : 0x1Cu,
                                .enemy_base_slot = gs::ram(0x03001F28u)});
    options.rom_patch = [](std::vector<std::uint8_t>* rom) {
        set_revision_identities(*rom);
        gsr::custom_battle::prepare_animation_assets(rom);
    };
    // No Nintendo code runs by default: our own stand-ins
    // replace every BIOS service. `--bios <path>` in a GBARECOMP_LINK_BIOS
    // build is the comparison run.
    options.no_bios_by_default = true;
    options.function_entry_observer = golden_sun_function_entry_observer;
    {
        namespace field = gsr::field_character;
        static constexpr unsigned alternate_bodies[] = {18};  // Young Isaac.
        static const field::Game walking_character{
            GS_ROM(0x0800AA0Cu), GS_ROM(0x08185024u), kGoldenSunActorContextPtr,
            0x02000438u, 0x24u, 0x25u, field::kNoAction,
            field::kBrokenSealHeroes, alternate_bodies,
        };
        field::install(walking_character);
        gbarecomp::config_ui_set_game_page([] {
            field::draw();
            gsr::run_sheet::draw();
        });
        gsr::run_sheet::tbs::install();
        // Challenge Mode (challenge_mode.h). Its four bytes: save header
        // 0x02000028..2B, saved with the file (block 0 payload +0x28) but never
        // written by the header builder 0x0801F818 and zero in all 122 saved
        // first blocks measured (FACTS.md 4 November 2026).
        // HP share +0x14 as the HP setter 0x080782A0 leaves it; party of 4.
        static const gsr::challenge::Game challenge{
            0x02000028u, 0x02000040u, 0x02000438u, 0x02000500u, 0x14Cu, 0x38u, 0x14u,
            kGoldenSunActorContextPtr, gs::ram(0x03001E74u), kChallengeHeroes, 4,
        };
        gsr::challenge::install(challenge);
    }
    gsr::map_recorder_init();
    // Independent of the map recorder: its own env flag, its own session
    // directory, no shared state. See obj_recorder.h.
    gsr::obj_recorder_init();
    // After the recorder, for the same extra-draw chaining reason: each of
    // these captures whatever the previous one installed.
    gsr::room_buffer_init();
    gsr::trackers::install(gsr::djinn_tracker::tbs::kGame, gsr::treasure_tracker::tbs::kGame,
                           gsr::trackers::tbs::kText);
    gsr::retreat_travel::tbs::install(
        [] { return g_field_psynergy_fast.load() != 0; }, load_menu_font);
    gsr::radial_psynergy::tbs::install(load_menu_font);
    gsr::save_files::tbs::install_autosave();
    gsr::intro_music::choices.install();
    // gbarecomp keeps one savestate-load hook.
    gbarecomp::set_savestate_load_hook([] {
        gsr::retreat_travel::reset();
        gsr::qol::reset();
        gsr::radial_psynergy::reset();
        gsr::autosave::reset();
        gsr::custom_battle::tbs::reset();
        gsr::random_weapons::tbs::reset();
        g_hard_prompt_open = false;
        g_question_hold = QuestionHold::Off;
        gsr::intro_music::choices.on_load();
        gsr::intro_music::music.update(true);
        g_gpu_readback.invalidate();
        g_history.clear();
        g_world_settled_capture = {};
        g_world_margin_rgb.clear();
        g_world_margin_w = g_world_margin_h = 0;
        g_world_palette_effect.reset_indices();
        g_gpu_field_capture.valid = false;
        g_settings_open.store(false);
        g_settings_screen_text = false;
        g_hard_prompt_open = false;
        g_seamless_phase = SeamlessPhase::Off;
        gbarecomp::set_screen_veil(false, true);
        // The camera inset keys off the map number taken at the last room
        // setup; a state loaded from the title screen skips that setup, so
        // the inset would stay off and the view show 60/40 px past the room's
        // edge (gpu_frame_0129/0130). Take the loaded room's.
        g_camera_room_map = bus_read_u16(kCameraMapNumberAddr);
    });
    // Two fresh, single-purpose gbarecomp hooks (nothing else installs
    // either), so no chaining is needed: gpu_field_capture_hook copies this
    // frame's description out at VBlank start, gpu_field_present_override
    // draws it at present time. With GSR_GPU_FIELD off, the capture hook
    // skips the copy and the present override declines the frame.
    gbarecomp::set_scene_capture_hook(gpu_field_capture_hook);
    gbarecomp::set_frame_present_override_hook(golden_sun_present_override);
    options.max_view_width = 480;
    options.max_view_height = 320;
    options.aspect_view = true;
    options.frame_interpolation_available = true;
    options.enhanced_timing_available = true;
    options.native_renderer_available = true;
    options.max_resize_view_width = 240;
    options.resize_driven_view = false;
    options.extended_view_init = install_golden_sun_widescreen;
    options.thumb_alu_immediate_override = golden_sun_thumb_alu_immediate;
    options.thumb_literal_override = golden_sun_thumb_literal;
    options.swi_override = golden_sun_swi_override;
    options.conditional_branch_override = golden_sun_conditional_branch;
    options.keyinput_filter = cheat_menu_keyinput_filter;
    options.paused_overlay = cheat_menu_paint;
    options.save_extension_bytes = gsr::save_files::tbs::kExtensionBytes;
    gsr::qol::switches = kGameSwitches;
    // The player's switches are Quality of Life rows; F1 keeps only the
    // developer-only one after them.
    gsr::qol::switch_count = kPlayerGameSwitchCount;
    install_quality_of_life();
    gbarecomp::config_ui_set_game_switches(
        kGameSwitches + kPlayerGameSwitchCount,
        gbarecomp::developer_session() ? kGameSwitchCount - kPlayerGameSwitchCount : 0);
    // Localized ROMs get their own F1 menu and settings page two.
    if (gs::rom_version() == gs::RomVersion::Spain)
        gbarecomp::config_ui_set_translations(
            gsr::spanish_menu::kTable,
            static_cast<int>(std::size(gsr::spanish_menu::kTable)));
    else if (gs::rom_version() == gs::RomVersion::Italy)
        gbarecomp::config_ui_set_translations(
            gsr::italian_menu::kTable,
            static_cast<int>(std::size(gsr::italian_menu::kTable)));
    else if (gs::rom_version() == gs::RomVersion::France)
        gbarecomp::config_ui_set_translations(
            gsr::french_menu::kTable,
            static_cast<int>(std::size(gsr::french_menu::kTable)));
    else if (gs::rom_version() == gs::RomVersion::Germany)
        gbarecomp::config_ui_set_translations(
            gsr::german_menu::kTable,
            static_cast<int>(std::size(gsr::german_menu::kTable)));
    options.launcher_region = gs::rom_version_name();
    options.launcher_game_config = "game.toml";
    options.launcher_expose_widescreen = true;
    options.launcher_expose_adaptive_view = false;
    options.widescreen_view_width = 360;
    options.widescreen_view_height = 240;
    options.launcher_aspect_labels = gbarecomp::kViewModeLabels;
    options.launcher_aspect_view_widths = gbarecomp::kViewModeWidths;
    options.launcher_aspect_view_heights = gbarecomp::kViewModeHeights;
    options.launcher_num_aspects = gbarecomp::kViewModeCount;
    g_runtime_ram_dispatch_hook = verified_ram_dispatch;
    g_runtime_ram_identity_confirmed_hook = verified_ram_identity_confirmed;
    g_runtime_call_return_hook = verified_ram_dispatch_return_hook;
    g_runtime_guest_step_boundary_hook = verified_ram_dispatch_outer_boundary;
    g_runtime_mem_write_override = golden_sun_write_override;
    init_ram_code_page_masks();
    g_verified_ram_cache.clear();
    reset_verified_identity_cache();
    {
        std::size_t static_aot_ranges = 0;
        for (const auto& image : transient_code_images()) {
            if (image.dispatch_table == nullptr) ++static_aot_ranges;
        }
        std::fprintf(stderr,
            "GoldenSunRecomp: %zu transient-RAM range(s) registered to defer "
            "to the static AOT dispatch table once their identity matches "
            "(no heal/JIT needed).\n",
            static_aot_ranges);
    }
    reset_relocatable_identity_cache();
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
            print_ram_cache_image_detail();
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
