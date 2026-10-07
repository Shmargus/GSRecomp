// map_recorder.cpp — see map_recorder.h.

#include "map_recorder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "crc32.h"
#include "env_flag.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "gba_vram_trace.h"
#include "host_config_ui.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif

namespace gsr {
namespace {

bool g_enabled = false;

// ---- session directory ---------------------------------------------------
// Separate from function_tracer's logs/trace_<session>/ -- this is a
// different diagnostic with a different output shape (memory images, not
// per-window call tables). Same stamping approach as
// function_tracer.cpp's ensure_session_dir().
std::string g_session_dir;
bool g_session_ready = false;

void ensure_session_dir() {
    if (g_session_ready) return;
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_buf);
    g_session_dir = std::string("logs/maprec_") + stamp;
    std::error_code ec;
    std::filesystem::create_directories(g_session_dir, ec);
    g_session_ready = true;
}

// D4-equivalent (function_tracer.cpp): sticky write-failure flag so a
// silently-lost capture shows up in the UI instead of just vanishing.
bool g_write_failed = false;
std::string g_write_failed_path;

void note_write_failure(const std::string& path) {
    g_write_failed = true;
    g_write_failed_path = path;
}

std::string sanitize_tag(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
            c == '_') {
            out.push_back(c);
        } else if (c == ' ') {
            out.push_back('_');
        }
    }
    if (out.size() > 48) out.resize(48);
    if (out.empty()) out = "untagged";
    return out;
}

// ---- fixed-page CRC32 delta log (frames.csv) -----------------------------
//
// Page geometry is fixed, not derived from anything guest-specific: EWRAM
// 64 pages x 4KB (256KB total), IWRAM 8 pages x 4KB (32KB), VRAM 48 pages x
// 2KB (96KB), PAL 1 page (1KB), OAM 1 page (1KB) -- 122 pages, matching
// gba::GbaBus's actual buffer sizes (gba_bus.h: ewram_ 256*1024, iwram_
// 32*1024, pal_ 1024, vram_ 96*1024, oam_ 1024). Page index space is
// 0-63 EWRAM, 64-71 IWRAM, 72-119 VRAM, 120 PAL, 121 OAM.
constexpr std::size_t kEwramPages = 64;
constexpr std::size_t kIwramPages = 8;
constexpr std::size_t kVramPages = 48;
constexpr std::size_t kPalPages = 1;
constexpr std::size_t kOamPages = 1;
constexpr std::size_t kEwramPageBytes = 4096;
constexpr std::size_t kIwramPageBytes = 4096;
constexpr std::size_t kVramPageBytes = 2048;
constexpr std::size_t kPalPageBytes = 1024;
constexpr std::size_t kOamPageBytes = 1024;
constexpr std::size_t kEwramPageBase = 0;
constexpr std::size_t kIwramPageBase = kEwramPageBase + kEwramPages;       // 64
constexpr std::size_t kVramPageBase = kIwramPageBase + kIwramPages;        // 72
constexpr std::size_t kPalPageBase = kVramPageBase + kVramPages;           // 120
constexpr std::size_t kOamPageBase = kPalPageBase + kPalPages;             // 121
constexpr std::size_t kTotalPages = kOamPageBase + kOamPages;              // 122

// Last-seen CRC32 per page. Zero-initialized, so the very first frame's
// comparison reports every nonzero page as "changed" -- a deliberate free
// baseline rather than a special case.
std::uint32_t g_page_crc[kTotalPages] = {};

// frames.csv buffered writer, same buffer/flush shape as function_tracer's
// signals.csv (log_raw_signals / flush_signal_log): append to an in-memory
// buffer, flush every kFramesFlushRows rows so an open frame->change row
// doesn't reopen the file every guest frame. 300 mirrors the flush cadence
// function_tracer.cpp already established for its own per-frame CSV
// (kSignalFlushRows) -- reused here rather than picked fresh.
std::string g_frames_buf;
std::size_t g_frames_buf_rows = 0;
bool g_frames_header_written = false;
constexpr std::size_t kFramesFlushRows = 300;

void flush_frames_log() {
    if (g_frames_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/frames.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        note_write_failure(path);
        g_frames_buf.clear();
        g_frames_buf_rows = 0;
        return;
    }
    std::fwrite(g_frames_buf.data(), 1, g_frames_buf.size(), f);
    std::fclose(f);
    g_frames_buf.clear();
    g_frames_buf_rows = 0;
}

// Signed reads for the affine BG parameters/reference points, copied
// verbatim from gba_ppu.cpp's read_s16 / read_s28_ref (those are file-
// static there, so unreachable from this translation unit) -- same GBA
// hardware bit layout (GBATEK "LCD I/O BG Rotation/Scaling"), not a guess.
std::int32_t read_s16_field(const std::uint8_t* io, std::uint32_t off) {
    std::int16_t v = static_cast<std::int16_t>(io[off] | (io[off + 1] << 8));
    return static_cast<std::int32_t>(v);
}
std::int32_t read_s28_field(const std::uint8_t* io, std::uint32_t off) {
    std::uint32_t v = static_cast<std::uint32_t>(io[off]) |
                       (static_cast<std::uint32_t>(io[off + 1]) << 8) |
                       (static_cast<std::uint32_t>(io[off + 2]) << 16) |
                       (static_cast<std::uint32_t>(io[off + 3]) << 24);
    v &= 0x0FFFFFFFu;
    if (v & 0x08000000u) v |= 0xF0000000u;
    return static_cast<std::int32_t>(v);
}
std::uint16_t read_u16_field(const std::uint8_t* io, std::uint32_t off) {
    return static_cast<std::uint16_t>(io[off] | (io[off + 1] << 8));
}

// IO register offsets below are the standard GBA memory-mapped I/O layout
// (GBATEK), cross-checked against this project's own PPU implementation
// wherever it already touches the register (gba_ppu.cpp: DISPCNT/BGxCNT/
// HOFS/VOFS/affine params at gba_ppu.cpp:857-867, WININ/WINOUT/BLDCNT/
// BLDALPHA/BLDY at gba_ppu.cpp:644-685 and :999-1000). MOSAIC (0x4C) is
// not read anywhere in this project's PPU, but its offset is the only
// value consistent with the already-verified spacing of its neighbours
// (WINOUT at 0x4A, BLDCNT at 0x50, both confirmed): it fills the gap
// exactly. Dumped raw in every full snapshot's IO section regardless of
// whether this decoded list is complete.
void append_io_fields(std::string& line, const std::uint8_t* io) {
    char buf[512];
    const std::uint16_t dispcnt = read_u16_field(io, 0x00);
    const std::uint16_t bg0cnt = read_u16_field(io, 0x08);
    const std::uint16_t bg1cnt = read_u16_field(io, 0x0A);
    const std::uint16_t bg2cnt = read_u16_field(io, 0x0C);
    const std::uint16_t bg3cnt = read_u16_field(io, 0x0E);
    const std::uint16_t bg0hofs = read_u16_field(io, 0x10) & 0x01FFu;
    const std::uint16_t bg0vofs = read_u16_field(io, 0x12) & 0x01FFu;
    const std::uint16_t bg1hofs = read_u16_field(io, 0x14) & 0x01FFu;
    const std::uint16_t bg1vofs = read_u16_field(io, 0x16) & 0x01FFu;
    const std::uint16_t bg2hofs = read_u16_field(io, 0x18) & 0x01FFu;
    const std::uint16_t bg2vofs = read_u16_field(io, 0x1A) & 0x01FFu;
    const std::uint16_t bg3hofs = read_u16_field(io, 0x1C) & 0x01FFu;
    const std::uint16_t bg3vofs = read_u16_field(io, 0x1E) & 0x01FFu;
    const std::int32_t bg2pa = read_s16_field(io, 0x20);
    const std::int32_t bg2pb = read_s16_field(io, 0x22);
    const std::int32_t bg2pc = read_s16_field(io, 0x24);
    const std::int32_t bg2pd = read_s16_field(io, 0x26);
    const std::int32_t bg2x = read_s28_field(io, 0x28);
    const std::int32_t bg2y = read_s28_field(io, 0x2C);
    const std::int32_t bg3pa = read_s16_field(io, 0x30);
    const std::int32_t bg3pb = read_s16_field(io, 0x32);
    const std::int32_t bg3pc = read_s16_field(io, 0x34);
    const std::int32_t bg3pd = read_s16_field(io, 0x36);
    const std::int32_t bg3x = read_s28_field(io, 0x38);
    const std::int32_t bg3y = read_s28_field(io, 0x3C);
    const std::uint16_t win0h = read_u16_field(io, 0x40);
    const std::uint16_t win1h = read_u16_field(io, 0x42);
    const std::uint16_t win0v = read_u16_field(io, 0x44);
    const std::uint16_t win1v = read_u16_field(io, 0x46);
    const std::uint16_t winin = read_u16_field(io, 0x48);
    const std::uint16_t winout = read_u16_field(io, 0x4A);
    const std::uint16_t mosaic = read_u16_field(io, 0x4C);
    const std::uint16_t bldcnt = read_u16_field(io, 0x50);
    const std::uint16_t bldalpha = read_u16_field(io, 0x52);
    const std::uint16_t bldy = read_u16_field(io, 0x54) & 0x1Fu;

    const int n = std::snprintf(buf, sizeof(buf),
        "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
        "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
        dispcnt, bg0cnt, bg1cnt, bg2cnt, bg3cnt,
        bg0hofs, bg0vofs, bg1hofs, bg1vofs, bg2hofs, bg2vofs, bg3hofs, bg3vofs,
        bg2pa, bg2pb, bg2pc, bg2pd, bg2x, bg2y,
        bg3pa, bg3pb, bg3pc, bg3pd, bg3x, bg3y,
        win0h, win0v, win1h, win1v, winin, winout,
        mosaic, bldcnt, bldalpha, bldy);
    if (n > 0) {
        line.append(buf, static_cast<std::size_t>(
            n < static_cast<int>(sizeof(buf)) ? n
                                               : static_cast<int>(sizeof(buf)) - 1));
    }
}

// The metatile id grid (FACTS.md: 0x02010000, 128x128 u32 cells, 64 KB)
// occupies EWRAM byte range 0x10000..0x1FFFF, i.e. page indices 16..31 in
// this file's fixed EWRAM page numbering (page 0 = EWRAM+0x0000). Tracked
// separately from the overall changed-page count so a room load (all 16 of
// these pages rewritten) can be told apart from an in-room event edit
// (FACTS.md: 12 of 16,384 cells, at most a couple of these pages) or
// ordinary scrolling.
constexpr std::size_t kGridPageFirst = 16;
constexpr std::size_t kGridPageLast = 31;  // inclusive

// Diffs the 122 fixed pages against g_page_crc, updating it in place, and
// appends any changed pages as "idx:crc" ('|'-joined, matching
// function_tracer.cpp's convention of never using ',' inside a CSV field)
// to `out`. Returns the number of pages that changed. `grid_changed_pages`
// receives how many of those changes fell in the metatile grid's own page
// range, for the room-load detector below.
std::size_t diff_pages(const gba::GbaBus& bus, std::string& out,
                       std::size_t& grid_changed_pages) {
    std::size_t changed = 0;
    grid_changed_pages = 0;
    auto check_range = [&](const std::uint8_t* base, std::size_t page_bytes,
                            std::size_t page_count, std::size_t index_base) {
        for (std::size_t i = 0; i < page_count; ++i) {
            const std::uint32_t crc =
                gba::crc32(base + i * page_bytes, page_bytes);
            std::uint32_t& prev = g_page_crc[index_base + i];
            if (prev == crc) continue;
            prev = crc;
            char cell[24];
            const int n = std::snprintf(cell, sizeof(cell), "%s%zu:%08X",
                                        changed == 0 ? "" : "|",
                                        index_base + i, crc);
            if (n > 0) out.append(cell, static_cast<std::size_t>(n));
            ++changed;
            if (index_base == kEwramPageBase && i >= kGridPageFirst &&
                i <= kGridPageLast) {
                ++grid_changed_pages;
            }
        }
    };
    check_range(bus.ewram_ptr(), kEwramPageBytes, kEwramPages, kEwramPageBase);
    check_range(bus.iwram_ptr(), kIwramPageBytes, kIwramPages, kIwramPageBase);
    check_range(bus.vram_ptr(), kVramPageBytes, kVramPages, kVramPageBase);
    check_range(bus.pal_ptr(), kPalPageBytes, kPalPages, kPalPageBase);
    check_range(bus.oam_ptr(), kOamPageBytes, kOamPages, kOamPageBase);
    return changed;
}

// ---- room-load EWRAM write bitmap (roomload_<frame>.bin) ------------------
//
// Goal: recover the room-bounds struct field offsets by intersecting which
// EWRAM 32-byte lines are written during a room-load burst across several
// loads (tools/decode_snap.py --roomload-intersect). Snapshot diffing alone
// cannot do this: thousands of EWRAM addresses vary per room, so the
// discriminator has to be "written during THIS burst", not "differs between
// two snapshots".
//
// EWRAM is 256 KB; a 32-byte line is one bit, so one frame's bitmap is
// 256*1024/32/8 = 1024 bytes. That is cheap enough to keep a ring of the
// last few frames every session (see map_recorder_on_ewram_write), rather
// than writing one per frame to disk -- an 8-minute session at 1 KB/frame
// would otherwise be ~28 MB and almost all of it unrelated to a room load.
constexpr std::size_t kEwramLineBytes = 32;
constexpr std::size_t kEwramTotalBytes = kEwramPages * kEwramPageBytes;  // 256 KiB
constexpr std::size_t kEwramLineCount = kEwramTotalBytes / kEwramLineBytes;  // 8192
constexpr std::size_t kEwramBitmapBytes = kEwramLineCount / 8;  // 1024
constexpr std::uint32_t kEwramBase = 0x02000000u;
constexpr std::uint32_t kEwramMirrorMask = 0x0003FFFFu;  // 256 KiB, matches
                                                         // gba_memory.cpp's
                                                         // resolve_offset().

// How many frames of context to keep before a detected load, and how many
// more to capture after it, per event.
constexpr std::size_t kRoomLoadRingFrames = 8;
constexpr std::size_t kRoomLoadPostFrames = 8;

// Room-load detection threshold for "how many of the 16 grid pages changed
// this frame". Measured 2026-09-04 across the five existing GSR_MAP_RECORD
// sessions (logs/maprec_*/frames.csv, kGridPageFirst..kGridPageLast column):
// every frame outside a room transition changed at most 11 of the 16 grid
// pages (scrolling and in-room event edits both stay well under that), while
// every observed room-load frame changed at least 14, with the full 16
// usually split 14-15/16 or 15/16 across two consecutive frames because the
// grid rewrite crosses a frame boundary. 12 is the smallest integer above
// the measured ordinary-play ceiling (11), so it flags every observed load
// while staying clear of anything measured during ordinary play.
constexpr std::size_t kRoomLoadGridPageThreshold = 12;

struct EwramFrameBitmap {
    std::uint64_t frame = 0;
    std::array<std::uint8_t, kEwramBitmapBytes> bits{};
};

// Lines written since the bitmap was last finalized (see
// finalize_ewram_frame_bitmap). Set from map_recorder_on_ewram_write, which
// runs on every EWRAM CPU store while the recorder is enabled.
std::array<std::uint8_t, kEwramBitmapBytes> g_ewram_current_bitmap{};

// Ring of the last kRoomLoadRingFrames finalized bitmaps, oldest overwritten
// first.
std::array<EwramFrameBitmap, kRoomLoadRingFrames> g_ewram_ring{};
std::size_t g_ewram_ring_count = 0;
std::size_t g_ewram_ring_next = 0;

// While capturing, holds the ring snapshot taken at detection plus the
// frames captured since. Empty when no capture is in progress.
std::vector<EwramFrameBitmap> g_room_load_capture;
std::size_t g_room_load_post_remaining = 0;
std::uint64_t g_room_load_trigger_frame = 0;

constexpr char kRoomLoadMagic[8] = {'G', 'S', 'R', 'R', 'L', 'O', 'D', '1'};
constexpr std::uint32_t kRoomLoadVersion = 1;

void write_room_load_capture() {
    ensure_session_dir();
    char name[64];
    std::snprintf(name, sizeof(name), "/roomload_%llu.bin",
                 static_cast<unsigned long long>(g_room_load_trigger_frame));
    const std::string path = g_session_dir + name;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        note_write_failure(path);
        g_room_load_capture.clear();
        return;
    }
    std::fwrite(kRoomLoadMagic, 1, sizeof(kRoomLoadMagic), f);
    auto write_u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto write_u64 = [&](std::uint64_t v) { std::fwrite(&v, 8, 1, f); };
    write_u32(kRoomLoadVersion);
    write_u64(g_room_load_trigger_frame);
    write_u32(static_cast<std::uint32_t>(g_room_load_capture.size()));
    write_u32(static_cast<std::uint32_t>(kEwramBitmapBytes));
    write_u32(kEwramBase);
    write_u32(static_cast<std::uint32_t>(kEwramLineBytes));
    write_u32(static_cast<std::uint32_t>(kEwramLineCount));
    for (const EwramFrameBitmap& fb : g_room_load_capture) {
        write_u64(fb.frame);
        std::fwrite(fb.bits.data(), 1, fb.bits.size(), f);
    }
    std::fclose(f);
    g_room_load_capture.clear();
}

// Called once per finalized frame (see log_frame_delta) with the bitmap that
// accumulated since the previous call and this check's grid-page-change
// count. Feeds the ring buffer and, when a load is detected or already in
// progress, the active capture.
void finalize_ewram_frame_bitmap(std::uint64_t frame,
                                 std::size_t grid_changed_pages) {
    EwramFrameBitmap finished;
    finished.frame = frame;
    finished.bits = g_ewram_current_bitmap;
    g_ewram_current_bitmap.fill(0);

    if (!g_room_load_capture.empty() || g_room_load_post_remaining > 0) {
        // Already capturing a prior event: keep it uninterrupted rather
        // than restarting on an adjacent load-boundary frame (a load's grid
        // rewrite can itself straddle two consecutive frames -- see the
        // threshold comment above).
        g_room_load_capture.push_back(finished);
        if (g_room_load_post_remaining > 0 && --g_room_load_post_remaining == 0) {
            write_room_load_capture();
        }
    } else if (grid_changed_pages >= kRoomLoadGridPageThreshold) {
        g_room_load_trigger_frame = frame;
        g_room_load_capture.clear();
        g_room_load_capture.reserve(kRoomLoadRingFrames + kRoomLoadPostFrames + 1);
        for (std::size_t i = 0; i < g_ewram_ring_count; ++i) {
            const std::size_t idx =
                (g_ewram_ring_next + kRoomLoadRingFrames - g_ewram_ring_count + i) %
                kRoomLoadRingFrames;
            g_room_load_capture.push_back(g_ewram_ring[idx]);
        }
        g_room_load_capture.push_back(finished);
        g_room_load_post_remaining = kRoomLoadPostFrames;
    }

    g_ewram_ring[g_ewram_ring_next] = finished;
    g_ewram_ring_next = (g_ewram_ring_next + 1) % kRoomLoadRingFrames;
    if (g_ewram_ring_count < kRoomLoadRingFrames) ++g_ewram_ring_count;
}

// Appends one frames.csv row iff at least one of the 122 pages changed
// this frame. `frame` is the RAW, unmodified value from
// runtime_current_frame() -- see the header comment on
// map_recorder_on_entry and the note on savestate loads below.
void log_frame_delta(std::uint64_t frame, const gba::GbaBus& bus) {
    std::string changed_cell;
    std::size_t grid_changed_pages = 0;
    const std::size_t changed = diff_pages(bus, changed_cell, grid_changed_pages);
    finalize_ewram_frame_bitmap(frame, grid_changed_pages);
    if (changed == 0) return;

    if (!g_frames_header_written) {
        g_frames_buf +=
            "frame,changed_pages,dispcnt,bg0cnt,bg1cnt,bg2cnt,bg3cnt,"
            "bg0hofs,bg0vofs,bg1hofs,bg1vofs,bg2hofs,bg2vofs,bg3hofs,bg3vofs,"
            "bg2pa,bg2pb,bg2pc,bg2pd,bg2x,bg2y,"
            "bg3pa,bg3pb,bg3pc,bg3pd,bg3x,bg3y,"
            "win0h,win0v,win1h,win1v,winin,winout,"
            "mosaic,bldcnt,bldalpha,bldy\n";
        g_frames_header_written = true;
    }

    // NOTE: frame is written exactly as runtime_current_frame() returns it.
    // A savestate load can jump this value non-monotonically, backward or
    // forward by however much guest time the save recorded (see FACTS.md /
    // runtime_bus_bridge.h's set_savestate_load_hook comment) -- this
    // recorder does not smooth, offset or rebase that, so any offline
    // consumer of frames.csv (or a snapshot header's frame field) must
    // treat frame numbers as non-monotonic rather than assuming they only
    // increase. This is what lets a snapshot be tied to the same raw frame
    // number function_tracer.cpp's window start/end frames use.
    char prefix[32];
    const int n = std::snprintf(prefix, sizeof(prefix), "%llu,",
                                static_cast<unsigned long long>(frame));
    g_frames_buf.append(prefix, static_cast<std::size_t>(n > 0 ? n : 0));
    g_frames_buf += changed_cell;
    g_frames_buf.push_back(',');
    append_io_fields(g_frames_buf, bus.io().raw());
    g_frames_buf.push_back('\n');

    if (++g_frames_buf_rows >= kFramesFlushRows) flush_frames_log();
}

// ---- full snapshots (snap_<seq>_<tag>.bin + .png) ------------------------
//
// Format "GSRSNAP1": magic, version, section count, RAW frame number, a
// free-text scene tag, then a self-describing section table (name +
// absolute file offset + length for each of IO/EWRAM/IWRAM/VRAM/PAL/OAM) so
// tools/decode_snap.py never has to assume a layout. All integers little-
// endian (native on this host).
constexpr char kSnapMagic[8] = {'G', 'S', 'R', 'S', 'N', 'A', 'P', '1'};
constexpr std::uint32_t kSnapVersion = 1;
constexpr std::size_t kTagBytes = 64;
// 8 since 2026-09-16: LINEIO and AFFREF joined the six memory sections.
// Readers look sections up by NAME out of the directory (see
// tools/scene_renderer_check.cpp's load_snapshot), so older tools ignore the
// two new ones and older captures simply do not offer them.
constexpr std::size_t kSectionCount = 8;

std::uint32_t g_snap_seq = 0;

void append_u32(std::string& out, std::uint32_t v) {
    char b[4] = {static_cast<char>(v & 0xFFu), static_cast<char>((v >> 8) & 0xFFu),
                 static_cast<char>((v >> 16) & 0xFFu), static_cast<char>((v >> 24) & 0xFFu)};
    out.append(b, 4);
}
void append_u64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

bool write_snapshot_bin(const std::string& path, std::uint64_t frame,
                        const std::string& tag, const gba::GbaBus& bus) {
    struct Section {
        const char* name;
        const std::uint8_t* data;
        std::uint64_t length;
    };
    // Each scanline's own register file and affine reference, not just the
    // frame's last one.
    //
    // Without these a snapshot cannot express a frame that rewrites a
    // register mid-screen, and Golden Sun does that constantly: a battle
    // rewrites BGxCNT twice down one frame, Tret swaps BG3's scroll between a
    // room position and a scrolling accumulator in horizontal bands, and the
    // room-transition iris rewrites WIN0H every row. A corpus of end-of-frame
    // reads therefore PASSES a renderer that gets all of those wrong -- which
    // is not hypothetical: the 2,429-snapshot gate reported the same 2,336
    // identical frames before and after the fix for a battle fault that made
    // every battle frame wrong (FACTS.md, 2026-09-16). Only a live F12 dump,
    // which already carries these two sections, could tell the two renderers
    // apart.
    //
    // Same layout the F12 dump uses, so one reader serves both: 160 rows of
    // GbaPpu::kLineIoBytes, then 160 rows of four int32 affine references.
    // A row the console never latched falls back to the frame's own register
    // file, exactly as the live dump does, so a reader never has to know
    // which rows were captured.
    constexpr std::size_t kRows = gba::GbaPpu::kScreenHeight;
    constexpr std::size_t kLineIo = gba::GbaPpu::kLineIoBytes;
    std::vector<std::uint8_t> line_io(kRows * kLineIo);
    std::vector<std::uint8_t> affine_ref(kRows * 4u * sizeof(std::int32_t));
    {
        const gba::GbaPpu* ppu = gbarecomp::active_ppu();
        const std::uint8_t* rows =
            ppu ? ppu->latched_native_line_io() : nullptr;
        const bool* rows_valid =
            ppu ? ppu->latched_native_line_io_valid() : nullptr;
        const std::int32_t* refs =
            ppu ? ppu->latched_native_affine_line_refs() : nullptr;
        const bool* refs_valid =
            ppu ? ppu->latched_native_affine_line_ref_valid() : nullptr;
        for (std::size_t y = 0; y < kRows; ++y) {
            const std::uint8_t* src =
                (rows && rows_valid && rows_valid[y]) ? rows + y * kLineIo
                                                      : bus.io().raw();
            std::memcpy(&line_io[y * kLineIo], src, kLineIo);
            if (refs && refs_valid && refs_valid[y]) {
                std::memcpy(&affine_ref[y * 4u * sizeof(std::int32_t)],
                            refs + y * 4u, 4u * sizeof(std::int32_t));
            }
            // else: left zero. Only an affine layer reads these, and a frame
            // that has one always latches them.
        }

        // The PPU latches its per-row registers at VBlank, for the frame that
        // has just finished. A snapshot taken part-way through the NEXT frame
        // therefore pairs this frame's memory with the previous frame's rows,
        // and a reader driven from both sees a scene that never existed.
        // Measured 2026-09-16 on the first session recorded with these
        // sections: snap_00007's memory says DISPCNT 0x7741 while every
        // latched row says 0x0001, and three such frames were reported as
        // renderer faults that were nothing of the kind.
        //
        // DISPCNT is the cheapest witness -- it is the first register of both
        // copies. When the two disagree the latch belongs to another frame,
        // so fall back to repeating this frame's own register file: the
        // sections stay present and well formed, and a reader driven from
        // them behaves exactly as it did before they existed.
        const unsigned live_dispcnt =
            bus.io().raw()[0] | (static_cast<unsigned>(bus.io().raw()[1]) << 8);
        const unsigned row0_dispcnt =
            line_io[0] | (static_cast<unsigned>(line_io[1]) << 8);
        if (row0_dispcnt != live_dispcnt) {
            for (std::size_t y = 0; y < kRows; ++y)
                std::memcpy(&line_io[y * kLineIo], bus.io().raw(), kLineIo);
            std::fill(affine_ref.begin(), affine_ref.end(),
                      static_cast<std::uint8_t>(0));
        }
    }

    const Section sections[kSectionCount] = {
        {"IO", bus.io().raw(), gba::GbaIo::kIoSize},
        {"EWRAM", bus.ewram_ptr(), kEwramPages * kEwramPageBytes},
        {"IWRAM", bus.iwram_ptr(), kIwramPages * kIwramPageBytes},
        {"VRAM", bus.vram_ptr(), kVramPages * kVramPageBytes},
        {"PAL", bus.pal_ptr(), kPalPages * kPalPageBytes},
        {"OAM", bus.oam_ptr(), kOamPages * kOamPageBytes},
        {"LINEIO", line_io.data(), line_io.size()},
        {"AFFREF", affine_ref.data(), affine_ref.size()},
    };

    std::string header;
    header.append(kSnapMagic, sizeof(kSnapMagic));
    append_u32(header, kSnapVersion);
    append_u32(header, static_cast<std::uint32_t>(kSectionCount));
    append_u64(header, frame);
    std::string tag_field(kTagBytes, '\0');
    std::memcpy(&tag_field[0], tag.data(), std::min(tag.size(), kTagBytes - 1));
    header += tag_field;

    const std::size_t table_off = header.size();
    std::uint64_t data_off = table_off + kSectionCount * (8 + 8 + 8);
    std::string table;
    std::string data;
    for (const Section& s : sections) {
        std::string name_field(8, '\0');
        std::memcpy(&name_field[0], s.name, std::min(std::strlen(s.name), std::size_t{8}));
        table += name_field;
        append_u64(table, data_off);
        append_u64(table, s.length);
        data.append(reinterpret_cast<const char*>(s.data), s.length);
        data_off += s.length;
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        note_write_failure(path);
        return false;
    }
    std::fwrite(header.data(), 1, header.size(), f);
    std::fwrite(table.data(), 1, table.size(), f);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return true;
}

void write_snapshot_png(const std::string& path) {
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!ppu || !ppu->has_latched_framebuffer()) return;
    if (!gbarecomp::write_png(path, ppu->latched_framebuffer(), ppu->render_width(),
                              ppu->render_height())) {
        note_write_failure(path);
    }
}

std::uint64_t g_last_snapshot_frame = 0;
bool g_have_prev_dispcnt = false;
std::uint16_t g_prev_dispcnt_mode = 0;
std::uint16_t g_prev_dispcnt_enable = 0;
bool g_took_first_snapshot = false;

// b) periodic cadence while a scene is active. 300 is an ARBITRARY starting
// value, not a measured threshold -- there is no data yet to tune it from
// (the same situation function_tracer.cpp's own thresholds were in before
// their first session; see its "there is no real per-frame register data
// to tune any of these constants from" comment). Expected to be revisited
// once the first map-recorder session's data exists.
constexpr std::uint64_t kPeriodicSnapshotFrames = 300;

void take_snapshot(const std::string& reason_tag, const gba::GbaBus& bus) {
    ensure_session_dir();
    const std::uint64_t frame = runtime_current_frame();
    const std::string safe = sanitize_tag(reason_tag);
    char base[96];
    std::snprintf(base, sizeof(base), "snap_%05u_%s",
                 static_cast<unsigned>(g_snap_seq++), safe.c_str());
    write_snapshot_bin(g_session_dir + "/" + base + ".bin", frame, reason_tag, bus);
    write_snapshot_png(g_session_dir + "/" + base + ".png");
    g_last_snapshot_frame = frame;
    g_took_first_snapshot = true;
}

// Checks triggers (a) mode/layer-enable change and (b) periodic cadence.
// (c) manual snapshot is handled directly from the button callback, not
// here. DISPCNT bit layout is the standard GBA one (GBATEK): bits 0-2 are
// the BG mode, bits 8-15 are the BG0-3/OBJ/WIN0/WIN1/OBJWIN enable bits --
// both already exercised elsewhere in this codebase (dispcnt & 0x0100 <<
// layer in gba_ppu.cpp's render_affine_bg/render_regular_bg call sites).
void check_snapshot_triggers(std::uint64_t frame, const gba::GbaBus& bus) {
    const std::uint16_t dispcnt = read_u16_field(bus.io().raw(), 0x00);
    const std::uint16_t mode = dispcnt & 0x0007u;
    const std::uint16_t enable_bits = dispcnt & 0xFF00u;

    bool mode_changed = !g_have_prev_dispcnt;
    if (g_have_prev_dispcnt &&
        (mode != g_prev_dispcnt_mode || enable_bits != g_prev_dispcnt_enable)) {
        mode_changed = true;
    }
    g_have_prev_dispcnt = true;
    g_prev_dispcnt_mode = mode;
    g_prev_dispcnt_enable = enable_bits;

    if (mode_changed) {
        take_snapshot(g_took_first_snapshot ? "modechange" : "initial", bus);
        return;
    }

    if (frame >= g_last_snapshot_frame &&
        (frame - g_last_snapshot_frame) >= kPeriodicSnapshotFrames) {
        take_snapshot("periodic", bus);
    }
}

// ---- manual "Snapshot now" control (extension point c) -------------------
char g_label_buf[64] = "";
bool g_window_focused = false;

#ifdef GBARECOMP_HAVE_IMGUI
void draw_recorder_window() {
    if (!g_enabled) return;

    const ImGuiViewport* main_vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(main_vp->WorkPos.x + main_vp->WorkSize.x + 24.0f,
               main_vp->WorkPos.y + 380.0f),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380.0f, 140.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Scene Recorder", nullptr, ImGuiWindowFlags_NoDocking)) {
        g_window_focused = false;
        ImGui::End();
        return;
    }
    g_window_focused = ImGui::IsWindowFocused();

    if (g_write_failed) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "WRITE FAILED: %s",
                           g_write_failed_path.c_str());
    }
    ImGui::Text("Snapshots saved: %u", g_snap_seq);
    ImGui::InputText("Label", g_label_buf, sizeof(g_label_buf));
    if (ImGui::Button("Snapshot now")) {
        gba::GbaBus* bus = gbarecomp::active_bus();
        if (bus) {
            const std::string tag = g_label_buf[0]
                                        ? std::string("manual_") + g_label_buf
                                        : std::string("manual");
            take_snapshot(tag, *bus);
        }
        g_label_buf[0] = '\0';
    }
    ImGui::End();
}

bool recorder_wants_keyboard() {
    if (!g_enabled) return false;
    return g_window_focused && ImGui::GetIO().WantCaptureKeyboard;
}
#else
void draw_recorder_window() {}
bool recorder_wants_keyboard() { return false; }
#endif

// ---- sharing host_config_ui.h's single extra-draw slot with the tracer --
//
// gbarecomp::g_config_ui_extra_draw / g_config_ui_extra_wants_keyboard are
// each a single function pointer (host_config_ui.h), not a list -- only
// one consumer can be wired directly. function_tracer_init() may already
// have installed its own callback there before map_recorder_init() runs
// (see runner_main.cpp's call order); to let both diagnostics' toggles
// work independently, this captures whatever was already installed and
// chains through it before drawing the recorder's own window. If the
// tracer is disabled, function_tracer_init() never touches the pointer
// (see its own `if (!g_enabled) return;`), so the captured value is
// nullptr and this simply skips the chained call.
void (*g_prev_extra_draw)() = nullptr;
bool (*g_prev_extra_wants_keyboard)() = nullptr;

void combined_extra_draw() {
    if (g_prev_extra_draw) g_prev_extra_draw();
    draw_recorder_window();
}
bool combined_extra_wants_keyboard() {
    if (g_prev_extra_wants_keyboard && g_prev_extra_wants_keyboard()) return true;
    return recorder_wants_keyboard();
}

// The recorder's own EWRAM write feed.
//
// Installing this from main(), alongside map_recorder_init(), does not work:
// run_game() clears both observer slots on entry
// (gbarecomp/src/runtime/runtime.cpp:889-890) so that a game's enhancement
// hooks cannot leak into a later faithful run in the same process. That
// clear happens after main() has installed anything, and before a single
// guest instruction executes -- which is why session_20260905_092954 still
// wrote 11 room-load captures with zero lines set. So install from the
// per-frame path below, which runs inside run_game().
//
// Only ever fill a slot nobody else owns. install_golden_sun_widescreen
// installs golden_sun_wide_ewram_write_observer in the same slots, and that
// one already chains to map_recorder_on_ewram_write, so replacing it would
// silently drop the widescreen bookkeeping while gaining nothing.
void ewram_write_observer(std::uint32_t address, std::uint32_t size) {
    // Same DMA guard as the widescreen observer, so both feeds see the same
    // event stream: DMA has descriptor-level provenance elsewhere and must
    // not be counted here one copied unit at a time.
    if (gba::vram_trace::dma_active()) return;
    map_recorder_on_ewram_write(address, size);
}

void ensure_ewram_observer_installed() {
    if (!gba::g_ws_ewram_write_observer) {
        gba::g_ws_ewram_write_observer = &ewram_write_observer;
    }
    if (!g_runtime_fast_ewram_write_observer) {
        g_runtime_fast_ewram_write_observer = &ewram_write_observer;
    }
}

}  // namespace

// Defined below, next to the writer-argument table it flushes.
void write_writer_args();

void map_recorder_init() {
    g_enabled = gbarecomp::env_flag("GSR_MAP_RECORD");
    if (!g_enabled) return;

    ensure_session_dir();
    std::atexit(write_writer_args);
    g_prev_extra_draw = gbarecomp::g_config_ui_extra_draw;
    g_prev_extra_wants_keyboard = gbarecomp::g_config_ui_extra_wants_keyboard;
    gbarecomp::g_config_ui_extra_draw = &combined_extra_draw;
    gbarecomp::g_config_ui_extra_wants_keyboard = &combined_extra_wants_keyboard;
}

void map_recorder_on_entry(std::uint32_t entry_pc) {
    (void)entry_pc;
    if (!g_enabled) return;  // OFF: one predictable branch, nothing else.

    static std::uint64_t s_last_seen_frame = ~std::uint64_t{0};
    const std::uint64_t frame = runtime_current_frame();
    if (frame == s_last_seen_frame) return;
    s_last_seen_frame = frame;

    // Once per frame, not once ever: run_game() may clear the slots again on
    // a second run in the same process, and the cost is two pointer tests.
    ensure_ewram_observer_installed();

    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    log_frame_delta(frame, *bus);
    check_snapshot_triggers(frame, *bus);
}

// ---- field tilemap writer argument capture ------------------------------
//
// The six entry points below are every function in the first 256 KB of ROM
// whose literal pool carries the field map signature {destination screenblock
// base, 0x02010000, 0x02020000, 0x02020004}. Found by scanning the ROM for
// that signature, then attributing each pool to the function it terminates;
// cross-checked against the store PCs the VRAM trace recorded in
// session_20260905_104619. Not a guess, and not a list to extend without
// re-running that scan.
constexpr std::uint32_t kFieldTilemapWriters[6] = {
    0x0800FEC8u, 0x0800FF54u, 0x08010230u,
    0x08010424u, 0x08010560u, 0x08010788u,
};

struct WriterArgs {
    std::uint32_t pc, r0, r1, r2, r3;
    std::uint16_t room_w, room_h;
    std::uint64_t count;
};

// Bounded so a long session cannot grow this without limit. 4096 distinct
// combinations is far more than the handful of regions a room can use; if it
// ever fills, the overflow count says so rather than the table silently
// dropping observations.
constexpr std::size_t kMaxWriterArgs = 4096;
std::array<WriterArgs, kMaxWriterArgs> g_writer_args{};
std::size_t g_writer_args_used = 0;
std::uint64_t g_writer_args_overflow = 0;

void write_writer_args() {
    if (g_writer_args_used == 0) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/writer_args.csv";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        note_write_failure(path);
        return;
    }
    std::fprintf(f, "writer_pc,room_w,room_h,r0,r1,r2,r3,count\n");
    for (std::size_t i = 0; i < g_writer_args_used; ++i) {
        const WriterArgs& a = g_writer_args[i];
        std::fprintf(f, "0x%08X,%u,%u,%u,%u,%u,%u,%llu\n", a.pc, a.room_w,
                     a.room_h, a.r0, a.r1, a.r2, a.r3,
                     static_cast<unsigned long long>(a.count));
    }
    if (g_writer_args_overflow != 0) {
        std::fprintf(f, "# %llu observations dropped: table full\n",
                     static_cast<unsigned long long>(g_writer_args_overflow));
    }
    std::fclose(f);
}

void map_recorder_on_ewram_write(std::uint32_t address, std::uint32_t size) {
    (void)size;  // A naturally aligned <=4-byte store cannot cross a
                // 32-byte line, so only the start address decides the line.
    if (!g_enabled) return;  // OFF: one predictable branch, nothing else.
    const std::uint32_t offset = address & kEwramMirrorMask;
    const std::uint32_t line = offset / kEwramLineBytes;
    if (line >= kEwramLineCount) return;  // Defensive; the mask above makes
                                          // this unreachable.
    g_ewram_current_bitmap[line / 8] |=
        static_cast<std::uint8_t>(1u << (line % 8));
}

void map_recorder_on_function_args(std::uint32_t entry_pc, std::uint32_t r0,
                                   std::uint32_t r1, std::uint32_t r2,
                                   std::uint32_t r3) {
    if (!g_enabled) return;  // OFF: one predictable branch, nothing else.
    bool is_writer = false;
    for (std::uint32_t pc : kFieldTilemapWriters) {
        if (pc == entry_pc) { is_writer = true; break; }
    }
    if (!is_writer) return;

    // Tag each observation with the room it happened in: the regions are
    // chosen per call, and per room is the grouping that makes them
    // interpretable.
    std::uint16_t room_w = 0, room_h = 0;
    if (const gba::GbaBus* bus = gbarecomp::active_bus()) {
        const std::uint8_t* ew = bus->ewram_ptr();
        room_w = static_cast<std::uint16_t>(ew[0x30DC2] | (ew[0x30DC3] << 8));
        room_h = static_cast<std::uint16_t>(ew[0x30DC6] | (ew[0x30DC7] << 8));
    }

    for (std::size_t i = 0; i < g_writer_args_used; ++i) {
        WriterArgs& a = g_writer_args[i];
        if (a.pc == entry_pc && a.r0 == r0 && a.r1 == r1 && a.r2 == r2 &&
            a.r3 == r3 && a.room_w == room_w && a.room_h == room_h) {
            ++a.count;
            return;
        }
    }
    if (g_writer_args_used >= kMaxWriterArgs) {
        ++g_writer_args_overflow;
        return;
    }
    g_writer_args[g_writer_args_used++] =
        WriterArgs{entry_pc, r0, r1, r2, r3, room_w, room_h, 1};
}

}  // namespace gsr
