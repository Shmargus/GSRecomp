#include "custom_battle_tbs.h"

#include <algorithm>
#include <map>
#include <vector>

#include "../../shared_game/battle_sprite_memory.h"
#include "../../shared_game/custom_battle.h"
#include "random_psynergy_tbs.h"
#include "settings_page.h"
#include "gba_bus.h"
#include "game_options.h"
#include "../../shared_game/gs_text_encode.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "story_progress.h"
#include "rom_version.h"

namespace gsr::custom_battle::tbs {
namespace {

// Measured, verified ROM; FACTS.md "Custom Battle Mode" 2026-10-29 and
// docs/features/CUSTOM_BATTLE_RESEARCH.md.
//
// The receptionist (room code overlay rom_7fb4a8 at 0x02008000; her handler
// 0x02008B94 keeps text base 0x2930 in r6, literal 0x02008D48): text 0x2935
// "...Would you like to battle a monster?" through 0x08092B94 (next text) and
// 0x08092C40(8, 0) (show it), then Yes/No 0x08091C7C(0, 0) from 0x02008BEE,
// 0 = Yes. Both answers then set their line at 0x02008C76 (0x2937 "step into
// the circle", 0x2936 for No). Resume points 0x02008BE2 (after her first
// set-text call) and 0x02008BEA (after her show call).
//
// Each decode of a line starts with 0x08019BAC(state, text ID) (0x080180BE
// in the printing decoder); a pass can stop at the line's {1E} before its
// end, so our line restarts there rather than at its end.
//
// The Yes/No's menu loop 0x08028574 (via 0x08015390 -> 0x08028DF4): context
// [0x03001F38], cursor +0x8C; it waits 0x080030F8(1) from 0x080285E8 and
// reads new presses through r7 (0x03001C94) at once: A returns the cursor
// (sound 0x70 through 0x080F9010 from 0x08028674), B or Start return -1;
// repeated Left/Up or Right/Down (0x03001B04, a literal, not r7) move the
// cursor and play 0x6F. 0x080000B8 is a ROM word that reads 0.
//
// The builder 0x080C1FFC: slots cleared from 0x080C22DA (0x08077008), stat
// adjustment 0x080C1C54 from 0x080C2338; small enemy 0x080C23C0; enemy data
// 0x08077198. Levels as 0x080C1AFC takes them: party count 0x08077148, IDs
// at bytes 0x02000438.., battle context [0x03001E74], adjustment
// 0x080770E0(0x3F8).
//
// Formations 0x080C5C38: 0x17C rows of 16 bytes, IDs - 8 at +1..5, minimum
// counts +6..10, maximum +11..15.
//
// A monster's battle picture (bench, Bat 0x0C in Battle Mode, its unpacked
// data matched EWRAM 0x0201C000 byte for byte, 29 Oct): 0x080B6D60 takes
// 0x080C2384(id): halfword sprite at 0x080C7420 + id * 8 (row 0 for id >
// 0xAB); 0x080C23A0(id): colour variant (byte +3 >> 1) & 15. 0x08009270 ->
// 0x0800B6B8(slot, buffer, sprite, variant): the sprite's 20-byte record
// 0x08185024 + sprite * 20 (width +0, height +1); its asset from (asset,
// sprite) halfword pairs at 0x08012FA0; the asset (table 0x08320000)
// unpacked by 0x08005340 (0x08002544). With a variant, colours up to 0xDF go
// through 256-byte table 0x080092B8 + (variant - 1) * 256. A frame decodes
// row by row (0x08009BB8): 0 ends it, above 0xDF skips (byte - 0xDF) clear
// pixels, anything else is a colour of the OBJ palette 0x0800779C.
// 0x080B6D30 loads enemy slot r0's artwork (ID byte unit + 0x128); tag ->
// data 0x0800B798. 080B6DF6..080B6E08 loads a second sprite for 0x1DC and
// 0x1E3 (Kraken, Fusion Dragon).
constexpr unsigned kComposites[] = {0x1DCu, 0x1E3u};

// Enemy name: text ID + 0x287 (enemy initialization 08079478, FACTS.md).
// Rooms 0x0809D170: 8-byte rows (room, entrance, flag, first set) ended by
// room -1. Sets 0x0809C610: 28 bytes, eight formations at +4, eight weights
// at +20. A room fights only its first set: zone 1 is its F0 tiles, or every
// tile when the row's flag has bit 15; zone 2 (F1 tiles) occurs in no
// encounter room's map (0808BFAA, 0808ACE0; FACTS 2026-10-29).
const std::uint32_t kRoomSets = gs::rom(0x0809D170u) - mods::kRomBase;
const std::uint32_t kEncounterSets = gs::rom(0x0809C610u) - mods::kRomBase;
constexpr std::uint16_t kAllTilesBit = 0x8000u;
// World map: 0x0808ADF0 reads 8-byte rows (region, terrain, flag, set) at
// 0x0809D7A8, -1 ended. Where each region lies is not measured; sets are
// numbered in story order (room first sets rise from Sol Sanctum to Venus
// Lighthouse; the one live check, region 6 near Suhalla, uses sets 80/81
// between Lunpa's 79 and Suhalla's 82), so a region opens with the room set
// that follows its own: never before, at worst one area late.
const std::uint32_t kWorldMapSets = gs::rom(0x0809D7A8u) - mods::kRomBase;
// Djinn fights: 0x08091F14 -> 0x0808B074(n), (set, index) at 0x0809D8B0 + 4n;
// room code asks for n up to 66.
const std::uint32_t kDjinnFights = gs::rom(0x0809D8B0u) - mods::kRomBase;
constexpr unsigned kDjinnFightCount = 67u;
constexpr unsigned kFormationCount = 0x17Cu;
// The save's Clear Data flag (seamless_mode.h), saved and loaded with the
// 0x02000000 block.
constexpr std::uint32_t kClearFlag = 0x0200044Fu;
// The Bosses page: the Golden Sun Wiki's "Bosses in Golden Sun" list (Jimmy,
// 29 Oct), matched by name and fight to these IDs: Mystery Woman/Man, Bandit
// (Vault), Tret, Saturos (Mercury Lighthouse), Killer Ape, Hydros Statue,
// Manticore, Kraken, Toadonpa, Storm Lizard, Tempest Lizard, Saturos and
// Menardi (Venus Lighthouse), Fusion Dragon, Deadbeard.
constexpr unsigned kBossIds[] = {0x7D, 0x7A, 0x1D, 0x93, 0x7B, 0x4E, 0x83, 0x59,
                                 0x79, 0x8D, 0x91, 0x92, 0x7C, 0x7E, 0x94, 0x95};

unsigned rom_u8(Rom rom, std::uint32_t at) { return at < rom.size() ? rom[at] : 0u; }
int rom_s16(Rom rom, std::uint32_t at) {
    return static_cast<std::int16_t>(rom_u8(rom, at) | (rom_u8(rom, at + 1) << 8));
}

// Each room's earliest guide section (adventure_walkthrough_data.h).
unsigned room_section(unsigned room) {
    unsigned best = ~0u;
    for (const auto& scope : adventure::walkthrough::locations)
        if (scope.room == room) best = std::min(best, scope.section);
    return best;
}

// Room rows whose first set never fights: no bit 15, and no F0 tile in the
// room's map (original map decompression, every encounter room checked).
struct SilentRow { unsigned room, first; };
constexpr SilentRow kSilentRows[] = {{63, 39}, {91, 52}, {184, 95}};

// Fights room code asks for: 0x08091EB0(set, index) (veneer 0x0808A250) in
// each room overlay, constants from the call site; index -1 = computed there
// (Colosso), so every index. Room = the earliest-section room of the overlay.
// The Mercury Lighthouse Mimic chest (room 53, kind 0x13, flag EF0) asks
// for set 0x63 index 0 through 0x0808DB98; it is the only Mimic chest found.
// The world map's set 12/6 is Flint's battle lesson (rom_77a7c8 0x02009C8C,
// text 0xC67), guide section 3; room 0 stands for that section.
struct ScriptedFight { unsigned set; int index; unsigned room; unsigned section = 0; };
constexpr ScriptedFight kScriptedFights[] = {
    {12, 0, 6}, {12, 1, 6}, {12, 2, 3}, {12, 3, 3}, {12, 4, 5}, {12, 5, 21},
    {36, 0, 45}, {36, 1, 53}, {36, 2, 58},
    {53, 0, 68}, {53, 1, 68}, {53, 2, 80}, {53, 3, 80}, {53, 4, 89}, {53, 5, 89},
    {62, 0, 111}, {62, 1, 111}, {62, 2, 111}, {62, 3, 109},
    {72, -1, 143},
    {84, 0, 113}, {84, 1, 113}, {84, 2, 113}, {84, 3, 113}, {84, 4, 113}, {84, 5, 113},
    {98, 0, 187}, {98, 1, 187}, {98, 2, 160}, {98, 3, 160}, {98, 4, 160},
    {98, 5, 164}, {98, 6, 164}, {98, 7, 164},
    {99, 0, 53},
    {12, 6, 0, 3},
};

unsigned set_formation(Rom rom, unsigned set, unsigned index) {
    return static_cast<unsigned>(rom_s16(rom, kEncounterSets + set * 28u + 4u + index * 2u));
}
bool set_has(Rom rom, unsigned set, unsigned index) {
    return rom_u8(rom, kEncounterSets + set * 28u + 20u + index) != 0;
}

// Formation monsters but Djinn, ordered by the earliest guide section that
// meets them (rooms, world map, scripted fights).
void catalogue(Rom rom, std::vector<Monster>* out) {
    std::map<unsigned, Monster> found;
    for (unsigned f = 0; f < kFormationCount; ++f)
        for (const unsigned id : formation_members(rom, f)) found[id].id = id;
    for (unsigned n = 0; n < kDjinnFightCount; ++n) {
        const std::uint32_t at = kDjinnFights + n * 4u;
        const unsigned f = set_formation(rom, unsigned(rom_s16(rom, at)), unsigned(rom_s16(rom, at + 2)));
        if (f < kFormationCount)
            for (const unsigned id : formation_members(rom, f)) found.erase(id);
    }
    const auto place = [&](unsigned f, unsigned section) {
        if (f >= kFormationCount || section == ~0u) return;
        for (const unsigned id : formation_members(rom, f)) {
            auto it = found.find(id);
            if (it != found.end()) it->second.order = std::min(it->second.order, section);
        }
    };
    const auto place_set = [&](unsigned set, unsigned section) {
        for (unsigned i = 0; i < 8; ++i)
            if (set_has(rom, set, i)) place(set_formation(rom, set, i), section);
    };
    std::map<unsigned, unsigned> first_set_section;  // room first set -> section
    for (std::uint32_t at = kRoomSets; at + 8 <= rom.size() && rom_s16(rom, at) != -1; at += 8) {
        const unsigned room = static_cast<unsigned>(rom_s16(rom, at));
        const unsigned flag = rom_u8(rom, at + 4) | (rom_u8(rom, at + 5) << 8);
        const int first = rom_s16(rom, at + 6);
        if (first <= 0) continue;
        bool silent = false;
        for (const auto& row : kSilentRows)
            silent |= !(flag & kAllTilesBit) && row.room == room && row.first == unsigned(first);
        if (silent) continue;
        const unsigned section = room_section(room);
        place_set(unsigned(first), section);
        if (section != ~0u) {
            auto& s = first_set_section[unsigned(first)];
            s = s ? std::min(s, section) : section;
        }
    }
    for (std::uint32_t at = kWorldMapSets; at + 8 <= rom.size() && rom_s16(rom, at) != -1; at += 8) {
        const int set = rom_s16(rom, at + 6);
        const auto next = first_set_section.upper_bound(unsigned(set));
        if (set > 0 && next != first_set_section.end()) place_set(unsigned(set), next->second);
    }
    for (const auto& fight : kScriptedFights)
        for (unsigned i = 0; i < 8; ++i)
            if ((fight.index < 0 && set_has(rom, fight.set, i)) || int(i) == fight.index)
                place(set_formation(rom, fight.set, i),
                      fight.section ? fight.section : room_section(fight.room));
    for (auto& [id, m] : found) {
        m.boss = std::find(std::begin(kBossIds), std::end(kBossIds), id) != std::end(kBossIds);
        out->push_back(m);
    }
}

// The furthest guide section the save has reached: the last one with a
// finished step (the route's done and milestone rules, read from the
// loaded save). Clear Data reaches everything.
unsigned reached_section() {
    if (bus_read_u8(kClearFlag) == 1u) return ~0u;
    auto* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    using namespace adventure::walkthrough;
    using story_progress::evaluate;
    using story_progress::Truth;
    const unsigned char* ram = bus->ewram_ptr();
    unsigned reached = 0;
    for (const auto& section : sections)
        for (unsigned i = section.first; i < section.end; ++i)
            if (evaluate(entries[i].done, ram) == Truth::Yes ||
                evaluate(entries[i].milestone, ram) == Truth::Yes)
                reached = std::max(reached, section.id);
    return reached;
}

const Game kGame = {
    .yes_no_pc = gs::rom(0x08091C7Cu),
    .her_yes_no_return = gs::ovl("rom_7fb4a8", 0x02008BF2u),
    .her_line_return = gs::ovl("rom_7fb4a8", 0x02008C7Au),
    .yes_line = gs::text(0x2937u),
    .no_line = gs::text(0x2936u),
    .show_and_ask = gs::ovl("rom_7fb4a8", 0x02008BE2u),
    .ask = gs::ovl("rom_7fb4a8", 0x02008BEAu),
    .text_base_literal = gs::ovl("rom_7fb4a8", 0x02008D48u),
    .text_base = gs::text(0x2930u),
    .question_text = gs::text(0x2935u),
    .set_text_pc = gs::rom(0x08092B94u),
    .decode_start_pc = gs::rom(0x08019BACu),
    .menu_context_ptr = gs::ram(0x03001F38u),
    .wait_frames_pc = gs::rom(0x080030F8u),
    .menu_wait_return = gs::rom(0x080285ECu),
    .sound_pc = settings_page::kSoundPc,
    .menu_select_return = gs::rom(0x08028678u),
    .keys_pressed = gs::ram(0x03001C94u),
    .keys_repeat = gs::ram(0x03001B04u),
    .menu_key_reg = 7u,
    .menu_keys_blind = gs::rom(0x080000B8u),
    .menu_keys_live = gs::ram(0x03001C94u),
    .menu_hears_directions = true,
    .slot_getter_pc = gs::rom(0x08077008u),
    .clear_slots_return = gs::rom(0x080C22DEu),
    .stat_adjust_pc = gs::rom(0x080C1C54u),
    .stat_adjust_return = gs::rom(0x080C233Cu),
    .small_enemy_pc = gs::rom(0x080C23C0u),
    .enemy_data_pc = gs::rom(0x08077198u),
    .party_count_pc = gs::rom(0x08077148u),
    .party_ids = 0x02000438u,
    .battle_context_ptr = gs::ram(0x03001E74u),
    .byte_flag_pc = gs::rom(0x080770E0u),
    // The builder's enemy loop fetches each new enemy at 080C232C (return
    // 080C2330), before its test of the word at sp + 0x20.
    .scale_getter_return = gs::rom(0x080C2330u),
    .scaled = [] { return gsr::random_psynergy::tbs::current().scaled; },
    // The room backdrop asset is cached at context +0x648 (080B6498),
    // then passed to the graphics loader at 080B6680.
    .arena_table = gs::rom(0x0809E270u),
    .arena_offset = 0x648u,
    .arena_variant = 0,
    .formations = gs::rom(0x080C5C38u),
    .formation_count = kFormationCount,
    .formation_row = 16u,
    .member_offset = 1u,
    .minimum_offset = 6u,
    .maximum_offset = 11u,
    .member_halfwords = false,
    .enemy_sprites = gs::rom(0x080C7420u),
    .last_sprite_row = 0xABu,
    .sprite_records = gs::rom(0x08185024u),
    .sprite_assets = gs::rom(0x08012FA0u),
    .asset_table = gs::rom(0x08320000u),
    .colour_tables = gs::rom(0x080092B8u),
    .picture_palette = gs::rom(0x0800779Cu),
    .composites = kComposites,
    .load_enemy_animation = gs::rom(0x080B6D30u),
    .animation_lookup = gs::rom(0x0800B798u),
    .unit_enemy_id = 0x128u,
    .unit_id_halfword = false,
    .max_rom_bytes = mods::kMaxRomBytes,
    .text_table = gs::rom(0x080736B8u) - mods::kRomBase,
    .tree_table = gs::rom(0x0803842Cu) - mods::kRomBase,
    .glyph_widths = gs::rom(0x080370D4u),
    .enemy_name_text = gs::text(0x287u),
    .catalogue = catalogue,
    .reached = reached_section,
    .met = nullptr,
};

bool g_in_sprite_allocate = false;

}  // namespace

void install() { custom_battle::install(kGame); }

void on_entry(std::uint32_t pc) {
    if (busy() || g_in_sprite_allocate) return;
    const std::uint32_t lr = g_cpu.R[14] & ~1u;
    if (pc == GS_ROM(0x08003FA4u) && lr == GS_ROM(0x0800AFFCu) &&
        read_flag(kArenaFlag) && gbarecomp::game_option_get(kOptionKey, kOptionDefault) != 0) {
        const unsigned owner = g_cpu.R[0], bytes = g_cpu.R[1];
        if (!sprites::fighter_request(owner, bytes) || g_cpu.R[2] != 0u) return;
        // Preserve every successful console grant (including reuse). Only
        // an allocation failure needs extra storage; native free/reset still
        // owns the grant's lifetime, also after loading a saved battle.
        std::uint32_t result = 0;
        g_in_sprite_allocate = true;
        const bool returned = gbarecomp::guest_call(pc | 1u, owner, bytes, 0, 0, &result);
        g_in_sprite_allocate = false;
        if (!returned) return;
        const std::uint32_t entry = gs::ram(0x03001B10u) + owner * 4u;
        if (!sprites::live_grant(owner, bus_read_u16, gs::ram(0x03001B10u))) {
            const int offset = sprites::first_fit(owner, bytes, bus_read_u16, gs::ram(0x03001B10u));
            if (offset >= 0) {
                if (auto* bus = gbarecomp::active_bus()) bus->set_obj_extra(true);
                bus_write_u16(entry, std::uint16_t(bytes));
                bus_write_u16(entry + 2u, std::uint16_t(offset));
                result = unsigned(offset) / 32u;
            }
        } else if (sprites::extra_grant(owner, bus_read_u16, gs::ram(0x03001B10u))) {
            if (auto* bus = gbarecomp::active_bus()) bus->set_obj_extra(true);
        }
        gbarecomp::guest_return_now(pc, result);
        return;
    }
    if (pc == GS_ROM(0x08002DF0u) && lr == GS_ROM(0x0800B042u)) {
        // 0800AA0C has just written this record's tile at +8. Its source
        // record is sp+2C and its allocation owner is byte +1C.
        const auto record = bus_read_u32(g_cpu.R[13] + 0x2Cu);
        const auto owner = bus_read_u8(record + 0x1Cu);
        if (owner < sprites::kOwners && (bus_read_u16(record + 4u) & 0x2000u)) {
            if (sprites::extra_grant(owner, bus_read_u16, gs::ram(0x03001B10u))) {
                if (auto* bus = gbarecomp::active_bus()) bus->set_obj_extra(true);
                bus_write_u16(record + 8u, bus_read_u16(record + 8u) | sprites::kPageMarker);
            } else if (auto* bus = gbarecomp::active_bus(); bus && bus->obj_extra()) {
                // A resized/released sprite may move back to the first page.
                bus_write_u16(record + 8u, bus_read_u16(record + 8u) & ~sprites::kPageMarker);
            }
        }
    }
    custom_battle::on_entry(pc);
}

int sprite_tile(int, std::uint16_t attr0, std::uint16_t, std::uint16_t attr2) {
    const auto* bus = gbarecomp::active_bus();
    return bus && bus->obj_extra() ? sprites::tile(attr0, attr2) : -1;
}

void reset() {
    // A state can contain extended grants before this process has granted
    // any. The engine already saves/restores that page's pixels.
    if (auto* bus = gbarecomp::active_bus()) {
        for (unsigned owner = 0; owner < sprites::kOwners; ++owner) {
            if (sprites::extra_grant(owner, bus_read_u16, gs::ram(0x03001B10u))) {
                bus->set_obj_extra(true);
                break;
            }
        }
    }
    custom_battle::reset();
}

}  // namespace gsr::custom_battle::tbs
