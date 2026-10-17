#pragma once

// F12 history, shared by both games: the last 2 seconds of presented frames.
//
// Every presented frame is kept in a ring of 120 (2 s at 60 frames a second):
// the exact picture shown, the host time since the previous one, each row's
// display registers as the row was drawn, and -- when the game's graphics
// card renderer had a capture -- that frame's renderer snapshot (the game's
// own format, replayed by its bench tool). write() saves the ring, oldest
// first, to logs/<prefix>NNNN/:
//   frame_NNN.ppm   the picture shown
//   frame_NNN.bin   the renderer snapshot, when there was one
//   index.csv       one line per frame (guest frame, host ms, flags, files)
//   rows.csv        each frame's registers per row (IO 0x00..0x57 as
//                   halfwords, then BG2/BG3's x/y reference)
//   memory.bin      the game's memory at the press (EWRAM 0x40000, IWRAM
//                   0x8000, IO 0x400, palettes 0x400, VRAM 0x18000, OAM 0x400)
//   gpu_frame.bin   the newest snapshot again, for report tools
//   <note file>     the caller's note, when it gave one
// The folder prefix tells the games apart: "gpu_rewind_" (The Broken Seal),
// "tla_rewind_" (The Lost Age); the launcher packs both into bug reports.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gba { class GbaPpu; }

namespace gbarecomp {

// True for a frame both games should not show: drawn in mode 1 with BG2's
// origin reset to (0, 0) while a per-row BG2CNT table still runs. A battle
// spell's setup stores 0 to BG2X/BG2Y and its windows while the arena's line
// table is live; the next frame sets up the spell's own layer. On the console
// that code runs late in the frame; with the CPU overclocked (battle 2x) it
// runs before row 0 and the whole frame shows the arena from (0, 0): a torn
// background with a sky band and a black bottom. Measured: TLA F12 0088/0089
// (FACTS.md 2026-10-08); TBS gpu_rewind_0021 frame 89 (Inferno) and 0017
// frame 53 (Fireball), the only two of 3,001 saved TBS frames that match.
// The caller decides that a battle is running and repeats its last picture.
bool spell_setup_torn_frame(const gba::GbaPpu& ppu);

struct HistoryFrame {
    unsigned long long guest_frame = 0;
    double host_ms = 0.0;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgb;
    std::string scene;                 // renderer snapshot; empty = none
    std::uint64_t scene_frame = 0;
    bool gpu_drawn = false;
    bool battle_2x = false;
    bool held_frame = false;
    std::vector<std::uint8_t> rows;    // 160 x 0x58 register bytes
    std::vector<std::int32_t> refs;    // 160 x 4 affine references
};

class FrameHistory {
public:
    static constexpr std::size_t kFrames = 120;

    explicit FrameHistory(const char* folder_prefix) : prefix_(folder_prefix) {}

    // Adds a frame: its picture, timing and row registers are filled in here,
    // the rest (snapshot, flags) by the caller on the returned entry.
    // A guest clock that went backwards (a state load) starts a new history.
    HistoryFrame& record(const std::uint8_t* rgb, std::uint32_t width,
                         std::uint32_t height);

    // Capture guest identity/row tables before queuing a GPU picture. Timing
    // is filled only when that picture is actually presented.
    static HistoryFrame capture();
    HistoryFrame& record(const std::uint8_t* rgb, std::uint32_t width,
                         std::uint32_t height, HistoryFrame frame);
    void clear();

    std::size_t count() const { return count_; }
    // back(0) is the newest frame; back(i) for i < count().
    const HistoryFrame& back(std::size_t i) const;

    // Writes the folder; returns its path, or empty when it could not.
    std::string write(const std::string& note = {},
                      const char* note_file = "note.txt") const;

private:
    const HistoryFrame& oldest_first(std::size_t i) const;

    const char* prefix_;
    std::vector<HistoryFrame> ring_;
    std::size_t next_ = 0, count_ = 0;
    double last_present_ms_ = -1.0;
    unsigned long long last_live_guest_ = 0;
};

}  // namespace gbarecomp
