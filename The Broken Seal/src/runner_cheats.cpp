// Cheat menu, new-game checklist, Hard Mode prompt.
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
#include "../../shared_game/run_code.h"
#include "../../shared_game/new_game_flow.h"
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

// ---- Cheat menu (src/cheat_menu.h) ------------------------------------------
// The original window remains for the new-game prompts; cheats live in F1.
bool g_cheat_menu_swallow = false;
}  // namespace
namespace gsr_runner {
std::uint16_t g_cheat_window_palette[16] = {};
bool g_cheat_window_palette_known = false;
}  // namespace gsr_runner
namespace {

// New Game onboarding (shared_game/new_game_flow.h; Jimmy, 7 November 2026):
// the cheat menu's window and key capture. "Name the other characters?" is
// asked on entry to the naming routine for the second hero; the rest
// (Difficulty, Randomize and its follow-ups, Challenge, the last choices) on
// entry to Func_77f40, as the checklist was. Each question sits under a bar
// whose text explains the highlighted row.
}  // namespace
namespace gsr_runner {
bool g_hard_prompt_open = false;
}  // namespace gsr_runner
namespace {
gsr::new_game::Flow g_flow;
std::uint16_t g_hard_prompt_held = 0;
bool g_skip_opening_pending = false;        // "Skip Prologue & Sol Sanctum" chosen
// Frame the highlighted row was entered: its help text waits, then scrolls.
unsigned long long g_check_row_since = 0;

// The naming loop (FACTS "New Game onboarding"): the title overlay names
// heroes 0..r5-1 with 0x08020BD8(index) (veneer 0x08015320); r5 becomes 4
// when the overlay halfword 0x020096B2 (Spain, Italy 0x020096A2) is set, as
// the title's own secret code does. Set when Isaac is named; the question
// comes before Garet; No answers Garet, Ivan and Mia at once (0: not back).
enum class Names { Unasked, Yes, No };
Names g_names = Names::Unasked;

bool seamless_mode_on() {
    return gbarecomp::game_option_get(gbarecomp::seamless::kOptionKey, 0) != 0;
}

// Challenge Mode files stay in The Broken Seal (Jimmy, 5 November 2026); a
// randomized run goes on to The Lost Age and keeps its run there (7
// November 2026).
bool file_goes_on_to_tla() { return gsr::challenge::current().size == 0; }

// A hero's name for the picks: the game's own record (Isaac carries the
// player's name), else the English name.
std::string challenge_hero_name(unsigned id) {
    std::string name;
    for (std::uint32_t i = 0; i < 14u; ++i) {
        const std::uint8_t c = bus_read_u8(gsr::challenge::record(id) + i);
        if (c == 0) break;
        if (c < 0x20u) { name.clear(); break; }
        name += static_cast<char>(c);
    }
    return name.empty() ? gsr::field_character::kHeroes[id].name : name;
}

gsr::new_game::Options flow_options() {
    gsr::new_game::Options o;
    o.items_and_coins = true;
    o.extras = true;
    o.seamless = seamless_mode_on();
    o.autosave = gbarecomp::game_option_get(gsr::autosave::kOptionKey, 0) != 0;
    o.challenge_sizes = 3;
    for (unsigned id = 0; id < static_cast<unsigned>(kChallengeHeroes); ++id) o.heroes.push_back(id);
    o.hero_name = challenge_hero_name;
    o.fresh_seed = [] { return gsr::random_psynergy::tbs::fresh_seed(); };
    o.text = [](const char* s) { return settings_text(s); };
    return o;
}

void prompt_open(bool names) {
    load_menu_font();
    if (names) g_flow.open_names(flow_options());
    else g_flow.open(flow_options());
    g_hard_prompt_open = true;
    g_hard_prompt_held = 0x3FFu;
    g_check_row_since = runtime_current_frame();
    if (!names) g_skip_opening_pending = false;
}

bool g_challenge_levels_pending = false;
bool g_random_psynergy_pending = false;
bool g_random_weapons_pending = false;

// The answers take effect. Always written, so a new game never keeps an
// earlier game's choice.
void prompt_finish() {
    namespace ng = gsr::new_game;
    namespace rp = gsr::random_psynergy::tbs;
    namespace rw = gsr::random_weapons::tbs;
    const ng::Result& r = g_flow.result();
    g_hard_prompt_open = false;
    g_cheat_menu_swallow = true;
    using namespace gsr::hard_mode;
    const bool hard = r.difficulty == ng::kHard || r.difficulty == ng::kHardScaled;
    const bool scaled = r.difficulty == ng::kScaled || r.difficulty == ng::kHardScaled;
    bus_write_u8(kSavedFlag, hard ? kHardModeValue : 0u);
    bus_write_u8(kAutoSleepFlag, hard ? kHardModeValue : 0u);
    gbarecomp::game_option_set(gbarecomp::seamless::kOptionKey, r.seamless ? 1 : 0);
    gbarecomp::game_option_set(gsr::autosave::kOptionKey, r.autosave ? 1 : 0);
    g_skip_opening_pending = r.skip_opening;
    gsr::challenge::store(r.challenge);
    // One seed for the whole run (run_code.h).
    const bool psynergy = r.random[ng::kPsynergy], items = r.random[ng::kItems], coins = r.random[ng::kCoins];
    // Weapons and Armor change the finds; their stats shuffle only with Stats.
    const bool weapons = r.random[ng::kWeapons], armour = r.random[ng::kArmor];
    const bool stat_weapons = weapons && r.stats, stat_armour = armour && r.stats;
    const bool run = psynergy || items || coins || scaled || weapons || armour;
    rp::store({run ? r.seed : 0u, psynergy, r.fair_play, items, items && r.bargains, coins, scaled, weapons,
               armour, items && r.key_items});
    g_random_psynergy_pending = psynergy;
    const bool stats = stat_weapons || stat_armour;
    rw::store({stats ? r.seed : 0u, stats && r.scaling, stat_armour, stat_weapons});
    g_random_weapons_pending = stats;
    g_challenge_levels_pending = r.challenge.size > 0;
}

// Picks start at level 1 (challenge_mode.h): their level-0 records come back
// and the builder's own raise 0x080792FC(character, 1) runs, then the stat
// rebuild 0x08077428, as the hero builder does; then full HP and PP (HP/PP
// +0x34/+0x36 to +0x38/+0x3A, shares +0x14/+0x16 = 0x4000). Run from a
// function entry, where calling the game is safe. (F12 0085: restoring the
// level-0 record alone left Ivan and Mia at level 0 with 0 max HP.)
void challenge_apply_levels() {
    if (!g_challenge_levels_pending) return;
    g_challenge_levels_pending = false;
    const unsigned changed =
        gsr::challenge::restore_start_records(gsr::challenge::current(), 0xFu);
    // Mia starts with Ivan's gear (Jimmy, 4 November: her level-10 kit is far
    // ahead at level 1). As the hero builder 0x08078EE8 does: clear the 15
    // item slots +0xD8, then add 0x08078588(id, item) and equip
    // 0x08078708(id, slot) each of hero table 0x080844EC + id*0xB4 + 0x98's
    // 13 items. Ivan: Wooden Stick, Travel Vest, Leather Armlet, Circlet,
    // Herb; each lists Mia (bit 3) in its equip mask (item +4).
    constexpr unsigned kIvan = 2, kMia = 3;
    if (changed & (1u << kMia)) {
        const std::uint32_t record = gsr::challenge::record(kMia);
        for (std::uint32_t slot = 0; slot < 15u; ++slot) bus_write_u16(record + 0xD8u + slot * 2u, 0);
        const std::uint32_t items = GS_ROM(0x080844ECu) + kIvan * 0xB4u + 0x98u;
        for (std::uint32_t i = 0; i < 13u; ++i) {
            const std::uint32_t item = bus_read_u16(items + i * 2u) & 0x1FFu;
            if (item == 0) continue;
            std::uint32_t slot = 0, result = 0;
            gbarecomp::guest_call(GS_ROM(0x08078588u) | 1u, kMia, item, 0, 0, &slot);
            gbarecomp::guest_call(GS_ROM(0x08078708u) | 1u, kMia, slot, 0, 0, &result);
        }
    }
    for (unsigned id = 0; id < 8u; ++id) {
        if (!(changed & (1u << id))) continue;
        std::uint32_t result = 0;
        gbarecomp::guest_call(GS_ROM(0x080792FCu) | 1u, id, 1u, 0, 0, &result);
        gbarecomp::guest_call(GS_ROM(0x08077428u) | 1u, id, 0, 0, 0, &result);
        const std::uint32_t record = gsr::challenge::record(id);
        bus_write_u16(record + 0x38u, bus_read_u16(record + 0x34u));
        bus_write_u16(record + 0x3Au, bus_read_u16(record + 0x36u));
        bus_write_u16(record + 0x14u, 0x4000u);
        bus_write_u16(record + 0x16u, 0x4000u);
    }
}

}  // namespace
namespace gsr_runner {
QuestionHold g_question_hold = QuestionHold::Off;
}  // namespace gsr_runner
namespace {
std::uint32_t g_question_args[4] = {};
std::uint32_t g_question_return = 0;
std::uint32_t g_question_pc = 0;  // the held entry, re-entered once answered

// Hold at the entry `pc`: return at once into the frame routine, which comes
// back once a frame until the question closes; then `pc` runs with its own
// arguments and return address (question_hold_frame_entry).
void hold_at(std::uint32_t pc) {
    for (int i = 0; i < 4; ++i) g_question_args[i] = g_cpu.R[i];
    g_question_return = g_cpu.R[14];
    g_question_pc = pc;
    g_question_hold = QuestionHold::Waiting;
    g_cpu.R[14] = GS_ROM(0x080030F8u) | 1u;
    gbarecomp::guest_return_now(pc, 1u);
}
bool resume_if_held(std::uint32_t pc) {
    if (g_question_hold != QuestionHold::Resuming || g_question_pc != pc) return false;
    for (int i = 0; i < 4; ++i) g_cpu.R[i] = g_question_args[i];
    g_cpu.R[14] = g_question_return;
    g_question_hold = QuestionHold::Off;
    return true;
}

}  // namespace
namespace gsr_runner {
void question_hold_new_game_entry() {
    if (resume_if_held(gsr::hard_mode::kNewGameEntryPc)) return;
    prompt_open(false);
    hold_at(gsr::hard_mode::kNewGameEntryPc);
}

// Entry to the naming routine 0x08020BD8(index) from the title code pack
// (EWRAM return): see Names above.
void question_hold_naming_entry() {
    const std::uint32_t pc = GS_ROM(0x08020BD8u);
    // The held call first: it comes back from the frame routine, so its LR is
    // that routine's leftover until the saved one returns (resume_if_held).
    if (resume_if_held(pc)) {
        if (g_names == Names::No) gbarecomp::guest_return_now(pc, 0u);
        return;
    }
    const std::uint32_t lr = g_cpu.R[14];
    if (lr < 0x02008000u || lr >= 0x0200A000u) return;
    const std::uint32_t index = g_cpu.R[0];
    if (index == 0) {
        g_names = Names::Unasked;
        bus_write_u16(gs::ovl("rom_7795e8", 0x020096B2u), 1u);
        return;
    }
    if (index > 3) return;
    if (g_names == Names::No) {
        gbarecomp::guest_return_now(pc, 0u);
        return;
    }
    if (g_names == Names::Unasked) {
        prompt_open(true);
        hold_at(pc);
    }
}
}  // namespace gsr_runner
namespace {

// Skip the opening (FACTS.md "Skip the opening", 4 November 2026). After
// Func_77f40 the new-game overlay rom_7795e8 (0x0200855E..0x02008576) sets
// room 0x02000400 = 8, entrance 0x02000402 = 0x14: the opening, then waits
// through 0x0809163C(0x1E) (veneer 0x0808A010, call 0x02008CA6). On entry to
// that wait the skip does what the opening would have left behind:
//  - the game's own time skip 0x08077F70 (clears 0x20/0x21, sets 0x901,
//    adult gear 0xF -> 0x10 and Psynergy for ids 0..2, +300 coins);
//  - Garet joins through the game's join 0x0807961C (Challenge Mode's
//    own join rule applies, as when the story adds him);
//  - story flags 8..0xEFF become those of a measured file standing on the
//    world map before Flint (Jimmy's file 4); joined bits 0..7 and the
//    0xF00 block (chest flags) are left as the new game made them;
//  - room/entrance history 0x02000400..0x0200040D from the same file:
//    room 2 (rom_77a7c8, the world map) entered by entrance 1 from Vale.
// Levels, items and the rest of the party are the new game's own.
constexpr std::uint16_t kSkipOpeningFlags[] = {
    0x100, 0x104, 0x106, 0x143, 0x144, 0x165, 0x169, 0x180, 0x182, 0x183,
    0x184, 0x235, 0x609, 0x60C, 0x60F, 0x800, 0x801, 0x802, 0x804, 0x808,
    0x809, 0x80A, 0x80B, 0x80C, 0x80D, 0x80E, 0x810, 0x811, 0x813, 0x814,
    0x815, 0x816, 0x817, 0x818, 0x819, 0x81A, 0x821, 0x823, 0x825, 0x826,
    0x827, 0x828, 0x829, 0x832, 0x836, 0x837, 0x838, 0x839, 0x83A, 0x83B,
    0x83C, 0x83D, 0x83E, 0x83F, 0x840, 0x841, 0x842, 0x85C, 0x879, 0x87B,
    0x87C, 0x87D, 0x87E, 0x87F, 0x896, 0x900, 0x901, 0x9F0,
};
constexpr std::uint16_t kSkipOpeningRooms[] = {2, 1, 9, 2, 2, 1, 2};

// Story flag `flag` (bank 0x02000040, bit per flag).
constexpr std::uint32_t kStoryFlags = 0x02000040u;
}  // namespace
namespace gsr_runner {
bool story_flag(std::uint32_t flag) {
    return (bus_read_u8(kStoryFlags + (flag >> 3)) >> (flag & 7u)) & 1u;
}

void skip_opening_apply() {
    constexpr std::uint32_t kFlags = kStoryFlags, kRoom = 0x02000400u;
    if (!g_skip_opening_pending) return;
    if (!story_flag(0x20u) ||
        bus_read_u16(kRoom) != 8u || bus_read_u16(kRoom + 2u) != 0x14u) return;
    g_skip_opening_pending = false;
    std::uint32_t result = 0;
    gbarecomp::guest_call(GS_ROM(0x08077F70u) | 1u, 0, 0, 0, 0, &result);
    gbarecomp::guest_call(GS_ROM(0x0807961Cu) | 1u, 1u, 0, 0, 0, &result);
    for (std::uint32_t flag = 8u; flag < 0xF00u; flag += 8u) bus_write_u8(kFlags + (flag >> 3), 0);
    for (const std::uint16_t flag : kSkipOpeningFlags) {
        const std::uint32_t at = kFlags + (flag >> 3);
        bus_write_u8(at, static_cast<std::uint8_t>(bus_read_u8(at) | (1u << (flag & 7u))));
    }
    for (std::uint32_t i = 0; i < std::size(kSkipOpeningRooms); ++i)
        bus_write_u16(kRoom + i * 2u, kSkipOpeningRooms[i]);
}

// Entry to the frame routine while held: wait one frame (r0 = 1), then come
// back here, or once answered go back into Func_77f40.
void question_hold_frame_entry() {
    if (g_question_hold != QuestionHold::Waiting) return;
    if (!g_hard_prompt_open && g_question_pc == gsr::hard_mode::kNewGameEntryPc) {
        challenge_apply_levels();
        // After the challenge's level-1 picks: the plan's lists from the start.
        if (g_random_psynergy_pending) gsr::random_psynergy::tbs::apply_new_game();
        g_random_psynergy_pending = false;
        // The new weapons, and the stats of the heroes holding one.
        if (g_random_weapons_pending) gsr::random_weapons::tbs::apply_new_game();
        g_random_weapons_pending = false;
    }
    g_cpu.R[0] = 1u;
    if (g_hard_prompt_open) {
        g_cpu.R[14] = GS_ROM(0x080030F8u) | 1u;
        return;
    }
    g_cpu.R[14] = g_question_pc | 1u;
    g_question_hold = QuestionHold::Resuming;
}

SeamlessPhase g_seamless_phase = SeamlessPhase::Off;
}  // namespace gsr_runner
namespace {
std::uint16_t g_seamless_slot = 0;

}  // namespace
namespace gsr_runner {
// Skip the credits into the game's own Clear Data save screen. It asks for
// a slot and confirms overwrites; only a successful save hands over to TLA.
void seamless_on_entry(std::uint32_t entry_pc) {
    using namespace gsr::seamless;
    if (g_seamless_phase == SeamlessPhase::Choosing &&
        entry_pc == kSaveEndPc &&
        (g_cpu.R[14] & ~1u) == kSaveReturnPc && g_cpu.R[8] == 0u) {
        // The game's slot is a row of the file box (save_files_tbs.h).
        g_seamless_slot = static_cast<std::uint16_t>(
            gsr::save_files::tbs::slot_file(bus_read_u16(kCurrentSlotAddr)));
        if (g_seamless_slot >= gbarecomp::seamless::kSlotCount) return;
        g_seamless_phase = SeamlessPhase::Leaving;
        gbarecomp::set_screen_veil(true);
        std::fprintf(stderr, "[seamless] Clear Data saved to chosen slot %u\n",
                     g_seamless_slot + 1u);
        return;
    }
    if (entry_pc != kRoomCodeLoadPc || g_seamless_phase != SeamlessPhase::Off ||
        bus_read_u16(kRoomAddr) != kCreditsRoom ||
        bus_read_u16(kEntranceAddr) != kCreditsEntrance || !seamless_mode_on() ||
        !file_goes_on_to_tla())
        return;
    g_seamless_phase = SeamlessPhase::Choosing;
    bus_write_u16(kRoomAddr, kSaveRoom);
    bus_write_u16(kEntranceAddr, kSaveEntrance);
    bus_write_u16(kNextRoomAddr, kSaveRoom);
    bus_write_u16(kNextEntranceAddr, kSaveEntrance);
}

// Once per game frame (the frame routine's wait): leave once saved and the
// screen is fully black.
void seamless_frame_end() {
    if (g_seamless_phase != SeamlessPhase::Leaving || !gbarecomp::screen_veil_covered())
        return;
    g_seamless_phase = SeamlessPhase::Off;
    gbarecomp::request_exit(gbarecomp::seamless::kExitCodeBase + g_seamless_slot);
}

// Developer sessions also list the developer-only cheats after these.
const gbarecomp::GameOptionSwitch kGameSwitches[] = {
    {gsr::seamless::kSwitchLabel, gsr::seamless::kSwitchCaption,
     gbarecomp::seamless::kOptionKey},
    {gsr::autosave::kSwitchLabel, gsr::autosave::kSwitchCaption,
     gsr::autosave::kOptionKey},
    {gsr::enemy_hp_bar::kSwitchLabel, gsr::enemy_hp_bar::kSwitchCaption,
     gsr::enemy_hp_bar::kOptionKey},
    {gsr::djinn_tracker::kSwitchLabel, gsr::djinn_tracker::kSwitchCaption,
     gsr::djinn_tracker::kOptionKey},
    {gsr::djinn_tracker::kShowAllLabel, gsr::djinn_tracker::kShowAllCaption,
     gsr::djinn_tracker::kShowAllKey},
    {gsr::treasure_tracker::kSwitchLabel, gsr::treasure_tracker::kSwitchCaption,
     gsr::treasure_tracker::kOptionKey},
    {gsr::auto_field_psynergy::kSwitchLabel, gsr::auto_field_psynergy::kSwitchCaption,
     gsr::auto_field_psynergy::kOptionKey},
    {gbarecomp::walk_through_walls::kSwitchLabel,
     gbarecomp::walk_through_walls::kSwitchCaption,
     gbarecomp::walk_through_walls::kOptionKey}};
const int kGameSwitchCount = static_cast<int>(std::size(kGameSwitches));
// Players see every switch except the last (walking through walls).
constexpr int kPlayerGameSwitchCount = static_cast<int>(std::size(kGameSwitches)) - 1;

std::uint16_t cheat_menu_keyinput_filter(std::uint16_t keyinput) {
    keyinput = gsr::radial_psynergy::keyinput(keyinput);
    keyinput = gsr::retreat_travel::keyinput(keyinput);
    keyinput = gsr::save_files::tbs::keyinput(keyinput);
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
        if (down) {
            // The settings screen's sounds (settings_page.h kSoundPc).
            const bool grid = g_flow.stage() == gsr::new_game::Stage::CodeEntry;
            const bool move = down & (kKeyUp | kKeyDown | (grid ? kKeyLeft | kKeyRight : 0));
            if (down & kKeyB) g_menu_sounds.push_back(gsr::custom_battle::kBackSound);
            else if (down & (kKeyA | kKeyStart)) g_menu_sounds.push_back(gsr::custom_battle::kSelectSound);
            else if (move) g_menu_sounds.push_back(gsr::custom_battle::kMoveSound);
            const bool names = g_flow.stage() == gsr::new_game::Stage::Names;
            g_flow.keys(down);
            if (g_flow.moved()) g_check_row_since = runtime_current_frame();
            if (!g_flow.is_open()) {
                if (names) {
                    g_names = g_flow.result().name_others ? Names::Yes : Names::No;
                    g_hard_prompt_open = false;
                    g_cheat_menu_swallow = true;
                } else {
                    prompt_finish();
                }
            }
        }
        return 0x3FFu;
    }
    if (g_cheat_menu_swallow) {
        if (pressed != 0) return 0x3FFu;
        g_cheat_menu_swallow = false;
    }
    return keyinput;
}
}  // namespace gsr_runner
namespace {

// The onboarding: a one-line bar across the top whose text explains the
// highlighted row (scrolling when it is wider than the bar), and below it the
// question's window, down to the screen's bottom (tile rows 3..19, inner
// height 16 * 8 - 8 = 120): title at 0 (a second line at 12), rows from 18
// (30 with the second line), 12 apart; eight rows and a gap end by 120. Both
// span tiles 1..28 so the longest label fits. The code's letter grid draws
// in the same window.
void paint_new_game_flow(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    using namespace gsr::cheat_menu;
    namespace rc = gsr::run_code;
    constexpr int kTileX = 1, kTiles = 28;
    constexpr int kInnerWidth = (kTiles - 2) * 8;
    // Scrolling: the start of the text shows for kHoldFrames, then it moves
    // one pixel every kStepFrames, coming round again after a kGap gap.
    // Starting values, not measured for reading comfort; Jimmy to judge.
    constexpr unsigned long long kHoldFrames = 60, kStepFrames = 2;
    constexpr int kGap = 48;
    const unsigned long long now = runtime_current_frame();
    const unsigned long long since = now >= g_check_row_since ? now - g_check_row_since : 0;
    const gsr::new_game::View view = g_flow.view();
    const std::string help = view.entry ? settings_text("A types, B erases, Start is OK.")
        : view.cursor < static_cast<int>(view.rows.size()) ? view.rows[static_cast<std::size_t>(view.cursor)].help
                                                         : std::string();

    menu_window_paint(rgb, width, height, kTileX, 0, kTiles, 3,
                      [&](auto& draw_text, auto& text_width, int inner_x,
                          int inner_y, auto&) {
        const int w = text_width(help);
        if (w <= kInnerWidth) {
            draw_text(help, inner_x + (kInnerWidth - w) / 2, inner_y);
            return;
        }
        const int offset = since <= kHoldFrames ? 0
            : static_cast<int>(((since - kHoldFrames) / kStepFrames) %
                               static_cast<unsigned long long>(w + kGap));
        const int left = inner_x, right = inner_x + kInnerWidth;
        draw_text(help, left - offset, inner_y, left, right);
        draw_text(help, left - offset + w + kGap, inner_y, left, right);
    });

    menu_window_paint(rgb, width, height, kTileX, kWindowTileY, kTiles, kWindowRows + 3,
                      [&](auto& draw_text, auto& text_width, int inner_x,
                          int inner_y, auto&) {
        const auto centred = [&](const std::string& text, int y) {
            draw_text(text, inner_x + (kInnerWidth - text_width(text)) / 2, inner_y + y);
        };
        if (view.entry) {
            // Title (or the refusal), the typed code, four rows of eight
            // symbols, the three actions; ">" marks the cursor.
            const auto& e = *view.entry;
            centred(settings_text(e.invalid ? "Not a valid code." : "Type a code"), 0);
            centred(e.shown(), 16);
            const int column_width = kInnerWidth / rc::kColumns;
            for (int row = 0; row < rc::kGridRows; ++row)
                for (int column = 0; column < rc::kColumns; ++column) {
                    const int x = inner_x + column * column_width + column_width / 2;
                    const int y = inner_y + 36 + row * 14;
                    const std::string symbol(1, rc::kAlphabet[row * rc::kColumns + column]);
                    if (e.row == row && e.column == column) draw_text(">", x - 8, y);
                    draw_text(symbol, x - text_width(symbol) / 2 + 1, y);
                }
            const int action_width = kInnerWidth / rc::kActions;
            for (int i = 0; i < rc::kActions; ++i) {
                const std::string label = settings_text(rc::kActionLabels[i]);
                const int x = inner_x + i * action_width + (action_width - text_width(label)) / 2;
                if (e.row == rc::kGridRows && e.column == i) draw_text(">", x - 8, inner_y + 100);
                draw_text(label, x, inner_y + 100);
            }
            return;
        }
        const auto paint = window_pixel_painter(rgb, width, height);
        // A 7x7 box in the letters' colour with their shadow below-right;
        // ticked, a 3x3 block fills its middle; locked, in the shadow colour.
        const auto checkbox = [&](int x, int y, bool ticked, bool locked) {
            using gsr::settings_page::kGlyphLetter;
            using gsr::settings_page::kGlyphShadow;
            const auto ink = locked ? kGlyphShadow : kGlyphLetter;
            for (int i = 0; i < 7; ++i) {
                paint(x + i + 1, y + 7, kGlyphShadow);
                paint(x + 7, y + i + 1, kGlyphShadow);
            }
            for (int i = 0; i < 7; ++i) {
                paint(x + i, y, ink);
                paint(x + i, y + 6, ink);
                paint(x, y + i, ink);
                paint(x + 6, y + i, ink);
            }
            if (ticked)
                for (int dy = 2; dy < 5; ++dy)
                    for (int dx = 2; dx < 5; ++dx) paint(x + dx, y + dy, ink);
        };
        centred(view.title, 0);
        int first = 18;
        if (!view.subtitle.empty()) {
            centred(view.subtitle, 12);
            first = 30;
        }
        constexpr int kPitch = 12, kGapY = 4;
        for (int row = 0; row < static_cast<int>(view.rows.size()); ++row) {
            const auto& r = view.rows[static_cast<std::size_t>(row)];
            const int y = inner_y + first + row * kPitch + (view.gap_before >= 0 && row >= view.gap_before ? kGapY : 0);
            if (row == view.cursor) draw_text(">", inner_x + kLabelX, y);
            int x = inner_x + kLabelX + 10;
            if (r.box >= 0) {
                checkbox(x, y, r.box == 1, r.locked);
                x += 12;
            }
            draw_text(r.label, x, y);
        }
    });
}

}  // namespace
namespace gsr_runner {
void cheat_menu_paint(std::uint8_t* rgb, std::uint32_t width,
                      std::uint32_t height) {
    if (g_hard_prompt_open) paint_new_game_flow(rgb, width, height);
}

// Custom Battle's opponent panel (custom_battle.h): the number of opponents,
// or the monster list with the highlighted one's battle picture.
void paint_custom_battle_panel(std::uint8_t* rgb, std::uint32_t width,
                               std::uint32_t height,
                               const gsr::custom_battle::PanelView& view) {
    namespace cb = gsr::custom_battle;
    menu_window_paint(rgb, width, height, cb::kPanelTileX, view.tile_y,
                      cb::kPanelTiles, cb::kPanelRows,
                      [&](auto& draw_text, auto& text_width, int inner_x,
                          int inner_y, auto& pixel) {
        cb::paint_panel(view, draw_text, text_width, inner_x, inner_y, pixel);
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
    int step = gbarecomp::game_option_get(
        pc == kExpStorePc ? kExpKey : kCoinKey, kNormalStep);
    if (step < 0 || step >= kStepCount) step = kNormalStep;
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
    int step = gbarecomp::game_option_get(kDropKey, kNormalStep);
    if (step < 0 || step >= kDropStepCount) step = kNormalStep;
    if (step == kNormalStep) return 0;
    const std::int64_t threshold =
        static_cast<std::int64_t>(static_cast<std::int32_t>(g_cpu.R[5])) *
        kStepHalves[step] / 2;
    const std::int64_t roll = static_cast<std::int32_t>(g_cpu.R[0]);
    *out_decision = threshold <= roll ? 1u : 0u;
    return *out_decision != original ? 1 : 0;
}

// F12 0058 frame 91: the casting canvas is already visible at (0, 0), but
// its first position (-102, -13) is still queued for VBlank. The arena's
// line table has stopped, so spell_setup_torn_frame no longer catches this
// incomplete setup picture. Keep the preceding picture until placement lands.
bool battle_spell_position_pending(const gba::GbaPpu& ppu) {
    using namespace gsr::battle_speed;
    if (!ppu.latched_native_line_io_valid()[0]) return false;
    const auto* row = ppu.latched_native_line_io();
    const auto half = [&](unsigned offset) {
        return unsigned(row[offset]) | (unsigned(row[offset + 1u]) << 8);
    };
    if ((half(0) & 7u) != 1u || (half(0) & 0x0400u) == 0u ||
        (half(0x0Cu) & 0x2000u) != 0u) return false;
    const auto* refs = ppu.latched_native_affine_line_refs();
    if (refs[0] != 0 || refs[1] != 0) return false;
    const auto count = std::min(bus_read_u16(kVBlankQueueCount),
                                static_cast<std::uint16_t>(kVBlankQueueMax));
    for (unsigned i = 0; i < count; ++i) {
        const auto entry = kVBlankQueueEntries + i * kVBlankQueueEntrySize;
        // The game's queued DMA copy of the two BG2 reference words.
        if (bus_read_u32(entry + 4u) != 0x04000028u ||
            bus_read_u32(entry + 8u) != 0x84000002u) continue;
        const auto source = bus_read_u32(entry);
        if (!((source >= 0x02000000u && source <= 0x0203FFF8u) ||
              (source >= 0x03000000u && source <= 0x03007FF8u))) continue;
        if (bus_read_u32(source) != 0u || bus_read_u32(source + 4u) != 0u)
            return true;
    }
    return false;
}
}  // namespace gsr_runner
namespace {

}  // namespace
