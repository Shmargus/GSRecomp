#include "custom_battle.h"
#include "custom_battle_arena.h"
#include "custom_battle_text.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "game_options.h"
#include "gs_text_encode.h"
#include "menu_text.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace gsr::custom_battle {
namespace {

Game g_game{};
bool g_installed = false;

// Dialogue control codes, as the game's text holds them.
constexpr char kNewLine = '\x03';
constexpr char kNextPage = '\x01';  // waits for A, then a fresh window
constexpr char kPrompt = '\x1E';    // ends a line a menu follows
constexpr std::size_t kWindowLines = 3u;
// Measured from the selected ROM's original receptionist question on first
// use. Her longest native line proves a width that fits her dialogue box.
unsigned g_dialogue_width = 0;
constexpr unsigned kColourTableCount = 5u;
constexpr unsigned kLastColour = 0xDFu;
constexpr std::uint32_t kSpriteRecordSize = 20u;

struct Catalogue {
    bool built = false;
    std::vector<Monster> monsters;  // in story order
} g_catalogue;

// The chosen fight, waiting for Battle Mode's builder.
struct Roster {
    bool armed = false;
    bool scaled = true;
    std::vector<unsigned> ids;
    unsigned arena = 0;  // 0 keeps the backdrop the game chose
} g_roster, g_fight;
int g_fight_target = 0;

// The questions, one per Yes/No of hers. Yes/No screens take her answer;
// on the Panel screen her Yes/No waits under the opponent panel, which
// answers it Yes (one picked) or No (done choosing). After each pick she asks to add
// another while one still fits.
enum class Screen { Idle, Choose, Strength, Panel, AddMore, Review, FightYes };
struct Picker {
    Screen screen = Screen::Idle;
    std::vector<const Monster*> list;  // what this save offers
    std::vector<const Monster*> picks;
    bool scaled = true;
    // The panel: up over her menu, its page, the cursor
    // on each page (into what fits), the first visible row, and the answer
    // it is closing her menu with.
    bool open = false;
    Page page = Page::Monsters;
    std::array<std::size_t, static_cast<std::size_t>(Page::Count)> cursors{};
    unsigned arena = 0;
    std::size_t first = 0;
    int closing = -1;  // 0 Yes, 1 No
    bool skip_select_sound = false;
    bool fresh = false;  // keys of the frame it opened belong to her question
    std::vector<std::uint32_t> sounds;

    std::size_t& cursor() { return cursors[static_cast<std::size_t>(page)]; }
} g_picker;

// Battle pictures already decoded, by monster.
struct Picture {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
};
std::map<unsigned, Picture> g_pictures;
std::vector<unsigned> g_arenas;
std::map<unsigned, std::vector<std::uint32_t>> g_arena_pictures;

// Immutable, unpacked animation assets in the in-memory ROM extension.
// Bank tags 8..13 encode the six native colour variants, independently of
// the game's six RAM animation slots. Native banks are only 0..7; the
// sprite record lookup masks the bank off with 0xFFF (both games). See TBS
// FACTS, F12 0064.
struct AnimationAsset { std::uint32_t address = 0; unsigned bytes = 0; };
std::map<unsigned, AnimationAsset> g_animation_assets;
constexpr unsigned kAnimationBank = 8u;

unsigned colour_variant(unsigned v) { return v > kColourTableCount ? 1u : v; }
bool composite_first(unsigned sprite) {
    return std::find(g_game.composites.begin(), g_game.composites.end(), sprite) !=
           g_game.composites.end();
}
unsigned animation_pieces(unsigned sprite) { return composite_first(sprite) ? 2u : 1u; }
unsigned animation_tag(unsigned sprite, unsigned variant) {
    // Both games build a composite's second half with the first's tag +
    // 0x2001: next sprite, two banks higher. Preserve that native encoding.
    const unsigned second = composite_first(sprite - 1u) ? 2u : 0u;
    return sprite | ((kAnimationBank + variant + second) << 12u);
}

std::mutex g_view_mutex;
PanelView g_view;

bool g_in_picker = false;
// A game routine did not come back (a savestate loaded mid-call).
bool g_abort = false;
std::string g_line;
int g_line_pos = -1;

std::uint32_t call(std::uint32_t pc, std::uint32_t r0 = 0, std::uint32_t r1 = 0) {
    std::uint32_t result = 0;
    if (g_abort || !gbarecomp::guest_call(pc | 1u, r0, r1, 0, 0, &result))
        g_abort = true;
    return result;
}

// The runtime's ROM, prepared extension included (read-only, not owned).
Rom rom_bytes() {
    const auto* bus = gbarecomp::active_bus();
    return bus && bus->rom_ptr() ? Rom(bus->rom_ptr(), bus->rom_size()) : Rom();
}

unsigned rom_u8(Rom rom, std::uint32_t at) {
    return at < rom.size() ? rom[at] : 0u;
}
int rom_s16(Rom rom, std::uint32_t at) {
    return static_cast<std::int16_t>(rom_u8(rom, at) | (rom_u8(rom, at + 1) << 8));
}
std::uint32_t rom_u32(Rom rom, std::uint32_t at) {
    return rom_u8(rom, at) | (rom_u8(rom, at + 1) << 8) | (rom_u8(rom, at + 2) << 16) |
           (std::uint32_t(rom_u8(rom, at + 3)) << 24);
}
std::uint32_t offset(std::uint32_t address) { return address - mods::kRomBase; }

void build_arenas(Rom rom) {
    if (!g_arenas.empty() || rom.empty()) return;
    std::set<unsigned> ids;
    for (auto at = offset(g_game.arena_table); at < rom.size(); at += 4) {
        const auto entry = rom_u32(rom, at);
        if (!entry) break;
        if (entry & 0x80000000u) ids.insert(entry & 0xFFFFu);
    }
    if (g_game.arena_variant) ids.insert(g_game.arena_variant);
    g_arenas.push_back(0);  // Default
    g_arenas.insert(g_arenas.end(), ids.begin(), ids.end());
}

const std::vector<std::uint32_t>& arena_picture(unsigned id) {
    auto [cached, inserted] = g_arena_pictures.try_emplace(id);
    if (inserted) {
        const Rom rom = rom_bytes();
        const auto entry = offset(g_game.asset_table) + id * 4u;
        const auto begin = offset(rom_u32(rom, entry));
        const auto end = offset(rom_u32(rom, entry + 4u));
        if (begin < end && end <= rom.size())
            cached->second = decode_arena(rom.subspan(begin, end - begin));
    }
    return cached->second;
}

// Monster `id`'s battle artwork: its first sprite, normalized colour variant
// and the number of consecutive sprites it is built from.
struct EnemyArt { unsigned sprite = 0, variant = 0, pieces = 1; };
EnemyArt enemy_art(Rom rom, unsigned id) {
    const std::uint32_t row = offset(g_game.enemy_sprites) + id * 8u;
    const unsigned sprite = unsigned(rom_s16(rom, row)) & 0xFFFFu;
    return {sprite, colour_variant((rom_u8(rom, row + 3u) >> 1) & 15u),
            animation_pieces(sprite)};
}

void build_catalogue() {
    if (g_catalogue.built) return;
    const Rom rom = rom_bytes();
    if (rom.empty()) return;
    g_catalogue.built = true;
    std::vector<Monster> found;
    g_game.catalogue(rom, &found);
    mods::TextEncoder text(rom, g_game.text_table, g_game.tree_table);
    std::map<std::string, unsigned> seen;
    for (auto& m : found) {
        if (!text.decode(m.id + g_game.enemy_name_text, &m.name) || m.name.empty()) m.name = "?";
        // Same name, different monster (another form or strength): number it.
        const unsigned n = ++seen[m.name];
        if (n > 1) m.name += " " + std::to_string(n);
        m.cost = call(g_game.small_enemy_pc, m.id) != 0 ? 1u : 2u;
        m.level = bus_read_u8(call(g_game.enemy_data_pc, m.id) + kLevelOffset);
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const Monster& a, const Monster& b) { return a.order < b.order; });
    g_catalogue.monsters = std::move(found);
}

std::vector<const Monster*> available() {
    build_catalogue();
    const unsigned reached = g_game.reached();
    std::vector<const Monster*> out;
    for (const auto& m : g_catalogue.monsters)
        if (reached == ~0u || m.order <= reached || (g_game.met && g_game.met(m)))
            out.push_back(&m);
    return out;
}

// The game's unpacker (TBS 0x08002544, TLA's ARM copy 0x080128A8), both
// formats, from ROM offset `at`. False on data that would not unpack (never
// seen; guards the preparation).
bool unpack(Rom rom, std::uint32_t at,
            std::vector<std::uint8_t>& out) {
    constexpr std::size_t kLimit = 0x40000;  // EWRAM, where the game unpacks it
    out.clear();
    const auto copy = [&](std::size_t distance, unsigned count) {
        if (distance == 0 || distance > out.size()) return false;
        for (unsigned k = 0; k < count; ++k) out.push_back(out[out.size() - distance]);
        return out.size() <= kLimit;
    };
    const unsigned format = rom_u8(rom, at++);
    if (format == 1) {
        // A flag byte (high bit first) per eight items: 0 a byte, 1 a copy
        // of (low nibble + 1) bytes, or (next byte + 0x11); 0/0 ends it.
        for (;;) {
            const unsigned flags = rom_u8(rom, at++);
            for (int bit = 7; bit >= 0; --bit) {
                if (!((flags >> bit) & 1u)) {
                    out.push_back(std::uint8_t(rom_u8(rom, at++)));
                    continue;
                }
                const unsigned a = rom_u8(rom, at), b = rom_u8(rom, at + 1);
                at += 2;
                const unsigned distance = ((a & 0xF0u) << 4) | b;
                unsigned count = a & 0xFu;
                if (count == 0) {
                    if (distance == 0) return true;
                    count = rom_u8(rom, at++) + 0x10u;
                }
                if (!copy(distance, count + 1)) return false;
            }
        }
    }
    if (format != 0) return false;
    // A bit stream, low bit first, refilled a halfword at a time; `spare` is
    // what the routine keeps in r4 (bits held beyond the next 16).
    std::uint32_t bits = 0;
    int spare = 0;
    if (at & 1u) {
        bits = rom_u8(rom, at++);
        spare = 8;
    }
    const auto take = [&](int n) {
        spare -= n;
        if (spare >= 0) return;
        spare += 16;
        bits |= (rom_u8(rom, at) | (rom_u8(rom, at + 1) << 8)) << spare;
        at += 2;
    };
    const auto bit = [&] {
        const bool set = bits & 1u;
        bits >>= 1;
        return set;
    };
    take(16);
    for (;;) {
        if (out.size() > kLimit) return false;
        if (bit()) {
            out.push_back(std::uint8_t(bits));
            bits >>= 8;
            take(9);
            continue;
        }
        unsigned count;
        if (!bit()) { count = 2; take(2); }
        else if (!bit()) { count = 3; spare -= 3; }  // refilled below
        else if (!bit()) { count = 4; take(4); }
        else if (!bit()) { count = 5; take(5); }
        else if (bit()) {
            count = bits & 3u;
            bits >>= 2;
            if (count) {
                count += 7;
                take(8);
            } else {
                count = bits & 0x7Fu;
                if (count == 0) return true;
                count += 0xA;
                bits >>= 7;
                take(15);
            }
        }
        else if (!bit()) { count = 6; take(7); }
        else { count = 7; take(7); }
        unsigned distance;
        if (bit()) {
            distance = bits & 0x1Fu;
            bits >>= 5;
            take(6);
        } else {
            // Far copies: 12 bits once 0x821 bytes are out, else as many as
            // the bytes out past 0x21 need.
            const std::uint32_t past = std::uint32_t(out.size()) - 0x21u;
            int width = 12;
            if (past < 0x800u) for (width = 0; width < 11 && (past >> width); ++width) {}
            distance = (bits & ((1u << width) - 1u)) + 0x20u;
            bits >>= width;
            take(1 + width);
        }
        if (!copy(distance + 1u, count)) return false;
    }
}

// The asset of `sprite` from the (asset, sprite) pairs, ~0u when the pairs
// do not list it: the game's loader then gives up too (The Lost Age's
// Poseidon, Dullahan and Doom Dragon load their artwork another way).
unsigned sprite_asset(Rom rom, unsigned sprite) {
    unsigned asset = ~0u;
    for (std::uint32_t at = offset(g_game.sprite_assets), n = 0; n < 0x100u; at += 4, ++n) {
        const unsigned s = unsigned(rom_s16(rom, at + 2)) & 0xFFFFu;
        if (s == 0) break;
        if (s == sprite) {
            asset = unsigned(rom_s16(rom, at)) & 0xFFFFu;
            break;
        }
    }
    return asset;
}

bool unpack_sprite(Rom rom, unsigned asset, std::vector<std::uint8_t>& data) {
    const auto source = rom_u32(rom, offset(g_game.asset_table) + asset * 4u);
    return source >= mods::kRomBase &&
           unpack(rom, source - mods::kRomBase, data) && data.size() >= 4;
}

// Monster `id`'s battle picture, first frame (custom_battle.h), from the
// prepared, already recoloured animation assets. A composite's pieces sit
// side by side, tops level, as battle draws Kraken (TBS F12 0064, OAM 8, 7).
const Picture& picture(unsigned id) {
    auto found = g_pictures.find(id);
    if (found != g_pictures.end()) return found->second;
    Picture& pic = g_pictures[id];
    const Rom rom = rom_bytes();
    if (rom.empty() || id > g_game.last_sprite_row) return pic;
    const EnemyArt art = enemy_art(rom, id);
    struct Piece { int width, height; std::uint32_t frame, end; };
    std::vector<Piece> pieces;
    int width = 0, height = 0;
    for (unsigned piece = 0; piece < art.pieces; ++piece) {
        const auto asset = g_animation_assets.find(animation_tag(art.sprite + piece, art.variant));
        if (asset == g_animation_assets.end()) return pic;
        const std::uint32_t record =
            offset(g_game.sprite_records) + (art.sprite + piece) * kSpriteRecordSize;
        const std::uint32_t at = offset(asset->second.address);
        pieces.push_back({int(rom_u8(rom, record)), int(rom_u8(rom, record + 1)),
                          offset(rom_u32(rom, at)), at + asset->second.bytes});
        width += pieces.back().width;
        height = std::max(height, pieces.back().height);
    }
    pic.width = width;
    pic.height = height;
    pic.pixels.assign(std::size_t(width * height), 0u);
    int left = 0;
    for (const Piece& piece : pieces) {
        const std::size_t size = std::size_t(piece.width * piece.height);
        std::size_t p = 0;
        for (std::uint32_t i = piece.frame; i < piece.end && rom_u8(rom, i) != 0; ++i) {
            const unsigned b = rom_u8(rom, i);
            if (b > kLastColour) {
                p += b - kLastColour;
                continue;
            }
            if (p >= size) break;
            const unsigned bgr =
                unsigned(rom_s16(rom, offset(g_game.picture_palette) + b * 2u)) & 0x7FFFu;
            const unsigned r = bgr & 31u, g = (bgr >> 5) & 31u, bl = (bgr >> 10) & 31u;
            const std::size_t x = std::size_t(left) + p % std::size_t(piece.width), y = p / std::size_t(piece.width);
            pic.pixels[y * std::size_t(width) + x] =
                0xFF000000u | (((r << 3) | (r >> 2)) << 16) |
                (((g << 3) | (g >> 2)) << 8) | ((bl << 3) | (bl >> 2));
            ++p;
        }
        left += piece.width;
    }
    return pic;
}

// From the entry of her last line's set-text call: her script goes on at
// `resume`, one of her own call returns. `line`: that call sets our line
// (her question's text ID, text intake below); otherwise it is skipped.
void resume_her_script(std::uint32_t resume, const std::string* line) {
    g_cpu.R[14] = resume | 1u;
    if (line) {
        g_line = *line + kPrompt;
        g_line_pos = 0;
        g_cpu.R[0] = g_game.question_text;
    } else {
        gbarecomp::guest_return_now(g_game.set_text_pc, 0);
    }
}

// Lines into windows of three, A between windows.
std::string paged(const std::vector<std::string>& lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += i % kWindowLines ? kNewLine : kNextPage;
        out += lines[i];
    }
    return out;
}

std::vector<std::string> dialogue_lines(const std::string& line) {
    const Rom rom = rom_bytes();
    const auto at = offset(g_game.glyph_widths);
    if (at > rom.size() || rom.size() - at < 224u)
        throw std::runtime_error("Custom Battle font advances are out of range");
    const auto advances = rom.subspan(at, 224u);
    if (!g_dialogue_width) {
        mods::TextEncoder decoder(rom, g_game.text_table, g_game.tree_table);
        std::string original;
        if (!decoder.decode(g_game.question_text, &original))
            throw std::runtime_error("Custom Battle question could not be decoded");
        unsigned width = 0;
        for (const unsigned char c : original) {
            if (c == kNewLine || c == kNextPage || c == kPrompt) {
                g_dialogue_width = std::max(g_dialogue_width, width);
                width = 0;
            } else if (c >= 32u) {
                width += advances[c - 32u];
            }
        }
        g_dialogue_width = std::max(g_dialogue_width, width);
        if (!g_dialogue_width)
            throw std::runtime_error("Custom Battle question has no measured text width");
    }
    return text::wrap(line, advances, g_dialogue_width);
}

// Native names with a translated conjunction and final question, paged into
// the receptionist's original three-line windows.
std::string review(const std::vector<const Monster*>& picks) {
    std::string list;
    for (std::size_t i = 0; i < picks.size(); ++i) {
        if (i) list += ' ';
        list += picks[i]->name;
        list += i + 1 == picks.size() ? "." : i + 2 == picks.size()
            ? " " + gsr::menu_text("and") : ",";
    }
    auto lines = dialogue_lines(list);
    const auto question = dialogue_lines(gsr::menu_text("Fight these opponents?"));
    lines.insert(lines.end(), question.begin(), question.end());
    return paged(lines);
}

// What the next opponent can be: monsters that fit the space and the five
// kinds, on one page.
std::vector<const Monster*> fitting(bool bosses) {
    const Picker& p = g_picker;
    std::vector<const Monster*> fits;
    if (p.picks.size() >= kMaxEnemies) return fits;
    unsigned used = 0;
    std::vector<unsigned> kinds;
    for (const auto* m : p.picks) {
        used += m->cost;
        if (std::find(kinds.begin(), kinds.end(), m->id) == kinds.end()) kinds.push_back(m->id);
    }
    for (const auto* m : p.list) {
        if (m->boss != bosses) continue;
        const bool new_kind = std::find(kinds.begin(), kinds.end(), m->id) == kinds.end();
        if (unsigned(kinds.size()) + (new_kind ? 1u : 0u) <= kMaxKinds &&
            used + m->cost <= kSpace)
            fits.push_back(m);
    }
    return fits;
}
std::vector<const Monster*> fitting() { return fitting(g_picker.page == Page::Bosses); }

// Another opponent fits on either page.
bool can_add() { return !fitting(false).empty() || !fitting(true).empty(); }

std::string screen_line() {
    Picker& p = g_picker;
    switch (p.screen) {
    case Screen::Choose:
        return paged(dialogue_lines(gsr::menu_text("Would you like to choose your opponents?")));
    case Screen::Strength:
        return paged(dialogue_lines(gsr::menu_text("Should they match your party's strength?")));
    case Screen::Panel:
        return paged(dialogue_lines(gsr::menu_text("Choose your opponents.")));
    case Screen::AddMore:
        return paged(dialogue_lines(gsr::menu_text("Add another opponent?")));
    case Screen::Review:
        return review(p.picks);
    default:
        return {};
    }
}

// The fight goes ahead (chosen or random): her own Yes runs on, flag 0x173
// and "step into the circle". From her No, she asks again and is answered.
void fight(bool at_yes) {
    g_line_pos = -1;
    if (at_yes) {
        g_picker.screen = Screen::Idle;
        return;
    }
    g_picker.screen = Screen::FightYes;
    resume_her_script(g_game.ask, nullptr);
}

// Shows the current screen's line and asks it, through her own steps.
void ask_screen() {
    const std::string line = screen_line();
    resume_her_script(g_game.show_and_ask, &line);
}

// Only reached after her native No path has already cleared the arena
// flag. Let that path finish its goodbye rather than asking again.
void cancel_selection() {
    g_picker.screen = Screen::Idle;
    g_picker.picks.clear();
    g_roster.armed = false;
    g_roster.ids.clear();
    g_roster.arena = 0;
    g_line_pos = -1;
}

// One of her Yes/No calls came back. Decide the next screen and ask it.
void answered(bool yes) {
    Picker& p = g_picker;
    switch (p.screen) {
    case Screen::Choose:
        if (!yes) {
            g_roster.armed = false;
            std::fprintf(stderr, "[custom-battle] random opponents\n");
            fight(false);
            return;
        }
        p.screen = Screen::Strength;
        break;
    case Screen::Strength:
        p.scaled = yes;
        p.picks.clear();
        p.page = Page::Monsters;
        p.cursors.fill(0);
        p.first = 0;
        p.arena = 0;
        build_arenas(rom_bytes());
        // Show a real preview on first entering Arena; Default stays selected.
        if (g_arenas.size() > 1) p.cursors[static_cast<std::size_t>(Page::Arena)] = 1;
        p.screen = Screen::Panel;
        break;
    case Screen::Panel:
        // Leaving the picker finishes selection; never send an empty
        // selection back through the strength question again.
        if (!yes && p.picks.empty()) {
            cancel_selection();
            return;
        }
        p.screen = yes && can_add() ? Screen::AddMore : Screen::Review;
        break;
    case Screen::AddMore:
        if (!yes && p.picks.empty()) {
            cancel_selection();
            return;
        }
        p.screen = yes ? Screen::Panel : Screen::Review;
        break;
    case Screen::Review:
        if (yes) {
            g_roster.armed = true;
            g_roster.scaled = p.scaled;
            g_roster.arena = p.arena;
            g_roster.ids.clear();
            for (const auto* m : p.picks) g_roster.ids.push_back(m->id);
            std::fprintf(stderr, "[custom-battle] %zu opponents chosen, %s\n",
                         p.picks.size(), p.scaled ? "matched to the party" : "normal strength");
            fight(true);
            return;
        }
        cancel_selection();
        return;
    default:
        return;
    }
    ask_screen();
}

// Her Yes/No came back: her last line's set-text call, Yes or No.
void her_answer(bool yes) {
    Picker& p = g_picker;
    if (p.screen == Screen::FightYes) {
        p.screen = Screen::Idle;
        return;
    }
    if (p.screen != Screen::Idle) {
        answered(yes);
        return;
    }
    // Her own "battle a monster?": Yes starts the questions.
    if (!yes || gbarecomp::game_option_get(kOptionKey, kOptionDefault) == 0) return;
    p.list = available();
    if (p.list.empty() || g_abort) return;
    p.picks.clear();
    p.screen = Screen::Choose;
    ask_screen();
}

// The panel's state for the painter (taken under g_view_mutex).
void publish() {
    Picker& p = g_picker;
    PanelView view;
    view.open = p.open && p.closing < 0;
    if (view.open) {
        const unsigned menu_row =
            bus_read_u16(bus_read_u32(g_game.menu_context_ptr) + kMenuRowOffset);
        view.tile_y = menu_row < kScreenTileRows / 2 ? 0 : kScreenTileRows - kPanelRows;
        view.page = p.page;
        for (std::size_t i = 0; i < p.picks.size(); ++i)
            view.chosen += (i ? ", " : "") + p.picks[i]->name;
        const bool arena = p.page == Page::Arena;
        const auto fits = arena ? std::vector<const Monster*>{} : fitting();
        const auto count = arena ? g_arenas.size() : fits.size();
        std::size_t& at = p.cursor();
        if (at >= count) at = 0;
        const std::size_t rows = std::size_t(kPanelVisibleRows);
        if (at < p.first) p.first = at;
        if (at >= p.first + rows) p.first = at + 1 - rows;
        if (p.first + rows > count)
            p.first = count > rows ? count - rows : 0;
        for (std::size_t i = p.first; i < count && i < p.first + rows; ++i) {
            if (arena) {
                std::string label = i == 0 ? gsr::menu_text("Default")
                                           : gsr::menu_text("Arena") + " " + std::to_string(i);
                if (g_arenas[i] == p.arena) label = "> " + label;
                view.rows.push_back(std::move(label));
            } else {
                view.rows.push_back(fits[i]->name);
            }
        }
        view.selected = int(at - p.first);
        view.more_above = p.first > 0;
        view.more_below = p.first + rows < count;
        if (arena && at < count && g_arenas[at]) {
            view.picture = arena_picture(g_arenas[at]);
            if (!view.picture.empty()) {
                view.picture_width = kArenaWidth;
                view.picture_height = kArenaHeight;
            }
        } else if (!arena && at < count) {
            if (!p.scaled) view.level = gsr::menu_text("Lv") + " " + std::to_string(fits[at]->level);
            const Picture& pic = picture(fits[at]->id);
            view.picture_width = pic.width;
            view.picture_height = pic.height;
            view.picture = pic.pixels;
        }
    }
    std::lock_guard<std::mutex> lock(g_view_mutex);
    g_view = std::move(view);
}

// Close the panel, answering her menu Yes or No.
void close_panel(bool yes) {
    g_picker.closing = yes ? 0 : 1;
}

// Once a frame while the panel is up: the game's own new presses and key
// repeat of the frame just run. A menu that still hears directions moved
// its hidden cursor and played the move sound itself; for one that does not,
// the panel plays it.
void panel_keys() {
    Picker& p = g_picker;
    if (p.fresh) {
        // The Yes/No is placed by now; publish again for its row.
        p.fresh = false;
        publish();
        return;
    }
    const std::uint32_t pressed = bus_read_u32(g_game.keys_pressed);
    const std::uint32_t moves = bus_read_u32(g_game.keys_repeat);
    const auto moved = [&] {
        if (!g_game.menu_hears_directions) p.sounds.push_back(kMoveSound);
    };
    if (pressed & (kKeyB | kKeyStart)) {
        p.sounds.push_back(kBackSound);
        close_panel(false);
        return;
    }
    if (pressed & (kKeyLeft | kKeyRight)) {
        constexpr int pages = static_cast<int>(Page::Count);
        const int direction = pressed & kKeyLeft ? -1 : 1;
        p.page = static_cast<Page>((static_cast<int>(p.page) + pages + direction) % pages);
        p.first = 0;
        moved();
        return;
    }
    const bool arena = p.page == Page::Arena;
    const auto fits = arena ? std::vector<const Monster*>{} : fitting();
    std::size_t& at = p.cursor();
    const std::size_t n = arena ? g_arenas.size() : fits.size();
    if (!n) return;
    if (at >= n) at = 0;
    if (pressed & kKeyA) {
        p.sounds.push_back(kSelectSound);
        if (arena) {
            p.arena = g_arenas[at];
            return;
        }
        p.picks.push_back(fits[at]);
        // The next pick starts on the same monster when it still fits.
        const auto next = fitting();
        const auto same = std::find(next.begin(), next.end(), p.picks.back());
        at = same != next.end() ? std::size_t(same - next.begin()) : 0;
        close_panel(true);
    } else if (moves & (kKeyUp | kKeyDown)) {
        at = (at + (moves & kKeyUp ? n - 1 : 1)) % n;
        moved();
    } else if (moves & (kKeyL | kKeyR)) {
        // A page of the list; the shoulders are not her menu's keys.
        const std::size_t jump = std::size_t(kPanelVisibleRows);
        at = moves & kKeyL ? (at > jump ? at - jump : 0) : std::min(n - 1, at + jump);
        p.sounds.push_back(kMoveSound);
    }
}

// Her menu's frame wait while the panel is up: run the panel and play its
// sounds. Her menu reads its keys through a register the wait keeps, so it
// points at a zero block while the panel is up; once the panel is done it
// points back at the live keys, with A pressed for her menu at once and the
// menu's own sound left out.
void panel_frame() {
    Picker& p = g_picker;
    if (p.closing < 0) panel_keys();
    for (const std::uint32_t sound : p.sounds) call(g_game.sound_pc, sound);
    p.sounds.clear();
    if (p.closing < 0) {
        g_cpu.R[g_game.menu_key_reg] = g_game.menu_keys_blind;
        publish();
        return;
    }
    bus_write_u16(bus_read_u32(g_game.menu_context_ptr) + kMenuCursorOffset,
                  std::uint16_t(p.closing));
    bus_write_u32(g_game.keys_pressed, kKeyA);
    g_cpu.R[g_game.menu_key_reg] = g_game.menu_keys_live;
    p.open = false;
    p.closing = -1;
    p.skip_select_sound = true;
    publish();
    gbarecomp::guest_return_now(g_game.wait_frames_pc, 0);
}

// Battle Mode's target level: party average plus the saved adjustment,
// 1..99 (TBS 0x080C1AFC, TLA 0x0812764C).
int target_level() {
    unsigned count = call(g_game.party_count_pc);
    const std::uint32_t context = bus_read_u32(g_game.battle_context_ptr);
    const unsigned limit = context && bus_read_u8(context + kThreeMemberOffset) ? 3u : 4u;
    count = std::min(count, limit);
    if (count == 0) return 1;
    int sum = 0;
    for (unsigned i = 0; i < count; ++i)
        sum += bus_read_u8(call(g_game.slot_getter_pc, bus_read_u8(g_game.party_ids + i)) +
                           kLevelOffset);
    int target = sum / int(count) +
                 static_cast<std::int8_t>(call(g_game.byte_flag_pc, kLevelAdjustByte));
    return std::clamp(target, 1, 99);
}

unsigned unit_enemy_id(std::uint32_t unit) {
    return g_game.unit_id_halfword ? bus_read_u16(unit + g_game.unit_enemy_id)
                                   : bus_read_u8(unit + g_game.unit_enemy_id);
}

}  // namespace

void install(const Game& game) {
    g_game = game;
    g_dialogue_width = 0;
    g_installed = true;
}

std::vector<unsigned> formation_members(Rom rom, unsigned f) {
    std::vector<unsigned> ids;
    const std::uint32_t row = offset(g_game.formations) + f * g_game.formation_row;
    bool any_minimum = false;
    for (unsigned i = 0; i < 5; ++i)
        any_minimum |= rom_u8(rom, row + g_game.minimum_offset + i) != 0;
    if (!any_minimum) return ids;
    for (unsigned i = 0; i < 5; ++i) {
        if (rom_u8(rom, row + g_game.maximum_offset + i) == 0) continue;
        const std::uint32_t at = row + g_game.member_offset + i * (g_game.member_halfwords ? 2u : 1u);
        const unsigned member = g_game.member_halfwords
            ? unsigned(rom_s16(rom, at)) & 0xFFFFu : rom_u8(rom, at);
        ids.push_back(member + 8u);
    }
    return ids;
}

bool read_flag(unsigned flag) {
    return (bus_read_u8(kFlags + (flag >> 3)) >> (flag & 7u)) & 1u;
}

bool busy() { return g_in_picker; }

void prepare_animation_assets(std::vector<std::uint8_t>* rom) {
    g_animation_assets.clear();
    if (!rom || !g_installed) return;
    const Rom view(*rom);
    std::set<std::pair<unsigned, unsigned>> keys;
    for (unsigned formation = 0; formation < g_game.formation_count; ++formation) {
        for (unsigned id : formation_members(view, formation)) {
            if (id > g_game.last_sprite_row) continue;
            const EnemyArt art = enemy_art(view, id);
            for (unsigned piece = 0; piece < art.pieces; ++piece)
                keys.emplace(art.sprite + piece, art.variant);
        }
    }
    // Stable order/layout on every launch, also when Custom Battle is off:
    // savestates keep ordinary guest pointers into these read-only assets.
    for (const auto& key : keys) {
        const auto [sprite, variant] = key;
        const unsigned asset = sprite_asset(Rom(*rom), sprite);
        if (asset == ~0u) continue;
        std::vector<std::uint8_t> data;
        if (!unpack_sprite(Rom(*rom), asset, data))
            throw std::runtime_error("Custom Battle animation could not be unpacked");
        const std::size_t at = (rom->size() + 3u) & ~std::size_t(3u);
        if (at + data.size() > g_game.max_rom_bytes)
            throw std::runtime_error("Custom Battle animation storage exceeds ROM extension");
        const auto address = mods::kRomBase + std::uint32_t(at);
        std::size_t frame = 0;
        // A frame may start at the data's very end (The Lost Age's sprite
        // 0x22B: Wyvern, Sky Dragon, Winged Dragon); the game's own buffer
        // runs on there, so that frame gets one 0 byte (ends at once).
        bool frame_at_end = false;
        for (; frame + 4u <= data.size(); frame += 4u) {
            const auto frame_offset = rom_u32(data, std::uint32_t(frame));
            if (!frame_offset) break;
            if (frame_offset > data.size())
                throw std::runtime_error("Custom Battle animation frame is out of range");
            frame_at_end |= frame_offset == data.size();
            const auto pointer = address + frame_offset;
            for (unsigned byte = 0; byte < 4; ++byte)
                data[frame + byte] = std::uint8_t(pointer >> (byte * 8u));
        }
        if (frame + 4u > data.size())
            throw std::runtime_error("Custom Battle animation has no frame terminator");
        if (variant) {
            const auto table = offset(g_game.colour_tables) + (variant - 1u) * 256u;
            for (std::size_t i = frame + 4u; i < data.size(); ++i)
                if (data[i] <= kLastColour) data[i] = std::uint8_t(rom_u8(*rom, table + data[i]));
        }
        if (frame_at_end) data.push_back(0);
        rom->resize(at, 0);
        rom->insert(rom->end(), data.begin(), data.end());
        g_animation_assets.emplace(animation_tag(sprite, variant),
                                   AnimationAsset{address, unsigned(data.size())});
    }
}

void on_entry(std::uint32_t pc) {
    if (g_in_picker || !g_installed) return;
    const std::uint32_t lr = g_cpu.R[14] & ~1u;
    if (pc == g_game.animation_lookup) {
        const unsigned tag = g_cpu.R[0] & 0xFFFFu;
        const auto found = g_animation_assets.find(tag);
        if (found != g_animation_assets.end()) {
            gbarecomp::guest_return_now(pc, found->second.address);
            return;
        }
    }
    if (pc == g_game.load_enemy_animation && g_cpu.R[0] >= kFirstEnemySlot &&
        g_cpu.R[0] < kFirstEnemySlot + kMaxEnemies &&
        // Read here, not per entry: this observer sees every function entry
        // (~2% of TLA Turbo battle time, session 20261030_121810).
        gbarecomp::game_option_get(kOptionKey, kOptionDefault) != 0 &&
        read_flag(kArenaFlag)) {
        g_in_picker = true;
        g_abort = false;
        const auto unit = call(g_game.slot_getter_pc, g_cpu.R[0]);
        g_in_picker = false;
        if (g_abort || !unit || bus_read_u8(unit + kLoadedOffset)) return;
        const unsigned id = unit_enemy_id(unit);
        const Rom rom = rom_bytes();
        if (rom.empty() || id > g_game.last_sprite_row) return;
        const EnemyArt art = enemy_art(rom, id);
        bool complete = true;
        for (unsigned piece = 0; piece < art.pieces; ++piece)
            complete &= g_animation_assets.count(animation_tag(art.sprite + piece, art.variant)) != 0;
        if (complete) {
            gbarecomp::guest_return_now(pc, animation_tag(art.sprite, art.variant));
            return;
        }
    }
    if (g_game.decode_start_pc && pc == g_game.decode_start_pc &&
        g_cpu.R[1] == g_game.question_text && g_line_pos >= 0) {
        g_line_pos = 0;  // each decode of our line starts at its start
        return;
    }
    Picker& p = g_picker;
    if (p.open && pc == g_game.wait_frames_pc && lr == g_game.menu_wait_return) {
        g_in_picker = true;
        g_abort = false;
        panel_frame();
        g_in_picker = false;
        return;
    }
    if (p.skip_select_sound && pc == g_game.sound_pc && lr == g_game.menu_select_return &&
        g_cpu.R[0] == kSelectSound) {
        p.skip_select_sound = false;
        bus_write_u32(g_game.keys_pressed, 0);
        gbarecomp::guest_return_now(g_game.sound_pc, 0);
        return;
    }
    if (pc == g_game.set_text_pc && lr == g_game.her_line_return &&
        (g_cpu.R[0] == g_game.yes_line || g_cpu.R[0] == g_game.no_line)) {
        // Her code, not another room's code at this address.
        if (bus_read_u32(g_game.text_base_literal) != g_game.text_base) return;
        g_in_picker = true;
        g_abort = false;
        her_answer(g_cpu.R[0] == g_game.yes_line);
        g_in_picker = false;
    } else if (pc == g_game.yes_no_pc && lr == g_game.her_yes_no_return &&
               p.screen != Screen::Idle) {
        if (p.screen == Screen::FightYes) {
            gbarecomp::guest_return_now(g_game.yes_no_pc, 0);
            return;
        }
        if (p.screen != Screen::Panel) return;
        // The panel opens over her menu.
        p.open = true;
        p.closing = -1;
        p.fresh = true;
        g_in_picker = true;
        publish();
        g_in_picker = false;
    } else if (pc == g_game.slot_getter_pc && lr == g_game.scale_getter_return && !g_fight.armed &&
               g_game.scaled && bus_read_u32(g_cpu.R[13] + kLevelsOffset) == 0 && g_game.scaled()) {
        // Scaled difficulty: only up, to the Arena's target.
        const std::uint32_t slot = g_cpu.R[0];
        g_in_picker = true;
        g_abort = false;
        const int level = bus_read_u8(call(g_game.slot_getter_pc, slot) + kLevelOffset);
        const int target = target_level();
        if (target > level) call(g_game.stat_adjust_pc, slot, static_cast<std::uint32_t>(target - level));
        g_in_picker = false;
    } else if (pc == g_game.slot_getter_pc && lr == g_game.clear_slots_return &&
               g_cpu.R[0] == kFirstEnemySlot) {
        g_fight.armed = false;
        if (!g_roster.armed || !read_flag(kArenaFlag)) return;
        const std::uint32_t sp = g_cpu.R[13];
        const std::uint32_t list = sp + kListOffset;
        if (bus_read_u32(sp + kListPointerOffset) != list) return;
        for (std::size_t i = 0; i < g_roster.ids.size(); ++i)
            bus_write_u16(list + 2u * std::uint32_t(i), std::uint16_t(g_roster.ids[i]));
        bus_write_u16(list + 2u * std::uint32_t(g_roster.ids.size()), 0);
        bus_write_u32(sp + kLevelsOffset, g_roster.scaled ? 1u : 0u);
        if (g_roster.arena) {
            // The room's backdrop is already cached, and the native
            // graphics loader reads it after the builder returns.
            const auto battle = bus_read_u32(g_game.battle_context_ptr);
            if (battle) bus_write_u16(battle + g_game.arena_offset, std::uint16_t(g_roster.arena));
        }
        g_fight = g_roster;
        g_in_picker = true;
        g_abort = false;
        g_fight_target = g_fight.scaled ? target_level() : 0;
        g_in_picker = false;
    } else if (pc == g_game.stat_adjust_pc && lr == g_game.stat_adjust_return && g_fight.armed) {
        const std::uint32_t index = g_cpu.R[0] - kFirstEnemySlot;
        if (index >= g_fight.ids.size()) return;
        g_in_picker = true;
        g_abort = false;
        const int level = bus_read_u8(call(g_game.enemy_data_pc, g_fight.ids[index]) + kLevelOffset);
        g_in_picker = false;
        const int levels = g_fight_target - level;
        if (levels == 0) gbarecomp::guest_return_now(g_game.stat_adjust_pc, 0);
        else g_cpu.R[1] = static_cast<std::uint32_t>(levels);
    }
}

bool text_character(std::uint32_t text_id, std::uint32_t original,
                    std::uint32_t* out_value) {
    if (g_line_pos < 0 || text_id != g_game.question_text) return false;
    std::uint32_t wanted = 0u;
    if (std::size_t(g_line_pos) < g_line.size())
        wanted = static_cast<unsigned char>(g_line[std::size_t(g_line_pos++)]);
    *out_value = original + (wanted - g_cpu.R[0]);
    return true;
}

bool line_text(std::uint32_t text_id, std::string& out) {
    if (g_line_pos < 0 || text_id != g_game.question_text) return false;
    out = g_line;
    return true;
}

PanelView panel_view() {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    return g_view;
}

void reset() {
    g_roster.armed = false;
    g_fight.armed = false;
    Picker& p = g_picker;
    p.screen = Screen::Idle;
    p.open = false;
    p.closing = -1;
    p.skip_select_sound = false;
    p.sounds.clear();
    g_line_pos = -1;
    publish();
}

}  // namespace gsr::custom_battle
