// Golden Sun: The Lost Age -- Custom Battle Mode: the Battle Arena's
// receptionist lets the player choose how many monsters to fight, each one,
// and whether they match the party's strength. The rules are The Broken
// Seal's, one copy (../../shared_game/custom_battle.h); this file is
// The Lost Age's measured table and catalogue. Launcher switch, off by
// default.
//
// Evidence (ROM SHA-1 b500663220cb9bf56b9f8e8c0c544f1d6fa3a824; FACTS.md
// 2026-10-30 "Custom Battle Mode"): ROM reading and disassembly, the room
// code unpacked and relocated by running the game's own 0x080130CC on it.
// Not yet run in the game.
//
// The receptionist: room 312's code (file 0x6B5, at 0x02008000). Her talk
// handler 0x02008BB0 keeps text base 0x13EC in r6 (literal 0x02008D94):
// 0x13F1 "You do not have an opponent. Would you like to battle a monster?"
// set at 0x02008C02 (set text 0x080D3BE8), shown at 0x02008C0A (0x080D3C88),
// Yes/No 0x080D2840(4, 0) from 0x02008C12. Yes sets 0x173 and her line
// 0x13F3, No clears it and sets 0x13F2; both set it at 0x02008CB2. Her calls
// go through RAM and ROM veneers that keep lr.
//
// Her Yes/No: 0x080D2840 -> 0x08038378 -> 0x0804D9E0 -> menu loop
// 0x0804D16C: context [0x030000E8], cursor +0x8C, row +0x94 (0x0804D3E8).
// It waits 0x08013560(1) from 0x0804D1E4 and reads keys through r6 =
// 0x03001150: new presses +4 (A, B, Start), repeat +0xC (directions), so a
// blinded menu hears no direction either; sounds through 0x081C0010, select
// from 0x0804D26E. 0x08000478..0x08000487 are ROM zeros.
//
// The builder 0x08127CD4 (The Broken Seal's 0x080C1FFC with moved
// addresses): flag 0x173 -> selector 0x0812764C; list sp + 0x60 (pointer
// sp + 0x1C), levels word sp + 0x20; slots from 0x08127FCA (0x08016CA4,
// r0 = 0x80 first); stat adjustment 0x0812786C from 0x0812802E; small
// enemy 0x081280FC; enemy data 0x080AD140 (rows 0x080B9E7C, 0x4C bytes).
// Levels as 0x0812764C takes them: party count 0x080AD0F0 and IDs at bytes
// 0x02000458 (0x0811A038), battle context [0x03000024], adjustment
// 0x08016D5C(0x3F8).
//
// Formations 0x0812CE7C: 0x294 rows of 24 bytes, IDs - 8 as halfwords at
// +0..8, minimum counts +10..14, maximum +15..19 (0x08127D1A..0x08127DBA,
// 0x08127588).
//
// Artwork: 0x0811A4E0 loads enemy slot r0's artwork (ID halfword unit +
// 0x14A, skipped when byte +0x129 is set); sprite 0x081280BC (halfword at
// 0x08130D0C + ID * 8, rows up to 0x182), variant 0x081280D8 (byte +3 bits
// 1..4); 0x080227E0 loads it: records 0x08300000 + (sprite & 0xFFF) * 20,
// (asset, sprite) pairs 0x0802E91C, assets 0x08680000, the ARM unpacker,
// colour tables 0x080203A8; tag -> data 0x080228BC. Seven composites take
// the next sprite as their second half, built with tag + 0x2001
// (0x0811A5AA..0x0811A5DE, 0x0811B804..0x0811B8A8). Picture palette: the
// same 64 bytes as The Broken Seal's 0x0800779C sit at 0x08017B10
// (TODO-EVIDENCE: not traced to the battle's palette load).
//
// Names: text ID + 0x424 (the name block 0x42C "???", Djinni 0x42D..0x466;
// levels and Kraken's composite sprite agree: Vermin and Bat Lv 1, Kraken
// 0x15C sprite 0x277). Text tables 0x080A9F54 / 0x08060C30 (0x08038580,
// 0x0803843C: The Broken Seal's routines with moved literals).
//
// Which monsters a save offers. Encounter sets are numbered in story order:
// room rows 0x080EE6D4 (room, entrance, flag, zone 1 set, zone 2 set; read
// by 0x080C9E50) rise with the room numbers, and the late revisits of early
// rooms use late sets (rooms 79..81 sets 96, 97). World map rows
// (region, terrain, flag, set) at 0x080EEDBC, and 0x080EEF34 on the second
// world map (0x080C9F2C). A won fight records each enemy kind as flag
// 0x600 + ID (IDs below 0x200), not in Battle Mode (0x08128312..0x0812833A).
// So a monster is offered when the save has beaten it, or when its first
// set comes no later than the latest first set among the monsters it has
// beaten: never early, at worst one area late. Monsters in no set (bosses
// and other scripted fights) are on the Bosses page, offered once beaten.
//
// Clear Data offers every opponent, as in The Broken Seal. A save file is
// a verbatim image of 0x02000000: load 0x08015E44 and save 0x08042F10 copy
// 0x2FF0 bytes between sector +0x10 and r1 = 0x02000000 (callers
// 0x080437B0, 0x0804384A, 0x0804350A). The title's Clear Data test
// 0x080432A4 reads summary byte +0x31 (= sector +0x31, filled by
// 0x08016054), so the loaded save's Clear Data mark is byte 0x02000021.
// Jimmy's save: slot 2 has 1 there, slots 1 and 3 have 0.

#include "tla_rom_version.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../shared_game/custom_battle.h"
#include "../../shared_game/custom_battle_panel.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

namespace cb = gsr::custom_battle;

constexpr unsigned kComposites[] = {0x231u, 0x243u, 0x245u, 0x247u, 0x249u, 0x277u, 0x27Eu};
const std::uint32_t kFormations = TLA_ROM(0x0812CE7Cu);
constexpr unsigned kFormationCount = 0x294u;
const std::uint32_t kEncounterSets = TLA_ROM(0x080EDACCu);  // 28 bytes: formations +4, weights +20
const std::uint32_t kRoomSets = TLA_ROM(0x080EE6D4u);
const std::uint32_t kWorldMapSets[] = {TLA_ROM(0x080EEDBCu), TLA_ROM(0x080EEF34u)};
constexpr unsigned kDefeatedFlag = 0x600u, kDefeatedIds = 0x200u;
constexpr std::uint32_t kClearData = 0x02000021u;  // non-zero: Clear Data
// Not monsters: "???" (0x08), the Djinn (0x09..0x42), "?" (0x43, 0x74).
bool not_a_monster(unsigned id) { return id <= 0x43u || id == 0x74u; }

std::map<unsigned, unsigned> g_first_set;  // monster -> earliest set

unsigned rom_u8(cb::Rom rom, std::uint32_t at) {
    at -= 0x08000000u;
    return at < rom.size() ? rom[at] : 0u;
}
int rom_s16(cb::Rom rom, std::uint32_t at) {
    return static_cast<std::int16_t>(rom_u8(rom, at) | (rom_u8(rom, at + 1) << 8));
}

void catalogue(cb::Rom rom, std::vector<cb::Monster>* out) {
    std::map<unsigned, cb::Monster> found;
    for (unsigned f = 0; f < kFormationCount; ++f)
        for (const unsigned id : cb::formation_members(rom, f))
            if (!not_a_monster(id)) found[id].id = id;
    const auto place_set = [&](unsigned set) {
        if (!set) return;
        const std::uint32_t row = kEncounterSets + set * 28u;
        for (unsigned i = 0; i < 8; ++i) {
            const unsigned f = unsigned(rom_s16(rom, row + 4u + i * 2u)) & 0xFFFFu;
            if (!rom_u8(rom, row + 20u + i) || f >= kFormationCount) continue;
            for (const unsigned id : cb::formation_members(rom, f)) {
                auto it = found.find(id);
                if (it != found.end()) it->second.order = std::min(it->second.order, set);
            }
        }
    };
    for (std::uint32_t at = kRoomSets; rom_s16(rom, at) != -1; at += 8) {
        place_set(rom_u8(rom, at + 6));
        place_set(rom_u8(rom, at + 7));
    }
    for (const std::uint32_t table : kWorldMapSets)
        for (std::uint32_t at = table; rom_s16(rom, at) != -1; at += 8)
            if (rom_s16(rom, at + 6) > 0) place_set(unsigned(rom_s16(rom, at + 6)));
    g_first_set.clear();
    for (auto& [id, m] : found) {
        m.boss = m.order == ~0u;
        g_first_set[id] = m.order;
        out->push_back(m);
    }
}

bool beaten(unsigned id) {
    return id < kDefeatedIds && cb::read_flag(kDefeatedFlag + id);
}

unsigned reached() {
    if (gba::GbaBus* bus = gbarecomp::active_bus(); bus && bus->read8(kClearData) != 0u)
        return ~0u;
    unsigned latest = 0;
    for (const auto& [id, set] : g_first_set)
        if (set != ~0u && beaten(id)) latest = std::max(latest, set);
    return latest;
}

bool met(const cb::Monster& m) { return beaten(m.id); }

const cb::Game kGame = {
    .yes_no_pc = TLA_ROM(0x080D2840u),
    .her_yes_no_return = TLA_OVL("arena", 0x02008C16u),
    .her_line_return = TLA_OVL("arena", 0x02008CB6u),
    .yes_line = TLA_TEXT(0x13F3u),
    .no_line = TLA_TEXT(0x13F2u),
    .show_and_ask = TLA_OVL("arena", 0x02008C06u),
    .ask = TLA_OVL("arena", 0x02008C0Eu),
    .text_base_literal = TLA_OVL("arena", 0x02008D94u),
    .text_base = TLA_TEXT(0x13ECu),
    .question_text = TLA_TEXT(0x13F1u),
    .set_text_pc = TLA_ROM(0x080D3BE8u),
    .decode_start_pc = 0,  // whole lines through tla_custom_battle_text
    .menu_context_ptr = 0x030000E8u,
    .wait_frames_pc = TLA_ROM(0x08013560u),
    .menu_wait_return = TLA_ROM(0x0804D1E8u),
    .sound_pc = TLA_ROM(0x081C0010u),
    .menu_select_return = TLA_ROM(0x0804D272u),
    .keys_pressed = 0x03001154u,
    .keys_repeat = 0x0300115Cu,
    .menu_key_reg = 6u,
    .menu_keys_blind = TLA_ROM(0x08000478u),
    .menu_keys_live = 0x03001150u,
    .menu_hears_directions = false,
    .slot_getter_pc = TLA_ROM(0x08016CA4u),
    .clear_slots_return = TLA_ROM(0x08127FCEu),
    .stat_adjust_pc = TLA_ROM(0x0812786Cu),
    .stat_adjust_return = TLA_ROM(0x08128032u),
    .small_enemy_pc = TLA_ROM(0x081280FCu),
    .enemy_data_pc = TLA_ROM(0x080AD140u),
    .party_count_pc = TLA_ROM(0x080AD0F0u),
    .party_ids = 0x02000458u,
    .battle_context_ptr = 0x03000024u,
    .byte_flag_pc = TLA_ROM(0x08016D5Cu),
    // The builder's enemy loop fetches each new enemy at 08128020 (return
    // 08128024), before its test of the word at sp + 0x20.
    .scale_getter_return = TLA_ROM(0x08128024u),
    .scaled = [] { return tla_scaled_difficulty(); },
    // The room backdrop asset is cached at context +0x670 (081198C4).
    // The room selector can also return the 0x69 story variant (080CA4F4).
    .arena_table = TLA_ROM(0x080EF984u),
    .arena_offset = 0x670u,
    .arena_variant = 0x69u,
    .formations = kFormations,
    .formation_count = kFormationCount,
    .formation_row = 24u,
    .member_offset = 0u,
    .minimum_offset = 10u,
    .maximum_offset = 15u,
    .member_halfwords = true,
    .enemy_sprites = TLA_ROM(0x08130D0Cu),
    .last_sprite_row = 0x182u,
    .sprite_records = TLA_ROM(0x08300000u),
    .sprite_assets = TLA_ROM(0x0802E91Cu),
    .asset_table = TLA_ROM(0x08680000u),
    .colour_tables = TLA_ROM(0x080203A8u),
    .picture_palette = TLA_ROM(0x08017B10u),
    .composites = kComposites,
    .load_enemy_animation = TLA_ROM(0x0811A4E0u),
    .animation_lookup = TLA_ROM(0x080228BCu),
    .unit_enemy_id = 0x14Au,
    .unit_id_halfword = true,
    // The cartridge fills 16 MB; the ROM window reaches 32 MB.
    .max_rom_bytes = 32u * 1024u * 1024u,
    .text_table = TLA_ROM(0x080A9F54u) - 0x08000000u,
    .tree_table = TLA_ROM(0x08060C30u) - 0x08000000u,
    .glyph_widths = TLA_ROM(0x0805F484u),
    .enemy_name_text = TLA_TEXT(0x424u),
    .catalogue = catalogue,
    .reached = reached,
    .met = met,
};

}  // namespace

void tla_install_custom_battle(gbarecomp::RunOptions& options) {
    cb::install(kGame);
    options.rom_patch = cb::prepare_animation_assets;
}

void tla_custom_battle_on_entry(std::uint32_t pc) { cb::on_entry(pc); }

bool tla_custom_battle_text(std::uint32_t text_id, std::string& out) {
    return cb::line_text(text_id, out);
}

void tla_custom_battle_reset() { cb::reset(); }

bool tla_custom_battle_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    const cb::PanelView view = cb::panel_view();
    if (!view.open) return false;
    tla_menu_paint_window(rgb, width, height, cb::kPanelTileX, view.tile_y,
                          cb::kPanelTiles, cb::kPanelRows);
    // In the expanded view the console's picture sits inside the margins.
    int ox = 0, oy = 0;
    if (const gba::GbaPpu* ppu = gbarecomp::active_ppu()) {
        ox = static_cast<int>(ppu->view_extra_left());
        oy = static_cast<int>(ppu->view_extra_top());
    }
    auto pixel = [&](int x, int y) -> std::uint8_t* {
        x += ox;
        y += oy;
        if (x < 0 || y < 0 || x >= int(width) || y >= int(height)) return nullptr;
        return rgb + (std::size_t(y) * width + std::size_t(x)) * 3u;
    };
    auto draw_text = [&](const std::string& text, int x, int y) {
        tla_menu_paint_text(rgb, width, height, text, x, y);
    };
    auto text_width = [](const std::string& text) { return int(tla_menu_text_width(text)); };
    cb::paint_panel(view, draw_text, text_width, (cb::kPanelTileX + 1) * 8,
                    (view.tile_y + 1) * 8, pixel);
    return true;
}
