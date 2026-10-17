// Golden Sun: The Lost Age -- settings page two: Walk Speed, Encounters,
// Screen, No Slowdown and Better Field Psy.
//
// Golden Sun 1's second settings page, re-found on this ROM (FACTS.md,
// 2026-10-04). Down on Auto-Sleep (the last row) opens it; the game moves
// its cursor hand between page two's rows itself, Up on the first row
// returns to Auto-Sleep and Down on the last is withheld. Left/Right change
// the row's value:
//   Walk Speed   src/tla_walk_speed.cpp, the engine's walking-speed setting
//                (config.ini);
//   Encounters   src/tla_encounters.cpp (game_options.ini);
//   Screen       Normal 240x160 or Wide 360x240, the engine's view mode
//                (F1 menu, config.ini), drawn by src/tla_gpu_field.cpp;
//   No Slowdown  the engine's 50x CPU overclock (F1 menu, config.ini). It
//                once made this game far slower: the overclock stopped the
//                engine skipping idle waits (fixed in runtime_idle_backedge,
//                FACTS.md 2026-10-07);
//   Better Field Psy  field Psynergy casts at battle 2x
//                (src/tla_battle_speed.cpp, game_options.ini).
// A/B/Start close the screen as usual. "1/2" or "2/2" and a chevron sit
// under the window's bottom-right corner on both pages, painted on the
// finished picture (the strip under the window is the field, not a window).
//
// The settings loop 0x080403B8..0x0804049E, after its frame wait
// (0x08013560), loads the key block 0x03001150 once (literal at 0x080403C4)
// and tests [+4] (pressed: Select, A|Start, B) and [+0xC] (repeat: Up 0x40,
// Down 0x80, Left 0x20, Right 0x10); r7 is the screen, r8 the settings
// window, r9 the cursor row (wrapped mod 5). The literal hook decides the
// page and hands the game a ROM word pair holding only the keys it may see.
// The hand sits on text lines 0, 0x10, 0x28, 0x40, 0x58 for rows 0-4
// (0x08040376..0x080403A0: y = (row * 3 + window y) * 8 + 4, + 8 on row 0).
//
// Page two is drawn into the settings window's own text tiles at the end of
// every frame (the frame wait, 0x080134B8). The window system block
// [0x0300003C] keeps the BG0 map shadow at +8 (32 cells a row; VRAM
// 0x06002000); text tiles are 0x80.. in char block 0. Page two has one row,
// so the text tiles of page one's other rows are borrowed: their cells are
// blanked and the tiles carry page two's text where page one had none. All of
// it is put back on Up. Page one's sprites inside the window (sliders,
// icons) are hidden in the OAM shadow [0x03000050]. The help line (text ID
// 0x1146 + row) reads "Set walking speed." etc. on page two
// (src/tla_text.cpp).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "view_config.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kKeyBlockLiteralPc = 0x080403C4u;
constexpr uint32_t kKeyBlock = 0x03001150u;
constexpr uint32_t kPressedOffset = 0x4u;
constexpr uint32_t kRepeatOffset = 0xCu;
constexpr uint32_t kKeyA = 0x01u, kKeyB = 0x02u, kKeyStart = 0x08u;
constexpr uint32_t kKeyRight = 0x10u, kKeyLeft = 0x20u, kKeyUp = 0x40u,
                   kKeyDown = 0x80u;
constexpr uint32_t kLastRow = 4u;  // Auto-Sleep

// Stand-in key blocks in the ROM (SHA-1 gated): [X+4] & 0xF and [X+0xC] &
// 0xF0 are exactly what the loop then sees. 0x080000AC: none.
// 0x080004AC: Up only. 0x080030A4: Down only.
constexpr uint32_t kNoKeys = 0x080000ACu;
constexpr uint32_t kUpOnly = 0x080004ACu;
constexpr uint32_t kDownOnly = 0x080030A4u;

constexpr uint32_t kHelpIdBase = 0x1146u;

constexpr uint32_t kFrameWaitBranchPc = 0x080134B8u;

// On every cursor move the loop clears and redraws page one's Speech and
// Auto-Sleep values ("On"/"Off", text 0x1141 + value at x 0xA0 on line
// 0x40, 0x1144 + value on line 0x58; clear x 0xA0..0xB8). On page two those
// showed through for a frame or two, as Message speed's did
// (src/tla_text_speed.cpp). While page two shows, the clears start at their
// own end and the values are drawn as empty text.
constexpr uint32_t kValueTextIds[] = {0x1141u, 0x1142u, 0x1144u, 0x1145u};
constexpr uint32_t kSpeechClearXPc = 0x0804032Au;     // movs r1,#0xa0
constexpr uint32_t kValueClearEndX = 0xB8u;

// Window record: +8 width, +0xA height, +0xC x, +0xE y, in tiles (close
// routine 0x0803939C). Text at window (tx, ty) lands at ((x + 1) * 8 + tx,
// (y + 1) * 8 + ty).
constexpr uint32_t kWindowWidth = 0x8u, kWindowHeight = 0xAu,
                   kWindowX = 0xCu, kWindowY = 0xEu;
constexpr uint32_t kBg0Map = 0x06002000u;
constexpr uint32_t kBg0Chars = 0x06000000u;
constexpr uint16_t kBlankTile = 0x20u;
constexpr uint16_t kFirstTextTile = 0x80u;
constexpr uint16_t kLastTextTile = 0x1FFu;
constexpr uint32_t kOamShadowSlot = 0x03000050u;

constexpr int kLabelX = 8;
constexpr int kRowCount = 5;
constexpr int kWalkSpeedRow = 0, kEncounterRow = 1, kScreenRow = 2,
              kNoSlowdownRow = 3, kFieldPsyRow = 4;
constexpr const char* kRowLabels[kRowCount] = {
    "Walk Speed", "Encounters", "Screen", "No Slowdown", "Better Field Psy"};
constexpr const char* kRowHelp[kRowCount] = {
    "Set walking speed.", "Set how often monsters appear.",
    "Set the screen size.", "Remove slowdown.", "Speed up field Psynergy."};
// The cursor hand's lines: page one's five rows sit on lines 0, 0x10,
// 0x28, 0x40 and 0x58.
constexpr int kRowLineY[kRowCount] = {0x00, 0x10, 0x28, 0x40, 0x58};
constexpr int kSpeedCount = 4;
constexpr int kSpeedHalves[kSpeedCount] = {2, 3, 4, 6};
constexpr const char* kSpeedWords[kSpeedCount] = {"Normal", "1.5x", "2x", "3x"};
constexpr const char* kEncounterWords[4] = {"Normal", "Half", "Off", "Double"};
// View mode 0 is Native 240x160, 1 the expanded view (src/tla_runner.cpp).
constexpr auto& kScreenWords = gbarecomp::kViewModeLabels;
constexpr const char* kOnOffWords[2] = {"Off", "On"};

// Page marker: "1/2 " or "2/2 " and the font's '^' (glyph rows 0..3),
// flipped to point down on page one; its shadow one pixel down and right of
// the letter pixels, as the font's own.
constexpr const char* kPageOneMarker = "1/2 ";
constexpr const char* kPageTwoMarker = "2/2 ";
constexpr unsigned char kChevronGlyph = '^';
constexpr int kChevronRows = 4;
// Page one's "Window color" and page two's "Walk Speed" both start with W at
// window text (8, 0): it supplies the letter and shadow colours.
constexpr unsigned char kColourSampleLetter = 'W';

gba::GbaBus* bus() { return gbarecomp::active_bus(); }

int speed_index() {
    const int halves = runtime_get_mem_write_override_enabled();
    for (int i = 0; i < kSpeedCount; ++i)
        if (kSpeedHalves[i] == halves) return i;
    return 0;
}

std::string row_value(int row) {
    switch (row) {
    case kWalkSpeedRow: return kSpeedWords[speed_index()];
    case kEncounterRow: return kEncounterWords[tla_encounter_rate() & 3];
    case kScreenRow: return kScreenWords[std::clamp(runtime_get_view_mode(), 0, gbarecomp::kViewModeCount - 1)];
    case kNoSlowdownRow: return kOnOffWords[runtime_get_no_slowdown() != 0 ? 1 : 0];
    case kFieldPsyRow: return kOnOffWords[tla_field_psy_fast() ? 1 : 0];
    }
    return "";
}

void change_row(int row, bool right) {
    auto step = [&](int value, int count) {
        return right ? (value + 1) % count : (value + count - 1) % count;
    };
    switch (row) {
    case kWalkSpeedRow:
        runtime_set_mem_write_override_enabled(
            kSpeedHalves[step(speed_index(), kSpeedCount)]);
        break;
    case kEncounterRow:
        tla_set_encounter_rate(step(tla_encounter_rate(), tla_encounter_rate_count()));
        break;
    case kScreenRow:
        runtime_request_view_mode(step(runtime_get_view_mode(), gbarecomp::kViewModeCount));
        break;
    case kNoSlowdownRow:
        runtime_request_no_slowdown(runtime_get_no_slowdown() != 0 ? 0 : 1);
        break;
    case kFieldPsyRow:
        tla_set_field_psy_fast(!tla_field_psy_fast());
        break;
    }
}

int text_width(const std::string& text) {
    return static_cast<int>(tla_menu_text_width(text));
}

using Tile = std::array<uint8_t, 32>;

struct Page {
    int page = 1;
    int row = 0;  // the cursor row, r9
    bool drawn = false;
    unsigned long long seen_frame = 0;
    uint32_t wx = 0, wy = 0, ww = 0, wh = 0;
    // Text tiles whose pixels page two replaced: the game's pixels and ours.
    struct Saved { Tile original{}; Tile written{}; };
    std::unordered_map<uint16_t, Saved> tiles;
    // Cells page two changed: the game's value and ours.
    struct Cell { uint16_t original = 0; uint16_t written = 0; };
    std::unordered_map<uint32_t, Cell> cells;
    // OAM shadow entries hidden: index -> the game's attribute 0.
    std::unordered_map<int, uint16_t> hidden;
    // Page one's text tiles whose cells page two blanked, not lent out now.
    std::vector<uint16_t> spare;
};
Page g_page;

// For the page marker, painted at present time: 0 hidden, else the page;
// the window as x | y << 8 | width << 16 | height << 24, in tiles; the
// last frame the settings loop ran.
std::atomic<int> g_marker_page{0};
std::atomic<uint32_t> g_marker_window{0};
std::atomic<unsigned long long> g_marker_seen{0};

Tile read_tile(gba::GbaBus* b, uint16_t tile) {
    Tile t{};
    for (uint32_t i = 0; i < 32u; ++i) t[i] = b->read8(kBg0Chars + tile * 32u + i);
    return t;
}

void write_tile(gba::GbaBus* b, uint16_t tile, const Tile& t) {
    for (uint32_t i = 0; i < 32u; i += 2u)
        b->write16(kBg0Chars + tile * 32u + i,
                   static_cast<uint16_t>(t[i] | (t[i + 1] << 8)));
}

void write_cell(gba::GbaBus* b, uint32_t block, uint32_t cell, uint16_t v) {
    b->write16(block + kTlaWindowMapOffset + cell * 2u, v);
    b->write16(kBg0Map + cell * 2u, v);
}

bool text_tile(uint16_t tile) {
    return tile >= kFirstTextTile && tile <= kLastTextTile;
}

// Page one back where page two's changes are still in place.
void restore_page_one(gba::GbaBus* b) {
    if (!g_page.drawn) return;
    const uint32_t block = b->read32(kTlaWindowBlockSlot);
    const uint32_t oam = b->read32(kOamShadowSlot);
    for (const auto& [cell, v] : g_page.cells) {
        if (b->read16(block + kTlaWindowMapOffset + cell * 2u) == v.written)
            write_cell(b, block, cell, v.original);
    }
    for (const auto& [tile, saved] : g_page.tiles) {
        if (read_tile(b, tile) == saved.written) write_tile(b, tile, saved.original);
    }
    for (const auto& [index, attr0] : g_page.hidden) {
        const uint32_t entry = oam + static_cast<uint32_t>(index) * 8u;
        if ((b->read16(entry) & 0x0300u) == 0x0200u) b->write16(entry, attr0);
    }
    g_page.tiles.clear();
    g_page.cells.clear();
    g_page.hidden.clear();
    g_page.spare.clear();
    g_page.drawn = false;
}

// The screen is closing or gone: the game frees its window itself.
void drop_page() {
    g_marker_page.store(0);
    g_page.page = 1;
    g_page.row = 0;
    g_page.drawn = false;
    g_page.tiles.clear();
    g_page.cells.clear();
    g_page.hidden.clear();
    g_page.spare.clear();
}

std::vector<uint8_t> page_two_canvas(int width, int height) {
    std::vector<uint8_t> canvas(static_cast<size_t>(width) * height,
                                kTlaGlyphBackground);
    auto draw = [&](const std::string& text, int x, int y) {
        tla_menu_draw_text(text, x, y, [&](int px, int py, uint8_t v) {
            if (px < 0 || py < 0 || px >= width || py >= height) return;
            canvas[static_cast<size_t>(py) * width + px] = v;
        });
    };
    for (int row = 0; row < kRowCount; ++row) {
        draw(kRowLabels[row], kLabelX, kRowLineY[row]);
        std::string value = row_value(row);
        if (row == g_page.row) value = "< " + value + " >";
        draw(value, static_cast<int>(kTlaValueCentreX) - text_width(value) / 2, kRowLineY[row]);
    }
    return canvas;
}

// End of a frame with page two showing: its text in the window, page one's
// text tiles lent where page two needs them, page one's sprites hidden.
void draw_page_two(gba::GbaBus* b) {
    const uint32_t block = b->read32(kTlaWindowBlockSlot);
    const uint32_t oam = b->read32(kOamShadowSlot);
    if ((block >> 24) != 0x02u || (oam >> 24) != 0x03u) return;
    const uint32_t cols = g_page.ww - 2u, rows = g_page.wh - 2u;
    if (g_page.ww < 3u || g_page.wh < 3u || g_page.wx + g_page.ww > 32u ||
        g_page.wy + g_page.wh > 32u)
        return;
    const int width = static_cast<int>(cols * 8u);
    const int height = static_cast<int>(rows * 8u);
    const std::vector<uint8_t> canvas = page_two_canvas(width, height);

    struct Want { uint32_t cell; uint16_t value; Tile pixels; bool blank; };
    std::vector<Want> wants;
    for (uint32_t r = 0; r < rows; ++r) {
        for (uint32_t c = 0; c < cols; ++c) {
            Want w{};
            w.blank = true;
            for (int py = 0; py < 8; ++py) {
                for (int px = 0; px < 8; px += 2) {
                    const size_t at =
                        static_cast<size_t>(r * 8u + py) * width + c * 8u + px;
                    const uint8_t lo = canvas[at], hi = canvas[at + 1];
                    w.pixels[py * 4 + px / 2] = static_cast<uint8_t>(lo | (hi << 4));
                    if (lo != kTlaGlyphBackground || hi != kTlaGlyphBackground)
                        w.blank = false;
                }
            }
            w.cell = (g_page.wy + 1u + r) * 32u + (g_page.wx + 1u + c);
            w.value = b->read16(block + kTlaWindowMapOffset + w.cell * 2u);
            wants.push_back(w);
        }
    }
    auto ours = [&](const Want& w) {
        const auto own = g_page.cells.find(w.cell);
        return own != g_page.cells.end() && own->second.written == w.value;
    };
    for (Want& w : wants) {
        if (!w.blank) continue;
        const uint16_t tile = w.value & 0x3FFu;
        if (ours(w)) {
            // A tile lent here earlier that page two no longer needs: the
            // cell goes back to blank and the tile back to the spares.
            const Page::Cell cell = g_page.cells[w.cell];
            if (tile == kBlankTile) continue;
            if ((cell.original & 0x3FFu) == kBlankTile) {
                write_cell(b, block, w.cell, cell.original);
                g_page.cells.erase(w.cell);
            } else {
                const uint16_t blank =
                    static_cast<uint16_t>((w.value & 0xFC00u) | kBlankTile);
                g_page.cells[w.cell].written = blank;
                write_cell(b, block, w.cell, blank);
            }
            g_page.spare.push_back(tile);
            continue;
        }
        if (!text_tile(tile)) continue;
        // Page one's text where page two has none: the cell goes blank and
        // its tile can carry page two's text elsewhere.
        const uint16_t written =
            static_cast<uint16_t>((w.value & 0xFC00u) | kBlankTile);
        g_page.cells[w.cell] = {w.value, written};
        write_cell(b, block, w.cell, written);
        w.value = written;
        g_page.spare.push_back(tile);
    }
    // Five rows need more text tiles than page one's own (F12 0058: the
    // fifth label stopped at "Faster Field I" and its "< On >" lost a
    // tile). Then a text tile no cell of the window layer uses is borrowed,
    // from the top of the range: the game's lines here are allocated from
    // 0x80 up (63 of 0x80..0x1FF in use, all below 0xE4). Its pixels are
    // saved and put back with page one's. Never one whose bytes are a
    // background's map: in the field tiles 0x100..0x1FF are the four maps
    // (0x2000..0x3FFF), and 0x1FC..0x1FF left a strip of text in BG1's rows
    // 30-31 after the menu closed (F12 0097). 71 or more tiles stay free
    // below them (F12 0058, 0010, 0097).
    auto borrow_free_tile = [&]() {
        std::array<bool, 0x400> used{};
        for (uint32_t i = 0; i < 32u * 32u; ++i)
            used[b->read16(block + kTlaWindowMapOffset + i * 2u) & 0x3FFu] = true;
        for (const auto& [t, saved] : g_page.tiles) used[t] = true;
        for (const uint16_t t : g_page.spare) used[t] = true;
        for (uint32_t bg = 0; bg < 4u; ++bg) {
            const uint16_t cnt = b->read16(0x04000008u + bg * 2u);
            const uint32_t blocks = (cnt >> 14) == 3u ? 4u : (cnt >> 14) != 0u ? 2u : 1u;
            const uint32_t first = ((cnt >> 8) & 0x1Fu) * 0x800u / 32u;
            for (uint32_t t = first; t < first + blocks * 0x40u && t < 0x400u; ++t)
                used[t] = true;
        }
        for (uint16_t t = kLastTextTile; t >= kFirstTextTile; --t) {
            if (!used[t]) {
                g_page.spare.push_back(t);
                return true;
            }
        }
        return false;
    };
    for (Want& w : wants) {
        if (w.blank) continue;
        uint16_t tile = w.value & 0x3FFu;
        // The blank tile is shared by every empty cell of every window: a
        // blank cell that needs text, including one page two blanked itself
        // earlier (the "< >" moving to its row), gets a free text tile.
        // Drawing into the blank tile put "<" in every empty cell
        // (F12 0009/0010).
        if (tile == kBlankTile) {
            if (g_page.spare.empty() && !borrow_free_tile()) continue;
            tile = g_page.spare.back();
            g_page.spare.pop_back();
            const uint16_t written = static_cast<uint16_t>((w.value & 0xFC00u) | tile);
            const auto own = g_page.cells.find(w.cell);
            const uint16_t original =
                own != g_page.cells.end() ? own->second.original : w.value;
            g_page.cells[w.cell] = {original, written};
            write_cell(b, block, w.cell, written);
        } else if (!ours(w) && !text_tile(tile)) {
            continue;
        }
        if (!text_tile(tile)) continue;
        auto& saved = g_page.tiles[tile];
        const Tile now = read_tile(b, tile);
        if (now != saved.written) saved.original = now;
        if (now != w.pixels) write_tile(b, tile, w.pixels);
        saved.written = w.pixels;
    }

    // Page one's sprites inside the window, right of the cursor hand's column.
    const int left = static_cast<int>(g_page.wx + 2u) * 8;
    const int right = static_cast<int>(g_page.wx + g_page.ww - 1u) * 8;
    const int top = static_cast<int>(g_page.wy) * 8;
    const int bottom = static_cast<int>(g_page.wy + g_page.wh - 1u) * 8;
    for (int i = 0; i < 128; ++i) {
        const uint32_t entry = oam + static_cast<uint32_t>(i) * 8u;
        const uint16_t attr0 = b->read16(entry);
        if ((attr0 & 0x0300u) == 0x0200u) continue;
        int y = attr0 & 0xFF;
        if (y >= 160) y -= 256;
        int x = b->read16(entry + 2u) & 0x1FF;
        if (x >= 256) x -= 512;
        if (x < left || x >= right || y < top || y >= bottom) continue;
        g_page.hidden[i] = attr0;
        b->write16(entry, static_cast<uint16_t>((attr0 & ~0x0300u) | 0x0200u));
    }
    g_page.drawn = true;
}

void frame_end() {
    if (g_page.page != 2) return;
    gba::GbaBus* b = bus();
    if (!b) return;
    // Not in the settings loop any more (a savestate load, or a close this
    // hook did not see): forget page two.
    if (runtime_current_frame() > g_page.seen_frame + 30u) {
        drop_page();
        return;
    }
    draw_page_two(b);
}

int keys_literal(uint32_t* out_value) {
    gba::GbaBus* b = bus();
    if (!b) return 0;
    const uint32_t window = g_cpu.R[8];
    g_page.wx = b->read16(window + kWindowX) & 0xFFu;
    g_page.wy = b->read16(window + kWindowY) & 0xFFu;
    g_page.ww = b->read16(window + kWindowWidth) & 0xFFu;
    g_page.wh = b->read16(window + kWindowHeight) & 0xFFu;
    g_page.seen_frame = runtime_current_frame();
    const uint32_t row = g_cpu.R[9];
    g_page.row = static_cast<int>(row);
    g_marker_window.store(g_page.wx | g_page.wy << 8 | g_page.ww << 16 |
                          g_page.wh << 24);
    g_marker_seen.store(g_page.seen_frame);
    g_marker_page.store(g_page.page);
    const uint32_t pressed = b->read32(kKeyBlock + kPressedOffset);
    const uint32_t repeat = b->read32(kKeyBlock + kRepeatOffset);
    if (pressed & (kKeyA | kKeyB | kKeyStart)) {
        // The game closes the screen and frees its window.
        drop_page();
        return 0;
    }
    if (g_page.page != 2) {
        // Down on Auto-Sleep: the game wraps its hand to row 0, page two's.
        if (row == kLastRow && (repeat & kKeyDown)) {
            g_page.page = 2;
            g_page.drawn = false;
            g_marker_page.store(2);
        }
        return 0;
    }
    if (repeat & kKeyUp) {
        if (row == 0u) {
            // Back to Auto-Sleep: page one first, then the game's own redraw.
            restore_page_one(b);
            g_page.page = 1;
            g_marker_page.store(1);
        }
        *out_value = kUpOnly;
        return 1;
    }
    if (repeat & kKeyDown) {
        // The game moves its hand; not past page two's last row.
        *out_value = row + 1u < static_cast<uint32_t>(kRowCount) ? kDownOnly : kNoKeys;
        return 1;
    }
    if ((repeat & (kKeyLeft | kKeyRight)) && row < static_cast<uint32_t>(kRowCount))
        change_row(static_cast<int>(row), (repeat & kKeyRight) != 0u);
    // Select and Left/Right stay with page two.
    *out_value = kNoKeys;
    return 1;
}

int (*g_next_literal)(uint32_t, uint32_t, uint32_t*) = nullptr;
int (*g_next_alu_immediate)(uint32_t, uint32_t, uint32_t*) = nullptr;
int (*g_next_branch)(uint32_t, uint32_t, uint32_t*) = nullptr;

int settings_literal(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc == kKeyBlockLiteralPc && original == kKeyBlock)
        return keys_literal(out_value);
    return g_next_literal ? g_next_literal(pc, original, out_value) : 0;
}

int settings_alu_immediate(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if ((pc == kSpeechClearXPc || pc == kTlaAutoSleepClearXPc) && original == 0xA0u &&
        g_page.page == 2) {
        *out_value = kValueClearEndX;
        return 1;
    }
    return g_next_alu_immediate ? g_next_alu_immediate(pc, original, out_value) : 0;
}

int settings_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    if (pc == kFrameWaitBranchPc) frame_end();
    return g_next_branch ? g_next_branch(pc, original, out_decision) : 0;
}

// "1/2" or "2/2" and a chevron, right-aligned under the settings window's
// bottom-right corner, as Golden Sun 1's (its runner_main.cpp,
// paint_settings_window).
bool paint_marker(uint8_t* rgb, uint32_t width, uint32_t height) {
    if (!tla_settings_page_overlay_visible()) return false;
    const int page = g_marker_page.load();
    const uint32_t window = g_marker_window.load();
    const int wx = static_cast<int>(window & 0xFFu);
    const int wy = static_cast<int>((window >> 8) & 0xFFu);
    const int ww = static_cast<int>((window >> 16) & 0xFFu);
    const int wh = static_cast<int>(window >> 24);
    const int left = (wx + 1) * 8, top = (wy + 1) * 8;
    // In the expanded view the console's picture sits inside the margins.
    int ox = 0, oy = 0;
    if (const gba::GbaPpu* ppu = gbarecomp::active_ppu()) {
        ox = static_cast<int>(ppu->view_extra_left());
        oy = static_cast<int>(ppu->view_extra_top());
    }

    auto pixel = [&](int x, int y) -> uint8_t* {
        x += ox;
        y += oy;
        if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
            y >= static_cast<int>(height))
            return nullptr;
        return rgb + (static_cast<size_t>(y) * width + x) * 3u;
    };
    auto glyph = tla_menu_glyph;
    uint8_t letter[3] = {0xF8, 0xF8, 0xF8}, shadow[3] = {0x28, 0x28, 0x28};
    bool have_letter = false, have_shadow = false;
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 8; ++gx) {
            const uint8_t v = glyph(kColourSampleLetter, gx, gy);
            const uint8_t* p = pixel(left + kLabelX + gx, top + gy);
            if (!p) continue;
            if (v == kTlaGlyphLetter && !have_letter) {
                std::copy(p, p + 3, letter);
                have_letter = true;
            } else if (v == kTlaGlyphShadow && !have_shadow) {
                std::copy(p, p + 3, shadow);
                have_shadow = true;
            }
        }
    }
    auto put = [&](int x, int y, const uint8_t* colour) {
        if (uint8_t* p = pixel(x, y)) std::copy(colour, colour + 3, p);
    };
    const std::string marker = page == 2 ? kPageTwoMarker : kPageOneMarker;
    const int chevron_width = text_width(std::string(1, kChevronGlyph));
    int x = (wx + ww) * 8 - text_width(marker) - chevron_width;
    const int y = (wy + wh) * 8;
    tla_menu_draw_text(marker, x, y, [&](int px, int py, uint8_t v) {
        put(px, py, v == kTlaGlyphLetter ? letter : shadow);
    });
    x += text_width(marker);
    // The chevron, vertically centred on the 8-pixel line.
    auto letter_at = [&](int gx, int gy) {
        if (gx < 0 || gx >= 8 || gy < 0 || gy >= kChevronRows) return false;
        const int row = page == 2 ? gy : kChevronRows - 1 - gy;
        return glyph(kChevronGlyph, gx, row) == kTlaGlyphLetter;
    };
    const int cy = y + (8 - kChevronRows) / 2;
    for (int gy = 0; gy <= kChevronRows; ++gy) {
        for (int gx = 0; gx <= 8; ++gx) {
            if (letter_at(gx, gy)) put(x + gx, cy + gy, letter);
            else if (letter_at(gx - 1, gy - 1)) put(x + gx, cy + gy, shadow);
        }
    }
    return true;
}

}  // namespace

// For Instant text's caption (src/tla_text_speed.cpp).
bool tla_settings_page_two_showing() { return g_page.page == 2; }

// While page two shows: its help line, and page one's Speech/Auto-Sleep
// values as empty text (src/tla_text.cpp).
bool tla_settings_page_text(uint32_t text_id, std::string& out) {
    if (g_page.page != 2) return false;
    if (text_id >= kHelpIdBase && text_id < kHelpIdBase + kRowCount) {
        out = kRowHelp[text_id - kHelpIdBase];
        return true;
    }
    for (const uint32_t id : kValueTextIds) {
        if (text_id == id) {
            out.clear();
            return true;
        }
    }
    return false;
}

bool tla_settings_page_overlay_visible() {
    const uint32_t window = g_marker_window.load();
    return g_marker_page.load() != 0 &&
        runtime_current_frame() <= g_marker_seen.load() + 30u &&
        ((window >> 16) & 0xFFu) >= 3u && (window >> 24) >= 3u;
}

// Called by the present hook (src/tla_rewind.cpp).
bool tla_settings_page_paint(uint8_t* rgb, uint32_t width, uint32_t height) {
    return paint_marker(rgb, width, height);
}

// Chains in front of the literal, immediate and branch hooks already in
// RunOptions (Instant text's, battle speed's).
void tla_install_settings_page(gbarecomp::RunOptions& options) {
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = settings_literal;
    g_next_alu_immediate = options.thumb_alu_immediate_override;
    options.thumb_alu_immediate_override = settings_alu_immediate;
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = settings_branch;
}
