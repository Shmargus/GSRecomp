// RAM-code identity and verified/relocatable/stamp dispatch.
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

// Declared here for the functions below; they are used only in this file.
bool relocatable_resident_at(const RelocatableCodeImage& image,
                             std::uint32_t base, bool* prefix_passed);
unsigned int ram_code_page_epoch(std::uint32_t addr);
// An overlay's text extent and identity in the loaded revision. The USA
// registry is compiled in; a localized registry has the same ids in the same
// order (tools/build_localized_overlays.py) with its own extent and SHA-1.
struct OverlayRow {
    const char* id;
    std::uint32_t start;
    std::uint32_t end;
    const char* sha1;
};

constexpr OverlayRow kSpainOverlayRows[] = {
#define GSR_OVERLAY(id, start, end, sha1) {#id, start, end, sha1},
#include "../config/es/overlay-registry.inc"
#undef GSR_OVERLAY
};

constexpr OverlayRow kItalyOverlayRows[] = {
#define GSR_OVERLAY(id, start, end, sha1) {#id, start, end, sha1},
#include "../config/it/overlay-registry.inc"
#undef GSR_OVERLAY
};

constexpr OverlayRow kFranceOverlayRows[] = {
#define GSR_OVERLAY(id, start, end, sha1) {#id, start, end, sha1},
#include "../config/fr/overlay-registry.inc"
#undef GSR_OVERLAY
};

constexpr OverlayRow kGermanyOverlayRows[] = {
#define GSR_OVERLAY(id, start, end, sha1) {#id, start, end, sha1},
#include "../config/de/overlay-registry.inc"
#undef GSR_OVERLAY
};

OverlayRow overlay_row(const OverlayRow& usa) {
    if (gs::rom_version() == gs::RomVersion::Usa) return usa;
    const auto& rows = gs::rom_version() == gs::RomVersion::Germany
        ? kGermanyOverlayRows : gs::rom_version() == gs::RomVersion::France
        ? kFranceOverlayRows : gs::rom_version() == gs::RomVersion::Italy
        ? kItalyOverlayRows : kSpainOverlayRows;
    for (const OverlayRow& row : rows)
        if (std::strcmp(row.id, usa.id) == 0) return row;
    std::fprintf(stderr, "[rom_version] overlay %s has no %s registry row\n",
                 usa.id, gs::rom_version_name());
    std::abort();
}

// Not const: set_revision_identities() replaces the ROM-backed identities
// for a localized ROM before the game starts.
auto kTransientCodeImages = std::to_array<TransientCodeImage>({
    {0x03002000u, 0x0300207Cu, gs::rom(0x08002D5Cu),
     0,
     "a0e2414cc0e339ec6f61fbd387eb6325a8eabe84", "Func_2d5c",
     gsr_func2d5c_kDispatchTable, &gsr_func2d5c_kDispatchTableLen},
    {0x03002000u, 0x0300209Cu, gs::rom(0x0800A37Cu),
     0,
     "77bec232b22b723f3c5f5e0fedd28680fde50048", "Func_a37c",
     gsr_funca37c_kDispatchTable, &gsr_funca37c_kDispatchTableLen},
    {0x03002000u, 0x03002140u, gs::rom(0x08015430u),
     0,
     "d49cef2181ce6f9fee635daa0ba3b072d336109a", "Func_15430",
     nullptr, nullptr},
    {0x03002140u, 0x030021A0u, gs::rom(0x08015570u),
     0,
     "20f9190c9b7aac90e091beff5f7e54a46ba833eb", "Func_15570",
     nullptr, nullptr},
    // Func_2544 is a position-independent image. The row at 0x0300347c below
    // is the identity gate for the PCs the main corpus dispatches statically.
    {0x03002400u, 0x030024E0u, gs::rom(0x08001DC8u),
     0,
     "c2e9ace3620fc779387d91100c97071ebf21640b", "Func_1dc8",
     nullptr, nullptr},
    {0x0300387Cu, 0x0300395Cu, gs::rom(0x08001DC8u),
     0,
     "c2e9ace3620fc779387d91100c97071ebf21640b", "Func_1dc8_at_0300387c",
     nullptr, nullptr},
    {0x0300347Cu, 0x03003740u, gs::rom(0x08002544u),
     0,
     "b8967c9f00835f22da02cce31595588bb05496e6", "Func_2544_at_0300347c",
     nullptr, nullptr},
    // GS-011: the allocator reuses 0x03006000 for a second, larger ROM image.
    // DMA3 watch observed src=0x08002808 cnt=315 words; goldensun.elf agrees
    // (Func_2808, size 0x4ec, arm). SHA-1 keeps the two images distinct.
    // Player report 2026-10-03: live 0x00 vs ROM 0x01 at offset 0, written by
    // its own output pointer one byte past the buffer while still running; see
    // FACTS.md "Linux player crash: Func_2808 overwrote its own first byte".
    // Player report 2026-10-07 (CRASH-03 at Goma): offsets 0..4 overwritten,
    // FACTS.md "CRASH-03 recurred with no savestate loads". The volatile prefix
    // is the whole entry-only format check (ldrb/cmp/movne/bxne, +0x0..+0xF);
    // no branch in the routine targets below +0x10.
    {0x03006000u, 0x030064ECu, gs::rom(0x08002808u),
     0,
     "5fd23904086a4129fe3472dbf6658bb5e73f0363",
     "Func_2808_at_03006000",
     gsr_func2808_03006000_kDispatchTable,
     &gsr_func2808_03006000_kDispatchTableLen,
     16u},
    // Func_6abc's stack thunk (Func_6878 installs it at whatever depth it was
    // entered at) is a single position-independent image below, as are
    // Func_9bb8, Func_15430, Func_15570 and Func_158e8; the DMA3 census behind
    // that is in docs/GS011_TRANSIENT_IMAGES.md.
    // Func_a418 sits immediately below the 0x0300347c staging slot, so the
    // flash driver's working area is a contiguous run of separately installed
    // routines. DMA3 watch: src=0x0800a418, 31 words; entry observed through
    // _call_via_r4 in ARM. goldensun.elf sizes Func_a418 at exactly 0x7c.
    {0x03003400u, 0x0300347Cu, gs::rom(0x0800A418u),
     0,
     "d3261b6f7b57ba4e7da8e44cf555d009c7cc27d1",
     "Func_a418_at_03003400",
     gsr_funca418_03003400_kDispatchTable,
     &gsr_funca418_03003400_kDispatchTableLen},
    // 0x03003a84 is a reusable staging slot for the flash driver. Trace
    // #4699483 reprograms DMA3 SAD=0x08015e10 CNT=0x1f words over the
    // Func_15afc image below, and #4699487 enters it in ARM through
    // _call_via_r4. goldensun.elf sizes Func_15e10 at exactly 0x7c bytes.
    {0x03003A84u, 0x03003B00u, gs::rom(0x08015E10u),
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
    {0x03003A84u, 0x03003CFCu, gs::rom(0x08015AFCu),
     0,
     "f6ab01b56876f8d1b72a4bf5239bd5cbac620cb0",
     "Func_15afc_at_03003a84",
     gsr_func15afc_03003a84_kDispatchTable,
     &gsr_func15afc_03003a84_kDispatchTableLen},
    // Func_2cf4: Func_3a7c DMAs it to whatever its SP happens to be, so the
    // set of bases is bounded only by the call graph. It is a single
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
    // The third 24-byte template, at 0x03000904. A self-heal discovery run
    // reached 0x03000828 holding image SHA-1 d8d1bcfc..., which is exactly
    // what expanding that template produces.
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
    // player_builder/builder_main.cpp builds each variant.
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
    // Func_dc8's third self-modifying slot: 0x080037D4 copies one of five
    // 0x98-byte blocks from ROM 0x08000404 over 0x03000BD8..0x03000C70; each
    // ends at 0x03000C70. main.toml makes 0x03000BD8 a runtime_code_entry.
    // player_builder/builder_main.cpp builds each variant.
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
    // main.toml's [[code_copy]] runtime_start=0x03000000 source_start=
    // 0x08000770 size=0x1400 DMA-installs a fixed ROM image that kDispatchTable
    // already has AOT entries for (gf_irq_handler_03000000 and friends). Page 0
    // of IWRAM (0x03000000..0x03000FFF) is registered for dirty-tracking by the
    // synth-template rows above, so that DMA copy itself sets the page-0 dirty
    // bit before any dispatch ever reaches it, discarding the AOT fn on first
    // touch.
    //
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
    {0x03000000u, 0x030000E0u, gs::rom(0x08000770u), 0,
     "425e210c9524665da2b651e29039187452e03538",
     "main_copy_03000000_code_0000_arm", nullptr, nullptr},
    {0x03000000u, 0x030000E0u, gs::rom(0x08000770u), 1,
     "425e210c9524665da2b651e29039187452e03538",
     "main_copy_03000000_code_0000_thumb", nullptr, nullptr},
    {0x03000118u, 0x03000350u, gs::rom(0x08000888u), 0,
     "24be4da49c5bf5e6687b01c5d42e6e50b80a9b2c",
     "main_copy_03000000_code_0118_arm", nullptr, nullptr},
    {0x03000118u, 0x03000350u, gs::rom(0x08000888u), 1,
     "24be4da49c5bf5e6687b01c5d42e6e50b80a9b2c",
     "main_copy_03000000_code_0118_thumb", nullptr, nullptr},
    {0x03000380u, 0x03000716u, gs::rom(0x08000AF0u), 0,
     "6615b4c278a236c602a2573e1d734ce8a93f5b3a",
     "main_copy_03000000_code_0380_arm", nullptr, nullptr},
    {0x03000380u, 0x03000716u, gs::rom(0x08000AF0u), 1,
     "6615b4c278a236c602a2573e1d734ce8a93f5b3a",
     "main_copy_03000000_code_0380_thumb", nullptr, nullptr},
    {0x0300071Cu, 0x03000828u, gs::rom(0x08000E8Cu), 0,
     "d8b24aa5a6292596e6fcb757cc036ae655016845",
     "main_copy_03000000_code_071c_arm", nullptr, nullptr},
    {0x0300071Cu, 0x03000828u, gs::rom(0x08000E8Cu), 1,
     "d8b24aa5a6292596e6fcb757cc036ae655016845",
     "main_copy_03000000_code_071c_thumb", nullptr, nullptr},
    {0x030008D4u, 0x0300099Cu, gs::rom(0x08001044u), 0,
     "15701bb90db4b5c68dec0b4b3976a37639a06f70",
     "main_copy_03000000_code_08d4_arm", nullptr, nullptr},
    {0x030008D4u, 0x0300099Cu, gs::rom(0x08001044u), 1,
     "15701bb90db4b5c68dec0b4b3976a37639a06f70",
     "main_copy_03000000_code_08d4_thumb", nullptr, nullptr},
    {0x030009A8u, 0x03000A58u, gs::rom(0x08001118u), 0,
     "66c770afa4d055cdd754ab31bbb59bce0c19787b",
     "main_copy_03000000_code_09a8_arm", nullptr, nullptr},
    {0x030009A8u, 0x03000A58u, gs::rom(0x08001118u), 1,
     "66c770afa4d055cdd754ab31bbb59bce0c19787b",
     "main_copy_03000000_code_09a8_thumb", nullptr, nullptr},
    {0x03000A98u, 0x03000B5Cu, gs::rom(0x08001208u), 0,
     "b21981f20a35278add1fb2390af860ea504be0d4",
     "main_copy_03000000_code_0a98_arm", nullptr, nullptr},
    {0x03000A98u, 0x03000B5Cu, gs::rom(0x08001208u), 1,
     "b21981f20a35278add1fb2390af860ea504be0d4",
     "main_copy_03000000_code_0a98_thumb", nullptr, nullptr},
    {0x03000B60u, 0x03000BD8u, gs::rom(0x080012D0u), 0,
     "20471e8954c7f61e1f6d346db745689164320992",
     "main_copy_03000000_code_0b60_arm", nullptr, nullptr},
    {0x03000B60u, 0x03000BD8u, gs::rom(0x080012D0u), 1,
     "20471e8954c7f61e1f6d346db745689164320992",
     "main_copy_03000000_code_0b60_thumb", nullptr, nullptr},
    {0x03000C70u, 0x03000DF8u, gs::rom(0x080013E0u), 0,
     "a0530c6e4231247de55a8ae4c0737634fbc0480f",
     "main_copy_03000000_code_0c70_arm", nullptr, nullptr},
    {0x03000C70u, 0x03000DF8u, gs::rom(0x080013E0u), 1,
     "a0530c6e4231247de55a8ae4c0737634fbc0480f",
     "main_copy_03000000_code_0c70_thumb", nullptr, nullptr},
    {0x03001388u, 0x03001400u, gs::rom(0x08001AF8u), 0,
     "b4b275f498b4a12b1ba721a3832e6ffc6b065fd6",
     "main_copy_03000000_code_1388_arm", nullptr, nullptr},
    {0x03001388u, 0x03001400u, gs::rom(0x08001AF8u), 1,
     "b4b275f498b4a12b1ba721a3832e6ffc6b065fd6",
     "main_copy_03000000_code_1388_thumb", nullptr, nullptr},
#define GSR_OVERLAY(id, usa_start, usa_end, usa_sha1)                      \
    [] {                                                                   \
        const OverlayRow row =                                             \
            overlay_row({#id, usa_start, usa_end, usa_sha1});              \
        return TransientCodeImage{row.start, row.end, 0u, 1, row.sha1,     \
                                  "overlay_" #id,                          \
                                  gsr_overlay_##id##_kDispatchTable,       \
                                  &gsr_overlay_##id##_kDispatchTableLen};  \
    }(),
#include "overlay-registry.inc"
#undef GSR_OVERLAY
});
}  // namespace
namespace gsr_runner {
std::span<const TransientCodeImage> transient_code_images() { return kTransientCodeImages; }

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
}  // namespace gsr_runner
namespace {

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

// Not const, for the same reason as kTransientCodeImages.
std::array<RelocatableCodeImage, 29> kRelocatableCodeImages{{
    // Func_2cf4, DMA-copied onto the stack by Func_3a7c. Observed at
    // 0x03007ba4, 0x03007dbc, 0x03007dc4 and 0x03007dc8; the extent, mode and
    // identity are the same at every one of them, and goldensun.elf bounds the
    // ROM source at 0x08002cf4 size 0x68 with ARM mapping symbols.
    {gs::rom(0x08002CF4u),
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
    {gs::rom(0x08001DC8u),
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
    {gs::rom(0x08006ABCu),
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
    {gs::rom(0x08009BB8u),
     0,
     "38da8bb7176c71ef1e86ce50346af1d72174529c",
     "Func_9bb8_relocatable",
     &gsr_func9bb8_pic_kImageOrigin,
     &gsr_func9bb8_pic_kImageSize,
     gsr_func9bb8_pic_kDispatchTable,
     &gsr_func9bb8_pic_kDispatchTableLen},
    {gs::rom(0x08015430u),
     0,
     "d49cef2181ce6f9fee635daa0ba3b072d336109a",
     "Func_15430_relocatable",
     &gsr_func15430_pic_kImageOrigin,
     &gsr_func15430_pic_kImageSize,
     gsr_func15430_pic_kDispatchTable,
     &gsr_func15430_pic_kDispatchTableLen},
    // Func_15570 is always installed contiguously after Func_15430, so the two
    // are separate images at a fixed stride rather than one larger image.
    {gs::rom(0x08015570u),
     0,
     "20f9190c9b7aac90e091beff5f7e54a46ba833eb",
     "Func_15570_relocatable",
     &gsr_func15570_pic_kImageOrigin,
     &gsr_func15570_pic_kImageSize,
     gsr_func15570_pic_kDispatchTable,
     &gsr_func15570_pic_kDispatchTableLen},
    {gs::rom(0x080158E8u),
     0,
     "03e57ceee902e248334bb546cd32716edf90fca3",
     "Func_158e8_relocatable",
     &gsr_func158e8_pic_kImageOrigin,
     &gsr_func158e8_pic_kImageSize,
     gsr_func158e8_pic_kDispatchTable,
     &gsr_func158e8_pic_kDispatchTableLen},
    // ADDITIVE at 0x0300347c: that fixed row stays, because it gates PCs the
    // main corpus dispatches statically. This image covers every other base.
    {gs::rom(0x08002544u),
     0,
     "b8967c9f00835f22da02cce31595588bb05496e6",
     "Func_2544_relocatable",
     &gsr_func2544_pic_kImageOrigin,
     &gsr_func2544_pic_kImageSize,
     gsr_func2544_pic_kDispatchTable,
     &gsr_func2544_pic_kDispatchTableLen},
    // Func_6b84: session 20260923_114927 compiled 0x03007B64 at runtime; its
    // bytes equal ROM 0x08006b84, goldensun.elf Func_6b84 (THUMB, 0x24 bytes,
    // the last halfword a $d pad). A byte-copy loop, copied onto the stack.
    // config/usa/transient-func_6b84-relocatable.toml.
    {gs::rom(0x08006B84u),
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
    {gs::rom(0x08007994u),
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
    {gs::rom(0x08009E7Cu),
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
    {gs::rom(0x0800A0F8u),
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
    {gs::rom(0x08001EA8u),
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
    {gs::rom(0x08001EF8u),
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
    {gs::rom(0x08001F38u),
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
    {gs::rom(0x08001FB8u),
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
    {gs::rom(0x08006C24u),
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
    {gs::rom(0x0800203Cu),
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
    {gs::rom(0x08002098u),
     0,
     "92f07a6eb708e30d86f375fd841dac38bf6dc905",
     "Func_2098_relocatable",
     &gsr_func2098_pic_kImageOrigin,
     &gsr_func2098_pic_kImageSize,
     gsr_func2098_pic_kDispatchTable,
     &gsr_func2098_pic_kDispatchTableLen},
    // A pooled routine, found only by the 5,400-frame campaign run. It
    // aborted as an unknown identity at 0x0300387c, 208 bytes different from
    // the Func_1dc8 image that address had been hosting; a DMA3 watch on that
    // address records ch=3 src=0x08001b70 word=0/150 (150 words = 0x258 bytes)
    // as the last writer, the live first word 0xE92D40E2 matches ROM
    // 0x08001b70, and goldensun.elf sizes Func_1b70 at exactly 0x258 with an
    // $a at the entry. Registered position-independent from the start.
    //
    // This is the class relocation does NOT close: it removes the need to
    // enumerate bases, not the need to discover routines.
    {gs::rom(0x08001B70u),
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
    {gs::rom(0x08015AFCu),
     0,
     "f6ab01b56876f8d1b72a4bf5239bd5cbac620cb0",
     "Func_15afc_relocatable",
     &gsr_func15afc_pic_kImageOrigin,
     &gsr_func15afc_pic_kImageSize,
     gsr_func15afc_pic_kDispatchTable,
     &gsr_func15afc_pic_kDispatchTableLen},
    {gs::rom(0x08015E10u),
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
    {gs::rom(0x08015D74u),
     0,
     "4058793ba5db0477fc9c5c556edb91984dee3de0",
     "Func_15d74_relocatable",
     &gsr_func15d74_pic_kImageOrigin,
     &gsr_func15d74_pic_kImageSize,
     gsr_func15d74_pic_kDispatchTable,
     &gsr_func15d74_pic_kDispatchTableLen},
    {gs::rom(0x080155D0u),
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
    {gs::rom(0x08009BB8u),
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
    {gs::rom(0x08009BB8u),
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
    {gs::rom(0x080B5138u),
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
    {gs::rom(0x080F0024u),
     0,
     "603f4e8cb1f2e0603244cadea88fc355448ebe86",
     "Func_f0024_relocatable",
     &gsr_funcf0024_pic_kImageOrigin,
     &gsr_funcf0024_pic_kImageSize,
     gsr_funcf0024_pic_kDispatchTable,
     &gsr_funcf0024_pic_kDispatchTableLen,
     kFuncB5138ExcludedRanges,
     2},
    // Func_a418, also registered fixed at 0x03003400 (see
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
    {gs::rom(0x0800A418u),
     0,
     "d3261b6f7b57ba4e7da8e44cf555d009c7cc27d1",
     "Func_a418_relocatable",
     &gsr_funca418_pic_kImageOrigin,
     &gsr_funca418_pic_kImageSize,
     gsr_funca418_pic_kDispatchTable,
     &gsr_funca418_pic_kDispatchTableLen},
}};

}  // namespace
namespace gsr_runner {
// The SHA-1s above are the USA ROM's bytes at each image's source. A localized
// ROM moves and re-points those routines (the copies are otherwise the same
// code, FACTS.md "Spanish overlays and transient RAM code"), so for it each
// ROM-backed identity is the loaded ROM's own bytes at the mapped source,
// hashed exactly as the live check hashes the copy. Runs once, before the
// game starts; images with no ROM source (overlays, synth templates) keep
// their registry identity.
void set_revision_identities(const std::vector<std::uint8_t>& rom) {
    if (gs::rom_version() == gs::RomVersion::Usa) return;
    static std::deque<std::string> storage;
    const auto rom_bytes = [&rom](std::uint32_t rom_start, std::uint32_t size,
                                  const char* name) {
        const std::uint32_t offset = rom_start - 0x08000000u;
        if (rom_start < 0x08000000u || offset > rom.size() ||
            size > rom.size() - offset) {
            std::fprintf(stderr,
                "[rom_version] %s source 0x%08X+0x%X lies outside the ROM\n",
                name, rom_start, size);
            std::abort();
        }
        return rom.data() + offset;
    };
    for (auto& image : kTransientCodeImages) {
        if (image.rom_start == 0u) continue;
        const std::uint32_t size = image.end - image.start;
        const std::uint8_t* bytes = rom_bytes(image.rom_start, size, image.name);
        image.sha1 = storage.emplace_back(gba::sha1(bytes, size).hex()).c_str();
    }
    for (auto& image : kRelocatableCodeImages) {
        if (image.rom_start == 0u) continue;
        const std::uint32_t size = *image.size;
        const std::uint8_t* bytes = rom_bytes(image.rom_start, size, image.name);
        image.sha1 = storage
                         .emplace_back(gsr::sha1_excluding(
                             size,
                             [bytes](std::uint32_t offset) { return bytes[offset]; },
                             image.excluded_ranges, image.excluded_ranges_len))
                         .c_str();
    }
}
}  // namespace gsr_runner
namespace {

struct GoldenSunFunc1dc8WriterCache {
    bool valid = false;
    bool image_verified = false;
    std::uint32_t base = 0;
    std::uint64_t generation = 0;
    std::array<unsigned int, 64> page_epochs{};
};
std::array<GoldenSunFunc1dc8WriterCache, 4>
    g_golden_sun_func1dc8_writer_cache{};

}  // namespace
namespace gsr_runner {
void reset_golden_sun_func1dc8_writer_diagnostics() {
    g_golden_sun_func1dc8_writer_cache.fill({});
    g_golden_sun_func1dc8_writer_recognized = 0;
    g_golden_sun_func1dc8_writer_identity_mismatch = 0;
    g_golden_sun_func1dc8_writer_unknown_variant = 0;
    g_golden_sun_func1dc8_writer_logs = 0;
    g_golden_sun_func1dc8_last_base = 0;
    g_golden_sun_func1dc8_last_generation = 0;
}
}  // namespace gsr_runner
namespace {

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

}  // namespace
namespace gsr_runner {
bool golden_sun_func1dc8_writer_pc(std::uint32_t pc,
                                   gsr::Func1dc8WriterRoute route) {
    const bool diagnostics = golden_sun_wide_diagnostics_enabled();
    // Keep the fixed route when diagnostics are off;
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
}  // namespace gsr_runner
namespace {

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
        return gbarecomp::env_flag("GBARECOMP_RELOCATABLE_PROFILE",
                                   gsr_additional_debug_logging() != 0);
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
        return gbarecomp::env_flag("GBARECOMP_RELOCATABLE_LOG",
                                   gsr_additional_debug_logging() != 0);
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
}  // namespace
namespace gsr_runner {
unsigned long long g_relocatable_identity_hashes = 0;
unsigned long long g_relocatable_identity_cache_hits = 0;
// Epoch/local-word check failed but the live bytes still equal the snapshot
// taken at the last matching SHA-1, so the hash was skipped.
unsigned long long g_relocatable_identity_snapshot_hits = 0;
}  // namespace gsr_runner
namespace {

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

// Which branch of verified_ram_dispatch answered, for the unpacker guard's
// journal (shared_game/unpacker_guard.h).
gsr::RamResolvePath g_ram_resolve_path = gsr::RamResolvePath::None;

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
            gbarecomp::at_game_exit(report_relocatable_profile);
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
        g_ram_resolve_path = gsr::RamResolvePath::Relocatable;
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
    // opcode narrows the scan to a small exact candidate set; the full
    // identity hash below remains the authority.
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
    // Images without immutable ROM backing cannot use the opcode index, so
    // they get an exhaustive scan; identity hashing remains the authority and
    // ROM-backed images pay no cost here.
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
// registry aborting on it is a development guard that suits copies and is
// wrong for generated code.
//
// Policy:
//   * strict-static run  -> abort. A strict-static claim
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
// translation is worse than aborting.
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

// The Difficulty row (hard_mode.h, shared_game/difficulty_row.h): the
// screen's own byte only flips; the row's choice is kept beside it.
std::uint8_t g_hard_mode_saved_byte = 0;
void hard_mode_settings_setup() {
    using namespace gsr::hard_mode;
    g_hard_mode_saved_byte = bus_read_u8(kSavedFlag);
    gsr::difficulty_row::open(g_hard_mode_saved_byte == kHardModeValue,
                              gsr::random_psynergy::tbs::current().scaled);
    bus_write_u8(kSavedFlag, 0u);
}
void hard_mode_settings_teardown() {
    using namespace gsr::hard_mode;
    if (g_cpu.R[5] != 0u) {
        // B: nothing stored.
        bus_write_u8(kSavedFlag, g_hard_mode_saved_byte);
        return;
    }
    const int choice = gsr::difficulty_row::g_choice;
    const std::uint8_t value = gsr::difficulty_row::hard(choice) ? kHardModeValue : 0u;
    bus_write_u8(kSavedFlag, value);
    bus_write_u8(kAutoSleepFlag, value);
    auto run = gsr::random_psynergy::tbs::current();
    if (run.scaled != gsr::difficulty_row::scaled(choice)) {
        run.scaled = gsr::difficulty_row::scaled(choice);
        gsr::random_psynergy::tbs::store(run);
    }
}

// Djinn on a left-out hero go to the first pick once, at the first battle
// after the Djinn lesson (Jimmy, 4 November). The lesson after Flint scripts
// a hand-over to party slots, which can leave Flint with dead Isaac; the
// lesson battle then never ends. Only that once: the player may give Djinn
// to left-out heroes in the Djinn menu and they stay there (Jimmy,
// 4 November). Owned bits +0xF8 + element*4; the game's own move 0x0807A498
// (from, element, index, to) and recalculation 0x08077428, as its Djinn
// menu does (0x080AA9F8).
bool g_challenge_lesson_ended = false;

void challenge_djinn_to_leader() {
    namespace ch = gsr::challenge;
    if (!g_challenge_lesson_ended) return;
    g_challenge_lesson_ended = false;
    const ch::Config c = ch::current();
    if (c.size == 0) return;
    const unsigned leader = c.picks[0];
    for (unsigned id = 0; id < kChallengeHeroes; ++id) {
        if (!ch::excluded(id) || static_cast<int>(id) == ch::spared) continue;
        const std::uint32_t record = gsr::challenge::record(id);
        bool moved = false;
        for (unsigned element = 0; element < 4u; ++element) {
            const std::uint32_t owned = bus_read_u32(record + 0xF8u + element * 4u);
            for (unsigned index = 0; index < 7u; ++index) {
                if (!(owned & (1u << index))) continue;
                std::uint32_t result = 0;
                gbarecomp::guest_call(GS_ROM(0x0807A498u) | 1u, id, element, index, leader, &result);
                moved = true;
            }
        }
        if (!moved) continue;
        std::uint32_t result = 0;
        gbarecomp::guest_call(GS_ROM(0x08077428u) | 1u, id, 0, 0, 0, &result);
        gbarecomp::guest_call(GS_ROM(0x08077428u) | 1u, leader, 0, 0, 0, &result);
    }
}

}  // namespace
namespace gsr_runner {
// The Djinn lesson after Flint needs Isaac alive (Jimmy, 4 November): while
// story flag 0x16E is set (overlay rom_77a7c8 sets it at 0x02009D02 and
// clears it at 0x0200A22C; battle reads it at 0x080B64B6..) a left-out Isaac
// is spared, woken once with full HP. After it he goes down again, and his
// Djinn move to the first pick at the next battle (once).
void challenge_spare_for_lesson() {
    namespace ch = gsr::challenge;
    constexpr unsigned kIsaac = 0;
    const bool lesson = story_flag(0x16Eu);
    if (!lesson || !ch::excluded(kIsaac)) {
        if (ch::spared == static_cast<int>(kIsaac)) g_challenge_lesson_ended = true;
        ch::spared = -1;
        return;
    }
    if (ch::spared == static_cast<int>(kIsaac)) return;
    ch::spared = static_cast<int>(kIsaac);
    const std::uint32_t record = gsr::challenge::record(kIsaac);
    bus_write_u16(record + 0x38u, bus_read_u16(record + 0x34u));
    bus_write_u16(record + 0x14u, 0x4000u);
}
}  // namespace gsr_runner
namespace {

// Challenge Mode's revive blocks (challenge_mode.h; FACTS.md "Revive paths",
// 4 November 2026). A left-out hero answers every revive like a living one.
void challenge_on_entry(std::uint32_t pc) {
    namespace ch = gsr::challenge;
    if (pc == GS_ROM(0x0807961Cu)) {
        // Join (character): returns the member count. Full party: refused.
        std::uint32_t count = 0;
        if (ch::refuse_join(g_cpu.R[0], &count)) gbarecomp::guest_return_now(pc, count);
    } else if (pc == GS_ROM(0x08077F70u)) {
        // End of the opening: wakes ids 0 and 1 by name (FACTS.md, F12 0088).
        ch::wake_picks(0x34u, 0x36u, 0x3Au, 0x16u);
    } else if (pc == GS_ROM(0x0807A1B4u) &&
               (g_cpu.R[14] & ~1u) == GS_ROM(0x0807A188u)) {
        // Djinn grant 0x0807A0F4 handing (character, element, index) over;
        // it keeps the character in r9 for the second hand-over and its
        // return value. A left-out pick: a living pick instead.
        if (!ch::excluded(g_cpu.R[0])) return;
        const int id = ch::djinn_recipient(g_cpu.R[1], 0x118u);
        if (id >= 0) g_cpu.R[0] = g_cpu.R[9] = static_cast<std::uint32_t>(id);
    } else if (pc == GS_ROM(0x080AE778u)) {
        // Djinn menu give check (from slot, to slot): 1 = allowed. Menu
        // [0x03001F2C]: +0x219 member count, +0x208 u16 ids; totals as
        // 0x080AE7FC counts them (bits 0..19 of +0xF8 | +0x108, 4 elements).
        if (ch::current().size == 0) return;
        const std::uint32_t menu = bus_read_u32(gs::ram(0x03001F2Cu));
        int n = bus_read_u8(menu + 0x219u);
        if (n > 8) n = 8;
        unsigned ids[8];
        int totals[8];
        for (int i = 0; i < n; ++i) {
            ids[i] = bus_read_u16(menu + 0x208u + static_cast<std::uint32_t>(i) * 2u);
            const std::uint32_t r = ch::record(ids[i]);
            int total = 0;
            for (unsigned e = 0; e < 4u; ++e) {
                const std::uint32_t bits = (bus_read_u32(r + 0xF8u + e * 4u) |
                                            bus_read_u32(r + 0x108u + e * 4u)) & 0xFFFFFu;
                total += __builtin_popcount(bits);
            }
            totals[i] = total;
        }
        const bool ok = ch::djinn_give_balanced(ids, totals, n,
            static_cast<int>(g_cpu.R[0]), static_cast<int>(g_cpu.R[1]));
        gbarecomp::guest_return_now(pc, ok ? 1u : 0u);
    } else if (pc == GS_ROM(0x080B6B40u)) {
        // Battle lists the living (HP > 0) to give commands and take turns.
        challenge_spare_for_lesson();
        ch::keep_down();
        challenge_djinn_to_leader();
    } else if (pc == GS_ROM(0x080B6A60u)) {
        // Battle's party list (out): the only builder, called by setup,
        // commands and turns (10 callers, FACTS.md 12 November). Left-out
        // heroes are left off. At most 4, 3 when [battle] +0x44 is set;
        // record +0x12A = 2 per member.
        const ch::Config c = ch::current();
        if (c.size == 0) return;
        challenge_spare_for_lesson();
        gba::GbaBus* b = ch::bus();
        if (!b) return;
        const int cap = bus_read_u8(bus_read_u32(gs::ram(0x03001E74u)) + 0x44u) ? 3 : 4;
        gbarecomp::guest_return_now(pc, ch::battle_party(*b, c, g_cpu.R[0], cap, 0x12Au));
    } else if (pc == GS_ROM(0x080792FCu) &&
               (g_cpu.R[14] & ~1u) == GS_ROM(0x08078FDAu)) {
        // New game: (character, level) raises the level-1 hero to its story
        // level (0x08078FD6 in the hero builder 0x08078EE8).
        ch::keep_start_record(g_cpu.R[0]);
    } else if (pc == GS_ROM(0x08079F10u)) {
        // Battle: (?, character, ?, effect kind); 0 = no effect on them.
        if (ch::revive_kind(g_cpu.R[3]) && ch::excluded(g_cpu.R[1]))
            gbarecomp::guest_return_now(pc, 0u);
    } else if (pc == GS_ROM(0x080B27B0u)) {
        // Sanctum: (character, service); service 0 revives. 0 = not needed.
        if (g_cpu.R[1] == 0u && ch::excluded(g_cpu.R[0])) gbarecomp::guest_return_now(pc, 0u);
    } else if (pc == GS_ROM(0x080A9F10u)) {
        // Field Psynergy and items: (ability, user, target, ?). A revive on a
        // living target leaves message 0xD at [menu] + 0x25A and returns -1.
        if (!ch::excluded(g_cpu.R[2])) return;
        std::uint32_t ability = g_cpu.R[0] & 0x3FFFu;
        if (ability >= 0x208u) ability = 0;  // 0x08078B9C
        if (!ch::revive_kind(bus_read_u8(GS_ROM(0x0807EE58u) + ability * 16u + 3u))) return;
        bus_write_u16(bus_read_u32(gs::ram(0x03001F2Cu)) + 0x25Au, 0xDu);
        gbarecomp::guest_return_now(pc, 0xFFFFFFFFu);
    }
}

}  // namespace
namespace gsr_runner {
void golden_sun_function_entry_observer(std::uint32_t entry_pc) {
    if (gsr::qol::on_entry(entry_pc)) return;
    // Autosave notes the field poll before the radial may answer it.
    gsr::autosave::on_entry(entry_pc);
    // The radial first: it notes every dispatcher entry, Retreat's included.
    if (gsr::radial_psynergy::on_entry(entry_pc)) return;
    if (gsr::retreat_travel::tbs::on_entry(entry_pc)) return;
    // Random Psynergy: the Psynergy list rebuild 0x08078BF0(id) is ours.
    if (gsr::random_psynergy::tbs::on_entry(entry_pc, g_cpu.R[0])) {
        gbarecomp::guest_return_now(entry_pc, 0u);
        return;
    }
    challenge_on_entry(entry_pc);
    {
        // Random Treasure: field pickups, big coin amounts, gifts.
        std::uint32_t r0 = g_cpu.R[0];
        if (gsr::treasure::tbs::on_entry(entry_pc, g_cpu.R[14], g_cpu.R[7], &r0)) g_cpu.R[0] = r0;
    }
    gsr::save_files::tbs::on_entry(entry_pc);
    gsr::field_character::on_entry(entry_pc);
    gsr::intro_music::choices.on_entry(entry_pc);
    gsr::intro_music::music.on_entry(entry_pc);
    gsr::custom_battle::tbs::on_entry(entry_pc);
    if (entry_pc == gsr::text_speed_cheat::kLineStartPc &&
        (g_cpu.R[14] & ~1u) == gsr::text_speed_cheat::kLineStartReturn) {
        g_decoder_text_id = g_cpu.R[1];
        gsr::qol::text_pos = 0;
        gsr::qol::text_id = g_decoder_text_id;
    }
    if (entry_pc == gsr::hard_mode::kSettingsSetupEntryPc) {
        g_settings_screen_text = true;
        hard_mode_settings_setup();
    } else if (entry_pc == gsr::hard_mode::kSettingsTeardownEntryPc) {
        hard_mode_settings_teardown();
    } else if (entry_pc == gsr::hard_mode::kNewGameEntryPc) {
        question_hold_new_game_entry();
    } else if (entry_pc == GS_ROM(0x08020BD8u)) {
        question_hold_naming_entry();
    } else if (entry_pc == GS_ROM(0x0809163Cu)) {
        skip_opening_apply();
    } else if (entry_pc == gsr::hard_mode::kStatRecalcPc) {
        hard_mode_scale_enemy();
    }
    seamless_on_entry(entry_pc);
    gsr::enemy_hp_bar::on_entry(entry_pc, enemy_hp_bar_name_width);
    if (entry_pc == GS_ROM(0x080030F8u)) {  // Func_30f8, the frame routine
        if (!g_menu_sounds.empty()) play_menu_sounds();
        question_hold_frame_entry();
    }
    if (entry_pc == GS_ROM(0x08010000u)) g_tile_ring_redraw.on_follow_entry();  // Func_10000
    golden_sun_note_projected_obj_placement(entry_pc);
    golden_sun_obj_f0_entry_capture(entry_pc);
    golden_sun_obj_staging_handoff(entry_pc);
    record_golden_sun_obj_boulder_calc_entry(entry_pc);
    // Costs one predictable branch when its own toggle is disabled.
    gsr::map_recorder_on_entry(entry_pc);
    gsr::map_recorder_on_function_args(entry_pc, g_cpu.R[0], g_cpu.R[1],
                                       g_cpu.R[2], g_cpu.R[3]);
    gsr::room_buffer_on_function_args(entry_pc, g_cpu.R[0], g_cpu.R[1],
                                      g_cpu.R[2]);
    gsr::room_buffer_on_entry(entry_pc);
    // Spell sparks, taken from the game's own stamp call rather than decoded
    // out of its memory (engine/src/field/effect_capture.h). One comparison
    // while off.
    if (gsr::effect_capture_enabled() && gsr::effect_capture_observes(entry_pc)) {
        if (auto* bus = gbarecomp::active_bus()) {
            gsr::effect_capture_on_entry(entry_pc, &g_cpu.R[0],
                bus->ewram_ptr(), bus->iwram_ptr(), bus->rom_ptr(),
                bus->rom_size(), runtime_current_frame());
        }
    }
}
}  // namespace gsr_runner
namespace {

bool strict_static_run() {
    static const bool strict = [] {
        return gbarecomp::env_flag("GBARECOMP_STRICT_STATIC");
    }();
    return strict;
}

std::vector<std::uint32_t> g_dynamic_ram_pcs;
unsigned long long g_dynamic_ram_dispatches = 0;

}  // namespace
namespace gsr_runner {
std::unordered_map<std::uint64_t, VerifiedRamCacheEntry>
    g_verified_ram_cache;
}  // namespace gsr_runner
namespace {

// Hash each reviewed image once until one of its pages changes, rather than
// once per PC inside that image. A fight visits many interior PCs, so a PC-only
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
}  // namespace
namespace gsr_runner {
unsigned long long g_verified_identity_hashes = 0;
unsigned long long g_verified_identity_cache_hits = 0;
unsigned long long g_verified_identity_content_hits = 0;
unsigned long long g_verified_identity_invalidations = 0;
}  // namespace gsr_runner
namespace {
std::array<unsigned long long, kTransientCodeImages.size()>
    g_verified_identity_hash_counts{};
}  // namespace
namespace gsr_runner {
// Stamp byte-scan path (try_stamp_dispatch): entries into the scan, and SHA-1
// computations made inside it. Reported at exit with the counters above.
unsigned long long g_stamp_identity_scans = 0;
unsigned long long g_stamp_identity_hashes = 0;
}  // namespace gsr_runner
namespace {

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
    if (fn) {
        g_runtime_image_base = placement.base - placement.variant->offset;
        g_ram_resolve_path = gsr::RamResolvePath::Stamp;
    }
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
    g_ram_resolve_path = gsr::RamResolvePath::Dynamic;
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
            // like the engine's [ram-compile] line, because the data has to come from
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
    if (g_dynamic_ram_pcs.empty()) gbarecomp::at_game_exit(report_dynamic_ram_summary);
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

}  // namespace
namespace gsr_runner {
void verified_ram_dispatch_return_hook(std::uint32_t return_pc,
                                       std::uint32_t call_stack_depth) {
    gsr::unpacker_guard_note_return(return_pc);
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
}  // namespace gsr_runner
namespace {

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

}  // namespace
namespace gsr_runner {
void verified_ram_dispatch_outer_boundary() {
    // The runner has regained control only after the current top-level guest
    // dispatch returned. This can be a scheduling yield/SWI rather than a
    // guest BX-LR, so no return hook is guaranteed to have fired. Nested
    // markers are retired by the call-return hook; clear the remaining
    // top-level markers here so the next outer resume is a fresh dispatch.
    if (runtime_call_stack_depth() == 0u)
        g_verified_ram_active_depth = 0;
}
}  // namespace gsr_runner
namespace {

RuntimeGuestFn verified_ram_dispatch_resolve(std::uint32_t pc, int thumb) {
    // A PC already classified as generated code short-circuits everything
    // below. This must come FIRST: the identity scan re-hashes the whole
    // containing candidate image (SHA-1 over ~1.2 KB) and then every
    // relocatable image at this base (~2.5 KB more) before it can conclude
    // what it already concluded the first time. Generated code is HOT — a
    // single fight dispatches these ~100,000 times — so paying that scan per
    // dispatch made it crawl.
    const std::uint64_t key = verified_ram_key(pc, thumb);

    if (dynamic_ram_pc_repeat(pc)) return dispatch_dynamic_ram(pc, thumb);

    VerifiedRamActiveGuard active_guard(key);

    if (active_guard.self_dispatch) {
        // A verified-RAM native entry called back into runtime_dispatch for
        // the exact same pc/thumb it is currently executing, before
        // returning. Left alone this recurses through
        // runtime_dispatch -> verified_ram_dispatch -> fn() without bound
        // and overflows the host stack.
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
                g_ram_resolve_path = gsr::RamResolvePath::CacheNative;
                return cached->second.fn;
            }
            // A fixed identity owns the ordinary static dispatch entry.
            g_ram_resolve_path = gsr::RamResolvePath::CacheStatic;
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
                    g_ram_resolve_path = gsr::RamResolvePath::CacheNative;
                    return cached->second.fn;
                }
                g_ram_resolve_path = gsr::RamResolvePath::CacheStatic;
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
    // Not a local array: the candidate set is ~100 images, and several KB per
    // recursively nested guest call exhausted the Windows host stack. The
    // runtime is single-threaded and a successful nested dispatch makes its
    // caller return immediately, so reusable thread-local storage is safe.
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
            g_ram_resolve_path = gsr::RamResolvePath::IdentityStatic;
            return nullptr;
        }
        if (void (*fn)(void) = lookup_variant(candidate, pc, thumb)) {
            if (cacheable)
                cache_verified(VerifiedRamAction::Native, fn, candidate_index);
            mark_active();
            g_ram_resolve_path = gsr::RamResolvePath::IdentityNative;
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

// The unpacker's own generated entry for pc (Func_2808 at 0x03006000), or
// null: what the guard's journal compares each answer with.
RuntimeGuestFn unpacker_own_entry(std::uint32_t pc) {
    static const TransientCodeImage* const image = [] {
        for (const auto& candidate : kTransientCodeImages)
            if (candidate.start == 0x03006000u && !candidate.thumb &&
                candidate.dispatch_table != nullptr)
                return &candidate;
        return static_cast<const TransientCodeImage*>(nullptr);
    }();
    return image ? lookup_variant(*image, pc, 0) : nullptr;
}

}  // namespace
namespace gsr_runner {
RuntimeGuestFn verified_ram_dispatch(std::uint32_t pc, int thumb) {
    if (thumb) return verified_ram_dispatch_resolve(pc, thumb);
    if (pc == 0x03006000u) gsr::unpacker_guard_note_entry(pc);
    if (!gsr::g_unpacker_guard_pending)
        return verified_ram_dispatch_resolve(pc, thumb);
    g_ram_resolve_path = gsr::RamResolvePath::None;
    const RuntimeGuestFn fn = verified_ram_dispatch_resolve(pc, thumb);
    if (gsr::unpacker_guard_note_dispatch(pc, fn, unpacker_own_entry(pc),
                                          g_ram_resolve_path))
        return &verified_ram_dispatch_noop;  // redone; g_cpu is at the caller
    return fn;
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
        "The Golden Sun ROM this build was generated from is required and\n"
        "SHA-1 gated. By default\n"
        "no BIOS is read: the runtime's own replacements run every BIOS service.\n"
        "--bios <path> (a canonical, SHA-1 gated GBA BIOS) runs the recompiled\n"
        "BIOS path instead, in builds that link it.\n"
        "\n"
        "Expected ROM SHA-1: %s (%s)\n",
        gs::game_rom_sha1(), gs::rom_version_name());
}
void reset_verified_identity_cache() { g_verified_identity_cache = {}; }
void reset_relocatable_identity_cache() { g_relocatable_identity_cache = {}; }
// Moved from main(): the per-image lines of the RAM cache report.
void print_ram_cache_image_detail() {
    for (std::size_t i = 0; i < kTransientCodeImages.size(); ++i) {
        if (g_verified_identity_hash_counts[i] == 0) continue;
        std::fprintf(stderr, "  ram_cache_image=%s hashes=%llu matched=%d known=%d\n",
            kTransientCodeImages[i].name,
            g_verified_identity_hash_counts[i],
            g_verified_identity_cache[i].matched ? 1 : 0,
            g_verified_identity_cache[i].known_snapshot ? 1 : 0);
    }
}
}  // namespace gsr_runner
namespace {

}  // namespace
