#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "custom_battle_option.h"

// Custom Battle Mode, both games: Battle Mode's receptionist lets the player
// choose how many monsters to fight, each one, and whether they match the
// party's strength. Off by default (launcher switch, custom_battle_option.h).
// The game's own dialogue window, Yes/No menu and battle setup do the work;
// the runner asks the extra questions, shows the opponent panel (number, then
// each monster from a list with its battle picture, plus an Arena tab that
// picks the battle backdrop) and hands the game the chosen list and arena.
//
// One copy of the rules; each game installs its measured addresses (Game)
// and its own catalogue: custom_battle_tbs.cpp (FACTS.md "Custom Battle
// Mode" 2026-10-29, docs/features/CUSTOM_BATTLE_RESEARCH.md) and The Lost
// Age's src/tla_custom_battle.cpp (its FACTS.md 2026-10-30).
//
// How the receptionist is driven (measured in both games; the TLA handler is
// the same routine with moved addresses): her talk handler asks "...Would you
// like to battle a monster?" with a set-text call, a show call and Yes/No;
// Yes sets the arena flag 0x173, No clears it, and both set their own line
// with one set-text call and show it. Her script waits frames by swapping its
// stack out, so host code cannot hold her dialogue open; each extra question
// instead takes that last line's set-text call and sends her script back to
// her own steps: `show_and_ask` (after her first set-text call) shows the set
// line and asks Yes/No, `ask` (after her show call) only asks. Both are call
// returns, resumable entries of her code. Skipping her Yes path's own steps
// instead left show without a window (TBS bench, 29 Oct).
//
// The opponent panel: while it is open the Yes/No menu under it stays up,
// blind to A, B and Start; the panel takes the keys, and closes the menu with
// Yes (one picked) or No (finish choosing). Her menu loop's frame wait keeps
// a key pointer in a register (`menu_key_reg`); the panel points it at a ROM
// zero block while open, then back at the live keys with A pressed and the
// menu cursor set to the answer, the menu's own select sound left out.
//
// Battle Mode's fight: with flag 0x173 set the builder makes a 0-ended
// halfword enemy list at its sp + 0x60 (pointer at sp + 0x1C), then gets
// enemy slots 0x80.. (the first from `clear_slots_return`) and creates one
// enemy per entry, at most six. When the word at sp + 0x20 is nonzero it
// raises each enemy by that many levels (`stat_adjust_pc`); Battle Mode's
// word is target level - formation level. Five kinds at most, and a space of
// 6 with each enemy costing 2 - (small_enemy(id) != 0).
namespace gsr::custom_battle {

using Rom = std::span<const std::uint8_t>;

// Keys, active high, both games.
constexpr std::uint32_t kKeyA = 0x001u, kKeyB = 0x002u, kKeyStart = 0x008u;
constexpr std::uint32_t kKeyRight = 0x010u, kKeyLeft = 0x020u;
constexpr std::uint32_t kKeyUp = 0x040u, kKeyDown = 0x080u;
constexpr std::uint32_t kKeyR = 0x100u, kKeyL = 0x200u;
// Her menu's sounds, both games: 0x6F move, 0x70 select, 0x71 B or Start.
constexpr std::uint32_t kMoveSound = 0x6Fu, kSelectSound = 0x70u, kBackSound = 0x71u;
constexpr std::uint32_t kArenaFlag = 0x173u;
constexpr std::uint32_t kFlags = 0x02000040u;  // flag n: bit n & 7 of byte n >> 3
constexpr std::uint32_t kFirstEnemySlot = 0x80u;
constexpr std::uint32_t kListPointerOffset = 0x1Cu, kListOffset = 0x60u;
constexpr std::uint32_t kLevelsOffset = 0x20u;
constexpr unsigned kMaxEnemies = 6u, kMaxKinds = 5u, kSpace = 6u;
constexpr std::uint32_t kLevelOffset = 0xFu;  // in unit and enemy data
constexpr std::uint32_t kThreeMemberOffset = 0x44u;  // battle context byte
constexpr std::uint32_t kLevelAdjustByte = 0x3F8u;
constexpr std::uint32_t kLoadedOffset = 0x129u;  // unit byte: artwork kept

// The opponent panel. The Yes/No sits at the top or the bottom (her text box
// on the other side), its tile row the halfword at [menu context] + 0x94, so
// the panel takes 15 rows on the Yes/No's side. Native pixels, inside a
// window of the game's pieces drawn by the runner.
constexpr std::uint32_t kMenuCursorOffset = 0x8Cu, kMenuRowOffset = 0x94u;
constexpr int kScreenTileRows = 20;
constexpr int kPanelTileX = 1;
constexpr int kPanelTiles = 28, kPanelRows = 15;
constexpr int kPanelVisibleRows = 6;

// A monster Battle Mode can offer. The game's catalogue fills id, order (its
// story order; lower comes first) and boss; the core fills the rest.
struct Monster {
    unsigned id = 0;
    std::string name;
    unsigned cost = 2;       // space, kSpace in all
    unsigned order = ~0u;
    unsigned level = 0;
    bool boss = false;       // on the Bosses page
};

struct Game {
    // Her script: the Yes/No and its return in her code, her last line's
    // set-text call return and its Yes/No texts, her resume points, and the
    // literal holding her text base (her code, not another room's).
    std::uint32_t yes_no_pc, her_yes_no_return, her_line_return;
    std::uint32_t yes_line, no_line, show_and_ask, ask;
    std::uint32_t text_base_literal, text_base, question_text, set_text_pc;
    // 0, or the decoder's line start (r1 = text ID) for text_character.
    std::uint32_t decode_start_pc;
    // Her Yes/No's menu loop.
    std::uint32_t menu_context_ptr, wait_frames_pc, menu_wait_return;
    std::uint32_t sound_pc, menu_select_return;
    std::uint32_t keys_pressed, keys_repeat;
    unsigned menu_key_reg;
    std::uint32_t menu_keys_blind, menu_keys_live;
    bool menu_hears_directions;  // else the panel plays their move sound
    // Battle Mode's builder and the routines it uses.
    std::uint32_t slot_getter_pc, clear_slots_return;
    std::uint32_t stat_adjust_pc, stat_adjust_return;
    std::uint32_t small_enemy_pc, enemy_data_pc;
    std::uint32_t party_count_pc, party_ids, battle_context_ptr, byte_flag_pc;
    // Arena tab. arena_table: the game's room-to-backdrop table, u32 entries
    // ending at 0; an entry with bit 31 set names a backdrop asset in its low
    // 16 bits. arena_offset: where the battle context caches the room's
    // backdrop asset (u16) before the graphics loader reads it; a chosen
    // arena is written there. arena_variant: a backdrop the game can pick
    // that the table does not list (0 for none).
    std::uint32_t arena_table;
    unsigned arena_offset, arena_variant;
    // Formations: rows of `formation_row` bytes, five members (ID - 8) as
    // bytes or halfwords, minimum counts (all zero: Battle Mode refuses the
    // row) and maximum counts as bytes.
    std::uint32_t formations;
    unsigned formation_count, formation_row;
    unsigned member_offset, minimum_offset, maximum_offset;
    bool member_halfwords;
    // Battle artwork: sprite row (halfword sprite, byte +3 bits 1..4 colour
    // variant) per enemy ID up to last_sprite_row; 20-byte sprite records;
    // (asset, sprite) halfword pairs ending at sprite 0; the asset table;
    // 256-byte colour tables, five; the picture palette. A composite's first
    // sprite is listed; its second is the next sprite.
    std::uint32_t enemy_sprites;
    unsigned last_sprite_row;
    std::uint32_t sprite_records, sprite_assets, asset_table, colour_tables;
    std::uint32_t picture_palette;
    std::span<const unsigned> composites;
    // Enemy artwork loading (r0 = slot) and the tag -> data lookup; the
    // unit's enemy ID (byte or halfword).
    std::uint32_t load_enemy_animation, animation_lookup;
    std::uint32_t unit_enemy_id;
    bool unit_id_halfword;
    std::size_t max_rom_bytes;
    // Text: decoder tables, enemy name = ID + enemy_name_text.
    std::size_t text_table, tree_table;
    std::uint32_t glyph_widths;  // 224 native font advances, glyph - 32
    unsigned enemy_name_text;
    // The monsters Battle Mode can offer; the furthest story order the live
    // save has reached (~0u: every monster); and, optionally, a monster the
    // save has met whatever its order.
    void (*catalogue)(Rom rom, std::vector<Monster>* out);
    unsigned (*reached)();
    bool (*met)(const Monster& monster);
};
void install(const Game& game);

// For the games' catalogues: the members Battle Mode can field from formation
// `f` (none for a row without minimum counts; a member with maximum 0 never
// appears), and the live save's flags.
std::vector<unsigned> formation_members(Rom rom, unsigned f);
bool read_flag(unsigned flag);

// What the opponent panel shows, for the runner's painter
// (custom_battle_panel.h).
enum class Page { Monsters, Bosses, Arena, Count };

struct PanelView {
    bool open = false;
    int tile_y = 0;              // the panel's top tile row
    Page page = Page::Monsters;
    std::vector<std::string> rows;  // the visible names, empty when none fit
    int selected = 0;            // into rows
    bool more_above = false, more_below = false;
    std::string level;           // "Lv 12", empty when matched to the party
    std::string chosen;          // the picks so far
    int picture_width = 0, picture_height = 0;
    std::vector<std::uint32_t> picture;  // 0 clear, else 0xFFRRGGBB
};
PanelView panel_view();

void on_entry(std::uint32_t pc);
// True while Custom Battle runs a game routine; other hooks stand aside.
bool busy();
// Prepare immutable ROM-derived animations in memory; never writes the ROM file.
void prepare_animation_assets(std::vector<std::uint8_t>* rom);
// Our question line, for the text decoder. Character intake (The Broken
// Seal): true with *out_value when the character is one of our lines.
bool text_character(std::uint32_t text_id, std::uint32_t original,
                    std::uint32_t* out_value);
// Whole-line intake (The Lost Age's tla_*_text answers).
bool line_text(std::uint32_t text_id, std::string& out);
// Savestate loaded: forget any question or chosen list.
void reset();

}  // namespace gsr::custom_battle
