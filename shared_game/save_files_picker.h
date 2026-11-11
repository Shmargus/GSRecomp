// Nine save files on the games' own file screens, one copy for both runners
// (save_files.h holds the window and scrollbar rules). Each game describes
// its measured routines in a Game (The Broken Seal/src/save_files_tbs.h,
// The Lost Age/src/tla_save_files.cpp) and answers its own save layer; the
// file box, the screens' file questions and the scrollbar are handled here.
//
// Both games build the box the same way (GS1 FACTS / TLA FACTS "Nine save
// files"): picker(row, mode) with the row in r8, the result in r5, the
// three 0x40-byte summaries in the save workspace, rows drawn once before the
// loop, Up/Down through step(row + 2 or + 4, 3), and the screen's bobbing
// title tab kept at [sp+0x10].
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "menu_text.h"
#include "save_files.h"
#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace gsr::save_files {

constexpr std::size_t kSummary = 0x40;
constexpr std::uint32_t kNoEmptyRow = 999;
constexpr std::uint32_t kTabSlot = 0x10;  // sp offset of the title tab
constexpr std::uint16_t kKeyA = 0x001u;
using Entry = std::array<std::uint8_t, kSummary>;
using Entries = std::array<Entry, kFiles>;

struct Game {
    std::size_t extension_bytes;
    // Save layer: the workspace pointer's RAM word, its summary table, the
    // routine that fills it (returns the files present), the scan around it.
    std::uint32_t workspace, table, summaries, scan_open, scan_close;
    // The screens' questions over the three summaries.
    std::uint32_t first_empty, count, clear_flags;
    // Window routines: text line, name, free the text sprites, per-move
    // blit, frame wait; the row step; the box's exit.
    std::uint32_t text, name, clear, blit, wait, step, exit;
    // The picker and the return offsets of its calls.
    std::uint32_t picker, up_return, down_return, blit_return, wait_return, exit_return;
    int mode_reg, window_reg;  // the row is r8
    std::uint32_t newest_sp;   // the oldest save version it accepts
    std::uint32_t tiles_sp, dirty;  // [sp+tiles_sp] + dirty = 1 after a name; 0 = none
    // Row drawing: offsets of the `ldr r0, =text` loads (0 = none), the
    // `movs r2, #x` positions and the `ldr rN, =area text base` load.
    std::uint32_t empty_text, old_text, mismatch_text, not_clear_text, reunion_text;
    std::uint32_t text_x, name_x, area_x, area_text;
    // The Up/Down skip tests (conditional-branch overrides).
    std::array<std::uint32_t, 10> skips;
    // The game's parts: a file in the extension as its summary, and the
    // screens' flags over all nine files.
    Entry (*extension_entry)(int file);
    void (*set_flags)(const Entries& all);
    // A Challenge Mode file (challenge_mode.h), shown red; null = none.
    bool (*challenge)(int file) = nullptr;
    // The party leader's name on a file's row (see show_leaders): character
    // record 0's payload offset and the record size; a cartridge record's
    // size (sector or group); a file's first record in the extension.
    std::uint32_t characters = 0, character_size = 0;
    std::size_t flash_record = 0;
    const std::uint8_t* (*first_record)(int file) = nullptr;
};

struct Picker {
    bool open = false;
    int entry_base = 0;
    std::uint32_t window = 0;
    // Rows to draw once the cleared text has let go of its sprites: the
    // games' own lists clear, wait a frame, then draw.
    int redraw_in = 0;  // picker frame waits
    std::uint32_t mode = 0, newest = 0, tiles = 0;
    bool usable = true;  // the highlighted file can be picked on this screen
    std::array<bool, kFiles> red{};  // Challenge Mode files
};
inline bool g_swallow_a = false;  // A held since it was refused
inline Picker g_picker;
inline bool g_inner = false;  // our own calls into the game run it untouched

inline gba::GbaBus* bus() { return gbarecomp::active_bus(); }
inline std::uint8_t* extension(const Game& g) {
    gba::GbaBus* b = bus();
    return b && b->save().extension_size() >= g.extension_bytes
        ? b->save().extension_data() : nullptr;
}
inline int base(const Game& g) {
    const std::uint8_t* e = extension(g);
    return e ? window_base(e) : 0;
}
inline void set_base(const Game& g, int value) {
    std::uint8_t* e = extension(g);
    if (!e || (header_valid(e) && window_base(e) == value)) return;
    set_window_base(e, value);
    bus()->save().mark_extension_dirty();
}
constexpr std::size_t kRecordHeader = 0x10;  // record bytes before the payload
inline bool present(const std::uint8_t* record) {
    return std::memcmp(record, "CAMELOT", 7) == 0;
}
// A record the game's own load would accept: present, and its +8 sum of the
// first `summed` payload bytes matches (the rule answer_record writes). A
// damaged file reads as absent, as the cartridge's damaged groups do; its
// bytes stay in the extension until the player saves over it.
inline bool valid(const std::uint8_t* record, std::size_t summed) {
    if (!present(record)) return false;
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < summed; ++i) sum += record[kRecordHeader + i];
    return (sum & 0xFFFFu) == static_cast<std::uint32_t>(record[8] | (record[9] << 8));
}
inline std::uint32_t workspace(const Game& g) { return bus()->read32(g.workspace); }

// A file in the extension, answered as both games' save layers answer their
// flash records (TBS 0x08005A78/0x08005920/0x08005AC0, TLA 0x08015E44/
// 0x08015CA4/0x08015E8C). A record is `bytes` long: "CAMELOT", +7 `number`
// (the block or slot the game names it by), +8 u16 sum of the first `summed`
// payload bytes, +0xA generation (+1, back to 1 past 0xFDE8), payload from
// +0x10. Load copies the record to the game's `image` and the payload to
// r1; write takes the payload from r1. Returns to the game with its answer.
enum class RecordCall { Load, Write, Erase };
// Writes record `r` from the payload at guest `data`, as the save layers do.
inline void fill_record(std::uint8_t* r, std::size_t bytes, std::size_t summed,
                        std::uint8_t number, std::uint32_t data) {
    gba::GbaBus* b = bus();
    const std::size_t payload = bytes - kRecordHeader;
    std::uint16_t generation = present(r)
        ? static_cast<std::uint16_t>(r[0xA] | (r[0xB] << 8)) : 0u;
    generation = generation + 1u > 0xFDE8u ? 1u : static_cast<std::uint16_t>(generation + 1u);
    std::memset(r, 0, kRecordHeader);
    std::memcpy(r, "CAMELOT", 7);
    r[7] = number;
    for (std::size_t i = 0; i < payload; ++i) r[kRecordHeader + i] = b->read8(data + i);
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < summed; ++i) sum += r[kRecordHeader + i];
    r[8] = static_cast<std::uint8_t>(sum);
    r[9] = static_cast<std::uint8_t>(sum >> 8);
    r[0xA] = static_cast<std::uint8_t>(generation);
    r[0xB] = static_cast<std::uint8_t>(generation >> 8);
    b->save().mark_extension_dirty();
}
inline void answer_record(std::uint32_t pc, RecordCall op, std::uint8_t* r,
                          std::size_t bytes, std::size_t summed, std::uint8_t number,
                          std::uint32_t image) {
    gba::GbaBus* b = bus();
    const std::uint32_t data = g_cpu.R[1];
    const std::size_t payload = bytes - kRecordHeader;
    if (op == RecordCall::Load ? !valid(r, summed) : op == RecordCall::Erase && !present(r))
        return gbarecomp::guest_return_now(pc, 1u);
    if (op == RecordCall::Load) {
        for (std::size_t i = 0; i < bytes; ++i) b->write8(image + i, r[i]);
        for (std::size_t i = 0; i < payload; ++i) b->write8(data + i, r[kRecordHeader + i]);
        return gbarecomp::guest_return_now(pc, 0u);
    }
    if (op == RecordCall::Erase) {
        std::memset(r, 0xFF, bytes);
        b->save().mark_extension_dirty();
        return gbarecomp::guest_return_now(pc, 0u);
    }
    fill_record(r, bytes, summed, number, data);
    for (std::size_t i = 0; i < bytes; ++i) b->write8(image + i, r[i]);
    gbarecomp::guest_return_now(pc, 0u);
}
inline std::uint32_t literal_at(std::uint32_t pc) {
    const std::uint16_t op = bus()->read16(pc);
    return bus()->read32(((pc + 4u) & ~3u) + (op & 0xFFu) * 4u);
}
inline std::uint32_t immediate_at(std::uint32_t pc) { return bus()->read16(pc) & 0xFFu; }
inline std::uint32_t call(std::uint32_t pc, std::uint32_t r0 = 0, std::uint32_t r1 = 0,
                          std::uint32_t r2 = 0, std::uint32_t r3 = 0) {
    std::uint32_t result = 0;
    g_inner = true;
    gbarecomp::guest_call(pc | 1u, r0, r1, r2, r3, &result);
    g_inner = false;
    return result;
}

// The file a row of the box stores (the save layers' answer).
inline int row_file(const Game& g, int row) { return stored_file(base(g) + row); }

// The Autosave's row shows "Auto" where the hero's name goes (Jimmy, 11
// November 2026: "Autosave" ran into the text beside it); the name field is
// 12 bytes from +0x10, plain letters, 0-terminated (every save in Saves/).
constexpr std::size_t kNameAt = 0x10, kNameBytes = 12;
constexpr char kAutosaveName[] = "Auto";
static_assert(sizeof kAutosaveName <= kNameBytes);

// An empty Autosave row reads "No autosaved data" (Jimmy, 8 November 2026)
// instead of the game's usual empty-file line.
//
// How: the file box draws each row's line by calling the game's text routine
// (Game::text) with the text id in r0 and the row's y (row * 16) in r3. For
// an empty row that id is the literal at picker + empty_text (0 in both
// games), and the routine then starts decoding the line under the same id
// (TLA 0x08041FA4 -> 0x0803B094(id, 1); The Broken Seal 0x0801E74C ->
// 0x08018038(id, 1)). When such a call is for the Autosave's row (row 0 of
// the first window) we remember the id, and each game's text hook swaps in
// our line for it. The next text call, or the box closing, forgets it again.
// Only the letters drawn change: no save is read or written.
constexpr char kNoAutosave[] = "No autosaved data";
struct AutosaveLine {
    bool armed = false;
    std::uint32_t id = 0;
    int pos = -1;  // The Broken Seal's per-character position
};
inline AutosaveLine g_autosave_line;
inline void note_text_call(const Game& g, int shown, std::uint32_t id) {
    g_autosave_line = {};
    g_autosave_line.armed = g_picker.open && g.empty_text && shown == kAutosaveShown &&
                            id == literal_at(g.picker + g.empty_text);
    g_autosave_line.id = id;
}
// The whole line in English (The Lost Age's replacement translates it).
inline bool autosave_line(std::uint32_t text_id, std::string& out) {
    if (!g_autosave_line.armed || !g_picker.open || text_id != g_autosave_line.id) return false;
    out = kNoAutosave;
    return true;
}
// A character at a time in the player's language (The Broken Seal's decoder
// hook): the letters, then the 0 that ends the line; the same line again
// starts over.
inline bool autosave_character(std::uint32_t text_id, std::uint32_t* character) {
    std::string line;
    if (!autosave_line(text_id, line)) return false;
    line = menu_text(line.c_str());
    AutosaveLine& a = g_autosave_line;
    if (a.pos < 0) a.pos = 0;
    const auto pos = static_cast<std::size_t>(a.pos);
    *character = pos < line.size() ? static_cast<unsigned char>(line[pos]) : 0u;
    a.pos = pos < line.size() ? a.pos + 1 : -1;
    return true;
}

// Each file's row names the party leader (Jimmy, 11 November 2026), not the
// game's own choice. The header builder (TBS 0x0801F818, TLA 0x08043358; the
// same bytes in all ten ROMs) copies the 12-byte name of the field identity's
// record (word 0x02000434 / 0x02000454: Isaac / Felix, even when he is not
// in the party) to summary +0x10, and the party order's first four ids
// (0x080796C4 / 0x080AFE78 from 0x02000438 / 0x02000458; 0xFF ends) to
// +0x2C. So the leader is +0x2C, and his name is in the file's own character
// records (ids 0..7, as the record lookups 0x08077394 / 0x08016CA4 take
// them). Only the row's letters change; no save is read differently or
// written. A cartridge file's record is the one the summary was copied from.
// The builders take the level from record +0xF to summary +0x1C and the
// class from record +0x129 to +0x1D (TBS 0x0801F890, TLA 0x080433CE).
constexpr std::size_t kLeaderAt = 0x2C, kSummaryOwnBytes = 0x38;
constexpr std::size_t kLevelAt = 0x1C, kRecordLevel = 0xF;
constexpr std::size_t kClassAt = 0x1D, kRecordClass = 0x129;
constexpr unsigned kCharacters = 8;
inline void show_leaders(const Game& g, Entries& stored) {
    if (!g.first_record) return;
    std::vector<std::uint8_t> flash;
    if (gba::GbaBus* b = bus(); b && b->save().flash_enabled()) flash = b->save().flash_bytes();
    for (int f = 0; f < kFiles; ++f) {
        Entry& e = stored[f];
        const unsigned leader = e[kLeaderAt];
        if (!present(e.data()) || leader >= kCharacters) continue;
        const std::uint8_t* r = nullptr;
        if (f >= kRows) r = extension(g) ? g.first_record(f) : nullptr;
        else
            for (std::size_t s = 0; !r && s + g.flash_record <= flash.size(); s += g.flash_record)
                if (std::memcmp(flash.data() + s, e.data(), kSummaryOwnBytes) == 0) r = flash.data() + s;
        if (!r || !present(r)) continue;
        const std::uint8_t* hero = r + kRecordHeader + g.characters + leader * g.character_size;
        std::memcpy(e.data() + kNameAt, hero, kNameBytes);
        // The level too (Jimmy, 11 November). +0x1C is also "file in use":
        // every reader in both ROMs (pickers, detail panels, first-empty and
        // Clear Data questions) and ours only test it for zero or draw it,
        // so a nonzero level only ever replaces a nonzero one.
        // The class (+0x1D, record +0x129) is only drawn by the detail
        // panels (TBS 0x080201E4, TLA 0x08043C7C); 0 is a class, so it
        // follows the level's test.
        if (e[kLevelAt] != 0 && hero[kRecordLevel] != 0) {
            e[kLevelAt] = hero[kRecordLevel];
            e[kClassAt] = hero[kRecordClass];
        }
    }
}

// All nine summaries, in the order the box shows them. The cartridge's come
// from the game's own routine (the flash scan must be open); the table it
// fills is put back as it was.
inline Entries all_entries(const Game& g) {
    Entries stored{};
    gba::GbaBus* b = bus();
    const std::uint32_t table = workspace(g) + g.table;
    std::uint8_t saved[kRows * kSummary];
    for (std::size_t i = 0; i < sizeof saved; ++i) saved[i] = b->read8(table + i);
    call(g.summaries);
    for (int f = 0; f < kRows; ++f)
        for (std::size_t i = 0; i < kSummary; ++i) stored[f][i] = b->read8(table + f * kSummary + i);
    for (std::size_t i = 0; i < sizeof saved; ++i) b->write8(table + i, saved[i]);
    if (extension(g))
        for (int f = kRows; f < kFiles; ++f) stored[f] = g.extension_entry(f);
    show_leaders(g, stored);
    Entries out{};
    for (int shown = 0; shown < kFiles; ++shown) out[shown] = stored[stored_file(shown)];
    Entry& autosave = out[kAutosaveShown];
    if (present(autosave.data())) {
        std::memset(autosave.data() + kNameAt, 0, kNameBytes);
        std::memcpy(autosave.data() + kNameAt, kAutosaveName, sizeof kAutosaveName);
    }
    return out;
}

// The three summaries of the window at `window`; the count the summary
// routine returns.
inline std::uint32_t fill_table(const Game& g, int window, const Entries& all) {
    gba::GbaBus* b = bus();
    const std::uint32_t table = workspace(g) + g.table;
    std::uint32_t count = 0;
    for (int row = 0; row < kRows; ++row) {
        const Entry& e = all[window + row];
        for (std::size_t i = 0; i < kSummary; ++i) b->write8(table + row * kSummary + i, e[i]);
        if (present(e.data())) ++count;
    }
    return count;
}

// Which files each picker mode offers (the pickers' own row tests): +0x1C
// level (0 = empty), +0x31 Clear Data, +0x32 the transfer mark, +0x33
// (TLA's mode 6 refuses it). Saving (mode 0) never offers the Autosave.
inline bool eligible(std::uint32_t mode, const Entry& e, int shown) {
    const bool used = e[0x1C] != 0;
    switch (mode) {
    case 0: return shown != kAutosaveShown;
    case 1: return used && e[0x31] == 0;
    case 4: return used && e[0x32] != 0;
    case 5: return used && e[0x31] != 0;
    case 6: return used && e[0x33] == 0;
    default: return used;
    }
}

// The picker's row drawing for the rows now in the table.
inline void draw_rows(const Game& g) {
    gba::GbaBus* b = bus();
    const std::uint32_t pk = g.picker, window = g_picker.window, mode = g_picker.mode;
    const std::uint32_t table = workspace(g) + g.table;
    for (int row = 0; row < kRows; ++row) {
        const std::uint32_t e = table + row * kSummary, at = e + 0x1C;
        const std::uint32_t y = row * 16u;
        std::uint32_t text = 0;
        if (b->read8(at) == 0) text = g.empty_text;
        else if (b->read16(at + 0x1A) < g_picker.newest) text = g.old_text;
        else if (b->read32(at + 4) != b->read32(at + 0x1C)) text = g.mismatch_text;
        else if (mode == 5 && b->read8(at + 0x15) == 0) text = g.not_clear_text;
        else if (mode == 6 && g.reunion_text && b->read8(at + 0x17) != 0) text = g.reunion_text;
        if (text) {
            note_text_call(g, base(g) + row, literal_at(pk + text));
            call(g.text, literal_at(pk + text), window, immediate_at(pk + g.text_x), y);
            g_autosave_line = {};
            continue;
        }
        call(g.name, e + 0x10, window, immediate_at(pk + g.name_x), y);
        call(g.text, literal_at(pk + g.area_text) + b->read16(at + 2), window,
             immediate_at(pk + g.area_x), y);
        if (g.dirty) b->write8(g_picker.tiles + literal_at(pk + g.dirty), 1u);
    }
}

// Clears the box's row text, keeping the title tab. A window's sprites are a
// list: window +0 first, +4 last, each sprite's next at +0 (so the window is
// the list's own head link); the clear routine frees the whole list.
inline void clear_rows(const Game& g, std::uint32_t window, std::uint32_t tab) {
    gba::GbaBus* b = bus();
    const std::uint32_t second = tab ? b->read32(tab) : 0u;
    std::uint32_t link = window;
    while (second && b->read32(link) && b->read32(link) != tab) link = b->read32(link);
    const bool kept = second && b->read32(link) == tab;
    if (kept) b->write32(link, b->read32(second));
    call(g.clear, window);
    if (!kept) return;
    b->write32(second, 0u);
    b->write32(window, tab);
    b->write32(window + 4u, second);
}

inline int file_count(const Entries& all) {
    return static_cast<int>(std::count_if(all.begin(), all.end(),
        [](const Entry& e) { return present(e.data()); }));
}

// The screens' questions over nine files, inside the game's flash scan:
// `answer(all)` gives the result; -9 when the scan fails.
template <class Answer>
void answer_in_scan(const Game& g, std::uint32_t pc, Answer answer) {
    std::uint32_t result = static_cast<std::uint32_t>(-9);
    if (call(g.scan_open) == 0) result = answer(all_entries(g));
    call(g.scan_close);
    gbarecomp::guest_return_now(pc, result);
}

// count(non_clear): the files, less Clear Data files if asked.
inline void count_files(const Game& g, std::uint32_t pc) {
    const bool non_clear = g_cpu.R[0] != 0;
    answer_in_scan(g, pc, [&](const Entries& all) {
        int n = file_count(all);
        if (non_clear)
            n -= static_cast<int>(std::count_if(all.begin(), all.end(),
                [](const Entry& e) { return e[0x31] != 0; }));
        return static_cast<std::uint32_t>(as_three(n));
    });
}

// The Clear Data question: the game's flags, then the file count, + 100
// when every file is Clear Data.
inline void clear_flags(const Game& g, std::uint32_t pc) {
    answer_in_scan(g, pc, [&](const Entries& all) {
        g.set_flags(all);
        const int files = file_count(all);
        auto result = static_cast<std::uint32_t>(as_three(files));
        const int clear = static_cast<int>(std::count_if(all.begin(), all.end(),
            [](const Entry& e) { return present(e.data()) && e[0x31] != 0; }));
        if (files != 0 && clear == files) result += 0x64u;
        return result;
    });
}

// Function-entry observer for the file screens (the game's save layer is
// answered by the game's own hook first). One comparison chain while no
// file screen is up.
inline void on_entry(const Game& g, std::uint32_t pc) {
    if (g_inner || !extension(g)) return;
    const std::uint32_t pk = g.picker;
    const std::uint32_t lr = g_cpu.R[14] & ~1u;
    if (pc == g.summaries) {
        // Always ours: even the first window shows the Autosave first.
        gbarecomp::guest_return_now(pc, fill_table(g, base(g), all_entries(g)));
    } else if (pc == g.first_empty) {
        // Copy's destination: never the Autosave.
        const auto all = all_entries(g);
        const auto it = std::find_if(all.begin() + kAutosaveShown + 1, all.end(),
            [](const Entry& e) { return e[0x1C] == 0; });
        if (it == all.end()) return gbarecomp::guest_return_now(pc, kNoEmptyRow);
        const int file = static_cast<int>(it - all.begin());
        set_base(g, base_showing(base(g), file));
        gbarecomp::guest_return_now(pc, static_cast<std::uint32_t>(file - base(g)));
    } else if (pc == g.count) {
        count_files(g, pc);
    } else if (pc == g.clear_flags) {
        clear_flags(g, pc);
    } else if (pc == pk) {
        // Open on the first file the mode offers from the row asked for,
        // across all nine; the picker finds it itself once it is shown.
        g_picker = {true, base(g), 0};
        if (g.challenge)
            for (int f = 0; f < kFiles; ++f) g_picker.red[f] = g.challenge(stored_file(f));
        const std::uint32_t mode = g_cpu.R[1];
        g_picker.mode = mode;
        const int row = std::clamp(static_cast<int>(g_cpu.R[0]), 0, kRows - 1);
        const auto all = all_entries(g);
        int file = first_from(base(g) + row,
            [&](int f) { return eligible(mode, all[f], f); });
        // Saving from the Autosave (Jimmy, 8 November 2026): start on the
        // first empty file, not file 1; with all eight full, file 1 as
        // before (the game still asks before saving over a file).
        if (mode == 0 && base(g) + row == kAutosaveShown) {
            const auto empty = std::find_if(all.begin() + kAutosaveShown + 1, all.end(),
                [](const Entry& e) { return e[0x1C] == 0; });
            if (empty != all.end()) file = static_cast<int>(empty - all.begin());
        }
        if (file < 0) return;
        // In the window: start there (Save's own search takes every row,
        // so it would stay on the Autosave).
        if (file >= base(g) && file < base(g) + kRows) {
            g_cpu.R[0] = static_cast<std::uint32_t>(file - base(g));
            return;
        }
        set_base(g, base_showing(base(g), file));
        fill_table(g, base(g), all);
        g_cpu.R[0] = static_cast<std::uint32_t>(file - base(g));
    } else if (pc == g.step && (lr == pk + g.up_return || lr == pk + g.down_return)) {
        // Up/Down step one file through all nine (Jimmy, 3 November: stop
        // on every file); past the box's edge the window moves instead of
        // wrapping.
        const int was = base(g), from = was + static_cast<int>(g_cpu.R[8]);
        const int file = step(from, lr == pk + g.up_return ? -1 : 1, [](int) { return true; });
        const int window = base_showing(was, file);
        if (window != was) {
            set_base(g, window);
            fill_table(g, window, all_entries(g));
            // Clear the box's text now; its rows are drawn at the second
            // frame wait from here, once a frame has gone by.
            const std::uint32_t sp = g_cpu.R[13];
            clear_rows(g, g_cpu.R[g.window_reg], bus()->read32(sp + kTabSlot));
            g_picker.redraw_in = 2;
            g_picker.mode = g_cpu.R[g.mode_reg];
            g_picker.newest = bus()->read32(sp + g.newest_sp);
            g_picker.tiles = g.dirty ? bus()->read32(sp + g.tiles_sp) : 0u;
        }
        gbarecomp::guest_return_now(pc, static_cast<std::uint32_t>(file - window));
    } else if (pc == g.blit && lr == pk + g.blit_return) {
        g_picker.window = g_cpu.R[0];
        Entry e{};
        const int shown = base(g) + static_cast<int>(g_cpu.R[8] % kRows);
        const std::uint32_t row = workspace(g) + g.table + (g_cpu.R[8] % kRows) * kSummary;
        for (std::size_t i = 0; i < kSummary; ++i) e[i] = bus()->read8(row + i);
        g_picker.usable = eligible(g_picker.mode, e, shown);
    } else if (pc == g.wait && lr == pk + g.wait_return) {
        if (g_picker.redraw_in > 0 && --g_picker.redraw_in == 0) draw_rows(g);
    } else if (pc == g.text) {
        // A row's line, or any other text: see AutosaveLine.
        note_text_call(g, base(g) + static_cast<int>(g_cpu.R[3] / 16u), g_cpu.R[0]);
    } else if (pc == g.exit && lr == pk + g.exit_return) {
        // Cancelled: back to the window the game's slot number belongs to.
        if (static_cast<std::int32_t>(g_cpu.R[5]) == -1) set_base(g, g_picker.entry_base);
        g_picker = {};
        g_autosave_line = {};
    }
}

// Conditional-branch override for the file box's skip tests: never skip.
// -1 not ours, 0 the game's own decision, 1 changed.
inline int branch(const Game& g, std::uint32_t pc, std::uint32_t original,
                  std::uint32_t* out_decision) {
    if (pc < g.picker || pc - g.picker > 0x1000u) return -1;
    const std::uint32_t at = pc - g.picker;
    if (std::find(g.skips.begin(), g.skips.end(), at) == g.skips.end() || at == 0) return -1;
    *out_decision = 0u;
    return original != 0u ? 1 : 0;
}

// Keys before the game sees them: A on a file this screen cannot use does
// nothing (held A stays refused until it is let go).
inline std::uint16_t keyinput(std::uint16_t keys) {
    const bool held = (keys & kKeyA) == 0;
    if (!held) g_swallow_a = false;
    else if (g_picker.open && !g_picker.usable) g_swallow_a = true;
    return g_swallow_a ? static_cast<std::uint16_t>(keys | kKeyA) : keys;
}

// The file box's scrollbar, inside its right border, while a picker is up,
// and Challenge Mode files in red: their row's blue window turns red, the
// letters stay. `pixel(x, y)` is a native pixel's RGB, null off the picture.
template <class Put, class Pixel>
bool paint(const Game& g, Put& put, Pixel& pixel) {
    if (!g_picker.open || !g_picker.window || !extension(g)) return false;
    gba::GbaBus* b = bus();
    const int x = b->read16(g_picker.window + 0xC), y = b->read16(g_picker.window + 0xE);
    const int w = b->read16(g_picker.window + 8), h = b->read16(g_picker.window + 0xA);
    if (w < 3 || h < 3) return false;
    // Rows are 16 pixels from 4 inside the window's corner, the border 4
    // wide (Jimmy's F12 0086, TBS: window tile (1, 2), rows from y 20). The
    // window's blues become the same shades of red; letters, shadows and the
    // game's own highlight are not blue and stay.
    for (int row = 0; row < kRows; ++row) {
        const int file = base(g) + row;
        if (file >= kFiles || !g_picker.red[file]) continue;
        const int top = y * 8 + 4 + row * 16;
        const int bottom = std::min(top + 16, (y + h) * 8 - 4);
        for (int py = top; py < bottom; ++py)
            for (int px = x * 8 + 4; px < (x + w) * 8 - 4; ++px) {
                std::uint8_t* p = pixel(px, py);
                if (!p || p[2] <= p[0] + 16 || p[2] <= p[1]) continue;
                const std::uint8_t blue = p[2], green = p[1];
                p[0] = blue;
                p[1] = p[2] = static_cast<std::uint8_t>(green / 3);
            }
    }
    paint_scrollbar(put, (x + w - 1) * 8 + 2, (y + 1) * 8, (h - 2) * 8, base(g));
    return true;
}
// The scrollbar alone (a game without Challenge Mode files).
template <class Put>
bool paint(const Game& g, Put& put) {
    auto none = [](int, int) -> std::uint8_t* { return nullptr; };
    return paint(g, put, none);
}

}  // namespace gsr::save_files
