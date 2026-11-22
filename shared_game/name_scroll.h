// Scrolling names, one copy for both runners. A Psynergy list prints each
// name with the game's text printer, print(text id, window, x, y), and its PP
// cost to the right. A name wider than the widest class spell's (Random
// Psynergy's enemy skills: Lucid Prophecy, Water Blessing ...) would run into
// the cost, so it is printed cut to that width and then, every few frames,
// printed again from a later letter, back and forth, as a marquee.
//
// Each game's FACTS.md "Scrolling names" (22 November 2026), both games the
// same: the printer decodes the line, draws it into a fresh buffer filled
// with the window's background and copies whole 8-pixel tiles from the
// start cell to the line's end, reusing the text tile already in each cell
// (window system block B: tilemap shadow, 32 cells a row, cell =
// (y/8 + window y + 1) * 32 + window x + x/8 + 1, window x/y halfwords at
// +0xC/+0xE). So printing again over the same cells replaces the old letters
// and touches nothing past the line's end. The text colour comes from B's
// mode byte and colour halfword, kept per name and put back for our prints.
//
// A name stops scrolling when its window moves, or the cell where it starts
// no longer holds the tile it was printed into (the window closed or the
// list printed something else there), or the list prints that place again.
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "gs_text_encode.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "trace_once.h"

namespace gsr::name_scroll {

struct Game {
    std::uint32_t printer_pc;              // print(text id, window, x, y)
    const std::uint32_t* sites;            // the lists' name prints, return address
    int site_count;
    std::uint32_t block_slot;              // IWRAM word holding the window block B
    std::uint32_t tilemap;                 // B offset of the tilemap shadow
    std::uint32_t mode, colour;            // B offsets: mode byte, colour halfword
    std::size_t text_table, tree_table;    // ROM offsets (gs_text_encode.h)
    std::uint32_t advances;                // ROM address: 224 font advances, glyph - 32
    std::uint32_t name_text;               // ability name = this + id
    // Whether ability `id`'s name may scroll now, and the ids whose names set
    // the width (the class spells).
    bool (*wanted)(std::uint16_t id);
    std::vector<std::uint16_t> (*reference)();
};

// Frames the start and the end stay still, and frames per letter.
constexpr int kHold = 60, kStep = 10;

struct Slot {
    std::uint32_t text_id, window, x, y;
    std::uint8_t mode;
    std::uint16_t colour;
    std::int16_t window_x, window_y;
    std::uint16_t tile = 0;                 // the start cell's tile, once seen
    std::string name;                       // the whole name, game characters
    int first = 0, last = 0, timer = 0, direction = 1;
};

struct State {
    std::vector<Slot> slots;
    const Game* width_for = nullptr;
    unsigned width = 0;
    std::vector<std::uint8_t> advances;
    // The line our next print hands the decoder.
    bool armed = false;
    std::uint32_t armed_id = 0;
    std::string armed_text;
    int armed_pos = -1;
    bool printing = false;
};
inline State state;

inline unsigned width_of(const std::string& s) {
    unsigned w = 0;
    for (const unsigned char c : s)
        if (c >= 32u && c - 32u < state.advances.size()) w += state.advances[c - 32u];
    return w;
}

inline bool decode(const Game& g, gba::GbaBus& b, std::uint32_t text_id, std::string& out) {
    const std::span<const std::uint8_t> rom(b.rom_ptr(), b.rom_size());
    mods::TextEncoder decoder(rom, g.text_table, g.tree_table);
    return decoder.decode(text_id, &out) && !out.empty();
}

// The widest reference name, once per game.
inline unsigned limit(const Game& g, gba::GbaBus& b) {
    if (state.width_for == &g) return state.width;
    state.width_for = &g;
    state.width = 0;
    const std::uint32_t at = g.advances - 0x08000000u;
    if (!b.rom_ptr() || at + 224u > b.rom_size()) return 0;
    state.advances.assign(b.rom_ptr() + at, b.rom_ptr() + at + 224u);
    for (const std::uint16_t id : g.reference()) {
        std::string name;
        if (decode(g, b, g.name_text + id, name)) state.width = std::max(state.width, width_of(name));
    }
    return state.width;
}

// Letters `first` on that fit the width, then spaces up to it.
inline std::string window_of(const std::string& name, int first, unsigned width) {
    std::string out;
    unsigned w = 0;
    for (std::size_t i = static_cast<std::size_t>(first); i < name.size(); ++i) {
        const unsigned a = width_of(name.substr(i, 1));
        if (w + a > width) break;
        out += name[i];
        w += a;
    }
    const unsigned space = width_of(" ");
    while (space && w + space <= width) out += ' ', w += space;
    return out;
}

// The last first letter that still ends the name inside the width.
inline int last_first(const std::string& name, unsigned width) {
    int first = 0;
    while (first + 1 < static_cast<int>(name.size()) &&
           width_of(name.substr(static_cast<std::size_t>(first))) > width)
        ++first;
    return first;
}

inline void arm(std::uint32_t text_id, std::string text) {
    state.armed = true;
    state.armed_id = text_id;
    state.armed_text = std::move(text);
    state.armed_pos = -1;
}

// At the printer's entry (r: the guest registers).
inline void on_entry(const Game& g, std::uint32_t pc, const std::uint32_t* r) {
    if (pc != g.printer_pc || state.printing) return;
    state.armed = false;
    const std::uint32_t lr = r[14] & ~1u;
    const bool site = std::find(g.sites, g.sites + g.site_count, lr) != g.sites + g.site_count;
    const bool name_line = r[0] >= g.name_text && r[0] - g.name_text < 0x400u &&
                           g.wanted(static_cast<std::uint16_t>(r[0] - g.name_text));
    if (name_line)
        trace_once(0x2ull << 56 | std::uint64_t{lr} << 16 | (r[0] & 0xFFFFu),
                   "[name-scroll] print text=0x%X lr=0x%08X site=%d window=0x%08X x=%u y=%u", r[0], lr, site, r[1],
                   r[2], r[3]);
    if (!site) return;
    auto& slots = state.slots;
    slots.erase(std::remove_if(slots.begin(), slots.end(),
                               [&](const Slot& s) { return s.window == r[1] && s.x == r[2] && s.y == r[3]; }),
                slots.end());
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b || !name_line) return;
    const unsigned width = limit(g, *b);
    std::string name;
    const bool decoded = width && decode(g, *b, r[0], name);
    trace_once(0x3ull << 56 | (r[0] & 0xFFFFu), "[name-scroll] text=0x%X limit=%u decoded=%d name=\"%s\" width=%u",
               r[0], width, decoded, name.c_str(), decoded ? width_of(name) : 0u);
    if (!decoded || width_of(name) <= width) return;
    const std::uint32_t block = b->read32(g.block_slot);
    Slot s{r[0], r[1], r[2], r[3], b->read8(block + g.mode), b->read16(block + g.colour),
           static_cast<std::int16_t>(b->read16(r[1] + 0xC)), static_cast<std::int16_t>(b->read16(r[1] + 0xE))};
    s.name = name;
    s.last = last_first(name, width);
    s.timer = kHold;
    slots.push_back(s);
    arm(r[0], window_of(name, 0, width));
}

// The decoder's whole line for `text_id` (The Lost Age's text hook). It
// stays armed until the next print or frame, in case the line is decoded
// more than once.
inline bool line_text(std::uint32_t text_id, std::string& out) {
    if (!state.armed || text_id != state.armed_id) return false;
    out = state.armed_text;
    trace_once(0x4ull << 56 | (text_id & 0xFFFFu), "[name-scroll] served text=0x%X \"%s\"", text_id, out.c_str());
    return true;
}

// The same a character at a time (The Broken Seal's decoder hook).
inline bool text_character(std::uint32_t text_id, std::uint32_t* character) {
    if (!state.armed || text_id != state.armed_id) return false;
    trace_once(0x4ull << 56 | (text_id & 0xFFFFu), "[name-scroll] served text=0x%X \"%s\"", text_id,
               state.armed_text.c_str());
    State& s = state;
    if (s.armed_pos < 0) s.armed_pos = 0;
    const auto pos = static_cast<std::size_t>(s.armed_pos);
    *character = pos < s.armed_text.size() ? static_cast<unsigned char>(s.armed_text[pos]) : 0u;
    s.armed_pos = pos < s.armed_text.size() ? s.armed_pos + 1 : -1;  // a second pass starts again
    return true;
}

// Once a frame (emulation thread, outside an interrupt): moves each
// scrolling name on and prints it again when its first letter changes.
inline void frame(const Game& g) {
    State& st = state;
    if (st.slots.empty()) return;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return;
    st.armed = false;
    const std::uint32_t block = b->read32(g.block_slot);
    for (std::size_t i = 0; i < st.slots.size();) {
        Slot& s = st.slots[i];
        const auto wx = static_cast<std::int16_t>(b->read16(s.window + 0xC));
        const auto wy = static_cast<std::int16_t>(b->read16(s.window + 0xE));
        const int cell = ((static_cast<int>(s.y) >> 3) + wy + 1) * 32 + wx + (static_cast<int>(s.x) >> 3) + 1;
        const std::uint16_t tile = cell >= 0 && cell < 0x280 ? b->read16(block + g.tilemap + 2u * cell) : 0;
        if (wx != s.window_x || wy != s.window_y || tile == 0 || (s.tile && tile != s.tile)) {
            trace_once(0x5ull << 56 | (s.text_id & 0xFFFFu),
                       "[name-scroll] stop text=0x%X window %d,%d->%d,%d tile 0x%X->0x%X first=%d", s.text_id,
                       s.window_x, s.window_y, wx, wy, s.tile, tile, s.first);
            st.slots.erase(st.slots.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        s.tile = tile;
        ++i;
        if (--s.timer > 0) continue;
        s.first += s.direction;
        if (s.first >= s.last) s.first = s.last, s.direction = -1, s.timer = kHold;
        else if (s.first <= 0) s.first = 0, s.direction = 1, s.timer = kHold;
        else s.timer = s.direction > 0 ? kStep : kStep / 2;
        const std::uint8_t mode = b->read8(block + g.mode);
        const std::uint16_t colour = b->read16(block + g.colour);
        b->write8(block + g.mode, s.mode);
        b->write16(block + g.colour, s.colour);
        arm(s.text_id, window_of(s.name, s.first, st.width));
        st.printing = true;
        gbarecomp::guest_call(g.printer_pc | 1u, s.text_id, s.window, s.x, s.y, nullptr);
        st.printing = false;
        st.armed = false;
        b->write8(block + g.mode, mode);
        b->write16(block + g.colour, colour);
    }
}

// Savestate loaded or a new game: nothing scrolls until a list prints again.
inline void reset() {
    state.slots.clear();
    state.armed = false;
}

}  // namespace gsr::name_scroll
