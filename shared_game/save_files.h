// Nine save files over the games' own three-file list, one copy for both
// runners. The cartridge keeps files 1-3 exactly as before; files 4-9 live in
// the engine's battery extension (gba_save.h, the .sav4-9 companion file),
// which save states carry like the battery save.
//
// The game's file box still shows three rows. A window of three files slides
// over the nine: row r is file base + r. Each runner maps the game's slot
// numbers through the window, answers the game's "how many files" questions
// over all nine, scrolls the window when the cursor passes the top or bottom
// row, and paints the scrollbar.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace gsr::save_files {

constexpr int kFiles = 9;      // what the player sees
constexpr int kRows = 3;       // the game's box, and the files the cartridge holds
constexpr int kLastBase = kFiles - kRows;

// File 9 is the Autosave (autosave.h; Jimmy, 7 November 2026): shown first
// in the list, written only by the autosave, never offered to a manual save.
// It stays last in storage, so files 1-8 keep their places. Every index the
// file box works with is a shown position; the save layers store
// stored_file(shown).
constexpr int kAutosaveFile = kFiles - 1;
constexpr int kAutosaveShown = 0;
inline int stored_file(int shown) { return shown == kAutosaveShown ? kAutosaveFile : shown - 1; }

// The extension starts with this header; each game's records follow it.
// `base` is the window, kept here so a save state restores it with the
// files and the game's own slot number (a row) still names the same file.
constexpr char kMagic[4] = {'G', 'S', 'R', '9'};
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 16;
struct Header {
    char magic[4];
    std::uint8_t version, base;
};

inline bool header_valid(const std::uint8_t* ext) {
    return std::memcmp(ext, kMagic, 4) == 0 && ext[4] == kVersion;
}
inline int window_base(const std::uint8_t* ext) {
    return header_valid(ext) ? std::min<int>(ext[5], kLastBase) : 0;
}
inline void set_window_base(std::uint8_t* ext, int base) {
    std::memcpy(ext, kMagic, 4);
    ext[4] = kVersion;
    ext[5] = static_cast<std::uint8_t>(std::clamp(base, 0, kLastBase));
}

// The window that shows `file`, moving as little as possible.
inline int base_showing(int base, int file) {
    if (file < base) return file;
    if (file >= base + kRows) return file - (kRows - 1);
    return base;
}

// The next file from `from` in direction `dir` (+1 / -1) that `eligible`
// accepts, wrapping round the nine as the game wraps its three; `from`
// itself comes last. -1 when none is eligible.
template <class Eligible>
int step(int from, int dir, Eligible eligible) {
    for (int n = 1; n <= kFiles; ++n) {
        const int f = ((from + dir * n) % kFiles + kFiles) % kFiles;
        if (eligible(f)) return f;
    }
    return -1;
}

// The first eligible file at or after `from`, wrapping. -1 when none.
template <class Eligible>
int first_from(int from, Eligible eligible) {
    return step(from - 1, 1, eligible);
}

// A count over nine files told to code that knows three: none stays none,
// all nine is "full" (3), anything between is 1 or 2, so every test the game
// can make on its three-file count ("any file?", "any room?") still holds.
inline int as_three(int count) {
    if (count <= 0) return 0;
    if (count >= kFiles) return kRows;
    return std::min(count, kRows - 1);
}

// The scrollbar, inside the right border of the file box: a track down the
// rows with a thumb for the three files shown. `put(x, y, index)` paints one
// native pixel in the window palette (1 = letter white, 3 = shadow).
template <class Put>
void paint_scrollbar(Put& put, int x, int top, int height, int base) {
    constexpr std::uint8_t kTrack = 3, kThumb = 1;
    const int thumb = std::max(4, height * kRows / kFiles);
    const int y0 = top + (height - thumb) * base / kLastBase;
    for (int y = top; y < top + height; ++y) put(x + 1, y, kTrack);
    for (int y = y0; y < y0 + thumb; ++y)
        for (int dx = 0; dx < 3; ++dx) put(x + dx, y, kThumb);
    for (int dx = 0; dx < 3; ++dx) {
        put(x + dx, y0 - 1, kTrack);
        put(x + dx, y0 + thumb, kTrack);
    }
}

}  // namespace gsr::save_files
