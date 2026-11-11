// Golden Sun: The Lost Age -- the New Game checklist, Challenge Mode and
// Random Psynergy (rules: shared_game/challenge_mode.h and
// shared_game/random_psynergy.h, one copy with The Broken Seal).
//
// Jimmy's design for this game (5 November 2026): the same checklist as The
// Broken Seal (Hard Mode, Challenge Mode, Random Psynergy, Begin); any of the
// eight can be picked and travels from Idejima; Challenge and Random
// Psynergy are fresh games only (not with data transferred from Golden Sun);
// picks start at Felix's level.
//
// Measured, verified ROMs (FACTS.md "Challenge Mode and Random Psynergy",
// 5 November 2026; English address, the others in tla_rom_addresses.inc):
//
// Checklist. Before the transfer message (the importer's measured
// 0x08038040, text 0x24), the same frame hold shows the checklist. Fresh-only
// choices answer No without drawing the question; ordinary choices keep it.
// Answers wait until the opening hook below before modifying the new party.
// The title overlay's new game (asset 1610, unpacked to
// 0x02008000) names the characters (0x02008580), asks the level questions
// only while 0x02003860 (Clear Data) is set, then at 0x020086A0 calls
// 0x080D2AA4(0, 8) (veneer 0x080C8268; sets the opening's room and
// entrance) and goes on to the game. On that entry the checklist opens and
// the game waits in its frame routine 0x08013560(1), as The Broken Seal's
// does, until Begin; then the call goes ahead with its own arguments.
// The hero builder 0x080AF7AC (new game 0x080ADDF0, before the transfer
// question) builds all eight: items, class, then 0x080AFBB4(id, table
// 0x080C0F4C + id*0xB4 + 0x96) from level 0 (call 0x080AF89A), class update
// 0x080B0298, stats 0x080AD3F8. Story levels: Felix, Jenna, Sheba 5, Piers
// 18, Isaac's party 28.
//
// Storage. The header builder 0x08043358 writes payload 0x02000000..0x27 and
// 0x2C..0x2F only (r7 = 0x01FFFFF0, stores up to +0x3C); 0x02000028..0x2B
// (Challenge) and 0x02000030..0x37 (Random Psynergy) are zero in every slot
// of every TLA save in the collection and in all 212 F12 memory captures.
// A transfer from Golden Sun sets story flag 0x2F (importer 0x080AE5C2).
//
// Challenge hooks. Join 0x080AFDD8 / leave 0x080AFE1C keep the order bytes
// 0x02000458 (as The Broken Seal's); party of eight. Battle lists the living
// in 0x0811A188 (HP +0x38 > 0); the per-target check 0x080B075C(?, id, ?,
// kind) returns 0 for a revive kind (0x080B0740: 5, 0x38, 0x39, 0x49) on a
// living member; Sanctum 0x0810A898(id, service 0 revives) returns 0 when not
// needed; the field ability routine 0x081007F8(ability, user, target, ?)
// leaves message 0xD (revive on the living) at [0x030000DC] + 0x25E and
// returns -1. Djinn grant 0x080B0AB8 hands (id, element, index) to 0x080B0B78
// (call 0x080B0B50), keeping the id in r9; counts at record +0x118.
// Djinn menu give check 0x0810498C(from slot, to slot), only caller
// 0x0810495E in the target list 0x08104928: totals from 0x081049C8 (menu
// [0x030000DC] +0x215 count, +0x204 u16 ids; bits 0..19 of +0xF8 | +0x108
// per element, as The Broken Seal), one moved, returns 1 when the
// receiver's new total is at most the giver's + 1 (only the two compared,
// unlike The Broken Seal's every pair). Byte-identical in all five ROMs
// (8 November 2026).
//
// Random Psynergy. The rebuild 0x080AF4E4(id) (only from the class update
// 0x080B0298) has The Broken Seal's rules: class table 0x080C15F4 + class *
// 0x54 + 0x10, 16 entries (ability u16, level u8, pad); item records
// 0x080B2364 (+0xC == 3, ability +0x28). Classes 0..243; types 1..14 are the
// characters' classes (154 abilities, the latest at 54), types 15..17 the
// item classes (left out). Class-taught field Psynergy: Growth 12, Frost 24,
// Douse 33, Whirlwind 78, 185, plus Ply 93..95 and Avoid 150. All tables are
// byte-identical in the five ROMs.
#include "tla_rom_version.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"
#include "../../shared_game/autosave.h"
#include "../../shared_game/challenge_mode.h"
#include "../../shared_game/random_psynergy.h"
#include "../../shared_game/random_weapons.h"
#include "../../shared_game/run_code.h"
#include "../../shared_game/code_box.h"
#include "../../shared_game/run_sheet.h"
#include "../../shared_game/new_game_flow.h"
#include "tla_treasure_randomizer.h"

namespace {

namespace ch = gsr::challenge;
namespace rp = gsr::random_psynergy;
namespace rw = gsr::random_weapons;

constexpr std::uint32_t kRecords = 0x02000520u, kRecordSize = 0x14Cu;
constexpr std::uint32_t kLevel = 0xFu;
constexpr std::uint32_t kMaxHp = 0x34u, kMaxPp = 0x36u, kHp = 0x38u, kPp = 0x3Au;
constexpr std::uint32_t kHpShare = 0x14u, kPpShare = 0x16u;
constexpr std::uint32_t kStoryFlags = 0x02000040u;
constexpr std::uint32_t kTransferFlag = 0x2Fu;
constexpr std::uint32_t kDifficulty = 0x020004C7u;
constexpr std::uint8_t kHard = 2u;
constexpr unsigned kFelix = 4, kJenna = 5, kSheba = 6;

std::uint32_t record(unsigned id) { return kRecords + id * kRecordSize; }
gba::GbaBus* bus() { return gbarecomp::active_bus(); }

bool story_flag(gba::GbaBus& b, std::uint32_t flag) {
    return (b.read8(kStoryFlags + (flag >> 3)) >> (flag & 7u)) & 1u;
}

// --- Random Psynergy: this game's pool ------------------------------------------
// The pool: every ability a character class (type 1..14) of the 244
// teaches, with the earliest level any of them teaches it (154 abilities,
// the latest at 54), read from the player's ROM (shared_game/
// random_psynergy.h pool()); FACTS "Challenge Mode and Random Psynergy".
// The class table is byte-identical in all five ROMs (Spain 0x080CA600,
// Italy 0x080C1600, France 0x080C65F4, Germany 0x080CE5F4): English 0x080C15F4.
constexpr int kClasses = 244;

// The class-taught field Psynergy that opens puzzles: held by Felix or Jenna,
// in the party from the first scene, and known from the start.
constexpr std::uint8_t kStarters = (1u << kFelix) | (1u << kJenna);
constexpr rp::Key kKeys[] = {
    {12, kStarters}, {24, kStarters}, {33, kStarters}, {78, kStarters}, {185, kStarters},
};

const rp::Game& rp_game() {
    // Ids Isaac..Mia 0..3, Felix 4, Jenna 5, Sheba 6, Piers 7; the builder's
    // story levels (hero table +0x96).
    static const rp::Game g{
        0x02000030u, kRecords, kRecordSize, TLA_ROM(0x080AF4E4u), TLA_ROM(0x080B2364u),
        {TLA_ROM(0x080C15F4u), kClasses, true},
        8, {28, 28, 28, 28, 5, 5, 5, 18},
        kKeys, static_cast<int>(std::size(kKeys)),
        kStarters,  // Felix and Jenna, there from the first scene
    };
    return g;
}

// --- Random Equipment (first Random Weapons): this game's places -----------------
// Measured, verified ROMs (FACTS.md "Random Weapons", 5 November 2026): item
// records 0x080B2364 + id * 0x2C (ids 0..460), 111 weapons (type 1),
// byte-identical in all five ROMs; the Rusty forge pieces 417..427 stay as
// they are. Descriptions text 0x92 + id in all five, item names 0x25F + id
// in all five (the spoiler sheet), unleash names 0x5A7 + id (0x774); armour
// types 2, 3, 4, 5, 9 (156 pieces, Defense +0xA; random_weapons_plan.h);
// font advances 0x0805F484 (Custom Battle's); stat rebuild
// 0x080AD3F8(id) (the hero builder's). The choice: payload 0x02000038..3F,
// outside the header builder's writes and zero in every slot of every TLA
// save and all 212 captures (random_weapons_format.h).
constexpr std::uint16_t kRusty[] = {417, 418, 419, 420, 421, 422, 423, 424, 425, 426, 427};

const rw::Game& rw_game() {
    static const rw::Game g{
        0x02000038u, TLA_ROM(0x080B2364u), 1u, 460u, kRusty,
        TLA_ROM(0x080A9F54u) - 0x08000000u, TLA_ROM(0x08060C30u) - 0x08000000u,
        TLA_TEXT(0x92u), TLA_TEXT(0x5A7u), TLA_TEXT(0x25Fu), TLA_ROM(0x0805F484u),
        kRecords, kRecordSize, TLA_ROM(0x080AD3F8u), 8,
    };
    return g;
}

// --- the onboarding (shared_game/new_game_flow.h) --------------------------------
// Jimmy, 7 November 2026. "Name the other characters?" on entry to the
// naming routine for the second hero; the rest where the checklist opened
// (before the Golden Sun transfer question, or at the opening).
// Challenge sizes (index + 1 heroes).
constexpr const char* kSizes[ch::kMaxPicks] = {"Solo", "Duo", "Triple", "No 2nd party"};
// The pick rows, Felix's party first.
constexpr unsigned kPickOrder[8] = {kFelix, kJenna, kSheba, 7, 0, 1, 2, 3};
constexpr const char* kHeroNames[8] = {"Isaac", "Garet", "Ivan", "Mia",
                                       "Felix", "Jenna", "Sheba", "Piers"};

// The run's F1 page (run_sheet.h): the code and the spoiler sheet.
const gsr::run_sheet::Game& sheet_game() {
    static const gsr::run_sheet::Game g{&rp_game(), &rw_game(), kHeroNames, &gsr::treasure::tla::sheet_lines};
    return g;
}

gsr::new_game::Flow g_flow;
bool g_transferred = false;       // data from Golden Sun: no randomizing, no challenge
unsigned long long g_row_since = 0;
std::uint16_t g_keys_down = 0;   // pressed now (KEYINPUT inverted)
std::uint16_t g_keys_held = 0;   // seen last frame
bool g_levels_pending = false, g_random_pending = false, g_weapons_pending = false;
bool g_skip_opening_pending = false;
// The questions open before the Golden Sun transfer question, after naming,
// so a fresh-only choice can refuse the import. Their answers wait here until
// the usual place (the hold below) applies them.
bool g_early = false, g_chosen = false;
gsr::new_game::Result g_chosen_result{};

// The naming loops (FACTS "New Game onboarding"): the title overlay names
// heroes with 0x080382F8(index) in two loops, Felix's party 4..r7-1 before
// the transfer question (0x0200847C; r7 5, becoming 8) and, without a
// transfer, Isaac's 0..r7-1 after it (0x02008578; r7 1, becoming 4). Both
// grow when the overlay halfword 0x0200A4F8 (Spain 0x0200A4EC,
// Italy/France/Germany 0x0200A4F0) is set, as the title's own button code
// does. Set when Felix is named; the question comes before Jenna; No
// answers every other hero at once, except Isaac, whom the game always names.
enum class Names { Unasked, Yes, No };
Names g_names = Names::Unasked;
bool g_names_open = false;

// A randomized Broken Seal run carried over by the import (Jimmy, 7 November
// 2026): its two blocks (payload +0x30 and +0x38) win over this game's choices.
std::array<std::uint8_t, 16> g_imported_run{};
bool g_imported_run_set = false;

// The game waits in its frame routine while a question is open.
enum class Hold { Off, Waiting, Resuming };
Hold g_hold = Hold::Off;
bool g_before_import = false, g_skip_import_question = false;
std::uint32_t g_hold_pc = 0;     // the held entry (naming, import question, opening)
std::uint32_t g_args[4] = {};
std::uint32_t g_return = 0;

std::string hero_name(unsigned id) {
    gba::GbaBus* b = bus();
    std::string name;
    for (std::uint32_t i = 0; b && i < 14u; ++i) {
        const std::uint8_t c = b->read8(record(id) + i);
        if (c == 0) break;
        if (c < 0x20u) { name.clear(); break; }
        name += static_cast<char>(c);
    }
    return name.empty() ? kHeroNames[id] : name;
}

gsr::new_game::Options flow_options() {
    gsr::new_game::Options o;
    o.items_and_coins = true;
    o.extras = true;
    o.extras_seamless = false;
    o.skip_label = "Skip the opening";
    o.skip_help = "Begin on the world map outside Idejima, with Jenna and Sheba, as the journey starts.";
    o.autosave = gbarecomp::game_option_get(gsr::autosave::kOptionKey, 0) != 0;
    o.transferred = g_transferred;
    o.challenge_sizes = 4;
    o.heroes.assign(std::begin(kPickOrder), std::end(kPickOrder));
    o.hero_name = hero_name;
    o.fresh_seed = [] { return rp::fresh_seed(); };
    o.text = [](const char* s) { return tla_menu_label(s); };
    o.ask_code = gsr::code_box::asker(o.text);
    return o;
}

void open_flow(bool names) {
    gba::GbaBus* b = bus();
    g_transferred = b && story_flag(*b, kTransferFlag);
    if (names) g_flow.open_names(flow_options());
    else g_flow.open(flow_options());
    g_names_open = names;
    g_row_since = runtime_current_frame();
    g_keys_held = 0x3FFu;  // the key that confirmed the name is still down
}

bool flow_open() { return g_flow.is_open(); }

// Scaled difficulty and the randomizers live in the run's blocks; Hard in
// the difficulty byte.
void apply_choices(const gsr::new_game::Result& r) {
    namespace ng = gsr::new_game;
    gba::GbaBus* b = bus();
    if (!b) return;
    const bool hard = r.difficulty == ng::kHard || r.difficulty == ng::kHardScaled;
    const bool scaled = r.difficulty == ng::kScaled || r.difficulty == ng::kHardScaled;
    // Hard Mode: the difficulty byte (tla_hard_mode.cpp). Easy (Clear Data)
    // stays Easy unless Hard is chosen.
    if (hard) b->write8(kDifficulty, kHard);
    else if (b->read8(kDifficulty) == kHard) b->write8(kDifficulty, 0u);
    gbarecomp::game_option_set(gsr::autosave::kOptionKey, r.autosave ? 1 : 0);
    // Always written, so a new game never keeps an earlier game's choice.
    ch::store(r.challenge);
    // One seed for the whole run (run_code.h), as in The Broken Seal: Weapons
    // and Armor change the finds; their stats shuffle only with Stats.
    const bool psynergy = r.random[ng::kPsynergy], items = r.random[ng::kItems], coins = r.random[ng::kCoins];
    const bool finds_weapons = r.random[ng::kWeapons], finds_armour = r.random[ng::kArmor];
    const bool weapons = finds_weapons && r.stats, armour = finds_armour && r.stats;
    const bool run = psynergy || items || coins || scaled || finds_weapons || finds_armour;
    rp::store(rp_game(), {run ? r.seed : 0u, psynergy, r.fair_on(ng::kFairPsynergy), items, items && r.bargains,
                          coins, scaled, finds_weapons, finds_armour, r.fair_on(ng::kFairItems),
                          r.fair_on(ng::kFairCoins), r.fair_on(ng::kFairShops)});
    rw::store(rw_game(), {weapons || armour ? r.seed : 0u, (weapons || armour) && r.scaling, armour, weapons});
    if (g_imported_run_set) {
        // A randomized Broken Seal run goes on as it was, with this game's
        // Scaled choice added.
        rp::Config ip = rp::decode(&g_imported_run[0]);
        const rw::Config iw = rw::decode(&g_imported_run[8]);
        if (ip.seed == 0) ip.seed = iw.seed;
        ip.scaled = ip.scaled || scaled;
        rp::store(rp_game(), ip);
        rw::store(rw_game(), iw);
        g_imported_run_set = false;
    }
    g_random_pending = rp::current(rp_game()).psynergy;
    g_weapons_pending = rw::current(rw_game()).seed != 0;
    g_levels_pending = r.challenge.size > 0;
    g_skip_opening_pending = r.skip_opening;
}

void finish_flow() {
    if (g_names_open) {
        g_names = g_flow.result().name_others ? Names::Yes : Names::No;
        return;
    }
    if (!g_early) return apply_choices(g_flow.result());
    g_chosen_result = g_flow.result();
    g_chosen = true;
    g_early = false;
}

// Once a held frame: the new presses go to the open question.
void flow_keys() {
    const std::uint16_t pressed = g_keys_down & static_cast<std::uint16_t>(~g_keys_held);
    g_keys_held = g_keys_down;
    if (!pressed) return;
    g_flow.keys(pressed);
    if (g_flow.moved()) g_row_since = runtime_current_frame();
    if (!g_flow.is_open()) finish_flow();
}

// Keys go to the questions only while one is open.
std::uint16_t (*g_next_keys)(std::uint16_t) = nullptr;
std::uint16_t keys(std::uint16_t value) {
    if (flow_open()) {
        g_keys_down = static_cast<std::uint16_t>(~value & 0x3FFu);
        value = 0x3FFu;
    }
    return g_next_keys ? g_next_keys(value) : value;
}

// --- after Begin -----------------------------------------------------------------
std::uint32_t call(std::uint32_t fn, std::uint32_t a0, std::uint32_t a1 = 0) {
    std::uint32_t result = 0;
    gbarecomp::guest_call(fn | 1u, a0, a1, 0, 0, &result);
    return result;
}

// An Isaac's-party pick (or Piers) starts with the starting gear of the
// Felix's-party member who wears the same kind (as The Broken Seal's Mia
// takes Ivan's: their own kit is far ahead of Felix's level). As the hero
// builder does: empty the 15 item slots +0xD8, then add 0x080AED6C(id, item)
// and equip 0x080AEF34(id, slot) each of the donor's hero-table items (+0x98,
// 13 halfwords). Only items the pick may wear (item +4, bit per character)
// and plain items (+2 type 0) are given; the equip routine refuses the rest.
void give_starting_gear(gba::GbaBus& b, unsigned id) {
    unsigned donor;
    switch (id) {
    case 0: case 1: case 7: donor = kFelix; break;
    case 2: donor = kSheba; break;
    case 3: donor = kJenna; break;
    default: return;
    }
    const std::uint32_t r = record(id);
    for (std::uint32_t slot = 0; slot < 15u; ++slot) b.write16(r + 0xD8u + slot * 2u, 0);
    const std::uint32_t items = TLA_ROM(0x080C0F4Cu) + donor * 0xB4u + 0x98u;
    for (std::uint32_t i = 0; i < 13u; ++i) {
        const std::uint32_t item = b.read16(items + i * 2u) & 0x1FFu;
        if (item == 0) continue;
        const std::uint32_t entry = TLA_ROM(0x080B2364u) + item * 0x2Cu;
        const std::uint8_t type = b.read8(entry + 2u);
        const bool wearable = type >= 1u && type <= 5u && (b.read16(entry + 4u) >> id & 1u);
        if (type != 0u && !wearable) continue;
        const std::uint32_t slot = call(TLA_ROM(0x080AED6Cu), id, item);
        if (wearable) call(TLA_ROM(0x080AEF34u), id, slot);
    }
}

// Picks at Felix's level (Jimmy, 5 November). Above it: the kept level-0
// record comes back (challenge_mode.h) and the builder's own raise runs;
// below it (Easy's Clear Data levels): the raise alone. Then the class update
// and stats, as the builder does, and full HP and PP.
void apply_levels() {
    if (!g_levels_pending) return;
    g_levels_pending = false;
    gba::GbaBus* b = bus();
    if (!b) return;
    const ch::Config c = ch::current();
    const unsigned target = b->read8(record(kFelix) + kLevel);
    const unsigned changed = ch::restore_start_records(c, kLevel, target);
    for (int i = 0; i < c.size; ++i) {
        const unsigned id = c.picks[i];
        const std::uint32_t r = record(id);
        const bool restored = changed & (1u << id);
        if (!restored && b->read8(r + kLevel) >= target) continue;
        if (restored) give_starting_gear(*b, id);
        call(TLA_ROM(0x080AFBB4u), id, target);
        call(TLA_ROM(0x080B0298u), id);
        call(TLA_ROM(0x080AD3F8u), id);
        b->write16(r + kHp, b->read16(r + kMaxHp));
        b->write16(r + kPp, b->read16(r + kMaxPp));
        b->write16(r + kHpShare, 0x4000u);
        b->write16(r + kPpShare, 0x4000u);
    }
}

// Skip the opening (Jimmy, 8 November 2026: start where his F12 0215 stands,
// on the world map just outside Idejima; FACTS "Skip the opening"). At the
// opening's room request 0x080D2AA4(0, 8), the new game already built and
// the choices applied, it leaves what Venus Lighthouse and Idejima would:
//  - Sheba joins through the game's join 0x080AFDD8 (Challenge Mode's rules
//    apply as when the story adds her); Felix and Jenna the new game joined;
//  - the story flags 0215 holds from 8 up are set; none is cleared, so a
//    transfer from Golden Sun keeps its flags;
//  - room 2 (the world map) entrance 1 is requested instead, and the room
//    history 0x02000424..0x0200042D is 0215's (Idejima, room 0xC, left).
// Levels, items, coins and the rest are the new game's own (0215's Felix,
// Jenna and Sheba are at the builder's level 5 with its items).
constexpr std::uint16_t kSkipOpeningFlags[] = {
    0x02E, 0x144, 0x169, 0x1F8, 0x644, 0x647, 0x800, 0x801, 0x804, 0x805,
    0x807, 0x808, 0x809, 0x80B, 0x80E, 0x80F, 0x810, 0x811, 0x812, 0x815,
    0x817, 0x819, 0x81A, 0x81B, 0x844, 0x863, 0x864, 0x865, 0x867, 0x890,
    0x891, 0x892, 0x893, 0x894, 0x895, 0x896,
};
constexpr std::uint32_t kRoom = 0x02000420u;
constexpr std::uint16_t kOpeningRoom = 0, kOpeningEntrance = 8;
constexpr std::uint16_t kSkipRoom = 2, kSkipEntrance = 1;
constexpr std::uint16_t kSkipRoomHistory[] = {0xC, 1, 2, 1, 2};  // 0x02000424..

void skip_opening(std::uint32_t args[4]) {
    if (!g_skip_opening_pending) return;
    g_skip_opening_pending = false;
    gba::GbaBus* b = bus();
    if (!b || args[0] != kOpeningRoom || args[1] != kOpeningEntrance) return;
    call(TLA_ROM(0x080AFDD8u), kSheba);
    for (const std::uint16_t flag : kSkipOpeningFlags) {
        const std::uint32_t at = kStoryFlags + (flag >> 3);
        b->write8(at, static_cast<std::uint8_t>(b->read8(at) | (1u << (flag & 7u))));
    }
    for (std::uint32_t i = 0; i < std::size(kSkipRoomHistory); ++i)
        b->write16(kRoom + 4u + i * 2u, kSkipRoomHistory[i]);
    args[0] = kSkipRoom;
    args[1] = kSkipEntrance;
}

// --- the hold ----------------------------------------------------------------------
// Entry to `pc` (the naming routine, the transfer question or 0x080D2AA4 from
// the title's new game): return at once into the frame routine, which comes
// back to its own entry once a frame until the question closes; then return
// into `pc` with its saved arguments and return address.
void resume_hold() {
    for (int i = 0; i < 4; ++i) g_cpu.R[i] = g_args[i];
    g_cpu.R[14] = g_return;
    g_hold = Hold::Off;
}

void start_hold(std::uint32_t pc) {
    for (int i = 0; i < 4; ++i) g_args[i] = g_cpu.R[i];
    g_return = g_cpu.R[14];
    g_hold_pc = pc;
    g_hold = Hold::Waiting;
    g_cpu.R[14] = TLA_ROM(0x08013560u) | 1u;
    gbarecomp::guest_return_now(pc, 1u);
}

void hold_entry(std::uint32_t pc) {
    if (g_hold == Hold::Resuming) return resume_hold();
    g_before_import = false;
    if (g_chosen) {
        // Answered early, at the import.
        g_chosen = false;
        apply_choices(g_chosen_result);
    } else {
        open_flow(false);
    }
    start_hold(pc);
}

// Entry to the naming routine from the title (EWRAM return): see Names.
bool naming_entry(std::uint32_t pc) {
    // The held call first: it comes back from the frame routine, so its LR is
    // that routine's leftover until resume_hold restores the saved one.
    if (g_hold == Hold::Resuming && g_hold_pc == pc) {
        resume_hold();
        if (g_names == Names::No) {
            gbarecomp::guest_return_now(pc, 0u);
            return true;
        }
        return false;
    }
    const std::uint32_t lr = g_cpu.R[14];
    if (lr < 0x02008000u || lr >= 0x0200C000u) return false;
    gba::GbaBus* b = bus();
    if (!b) return false;
    const std::uint32_t index = g_cpu.R[0];
    if (index == kFelix) {
        g_names = Names::Unasked;
        b->write16(TLA_OVL("title", 0x0200A4F8u), 1u);
        return false;
    }
    // Isaac is always named, as in the game (Jimmy, 10 November 2026).
    if (index == 0 || index > 7) return false;
    if (g_names == Names::No) {
        gbarecomp::guest_return_now(pc, 0u);
        return true;
    }
    if (g_names == Names::Unasked) {
        open_flow(true);
        start_hold(pc);
        return true;
    }
    return false;
}

void hold_frame_entry() {
    if (g_hold != Hold::Waiting) return;
    if (flow_open()) flow_keys();
    g_cpu.R[0] = 1u;
    if (flow_open()) {
        g_cpu.R[14] = TLA_ROM(0x08013560u) | 1u;
        return;
    }
    if (g_hold_pc != TLA_ROM(0x080D2AA4u)) {
        // Resume the naming or the transfer question with its original
        // arguments. Choices are only applied at the opening's usual hold,
        // after native setup finishes.
        g_cpu.R[14] = g_hold_pc | 1u;
        g_hold = Hold::Resuming;
        return;
    }
    apply_levels();
    // After the picks' levels: the plan's lists from the start.
    if (g_random_pending) rp::apply_new_game(rp_game());
    g_random_pending = false;
    // The new equipment, and the stats of the heroes wearing any.
    if (g_weapons_pending) rw::apply_new_game(rw_game(), kHp, kMaxHp, kPp, kMaxPp);
    g_weapons_pending = false;
    // Last: "Skip the opening", if chosen (Sheba joins, flags, room).
    skip_opening(g_args);
    g_cpu.R[0] = 1u;
    g_cpu.R[14] = TLA_ROM(0x080D2AA4u) | 1u;
    g_hold = Hold::Resuming;
}

// --- painting ----------------------------------------------------------------------
// A put(x, y, index) that only writes between x0 and x1 (the scrolling help).
std::function<void(int, int, std::uint8_t)> clipped(
    const std::function<void(int, int, std::uint8_t)>& put, int x0, int x1) {
    return [put, x0, x1](int x, int y, std::uint8_t v) {
        if (x >= x0 && x < x1) put(x, y, v);
    };
}

// The onboarding: the help bar (tile rows 0..2; a long line holds, then
// scrolls, The Broken Seal's timing), then the question's window down to the
// screen's bottom (tile rows 3..19; inner rows from y 32, bottom border at
// (17 - 1) * 8 - 8 = 120 below): title at 0 (a second line at 12), rows from
// 18 (30), 12 apart; eight rows and a gap end by 120.
void paint_flow(std::uint8_t* rgb, std::uint32_t w, std::uint32_t h) {
    namespace rc = gsr::run_code;
    const auto put = tla_menu_pixel_writer(rgb, w, h);
    constexpr int kTileX = 1, kTiles = 28;
    constexpr int kInnerX = (kTileX + 1) * 8, kInnerWidth = (kTiles - 2) * 8;
    constexpr unsigned long long kHoldFrames = 60, kStepFrames = 2;
    constexpr int kGap = 48;
    const unsigned long long now = runtime_current_frame();
    const unsigned long long since = now >= g_row_since ? now - g_row_since : 0;
    const gsr::new_game::View view = g_flow.view();
    const std::string help = view.entry ? tla_menu_label("A types, B erases, Start is OK.")
        : view.cursor < static_cast<int>(view.rows.size()) ? view.rows[static_cast<std::size_t>(view.cursor)].help
                                                         : std::string();
    tla_menu_paint_window(rgb, w, h, kTileX, 0, kTiles, 3);
    {
        const int tw = static_cast<int>(tla_menu_text_width(help));
        const int y = 8;
        if (tw <= kInnerWidth) {
            tla_menu_draw_text(help, kInnerX + (kInnerWidth - tw) / 2, y, put);
        } else {
            const int offset = since <= kHoldFrames ? 0
                : static_cast<int>(((since - kHoldFrames) / kStepFrames) %
                                   static_cast<unsigned long long>(tw + kGap));
            const auto inside = clipped(put, kInnerX, kInnerX + kInnerWidth);
            tla_menu_draw_text(help, kInnerX - offset, y, inside);
            tla_menu_draw_text(help, kInnerX - offset + tw + kGap, y, inside);
        }
    }
    constexpr int kTileY = 3, kWindowRows = 17;
    tla_menu_paint_window(rgb, w, h, kTileX, kTileY, kTiles, kWindowRows);
    const int top = (kTileY + 1) * 8;
    const auto centred = [&](const std::string& text, int y) {
        tla_menu_draw_text(text, kInnerX + (kInnerWidth - static_cast<int>(tla_menu_text_width(text))) / 2, y, put);
    };
    if (view.entry) {
        const auto& e = *view.entry;
        centred(tla_menu_label(e.invalid ? "Not a valid seed." : "Type a seed"), top);
        centred(e.shown(), top + 16);
        const int column_width = kInnerWidth / rc::kColumns;
        for (int row = 0; row < rc::kGridRows; ++row)
            for (int column = 0; column < rc::kColumns; ++column) {
                const int x = kInnerX + column * column_width + column_width / 2;
                const int y = top + 36 + row * 14;
                const std::string symbol(1, rc::kAlphabet[row * rc::kColumns + column]);
                if (e.row == row && e.column == column) tla_menu_draw_text(">", x - 8, y, put);
                tla_menu_draw_text(symbol, x - static_cast<int>(tla_menu_text_width(symbol)) / 2 + 1, y, put);
            }
        const int action_width = kInnerWidth / rc::kActions;
        for (int i = 0; i < rc::kActions; ++i) {
            const std::string label = tla_menu_label(rc::kActionLabels[i]);
            const int x = kInnerX + i * action_width + (action_width - static_cast<int>(tla_menu_text_width(label))) / 2;
            if (e.row == rc::kGridRows && e.column == i) tla_menu_draw_text(">", x - 8, top + 100, put);
            tla_menu_draw_text(label, x, top + 100, put);
        }
        return;
    }
    const auto checkbox = [&](int x, int y, bool ticked, bool locked) {
        const std::uint8_t ink = locked ? kTlaGlyphShadow : kTlaGlyphLetter;
        for (int i = 0; i < 7; ++i) {
            put(x + i + 1, y + 7, kTlaGlyphShadow);
            put(x + 7, y + i + 1, kTlaGlyphShadow);
        }
        for (int i = 0; i < 7; ++i) {
            put(x + i, y, ink);
            put(x + i, y + 6, ink);
            put(x, y + i, ink);
            put(x + 6, y + i, ink);
        }
        if (ticked)
            for (int dy = 2; dy < 5; ++dy)
                for (int dx = 2; dx < 5; ++dx) put(x + dx, y + dy, ink);
    };
    centred(view.title, top);
    int first = 18;
    if (!view.subtitle.empty()) {
        centred(view.subtitle, top + 12);
        first = 30;
    }
    constexpr int kPitch = 12, kGapY = 4, kLabelX = 8;
    for (int row = 0; row < static_cast<int>(view.rows.size()); ++row) {
        const auto& r = view.rows[static_cast<std::size_t>(row)];
        const int y = top + first + row * kPitch + (view.gap_before >= 0 && row >= view.gap_before ? kGapY : 0);
        if (row == view.cursor) tla_menu_draw_text(">", kInnerX + kLabelX, y, put);
        int x = kInnerX + kLabelX + 10;
        if (r.box >= 0) {
            checkbox(x, y, r.box == 1, r.locked);
            x += 12;
        }
        tla_menu_draw_text(r.label, x, y, put);
    }
}

// The Djinn grant's hand-over, after 0x080B0B50 (Thumb return).
const std::uint32_t kGrantHandover = TLA_ROM(0x080B0B78u);
const std::uint32_t kGrantHandoverReturn = TLA_ROM(0x080B0B54u);

}  // namespace

void tla_install_new_game(gbarecomp::RunOptions& options) {
    g_next_keys = options.keyinput_filter;
    options.keyinput_filter = keys;
    // Challenge Mode's places (challenge_mode.h): four payload bytes at
    // 0x02000028; joined bits 0x02000040; order 0x02000458; records; HP
    // +0x38, share +0x14 (the builder sets it to 0x4000); field [0x0300006C],
    // battle slot 0x03000024 (tla.h); eight heroes, a party of eight; revive
    // kind 0x49 too; picks in the first places (the first four fight).
    static const ch::Game challenge{
        0x02000028u, kStoryFlags, 0x02000458u, kRecords, kRecordSize, kHp, kHpShare,
        0x0300006Cu, kTlaBattleSlot, 8, 8, 0x49u, true,
    };
    ch::install(challenge);
    gsr::run_sheet::install(sheet_game());
}

bool tla_new_game_on_entry(std::uint32_t pc) {
    // Random Psynergy: the list rebuild 0x080AF4E4(id) is ours.
    if (rp::on_entry(rp_game(), pc, g_cpu.R[0])) {
        gbarecomp::guest_return_now(pc, 0u);
        return true;
    }
    const std::uint32_t lr = g_cpu.R[14] & ~1u;
    {
        // Random Treasure: gifts and big coin amounts.
        std::uint32_t r0 = g_cpu.R[0];
        if (gsr::treasure::tla::on_entry(pc, lr, g_cpu.R[9], &r0)) g_cpu.R[0] = r0;
    }
    if (pc == TLA_ROM(0x08013560u)) {
        hold_frame_entry();
        ch::frame();
        rw::frame(rw_game());
        gsr::treasure::tla::frame(rp::current(rp_game()));
        gsr::run_sheet::frame();
    } else if (pc == TLA_ROM(0x080D2AA4u) && (g_cpu.R[14] == TLA_OVL("title", 0x020086A5u) ||
                                              (g_hold == Hold::Resuming && g_hold_pc == pc))) {
        hold_entry(pc);
    } else if (pc == TLA_ROM(0x080382F8u)) {
        // The naming routine: "Name the other characters?" before the second.
        if (naming_entry(pc)) return true;
    } else if (pc == TLA_ROM(0x080AFBB4u) && lr == TLA_ROM(0x080AF89Eu)) {
        // New game: the builder raises a level-0 hero to its story level.
        ch::keep_start_record(g_cpu.R[0]);
    } else if (pc == TLA_ROM(0x0811A188u)) {
        // Battle lists the living (HP > 0) for commands and turns.
        ch::keep_down();
    } else if (pc == TLA_ROM(0x080B075Cu)) {
        // Battle: (?, character, ?, effect kind); 0 = no effect on them.
        if (ch::revive_kind(g_cpu.R[3]) && ch::excluded(g_cpu.R[1])) {
            gbarecomp::guest_return_now(pc, 0u);
            return true;
        }
    } else if (pc == TLA_ROM(0x0810A898u)) {
        // Sanctum: (character, service); service 0 revives. 0 = not needed.
        if (g_cpu.R[1] == 0u && ch::excluded(g_cpu.R[0])) {
            gbarecomp::guest_return_now(pc, 0u);
            return true;
        }
    } else if (pc == TLA_ROM(0x081007F8u)) {
        // Field Psynergy and items: (ability, user, target, ?). A revive on a
        // left-out hero answers as on a living one: message 0xD, -1.
        if (!ch::excluded(g_cpu.R[2])) return false;
        gba::GbaBus* b = bus();
        if (!b) return false;
        std::uint32_t ability = g_cpu.R[0] & 0x3FFFu;
        if (ability > 0x2DEu) ability = 0;  // the getter 0x080AF43C
        if (!ch::revive_kind(b->read8(TLA_ROM(0x080B7C14u) + ability * 12u + 3u))) return false;
        b->write16(b->read32(0x030000DCu) + 0x25Eu, 0xDu);
        gbarecomp::guest_return_now(pc, 0xFFFFFFFFu);
        return true;
    } else if (pc == TLA_ROM(0x0810498Cu)) {
        // Djinn menu give check (from slot, to slot): in a Challenge file a
        // left-out hero may hand over or take any number (Jimmy's rule for
        // The Broken Seal, 5 November); between picks the game's own rule.
        gba::GbaBus* b = bus();
        if (!b || ch::current().size == 0) return false;
        const std::uint32_t menu = b->read32(0x030000DCu);
        const std::uint32_t count = b->read8(menu + 0x215u);
        if (g_cpu.R[0] >= count || g_cpu.R[1] >= count) return false;
        const unsigned from = b->read16(menu + 0x204u + g_cpu.R[0] * 2u);
        const unsigned to = b->read16(menu + 0x204u + g_cpu.R[1] * 2u);
        if (!ch::excluded(from) && !ch::excluded(to)) return false;
        gbarecomp::guest_return_now(pc, 1u);
        return true;
    } else if (pc == kGrantHandover && lr == kGrantHandoverReturn) {
        // Djinn grant (character, element, index): a left-out member gets
        // none; a living pick instead (the grant keeps the id in r9).
        if (!ch::excluded(g_cpu.R[0])) return false;
        const int id = ch::djinn_recipient(g_cpu.R[1], 0x118u);
        if (id >= 0) g_cpu.R[0] = g_cpu.R[9] = static_cast<std::uint32_t>(id);
    }
    return false;
}

bool tla_new_game_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    if (!flow_open()) return false;
    paint_flow(rgb, width, height);
    return true;
}

void tla_new_game_reset() {
    g_flow = {};
    g_early = g_chosen = false;
    g_hold = Hold::Off;
    g_before_import = g_skip_import_question = false;
    g_levels_pending = g_random_pending = g_weapons_pending = false;
    g_skip_opening_pending = false;
    g_imported_run_set = false;
    g_names = Names::Unasked;
    rw::reset();
}

// Fresh-only choices (Jimmy, 5 November 2026): a randomizer or a challenge
// chosen here means no transfer from Golden Sun (a randomized Broken Seal run
// brings its own, 7 November).
bool tla_new_game_fresh_only() {
    if (!g_chosen) return false;
    return g_chosen_result.any_random() || g_chosen_result.challenge.size > 0;
}

// The import took a Broken Seal file: its run's blocks (payload +0x30..+0x3F).
void tla_new_game_imported_run(const std::uint8_t* bytes) {
    if (!bytes) return;
    const bool run = rp::decode(bytes).seed != 0 || rw::decode(bytes + 8).seed != 0;
    if (!run) return;
    std::copy_n(bytes, 16, g_imported_run.begin());
    g_imported_run_set = true;
}

// Called by the importer only for ordinary interactive New Game, never
// Seamless. These are its existing measured message/choice/close entries.
bool tla_new_game_import_question(std::uint32_t pc) {
    if (pc == TLA_ROM(0x08038040u) &&
        (g_cpu.R[0] == TLA_TEXT(0x24u) ||
         (g_before_import && g_hold == Hold::Resuming && g_hold_pc == pc))) {
        if (g_before_import && g_hold == Hold::Resuming) {
            resume_hold();
            g_before_import = false;
        } else if (!g_chosen) {
            open_flow(false);
            g_early = g_before_import = true;
            start_hold(pc);
            return true;
        }
        g_skip_import_question = tla_new_game_fresh_only();
        if (g_skip_import_question) {
            gbarecomp::guest_return_now(pc, 0u);
            return true;
        }
    } else if (g_skip_import_question && pc == TLA_ROM(0x08038378u)) {
        gbarecomp::guest_return_now(pc, 1u);  // the game's No path
        return true;
    } else if (g_skip_import_question && pc == TLA_ROM(0x08038140u)) {
        g_skip_import_question = false;
        gbarecomp::guest_return_now(pc, 0u);
        return true;
    }
    return false;
}

void tla_new_game_draw_page() { gsr::run_sheet::draw(); }

bool tla_random_weapons_text(std::uint32_t text_id, std::string& out) {
    return rw::line_text(rw_game(), text_id, out);
}

// A Challenge Mode file's four bytes (red on the file screens).
bool tla_scaled_difficulty() { return rp::current(rp_game()).scaled; }
void tla_set_scaled_difficulty(bool on) {
    rp::Config c = rp::current(rp_game());
    if (c.scaled == on) return;
    c.scaled = on;
    rp::store(rp_game(), c);
}

bool tla_challenge_bytes(const std::uint8_t* bytes) {
    return bytes && ch::decode(bytes).size > 0;
}
