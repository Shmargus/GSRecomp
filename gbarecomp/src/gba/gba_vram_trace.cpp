#include "gba_vram_trace.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gba_io.h"
#include "bus.h"

extern "C" unsigned long long g_runtime_cycles;
extern "C" unsigned long long runtime_current_frame();

namespace gba::vram_trace {
namespace {

int g_default_enabled = 0;
int g_env_override = -1;
// Whether the environment has been consulted, kept separate from the ANSWER.
// g_env_override stays -1 when the variable is unset, because an unset
// variable must leave g_default_enabled in charge (and that one can still be
// changed at runtime by the setter below). Without this flag the -1 was doing
// double duty as "not read yet", so the common case -- variable not set --
// re-read the environment on every single call.
bool g_env_read = false;
DmaDescriptorObserver g_dma_descriptor_observer = nullptr;
OamShadowWriteObserver g_oam_shadow_write_observer = nullptr;
VideoWriteObserver g_video_write_observer = nullptr;
OamShadowWriteRangePredicate g_oam_shadow_write_range_predicate = nullptr;
unsigned g_trace_windows = 1u;
constexpr unsigned kMaxTraceWindows = 16u;

bool g_text_trace_enabled = false;
bool g_text_trace_open_failed = false;
bool g_text_trace_atexit_registered = false;
std::string g_text_trace_directory;
std::string g_text_trace_buffer;
std::FILE* g_text_trace_file = nullptr;
std::uint64_t g_text_trace_sequence = 0;
std::size_t g_text_trace_pending_rows = 0;

// Match the existing function-tracer signal flush cadence. This is a flush
// cadence only; the address range remains the live BG0 range selected by the
// guest's display registers and no payload is retained.
constexpr std::size_t kTextTraceFlushRows = 300u;

constexpr std::uint32_t kTextRegionChar = 1u;
constexpr std::uint32_t kTextRegionScreenMap = 2u;

void flush_text_trace();

std::uint16_t load16(const std::uint8_t* io, std::uint32_t off) {
    return static_cast<std::uint16_t>(io[off] |
                                      (std::uint16_t(io[off + 1u]) << 8));
}

bool trace_enabled() {
    // Called on every VRAM write. Measured 2026-09-16 at 11.5% of all host
    // samples in an Earthquake profile -- not because the trace does any
    // work (it is off), but because the getenv below ran every time: the
    // old guard was `g_env_override < 0`, which is also the state an UNSET
    // variable leaves behind, so the off-by-default case never cached.
    // Reading the environment is a linear scan of the process block, and
    // this is the single hottest caller of the C runtime in the profile.
    if (!g_env_read) {
        g_env_read = true;
        const char* env = std::getenv("GBARECOMP_VRAM_MAP_TRACE");
        if (env) {
            g_env_override = env[0] != '\0' && env[0] != '0' ? 1 : 0;
        }
    }
    return (g_env_override >= 0 ? g_env_override : g_default_enabled) != 0;
}

bool mode0_field(const std::uint8_t* io) {
    if (!io) return false;
    const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
    // Mode 0 and at least one of the field layers BG1..BG3 enabled.
    return (dispcnt & 0x7u) == 0u && (dispcnt & 0x0E00u) != 0u;
}

bool mode2_affine(const std::uint8_t* io) {
    if (!io) return false;
    const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
    // Mode 2 and at least one of the affine layers BG2/BG3 enabled.
    return (dispcnt & 0x7u) == 2u && (dispcnt & 0x0C00u) != 0u;
}

unsigned block_count(unsigned size) {
    switch (size & 3u) {
        case 1u: return 2u;  // 512x256
        case 2u: return 2u;  // 256x512
        case 3u: return 4u;  // 512x512
        default: return 1u;  // 256x256
    }
}

unsigned block_offset(unsigned size, unsigned index) {
    // A vertical 512x256 text map uses the next screenblock row (+2), not
    // the horizontally adjacent block (+1).
    if ((size & 3u) == 2u && index == 1u) return 2u;
    return index;
}

bool regular_bg_range_intersects(const std::uint8_t* io, unsigned bg,
                                std::uint32_t address,
                                std::uint64_t size) {
    const std::uint16_t cnt = load16(io, IoReg::BG0CNT + bg * 2u);
    const unsigned base = (cnt >> 8) & 0x1Fu;
    const unsigned map_size = (cnt >> 14) & 3u;
    const unsigned blocks = block_count(map_size);
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    for (unsigned i = 0; i < blocks; ++i) {
        const std::uint64_t block = static_cast<std::uint64_t>(
            base + block_offset(map_size, i));
        const std::uint64_t map_first = kVramStart + block * 0x800ull;
        const std::uint64_t map_last = map_first + 0x800ull;
        if (first < map_last && last > map_first) return true;
    }
    return false;
}

// Affine BG2/BG3 maps are one contiguous screenblock run (no vertical
// screenblock-row aliasing like text mode), (16 << size_code) tiles square
// at one byte per map entry, so the map is (16 << size_code)^2 bytes
// starting at the BGCNT screen base. Derived live from BGCNT; never a fixed
// screen base or size.
bool affine_range_intersects(const std::uint8_t* io, unsigned bg,
                             std::uint32_t address, std::uint64_t size) {
    const std::uint16_t cnt = load16(io, IoReg::BG0CNT + bg * 2u);
    const unsigned base = (cnt >> 8) & 0x1Fu;
    const unsigned size_code = (cnt >> 14) & 3u;
    const std::uint64_t tiles_per_side = 16ull << size_code;
    const std::uint64_t map_bytes = tiles_per_side * tiles_per_side;
    const std::uint64_t map_first = kVramStart + static_cast<std::uint64_t>(base) * 0x800ull;
    const std::uint64_t map_last = map_first + map_bytes;
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    return first < map_last && last > map_first;
}

bool dma_trace_bounds(std::uint32_t destination, std::uint32_t step,
                      std::uint32_t units, std::uint32_t dest_control,
                      std::uint32_t& first, std::uint32_t& last) {
    if (step == 0u || units == 0u) return false;
    const std::uint64_t span = static_cast<std::uint64_t>(step) * units;
    std::uint64_t lo = destination;
    std::uint64_t hi = destination;
    if (dest_control == 1u) {
        lo = destination >= span - step
            ? static_cast<std::uint64_t>(destination) - (span - step)
            : 0u;
        hi = static_cast<std::uint64_t>(destination) + step;
    } else if (dest_control == 2u) {
        hi = static_cast<std::uint64_t>(destination) + step;
    } else {
        hi = static_cast<std::uint64_t>(destination) + span;
    }
    if (lo > 0xFFFFFFFFull || hi > 0xFFFFFFFFull) return false;
    first = static_cast<std::uint32_t>(lo);
    last = static_cast<std::uint32_t>(hi);
    return true;
}

std::uint16_t bgcnt(const std::uint8_t* io, unsigned bg) {
    return load16(io, IoReg::BG0CNT + bg * 2u);
}

std::uint16_t hofs(const std::uint8_t* io, unsigned bg) {
    return load16(io, 0x10u + bg * 4u);
}

std::uint16_t vofs(const std::uint8_t* io, unsigned bg) {
    return load16(io, 0x12u + bg * 4u);
}

std::uint32_t selected_bg0_text_regions(const std::uint8_t* io,
                                        std::uint32_t address,
                                        std::uint64_t size) {
    if (!io || size == 0u) return 0u;
    const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
    const unsigned mode = dispcnt & 7u;
    // BG0 is a regular text layer in display modes 0 and 1. Keep tracing its
    // selected ranges while hidden too: menu/font uploads can precede the
    // display enable write that makes them visible.
    if (mode > 1u) return 0u;

    const std::uint16_t cnt = load16(io, IoReg::BG0CNT);
    const unsigned char_block = (cnt >> 2) & 3u;
    const std::uint64_t char_first =
        kVramStart + static_cast<std::uint64_t>(char_block) * 0x4000ull;
    // Follow the renderer's tile addressing and physical VRAM clamp: tile
    // indices are 10 bits, each tile is 32 bytes in 4bpp or 64 bytes in
    // 8bpp, and reads at/after 96 KiB are skipped.
    const std::uint64_t char_span = (cnt & 0x0080u) != 0u
        ? 0x10000ull : 0x8000ull;
    const std::uint64_t char_last =
        std::min<std::uint64_t>(char_first + char_span, kVramEnd);
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    std::uint32_t regions = 0u;
    if (first < char_last && last > char_first)
        regions |= kTextRegionChar;
    if (regular_bg_range_intersects(io, 0u, address, size))
        regions |= kTextRegionScreenMap;
    return regions;
}

const char* text_region_name(std::uint32_t regions) {
    switch (regions) {
        case kTextRegionChar: return "char";
        case kTextRegionScreenMap: return "screenmap";
        case kTextRegionChar | kTextRegionScreenMap: return "char+screenmap";
        default: return "unknown";
    }
}

bool ensure_text_trace_file() {
    if (!g_text_trace_enabled || g_text_trace_open_failed) return false;
    if (g_text_trace_file) return true;
    const std::string path = g_text_trace_directory + "/text_vram_writes.csv";
    g_text_trace_file = std::fopen(path.c_str(), "wb");
    if (!g_text_trace_file) {
        g_text_trace_open_failed = true;
        return false;
    }
    static constexpr char kHeader[] =
        "sequence,frame,cycle,source_kind,pc,address,range_size,dma_channel,"
        "dma_source,dma_destination,dma_bytes,dma_control,dma_start_mode,dispcnt,"
        "bg0cnt,bg0hofs,bg0vofs,char_base,screen_base,region\n";
    if (std::fputs(kHeader, g_text_trace_file) == EOF) {
        std::fclose(g_text_trace_file);
        g_text_trace_file = nullptr;
        g_text_trace_open_failed = true;
        return false;
    }
    return true;
}

void append_text_trace_row(const std::uint8_t* io, const char* source_kind,
                           std::uint32_t pc, std::uint32_t address,
                           std::uint32_t range_size, int channel,
                           std::uint32_t dma_source,
                           std::uint32_t dma_destination,
                           std::uint32_t dma_bytes,
                           std::uint16_t dma_control, int start_mode,
                           std::uint32_t regions) {
    if (!g_text_trace_enabled || !ensure_text_trace_file()) return;
    const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
    const std::uint16_t cnt = load16(io, IoReg::BG0CNT);
    const unsigned char_block = (cnt >> 2) & 3u;
    const unsigned screen_block = (cnt >> 8) & 0x1Fu;
    const std::uint32_t char_base = static_cast<std::uint32_t>(
        kVramStart + static_cast<std::uint32_t>(char_block) * 0x4000u);
    const std::uint32_t screen_base = static_cast<std::uint32_t>(
        kVramStart + static_cast<std::uint32_t>(screen_block) * 0x800u);
    char line[512];
    const int written = std::snprintf(
        line, sizeof(line),
        "%llu,%llu,%llu,%s,0x%08X,0x%08X,%u,%d,0x%08X,0x%08X,%u,0x%04X,%d,"
        "0x%04X,0x%04X,%u,%u,0x%08X,0x%08X,%s\n",
        static_cast<unsigned long long>(g_text_trace_sequence++),
        static_cast<unsigned long long>(runtime_current_frame()),
        static_cast<unsigned long long>(g_runtime_cycles), source_kind,
        static_cast<unsigned>(pc), static_cast<unsigned>(address),
        static_cast<unsigned>(range_size), channel,
        static_cast<unsigned>(dma_source),
        static_cast<unsigned>(dma_destination),
        static_cast<unsigned>(dma_bytes),
        static_cast<unsigned>(dma_control), start_mode,
        static_cast<unsigned>(dispcnt), static_cast<unsigned>(cnt),
        static_cast<unsigned>(hofs(io, 0u)), static_cast<unsigned>(vofs(io, 0u)),
        static_cast<unsigned>(char_base), static_cast<unsigned>(screen_base),
        text_region_name(regions));
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(line)) {
        return;
    }
    g_text_trace_buffer.append(line, static_cast<std::size_t>(written));
    ++g_text_trace_pending_rows;
    if (g_text_trace_pending_rows >= kTextTraceFlushRows)
        flush_text_trace();
}

void flush_text_trace() {
    if (!g_text_trace_file || g_text_trace_buffer.empty()) return;
    const std::size_t bytes = std::fwrite(g_text_trace_buffer.data(), 1,
                                          g_text_trace_buffer.size(),
                                          g_text_trace_file);
    if (bytes != g_text_trace_buffer.size()) {
        g_text_trace_open_failed = true;
    } else {
        g_text_trace_buffer.clear();
        g_text_trace_pending_rows = 0;
        std::fflush(g_text_trace_file);
    }
}

// ---- spell-effect canvas trace (GSR_EFFECT_TRACE) -------------------------
//
// Golden Sun does not store its spell effects as tile art. It software-paints
// each frame's effect into an affine background's CHARACTER block, using that
// block as a 128x128 8-bit canvas: BG2's map holds 256 distinct tile ids,
// 0..255, one each, and the 16 KB behind it differs on every one of the six
// captured effect frames (FACTS.md, 2026-09-18). The hardware then stretches
// that canvas 2x horizontally across the console's 240-pixel screen, which is
// why a widened view shows the effect stopping in a straight line: the canvas
// is only as wide as the old screen, and the artwork runs to its edge.
//
// This trace exists to answer whether a wider canvas is worth building. It
// reports two things per frame:
//
//   * WHO paints. The writer PCs are guest ROM addresses, so each one can be
//     looked up in the symbol corpus and the routine's coordinate arithmetic
//     read directly -- that is what says whether the 128 is a constant that
//     can be widened or a shape baked through the whole routine.
//   * WHERE inside the canvas each writer lands, as a per-frame column
//     histogram. A painter that CLAMPS what it cannot fit shows up as a spike
//     on column 0 or the last column; one that drops it shows a normal
//     profile under artwork that is solid at the edge.
//
// Payload-free, like every other stream in this file: writer PC, byte counts
// and canvas coordinates only, never guest bytes.
//
// Rows are aggregated per frame per writer rather than one per store. The
// painter issues thousands of stores a frame and the question is which
// routines paint and how far they reach, not the order of the stores -- an
// unaggregated row per store would be both unreadable and slow enough to
// change what is being measured.
bool g_effect_env_read = false;
bool g_effect_enabled = false;
bool g_effect_open_failed = false;
bool g_effect_atexit_registered = false;
std::FILE* g_effect_writers_file = nullptr;
std::FILE* g_effect_columns_file = nullptr;
std::string g_effect_writers_buffer;
std::string g_effect_columns_buffer;
std::uint64_t g_effect_writer_rows = 0;
std::uint64_t g_effect_column_rows = 0;

// Budgets, sized as "how much answer is enough" rather than tuned to a
// measurement: a few minutes of battles at 60 Hz, with room for every writer
// in each frame.
constexpr std::uint64_t kEffectMaxWriterRows = 200000ull;
constexpr std::uint64_t kEffectMaxColumnRows = 40000ull;
constexpr unsigned kEffectMaxWriters = 48u;
constexpr unsigned kEffectMaxColumns = 256u;

struct EffectWriter {
    std::uint32_t pc = 0;
    std::uint8_t kind = 0;   // 0 CPU store, 1 DMA descriptor
    std::uint8_t layer = 0;
    std::uint32_t writes = 0;
    std::uint32_t bytes = 0;
    std::uint32_t min_col = 0xFFFFFFFFu;
    std::uint32_t max_col = 0;
    std::uint32_t min_row = 0xFFFFFFFFu;
    std::uint32_t max_row = 0;
};

EffectWriter g_effect_writers[kEffectMaxWriters];
unsigned g_effect_writer_used = 0;
std::uint32_t g_effect_columns[kEffectMaxColumns] = {};
bool g_effect_frame_open = false;
unsigned long long g_effect_frame = 0;
std::uint32_t g_effect_base = 0;
std::uint32_t g_effect_px = 0;
unsigned g_effect_layer = 0;
std::uint16_t g_effect_dispcnt = 0;
std::uint16_t g_effect_bgcnt = 0;
int g_effect_pa = 0;
int g_effect_ref_x = 0;

bool effect_trace_enabled() {
    // Same shape as trace_enabled(): consult the environment once. This runs
    // on every VRAM write, and a getenv per write was measured at 11.5% of a
    // profile once already.
    if (!g_effect_env_read) {
        g_effect_env_read = true;
        const char* env = std::getenv("GSR_EFFECT_TRACE");
        g_effect_enabled = env != nullptr && env[0] != '\0' && env[0] != '0';
    }
    return g_effect_enabled;
}

void flush_effect_files() {
    if (g_effect_writers_file && !g_effect_writers_buffer.empty()) {
        std::fwrite(g_effect_writers_buffer.data(), 1,
                    g_effect_writers_buffer.size(), g_effect_writers_file);
        g_effect_writers_buffer.clear();
        std::fflush(g_effect_writers_file);
    }
    if (g_effect_columns_file && !g_effect_columns_buffer.empty()) {
        std::fwrite(g_effect_columns_buffer.data(), 1,
                    g_effect_columns_buffer.size(), g_effect_columns_file);
        g_effect_columns_buffer.clear();
        std::fflush(g_effect_columns_file);
    }
}

void effect_emit_frame();

void flush_effect_at_exit() {
    effect_emit_frame();
    flush_effect_files();
}

// Numbered like the F12 frame dumps so a second capture session cannot
// silently overwrite the first one the user played for.
bool ensure_effect_files() {
    if (g_effect_open_failed) return false;
    if (g_effect_writers_file && g_effect_columns_file) return true;
    for (unsigned n = 1; n <= 999u; ++n) {
        char writers[128];
        char columns[128];
        std::snprintf(writers, sizeof writers,
                      "logs/effect_canvas_writers_%03u.csv", n);
        std::snprintf(columns, sizeof columns,
                      "logs/effect_canvas_columns_%03u.csv", n);
        std::FILE* probe = std::fopen(writers, "rb");
        if (probe) { std::fclose(probe); continue; }
        g_effect_writers_file = std::fopen(writers, "wb");
        g_effect_columns_file = std::fopen(columns, "wb");
        if (!g_effect_writers_file || !g_effect_columns_file) break;
        std::fputs("frame,layer,canvas_base,canvas_px,kind,pc,writes,bytes,"
                   "min_col,max_col,min_row,max_row,dispcnt,bgcnt,pa,ref_x\n",
                   g_effect_writers_file);
        std::fputs("frame,canvas_px,counts_by_column\n", g_effect_columns_file);
        std::fprintf(stderr, "[effect-trace] writing %s and %s\n", writers,
                     columns);
        if (!g_effect_atexit_registered) {
            g_effect_atexit_registered = true;
            std::atexit(&flush_effect_at_exit);
        }
        return true;
    }
    if (g_effect_writers_file) std::fclose(g_effect_writers_file);
    if (g_effect_columns_file) std::fclose(g_effect_columns_file);
    g_effect_writers_file = nullptr;
    g_effect_columns_file = nullptr;
    g_effect_open_failed = true;
    std::fprintf(stderr,
                 "[effect-trace] could not open logs/effect_canvas_*.csv; is "
                 "there a logs directory beside the game?\n");
    return false;
}

void effect_reset_frame() {
    g_effect_writer_used = 0;
    for (unsigned i = 0; i < kEffectMaxColumns; ++i) g_effect_columns[i] = 0u;
    g_effect_frame_open = false;
}

void effect_emit_frame() {
    if (!g_effect_frame_open) return;
    if (!ensure_effect_files()) { effect_reset_frame(); return; }
    char line[384];
    for (unsigned i = 0; i < g_effect_writer_used; ++i) {
        if (g_effect_writer_rows >= kEffectMaxWriterRows) break;
        const EffectWriter& w = g_effect_writers[i];
        const int written = std::snprintf(
            line, sizeof line,
            "%llu,%u,0x%08X,%u,%s,0x%08X,%u,%u,%u,%u,%u,%u,0x%04X,0x%04X,"
            "%d,%d\n",
            static_cast<unsigned long long>(g_effect_frame),
            static_cast<unsigned>(w.layer), g_effect_base, g_effect_px,
            w.kind ? "dma" : "cpu", w.pc, w.writes, w.bytes, w.min_col,
            w.max_col, w.min_row, w.max_row,
            static_cast<unsigned>(g_effect_dispcnt),
            static_cast<unsigned>(g_effect_bgcnt), g_effect_pa,
            g_effect_ref_x);
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof line)
            continue;
        g_effect_writers_buffer.append(line, static_cast<std::size_t>(written));
        ++g_effect_writer_rows;
    }
    if (g_effect_column_rows < kEffectMaxColumnRows) {
        const unsigned span =
            g_effect_px < kEffectMaxColumns ? g_effect_px : kEffectMaxColumns;
        char head[64];
        const int n = std::snprintf(
            head, sizeof head, "%llu,%u",
            static_cast<unsigned long long>(g_effect_frame), g_effect_px);
        if (n > 0) {
            g_effect_columns_buffer.append(head, static_cast<std::size_t>(n));
            for (unsigned c = 0; c < span; ++c) {
                char cell[16];
                const int m =
                    std::snprintf(cell, sizeof cell, ",%u", g_effect_columns[c]);
                if (m > 0)
                    g_effect_columns_buffer.append(cell,
                                                   static_cast<std::size_t>(m));
            }
            g_effect_columns_buffer.append("\n", 1);
            ++g_effect_column_rows;
        }
    }
    effect_reset_frame();
    // One frame's worth of rows is small, and a capture session ends with the
    // player closing the game, which is not always a clean exit. Flush per
    // frame so a hard close still leaves everything they played for on disk.
    flush_effect_files();
}

// The canvas is the affine layer's CHARACTER block, and both its base and its
// size come from BGCNT live -- never a remembered address, because the game
// moves both between spells. An affine map is (128 << size_code) pixels
// square at one byte per pixel, so the block spans px*px bytes.
bool effect_canvas_hit(const std::uint8_t* io, std::uint32_t address,
                       std::uint64_t size, unsigned* layer_out,
                       std::uint32_t* base_out, std::uint32_t* px_out) {
    const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
    const unsigned mode = dispcnt & 7u;
    if (mode != 1u && mode != 2u) return false;
    const unsigned last_bg = (mode == 1u) ? 2u : 3u;
    for (unsigned bg = 2u; bg <= last_bg; ++bg) {
        if ((dispcnt & (0x100u << bg)) == 0u) continue;
        const std::uint16_t cnt = bgcnt(io, bg);
        const std::uint32_t base = static_cast<std::uint32_t>(
            kVramStart + ((cnt >> 2) & 3u) * 0x4000u);
        const std::uint32_t px = 128u << ((cnt >> 14) & 3u);
        const std::uint64_t first = address;
        const std::uint64_t last = first + size;
        const std::uint64_t block_first = base;
        const std::uint64_t block_last =
            block_first + static_cast<std::uint64_t>(px) * px;
        if (first < block_last && last > block_first) {
            *layer_out = bg;
            *base_out = base;
            *px_out = px;
            return true;
        }
    }
    return false;
}

void effect_note(const std::uint8_t* io, std::uint8_t kind, std::uint32_t pc,
                 std::uint32_t address, std::uint64_t size) {
    if (!effect_trace_enabled() || size == 0u) return;
    unsigned layer = 0u;
    std::uint32_t base = 0u;
    std::uint32_t px = 0u;
    if (!effect_canvas_hit(io, address, size, &layer, &base, &px)) return;

    const unsigned long long frame = runtime_current_frame();
    if (g_effect_frame_open && frame != g_effect_frame) effect_emit_frame();
    if (!g_effect_frame_open) {
        g_effect_frame_open = true;
        g_effect_frame = frame;
    }

    // Frame-level context. Re-read per write so the frame records the state
    // the painter actually ran under rather than a stale first value.
    g_effect_layer = layer;
    g_effect_base = base;
    g_effect_px = px;
    g_effect_dispcnt = load16(io, IoReg::DISPCNT);
    g_effect_bgcnt = bgcnt(io, layer);
    const std::uint32_t param = 0x20u + (layer - 2u) * 0x10u;  // BG2PA/BG3PA
    const std::uint16_t pa_raw = load16(io, param);
    g_effect_pa = pa_raw >= 0x8000u ? static_cast<int>(pa_raw) - 0x10000
                                    : static_cast<int>(pa_raw);
    const std::uint32_t ref = 0x28u + (layer - 2u) * 0x10u;    // BG2X/BG3X
    std::uint32_t ref_raw =
        static_cast<std::uint32_t>(load16(io, ref)) |
        (static_cast<std::uint32_t>(load16(io, ref + 2u)) << 16);
    ref_raw &= 0x0FFFFFFFu;  // the reference is 28 bits
    g_effect_ref_x = (ref_raw & 0x08000000u)
                         ? static_cast<int>(ref_raw) - 0x10000000
                         : static_cast<int>(ref_raw);

    // Canvas coordinates. The block is a run of 8x8 tiles in map order, which
    // is what makes it a canvas rather than a tile set: tile k sits at cell k.
    const std::uint64_t clipped_first = address > base ? address : base;
    const std::uint64_t block_last =
        static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(px) * px;
    std::uint64_t clipped_last = static_cast<std::uint64_t>(address) + size;
    if (clipped_last > block_last) clipped_last = block_last;
    if (clipped_last <= clipped_first) return;

    const std::uint32_t tiles_across = px >> 3;
    auto column_of = [&](std::uint64_t at) {
        const std::uint32_t off = static_cast<std::uint32_t>(at - base);
        const std::uint32_t tile = off >> 6;
        return (tile % tiles_across) * 8u + (off & 7u);
    };
    auto row_of = [&](std::uint64_t at) {
        const std::uint32_t off = static_cast<std::uint32_t>(at - base);
        const std::uint32_t tile = off >> 6;
        return (tile / tiles_across) * 8u + ((off >> 3) & 7u);
    };
    const std::uint32_t first_col = column_of(clipped_first);
    const std::uint32_t last_col = column_of(clipped_last - 1u);
    const std::uint32_t first_row = row_of(clipped_first);
    const std::uint32_t last_row = row_of(clipped_last - 1u);
    const std::uint32_t lo_col = first_col < last_col ? first_col : last_col;
    const std::uint32_t hi_col = first_col < last_col ? last_col : first_col;
    const std::uint32_t lo_row = first_row < last_row ? first_row : last_row;
    const std::uint32_t hi_row = first_row < last_row ? last_row : first_row;

    // A CPU store spans at most four bytes, so counting its columns exactly
    // is cheap. A DMA can cover the whole canvas, and counting every column
    // of it would drown the histogram in one descriptor, so a bulk transfer
    // is recorded as one touch at each end of its reach. The writer row's
    // min/max columns still carry the whole span.
    if (kind == 0u) {
        for (std::uint64_t at = clipped_first; at < clipped_last; ++at) {
            const std::uint32_t c = column_of(at);
            if (c < kEffectMaxColumns) ++g_effect_columns[c];
        }
    } else {
        if (lo_col < kEffectMaxColumns) ++g_effect_columns[lo_col];
        if (hi_col != lo_col && hi_col < kEffectMaxColumns)
            ++g_effect_columns[hi_col];
    }

    EffectWriter* slot = nullptr;
    for (unsigned i = 0; i < g_effect_writer_used; ++i) {
        if (g_effect_writers[i].pc == pc && g_effect_writers[i].kind == kind) {
            slot = &g_effect_writers[i];
            break;
        }
    }
    if (!slot) {
        if (g_effect_writer_used >= kEffectMaxWriters) return;
        slot = &g_effect_writers[g_effect_writer_used++];
        *slot = EffectWriter{};
        slot->pc = pc;
        slot->kind = kind;
        slot->layer = static_cast<std::uint8_t>(layer);
    }
    ++slot->writes;
    slot->bytes += static_cast<std::uint32_t>(clipped_last - clipped_first);
    if (lo_col < slot->min_col) slot->min_col = lo_col;
    if (hi_col > slot->max_col) slot->max_col = hi_col;
    if (lo_row < slot->min_row) slot->min_row = lo_row;
    if (hi_row > slot->max_row) slot->max_row = hi_row;
}

// ---- effect staging-buffer trace (rides on GSR_EFFECT_TRACE) --------------
//
// The canvas trace above watches the VRAM side and found that the effect is
// not drawn there at all: it is composed in a 16 KB buffer on the game's own
// IWRAM heap and blitted whole into VRAM each frame (FACTS.md, 2026-09-18).
// The routine that actually stamps particles into that buffer is relocated
// into IWRAM at runtime, never appears as a ROM literal, and has no symbol --
// so it cannot be read out of the ROM. It has to be watched.
//
// No new bus hook is needed. Every IWRAM write already passes through
// trace_oam_shadow_write() (gba_bus.cpp, all three widths); nothing outside
// the OAM shadow range consumed it. This is that consumer.
//
// The range is the game's own IWRAM heap, whose bounds are literals in its
// allocator: it bumps from 0x03002000 and refuses past 0x030077FF
// (Func_4858 / Func_48b0, ROM 0x08004858 / 0x080048B0). Watching the whole
// heap rather than a remembered buffer address is deliberate -- the canvas is
// an allocation, so its address moves between effects and between runs, and
// the pointer lives in a global this hook cannot read.
//
// Two outputs, because two different questions:
//
//   * effect_stage_writers_NNN.csv -- per frame per writer, how much and over
//     what span. The stamp routine is whichever PC covers a 16 KB span; the
//     buffer base is the low end of that span.
//   * effect_stage_walk_NNN.csv -- a bounded raw sample, every write in order
//     for a handful of frames. This is the one that gives the STRIDE: a run of
//     8 bytes then a jump of 56 is tile-ordered 8x8, a run of 128 is linear.
//     Aggregates cannot show that; only the order of the addresses can.
//
// Payload-free like every other stream here: PC, address, size. Never a value.
constexpr std::uint32_t kStageHeapFirst = 0x03002000u;
constexpr std::uint32_t kStageHeapLast = 0x030077FFu;

bool g_stage_open_failed = false;
bool g_stage_atexit_registered = false;
std::FILE* g_stage_writers_file = nullptr;
std::FILE* g_stage_walk_file = nullptr;
std::string g_stage_writers_buffer;
std::string g_stage_walk_buffer;
std::uint64_t g_stage_writer_rows = 0;
std::uint64_t g_stage_walk_rows = 0;

constexpr std::uint64_t kStageMaxWriterRows = 200000ull;
// Enough consecutive writes to read a stride off, from enough separate frames
// that one odd frame cannot mislead. Not a threshold; a sample size. The total
// is generous because the byte-width filter below already keeps the budget
// from being spent on anything but the stamping.
constexpr std::uint64_t kStageMaxWalkRows = 16384ull;
constexpr unsigned kStageWalkRowsPerFrame = 512u;
// Frames between sampled frames. 16384/512 = 32 samples, so this spreads them
// over about two thousand frames -- a session, not its opening seconds.
constexpr unsigned long long kStageWalkFrameGap = 64ull;
constexpr unsigned kStageMaxWriters = 48u;

struct StageWriter {
    std::uint32_t pc = 0;
    std::uint32_t writes = 0;
    std::uint32_t bytes = 0;
    std::uint32_t low = 0xFFFFFFFFu;
    std::uint32_t high = 0;
};

StageWriter g_stage_writers[kStageMaxWriters];
unsigned g_stage_writer_used = 0;
bool g_stage_frame_open = false;
unsigned long long g_stage_frame = 0;
unsigned g_stage_walk_this_frame = 0;
// Whether THIS frame is one of the spaced-out sampled ones, and the last frame
// that was. Decided once when the frame opens, so a frame is sampled whole.
bool g_stage_walk_frame_taken = false;
unsigned long long g_stage_walk_last_frame = 0;
bool g_stage_walk_any = false;

void stage_emit_frame();

void flush_stage_files() {
    if (g_stage_writers_file && !g_stage_writers_buffer.empty()) {
        std::fwrite(g_stage_writers_buffer.data(), 1,
                    g_stage_writers_buffer.size(), g_stage_writers_file);
        g_stage_writers_buffer.clear();
        std::fflush(g_stage_writers_file);
    }
    if (g_stage_walk_file && !g_stage_walk_buffer.empty()) {
        std::fwrite(g_stage_walk_buffer.data(), 1, g_stage_walk_buffer.size(),
                    g_stage_walk_file);
        g_stage_walk_buffer.clear();
        std::fflush(g_stage_walk_file);
    }
}

void flush_stage_at_exit() {
    stage_emit_frame();
    flush_stage_files();
}

bool ensure_stage_files() {
    if (g_stage_open_failed) return false;
    if (g_stage_writers_file && g_stage_walk_file) return true;
    for (unsigned n = 1; n <= 999u; ++n) {
        char writers[128];
        char walk[128];
        std::snprintf(writers, sizeof writers,
                      "logs/effect_stage_writers_%03u.csv", n);
        std::snprintf(walk, sizeof walk, "logs/effect_stage_walk_%03u.csv", n);
        std::FILE* probe = std::fopen(writers, "rb");
        if (probe) { std::fclose(probe); continue; }
        g_stage_writers_file = std::fopen(writers, "wb");
        g_stage_walk_file = std::fopen(walk, "wb");
        if (!g_stage_writers_file || !g_stage_walk_file) break;
        std::fputs("frame,pc,writes,bytes,low,high,span\n",
                   g_stage_writers_file);
        std::fputs("frame,seq,pc,address,size\n", g_stage_walk_file);
        std::fprintf(stderr, "[effect-trace] writing %s and %s\n", writers,
                     walk);
        if (!g_stage_atexit_registered) {
            g_stage_atexit_registered = true;
            std::atexit(&flush_stage_at_exit);
        }
        return true;
    }
    if (g_stage_writers_file) std::fclose(g_stage_writers_file);
    if (g_stage_walk_file) std::fclose(g_stage_walk_file);
    g_stage_writers_file = nullptr;
    g_stage_walk_file = nullptr;
    g_stage_open_failed = true;
    return false;
}

void stage_emit_frame() {
    if (!g_stage_frame_open) return;
    if (!ensure_stage_files()) {
        g_stage_writer_used = 0;
        g_stage_frame_open = false;
        return;
    }
    char line[256];
    for (unsigned i = 0; i < g_stage_writer_used; ++i) {
        if (g_stage_writer_rows >= kStageMaxWriterRows) break;
        const StageWriter& w = g_stage_writers[i];
        const int written = std::snprintf(
            line, sizeof line, "%llu,0x%08X,%u,%u,0x%08X,0x%08X,%u\n",
            static_cast<unsigned long long>(g_stage_frame), w.pc, w.writes,
            w.bytes, w.low, w.high, w.high - w.low);
        if (written <= 0 || static_cast<std::size_t>(written) >= sizeof line)
            continue;
        g_stage_writers_buffer.append(line, static_cast<std::size_t>(written));
        ++g_stage_writer_rows;
    }
    g_stage_writer_used = 0;
    g_stage_frame_open = false;
    g_stage_walk_this_frame = 0;
    flush_stage_files();
}

void effect_stage_note(std::uint32_t pc, std::uint32_t address,
                       std::uint32_t size) {
    if (size == 0u) return;
    if (address < kStageHeapFirst || address > kStageHeapLast) return;

    const unsigned long long frame = runtime_current_frame();
    if (g_stage_frame_open && frame != g_stage_frame) stage_emit_frame();
    if (!g_stage_frame_open) {
        g_stage_frame_open = true;
        g_stage_frame = frame;
        g_stage_walk_this_frame = 0;
        g_stage_walk_frame_taken =
            !g_stage_walk_any ||
            frame >= g_stage_walk_last_frame + kStageWalkFrameGap;
    }

    // The ordered sample: single-byte writes issued BY CODE RUNNING IN THE
    // HEAP, and only every so many frames.
    //
    // Two attempts were spent getting this filter right, both times because
    // the budget went on the BIOS at boot instead of the stamping:
    //
    //   * sampling every write caught frames 0..27, all of it one BIOS word
    //     fill stepping +4 (logs/effect_stage_walk_001.csv).
    //   * narrowing to 1-byte writes caught frames 0..48, because the BIOS
    //     writes single bytes too -- 0x000020A0 and 0x000020B0 alone made
    //     124,096 of them (logs/effect_stage_walk_002.csv).
    //
    // What actually separates them is WHERE THE WRITER IS EXECUTING. The stamp
    // routine is relocated into the heap, immediately behind the canvas it
    // draws into, so its PC is inside the heap as well: the 136 pure 1-byte
    // writers measured inside this range are the stampers, and all 60 outside
    // it are BIOS or ROM. The BIOS cannot match, because no game code has been
    // copied here yet while it runs.
    //
    // The frame spacing is the other half. A budget spent on the first frames
    // that qualify would document one menu effect rather than a battle, so
    // take a frame only once every kStageWalkFrameGap frames; that spreads
    // 32 sampled frames across two thousand, which is a whole session rather
    // than its first few seconds.
    const bool walk_candidate =
        size == 1u && pc >= kStageHeapFirst && pc <= kStageHeapLast;
    if (walk_candidate && g_stage_walk_frame_taken &&
        g_stage_walk_rows < kStageMaxWalkRows &&
        g_stage_walk_this_frame < kStageWalkRowsPerFrame) {
        char row[96];
        const int written = std::snprintf(
            row, sizeof row, "%llu,%u,0x%08X,0x%08X,%u\n",
            static_cast<unsigned long long>(g_stage_frame),
            g_stage_walk_this_frame, pc, address, size);
        if (written > 0 && static_cast<std::size_t>(written) < sizeof row) {
            g_stage_walk_buffer.append(row, static_cast<std::size_t>(written));
            ++g_stage_walk_rows;
            ++g_stage_walk_this_frame;
            // Only a frame that actually produced a row counts as sampled, so
            // the spacing is measured between USEFUL frames rather than
            // between frames that merely qualified and had nothing to give.
            g_stage_walk_last_frame = g_stage_frame;
            g_stage_walk_any = true;
        }
    }

    StageWriter* slot = nullptr;
    for (unsigned i = 0; i < g_stage_writer_used; ++i) {
        if (g_stage_writers[i].pc == pc) { slot = &g_stage_writers[i]; break; }
    }
    if (!slot) {
        if (g_stage_writer_used >= kStageMaxWriters) return;
        slot = &g_stage_writers[g_stage_writer_used++];
        *slot = StageWriter{};
        slot->pc = pc;
    }
    ++slot->writes;
    slot->bytes += size;
    if (address < slot->low) slot->low = address;
    const std::uint32_t last = address + size - 1u;
    if (last > slot->high) slot->high = last;
}

unsigned& cpu_records() {
    static unsigned n = 0;
    return n;
}

unsigned& dma_records() {
    static unsigned n = 0;
    return n;
}

// Independent budgets so an overworld (Mode 2 affine) session cannot exhaust
// the Mode 0 field trace's record budget, or vice versa.
unsigned& affine_cpu_records() {
    static unsigned n = 0;
    return n;
}

unsigned& affine_dma_records() {
    static unsigned n = 0;
    return n;
}

// A per-writer budget for the Mode 0 stream, replacing a single shared count.
// The point of this trace is to ENUMERATE the writers, not to sample writes,
// and a shared count does not do that: in session_20260905_092954 one chatty
// writer pair (`0x0800FF1E` / `0x0800FF26`, the BG3 tilemap builder) took all
// 512 records at a single room load, so the BG1 and BG2 writers were never
// recorded at all. The two capacities below are how many distinct writers to
// keep room for and how many examples of each are enough to read its
// addressing pattern — sizes of the answer wanted, not thresholds tuned to a
// measurement.
constexpr unsigned kMode0MaxWriters = 64u;
constexpr unsigned kMode0RecordsPerWriter = 4u;

std::uint32_t g_mode0_pcs[kMode0MaxWriters] = {};
unsigned g_mode0_counts[kMode0MaxWriters] = {};
unsigned g_mode0_used = 0;

bool mode0_pc_budget(std::uint32_t pc) {
    for (unsigned i = 0; i < g_mode0_used; ++i) {
        if (g_mode0_pcs[i] != pc) continue;
        if (g_mode0_counts[i] >= kMode0RecordsPerWriter) return false;
        ++g_mode0_counts[i];
        return true;
    }
    if (g_mode0_used >= kMode0MaxWriters) return false;
    g_mode0_pcs[g_mode0_used] = pc;
    g_mode0_counts[g_mode0_used] = 1;
    ++g_mode0_used;
    return true;
}

void reset_mode0_pc_budget() { g_mode0_used = 0; }

// Per-visit budgets, not per-session. In session_20260905_084755 all 512
// affine CPU records were spent between cycle 7,878,553 and 57,203,487 --
// the first ~3.4 s of a run that reached cycle 595M, i.e. boot, before the
// savestate load -- so the overworld play the session was captured for
// recorded nothing at all. Reset both affine budgets on each transition
// into an affine Mode 2 scene so every visit gets its own records.
void note_affine_scene_edge(const std::uint8_t* io) {
    static bool s_was_affine = false;
    const bool now_affine = mode2_affine(io);
    if (now_affine && !s_was_affine) {
        affine_cpu_records() = 0;
        affine_dma_records() = 0;
    }
    s_was_affine = now_affine;
}

unsigned& dma_depth() {
    static unsigned n = 0;
    return n;
}

constexpr std::uint32_t kOamShadowStart = 0x0300347Cu;
constexpr std::uint32_t kOamShadowEnd   = 0x0300387Cu;
constexpr std::uint32_t kOamStart = 0x07000000u;
constexpr std::uint32_t kOamEnd = 0x07000400u;
constexpr std::size_t kOamSlotBytes = 8u;
constexpr std::size_t kOamSlotCount = 128u;

OamShadowTraceStats g_oam_shadow_stats{};
OamDmaTraceStats g_oam_dma_stats{};
OamAttr0TraceStats g_oam_attr0_stats{};
std::array<OamAttr0Provenance, kOamSlotCount> g_oam_attr0_provenance{};
std::uint64_t g_oam_attr0_generation = 0;
std::array<bool, kOamSlotCount> g_oam_shadow_slot_seen{};
std::array<bool, kOamSlotCount> g_oam_shadow_slot_overwritten{};
unsigned g_oam_shadow_records = 0;
unsigned g_oam_dma_records = 0;

bool oam_shadow_trace_enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* shadow_env = std::getenv("GSR_OAM_SHADOW_TRACE");
        const char* wide_env = std::getenv("GBARECOMP_VRAM_MAP_TRACE");
        const bool shadow = shadow_env && shadow_env[0] != '\0' &&
            shadow_env[0] != '0';
        const bool wide = wide_env && wide_env[0] != '\0' &&
            wide_env[0] != '0';
        // The WIDE-01 launcher toggle arms the complete payload-free object
        // capture. Keep the older standalone OAM toggle as a compatible alias.
        cached = (shadow || wide) ? 1 : 0;
    }
    return cached != 0;
}

std::uint32_t normalize_oam_shadow_address(std::uint32_t address) {
    return (address >> 24) == 0x03u
        ? 0x03000000u + (address & 0x00007FFFu) : address;
}

unsigned& oam_shadow_records() {
    return g_oam_shadow_records;
}

bool oam_dma_bounds(std::uint32_t destination, std::uint32_t step,
                    std::uint32_t units, std::uint32_t dest_control,
                    std::uint32_t& first, std::uint32_t& last) {
    if (step == 0u || units == 0u) return false;
    const std::uint64_t span = static_cast<std::uint64_t>(step) * units;
    std::uint64_t lo = destination;
    std::uint64_t hi = destination;
    if (dest_control == 1u) {
        lo = destination >= span - step
            ? static_cast<std::uint64_t>(destination) - (span - step)
            : 0u;
        hi = static_cast<std::uint64_t>(destination) + step;
    } else if (dest_control == 2u) {
        hi = static_cast<std::uint64_t>(destination) + step;
    } else {
        hi = static_cast<std::uint64_t>(destination) + span;
    }
    if (lo > 0xFFFFFFFFull || hi > 0xFFFFFFFFull || hi <= lo) return false;
    first = static_cast<std::uint32_t>(lo);
    last = static_cast<std::uint32_t>(hi);
    return first < kOamEnd && last > kOamStart;
}

struct OamSlotCounts {
    std::uint32_t used = 0;
    std::uint32_t visible = 0;
    std::uint32_t nonzero = 0;
    std::uint32_t raw_x_ge_240 = 0;
    std::uint32_t raw_y_ge_160 = 0;
};

void note_oam_attr0_write(std::uint32_t pc, std::uint32_t address,
                          std::uint32_t size, OamAttr0WriterKind kind) {
    address = normalize_oam_shadow_address(address);
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    if (size == 0u || first >= kOamShadowEnd || last <= kOamShadowStart)
        return;
    const std::uint64_t clipped_first =
        std::max<std::uint64_t>(first, kOamShadowStart);
    const std::uint64_t clipped_last =
        std::min<std::uint64_t>(last, kOamShadowEnd);
    for (std::size_t slot = 0; slot < kOamSlotCount; ++slot) {
        const std::uint32_t attr0 = kOamShadowStart +
            static_cast<std::uint32_t>(slot * kOamSlotBytes);
        const std::uint64_t attr0_end = static_cast<std::uint64_t>(attr0) + 2u;
        if (clipped_first >= attr0_end || clipped_last <= attr0) continue;
        OamAttr0Provenance& p = g_oam_attr0_provenance[slot];
        p.kind = kind;
        p.writer_pc = pc;
        p.generation = ++g_oam_attr0_generation;
        p.cycle = g_runtime_cycles;
        p.touched_bytes = 0;
        if (clipped_first <= attr0 && clipped_last > attr0)
            p.touched_bytes |= 1u;
        if (clipped_first < attr0 + 2u && clipped_last > attr0 + 1u)
            p.touched_bytes |= 2u;
    }
}

void note_oam_attr0_candidate(std::uint8_t raw_y, std::size_t slot) {
    if (raw_y < 160u || raw_y > 199u || slot >= kOamSlotCount) return;
    ++g_oam_attr0_stats.candidate_slots;
    if (raw_y == 192u) ++g_oam_attr0_stats.exact_192_slots;
    else ++g_oam_attr0_stats.other_slots;
    const OamAttr0Provenance& p = g_oam_attr0_provenance[slot];
    if (p.kind == OamAttr0WriterKind::Unseen) ++g_oam_attr0_stats.unseen_slots;
    for (std::uint32_t i = 0; i < g_oam_attr0_stats.group_count; ++i) {
        OamAttr0CandidateGroup& g = g_oam_attr0_stats.groups[i];
        if (g.raw_y == raw_y && g.kind == p.kind &&
            g.writer_pc == p.writer_pc) {
            g.slot_first = std::min<std::uint32_t>(g.slot_first,
                                                    static_cast<std::uint32_t>(slot));
            g.slot_last = std::max<std::uint32_t>(g.slot_last,
                                                   static_cast<std::uint32_t>(slot));
            ++g.slot_count;
            return;
        }
    }
    if (g_oam_attr0_stats.group_count >=
        sizeof(g_oam_attr0_stats.groups) / sizeof(g_oam_attr0_stats.groups[0])) {
        ++g_oam_attr0_stats.groups_dropped;
        return;
    }
    OamAttr0CandidateGroup& g =
        g_oam_attr0_stats.groups[g_oam_attr0_stats.group_count++];
    g.raw_y = raw_y;
    g.kind = p.kind;
    g.writer_pc = p.writer_pc;
    g.slot_first = g.slot_last = static_cast<std::uint32_t>(slot);
    g.slot_count = 1u;
    g.generation = p.generation;
    g.cycle = p.cycle;
}

OamSlotCounts count_oam_slots(armv4t::Bus* bus) {
    OamSlotCounts counts{};
    if (!bus) return counts;
    for (std::size_t slot = 0; slot < kOamSlotCount; ++slot) {
        const std::uint32_t address = kOamStart +
            static_cast<std::uint32_t>(slot * kOamSlotBytes);
        const std::uint16_t attr0 = bus->read16(address);
        const std::uint16_t attr1 = bus->read16(address + 2u);
        const std::uint16_t attr2 = bus->read16(address + 4u);
        const bool nonzero = attr0 != 0u || attr1 != 0u || attr2 != 0u;
        if (nonzero) ++counts.nonzero;
        const bool affine = (attr0 & 0x0100u) != 0u;
        const bool disabled = !affine && (attr0 & 0x0200u) != 0u;
        const unsigned object_mode = (attr0 >> 10) & 3u;
        const unsigned shape = (attr0 >> 14) & 3u;
        const bool used = nonzero && !disabled && shape < 3u &&
            object_mode != 3u;
        if (!used) continue;
        ++counts.used;
        if (object_mode != 2u) ++counts.visible;
        if ((attr1 & 0x01FFu) >= 240u) ++counts.raw_x_ge_240;
        if ((attr0 & 0x00FFu) >= 160u) ++counts.raw_y_ge_160;
    }
    return counts;
}

}  // namespace

bool overlaps_active_mode0_bg123(const std::uint8_t* io,
                                  std::uint32_t address,
                                  std::uint64_t size) {
    if (!mode0_field(io) || size == 0u) return false;
    const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
    for (unsigned bg = 1u; bg <= 3u; ++bg) {
        if ((dispcnt & (1u << (8u + bg))) == 0u) continue;
        if (regular_bg_range_intersects(io, bg, address, size)) return true;
    }
    return false;
}

bool dma_overlaps_active_mode0_bg123(const std::uint8_t* io,
                                     std::uint32_t destination,
                                     std::uint32_t step,
                                     std::uint32_t units,
                                     std::uint32_t dest_control) {
    std::uint32_t first = 0, last = 0;
    return dma_trace_bounds(destination, step, units, dest_control, first, last) &&
           last > first &&
           overlaps_active_mode0_bg123(io, first,
                                       static_cast<std::uint64_t>(last) - first);
}

bool overlaps_active_affine_bg23(const std::uint8_t* io,
                                 std::uint32_t address,
                                 std::uint64_t size) {
    if (!mode2_affine(io) || size == 0u) return false;
    const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
    for (unsigned bg = 2u; bg <= 3u; ++bg) {
        if ((dispcnt & (1u << (8u + bg))) == 0u) continue;
        if (affine_range_intersects(io, bg, address, size)) return true;
    }
    return false;
}

bool dma_overlaps_active_affine_bg23(const std::uint8_t* io,
                                     std::uint32_t destination,
                                     std::uint32_t step,
                                     std::uint32_t units,
                                     std::uint32_t dest_control) {
    std::uint32_t first = 0, last = 0;
    return dma_trace_bounds(destination, step, units, dest_control, first, last) &&
           last > first &&
           overlaps_active_affine_bg23(io, first,
                                       static_cast<std::uint64_t>(last) - first);
}

void trace_cpu_write(const std::uint8_t* io, std::uint32_t pc,
                     std::uint32_t address, std::uint32_t size) {
    if (dma_active()) return;
    if (g_video_write_observer)
        g_video_write_observer(pc, address, size, 0u, -1);
    if (g_text_trace_enabled) {
        const std::uint32_t regions =
            selected_bg0_text_regions(io, address, size);
        if (regions != 0u) {
            append_text_trace_row(io, "cpu", pc, address, size, -1, 0u, 0u,
                                  0u, 0u, -1, regions);
        }
    }
    // Independent of the map trace below and of its record budget: the effect
    // canvas is painted thousands of stores a frame, which would exhaust any
    // shared budget in a single explosion.
    effect_note(io, 0u, pc, address, size);
    if (!trace_enabled()) return;
    note_affine_scene_edge(io);
    if (mode0_pc_budget(pc) && overlaps_active_mode0_bg123(io, address, size)) {
        const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
        std::fprintf(stderr,
                     "[vram-map-cpu] cycle=%llu pc=0x%08X addr=0x%08X size=%u "
                     "dispcnt=0x%04X bg1cnt=0x%04X bg2cnt=0x%04X bg3cnt=0x%04X "
                     "bg1scroll=%u,%u bg2scroll=%u,%u bg3scroll=%u,%u\n",
                     static_cast<unsigned long long>(g_runtime_cycles), pc,
                     address, size, dispcnt, bgcnt(io, 1u), bgcnt(io, 2u),
                     bgcnt(io, 3u), hofs(io, 1u), vofs(io, 1u), hofs(io, 2u),
                     vofs(io, 2u), hofs(io, 3u), vofs(io, 3u));
        // counted by mode0_pc_budget() above
        return;
    }
    if (affine_cpu_records() < 512u &&
        overlaps_active_affine_bg23(io, address, size)) {
        const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
        std::fprintf(stderr,
                     "[vram-affine-cpu] cycle=%llu pc=0x%08X addr=0x%08X "
                     "size=%u dispcnt=0x%04X bg2cnt=0x%04X bg3cnt=0x%04X\n",
                     static_cast<unsigned long long>(g_runtime_cycles), pc,
                     address, size, dispcnt, bgcnt(io, 2u), bgcnt(io, 3u));
        ++affine_cpu_records();
    }
}

void trace_dma(const std::uint8_t* io, int channel, std::uint32_t pc,
               std::uint32_t source, std::uint32_t destination,
               std::uint32_t bytes, std::uint16_t control, int start_mode) {
    if (g_dma_descriptor_observer) {
        g_dma_descriptor_observer(channel, pc, source, destination, bytes,
                                  control, start_mode);
    }
    if (g_video_write_observer && destination >= 0x05000000u &&
        destination < 0x07000000u) {
        g_video_write_observer(pc, destination, bytes, source, channel);
    }
    const std::uint32_t step = (control & 0x0400u) ? 4u : 2u;
    const std::uint32_t units = bytes / step;
    const std::uint32_t dest_control = (control >> 5) & 3u;
    if (g_text_trace_enabled) {
        std::uint32_t first = 0u;
        std::uint32_t last = 0u;
        if (dma_trace_bounds(destination, step, units, dest_control, first,
                             last) && last > first) {
            const std::uint32_t regions = selected_bg0_text_regions(
                io, first, static_cast<std::uint64_t>(last) - first);
            if (regions != 0u) {
                append_text_trace_row(
                    io, "dma", pc, first,
                    static_cast<std::uint32_t>(last - first), channel, source,
                    destination, bytes, control, start_mode, regions);
            }
        }
    }
    {
        std::uint32_t first = 0u;
        std::uint32_t last = 0u;
        if (dma_trace_bounds(destination, step, units, dest_control, first,
                             last) && last > first) {
            effect_note(io, 1u, pc, first,
                        static_cast<std::uint64_t>(last) - first);
        }
    }
    if (!trace_enabled()) return;
    note_affine_scene_edge(io);
    if (dma_records() < 256u &&
        dma_overlaps_active_mode0_bg123(io, destination, step, units,
                                        dest_control)) {
        const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
        std::fprintf(stderr,
                     "[vram-map-dma] cycle=%llu pc=0x%08X mode=%d ch=%d "
                     "src=0x%08X dst=0x%08X size=%u cnt_h=0x%04X "
                     "dispcnt=0x%04X bg1cnt=0x%04X bg2cnt=0x%04X bg3cnt=0x%04X "
                     "bg1scroll=%u,%u bg2scroll=%u,%u bg3scroll=%u,%u\n",
                     static_cast<unsigned long long>(g_runtime_cycles), pc,
                     start_mode, channel, source, destination, bytes, control,
                     dispcnt, bgcnt(io, 1u), bgcnt(io, 2u), bgcnt(io, 3u),
                     hofs(io, 1u), vofs(io, 1u), hofs(io, 2u), vofs(io, 2u),
                     hofs(io, 3u), vofs(io, 3u));
        ++dma_records();
        return;
    }
    if (affine_dma_records() < 256u &&
        dma_overlaps_active_affine_bg23(io, destination, step, units,
                                        dest_control)) {
        const std::uint16_t dispcnt = load16(io, IoReg::DISPCNT);
        std::fprintf(stderr,
                     "[vram-affine-dma] cycle=%llu pc=0x%08X mode=%d ch=%d "
                     "src=0x%08X dst=0x%08X size=%u cnt_h=0x%04X "
                     "dispcnt=0x%04X bg2cnt=0x%04X bg3cnt=0x%04X\n",
                     static_cast<unsigned long long>(g_runtime_cycles), pc,
                     start_mode, channel, source, destination, bytes, control,
                     dispcnt, bgcnt(io, 2u), bgcnt(io, 3u));
        ++affine_dma_records();
    }
}

struct OamShadowRange {
    bool valid = false;
    std::size_t first_slot = 0;
    std::size_t last_slot = 0;
    std::uint64_t slot_overwrites = 0;
};

OamShadowRange account_oam_shadow_range(std::uint32_t address,
                                        std::uint32_t size, bool from_dma) {
    OamShadowRange result{};
    if (size == 0u) return result;
    address = normalize_oam_shadow_address(address);
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    if (first >= kOamShadowEnd || last <= kOamShadowStart) return result;
    const std::uint64_t overlap_first =
        std::max<std::uint64_t>(first, kOamShadowStart);
    const std::uint64_t overlap_last =
        std::min<std::uint64_t>(last, kOamShadowEnd);
    result.first_slot = static_cast<std::size_t>(
        (overlap_first - kOamShadowStart) / kOamSlotBytes);
    result.last_slot = static_cast<std::size_t>(
        (overlap_last - 1u - kOamShadowStart) / kOamSlotBytes);
    ++g_oam_shadow_stats.write_calls;
    g_oam_shadow_stats.bytes += overlap_last - overlap_first;
    if (from_dma) {
        ++g_oam_shadow_stats.dma_write_calls;
        g_oam_shadow_stats.dma_bytes += overlap_last - overlap_first;
    }
    for (std::size_t slot = result.first_slot;
         slot <= result.last_slot; ++slot) {
        ++g_oam_shadow_stats.slot_write_events;
        if (g_oam_shadow_slot_seen[slot]) {
            ++g_oam_shadow_stats.slot_overwrite_events;
            ++result.slot_overwrites;
            if (!g_oam_shadow_slot_overwritten[slot]) {
                g_oam_shadow_slot_overwritten[slot] = true;
                ++g_oam_shadow_stats.overwritten_slots;
            }
        } else {
            g_oam_shadow_slot_seen[slot] = true;
            ++g_oam_shadow_stats.unique_slots;
        }
    }
    result.valid = true;
    return result;
}

void trace_oam_shadow_write(std::uint32_t pc, std::uint32_t address,
                            std::uint32_t size) {
    // The effect staging trace rides on this hook -- every IWRAM write already
    // arrives here -- but it is armed by its own toggle and must not be gated
    // behind the OAM one. Its own range check follows immediately, so an IWRAM
    // write outside the game's heap costs two compares.
    if (effect_trace_enabled() && !dma_active())
        effect_stage_note(pc, address, size);
    if (!oam_shadow_trace_enabled() || dma_active() || size == 0u) return;
    note_oam_attr0_write(pc, address, size, OamAttr0WriterKind::Cpu);
    const OamShadowRange range = account_oam_shadow_range(address, size, false);
    if (!range.valid) return;
    if (oam_shadow_records() >= 512u) {
        ++g_oam_shadow_stats.records_dropped;
        return;
    }
    std::fprintf(stderr,
                 "[oam-shadow] cycle=%llu pc=0x%08X addr=0x%08X size=%u "
                 "slot=%zu..%zu slot_overwrites=%llu\n",
                 static_cast<unsigned long long>(g_runtime_cycles), pc,
                 address, size, range.first_slot, range.last_slot,
                 static_cast<unsigned long long>(range.slot_overwrites));
    ++oam_shadow_records();
}

void trace_oam_shadow_write_committed(std::uint32_t pc,
                                      std::uint32_t address,
                                      std::uint32_t size) {
    // The runner callback is also the production object-placement seam. Keep
    // it independent of the bounded diagnostic print toggle; otherwise an
    // Enhanced run never sees generated fast-IWRAM F0 commits.
    if (dma_active() || size == 0u ||
        (g_oam_shadow_write_range_predicate &&
         !g_oam_shadow_write_range_predicate(address, size))) return;
    if (g_oam_shadow_write_observer)
        g_oam_shadow_write_observer(pc, address, size);
}

void trace_oam_shadow_dma(armv4t::Bus* bus, int channel, std::uint32_t pc,
                          std::uint32_t source, std::uint32_t destination,
                          std::uint32_t bytes, std::uint16_t control,
                          int start_mode) {
    if (!oam_shadow_trace_enabled()) return;
    const std::uint32_t step = (control & 0x0400u) != 0u ? 4u : 2u;
    const std::uint32_t units = step != 0u ? bytes / step : 0u;
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    if (bytes == 0u || (bytes % step) != 0u ||
        !dma_trace_bounds(destination, step, units,
                          (control >> 5) & 3u, first, last)) {
        return;
    }
    const std::uint64_t span = static_cast<std::uint64_t>(last) - first;
    const OamShadowRange range = account_oam_shadow_range(
        first, static_cast<std::uint32_t>(span), true);
    if (!range.valid) return;
    // The measured producer DMA writes 708 bytes into the shadow, covering
    // slots 0..88. Establish DMA provenance for that destination span only;
    // the later shadow->OAM handoff must not replace it with a synthetic
    // writer.
    note_oam_attr0_write(pc, first, static_cast<std::uint32_t>(span),
                         OamAttr0WriterKind::Dma);
    if (oam_shadow_records() >= 512u) {
        ++g_oam_shadow_stats.records_dropped;
        return;
    }
    std::fprintf(
        stderr,
        "[oam-shadow-dma] cycle=%llu pc=0x%08X channel=%d start_mode=%d "
        "src=0x%08X dst=0x%08X size=%u slot=%zu..%zu "
        "slot_overwrites=%llu\n",
        static_cast<unsigned long long>(g_runtime_cycles), pc, channel,
        start_mode, source, destination, bytes, range.first_slot,
        range.last_slot, static_cast<unsigned long long>(range.slot_overwrites));
    ++oam_shadow_records();
}

void trace_oam_dma(armv4t::Bus* bus, int channel, std::uint32_t pc,
                   std::uint32_t source, std::uint32_t destination,
                   std::uint32_t bytes, std::uint16_t control,
                   int start_mode) {
    if (!oam_shadow_trace_enabled()) return;
    const std::uint32_t step = (control & 0x0400u) != 0u ? 4u : 2u;
    const std::uint32_t units = step != 0u ? bytes / step : 0u;
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    if (bytes == 0u || (bytes % step) != 0u ||
        !oam_dma_bounds(destination, step, units,
                        (control >> 5) & 3u, first, last)) {
        return;
    }
    const OamSlotCounts counts = count_oam_slots(bus);
    ++g_oam_dma_stats.transfers;
    g_oam_dma_stats.bytes += bytes;
    g_oam_dma_stats.used_slot_total += counts.used;
    g_oam_dma_stats.visible_slot_total += counts.visible;
    g_oam_dma_stats.nonzero_slot_total += counts.nonzero;
    g_oam_dma_stats.last_source = source;
    g_oam_dma_stats.last_destination = destination;
    g_oam_dma_stats.last_size = bytes;
    g_oam_dma_stats.last_used_slots = counts.used;
    g_oam_dma_stats.last_visible_slots = counts.visible;
    g_oam_dma_stats.last_nonzero_slots = counts.nonzero;
    g_oam_dma_stats.last_raw_x_ge_240 = counts.raw_x_ge_240;
    g_oam_dma_stats.last_raw_y_ge_160 = counts.raw_y_ge_160;
    // The final measured handoff copies the complete shadow to OAM. Read only
    // raw Y metadata from the committed image and join it with the latest
    // shadow ATTR0 provenance; slots 89..127 remain unseen unless separately
    // written by a CPU/DMA producer.
    if (bus && source == kOamShadowStart && destination == kOamStart &&
        bytes == 1024u && (control & 0x0060u) == 0u) {
        ++g_oam_attr0_stats.post_copies;
        for (std::size_t slot = 0; slot < kOamSlotCount; ++slot) {
            const std::uint32_t attr0_addr = kOamStart +
                static_cast<std::uint32_t>(slot * kOamSlotBytes);
            note_oam_attr0_candidate(
                static_cast<std::uint8_t>(bus->read16(attr0_addr) & 0x00FFu),
                slot);
        }
    }
    if (g_oam_dma_records >= 256u) {
        ++g_oam_dma_stats.records_dropped;
        return;
    }
    std::fprintf(
        stderr,
        "[oam-dma] cycle=%llu pc=0x%08X channel=%d start_mode=%d "
        "src=0x%08X dst=0x%08X size=%u used_slots=%u visible_slots=%u "
        "nonzero_slots=%u raw_x_ge_240=%u raw_y_ge_160=%u\n",
        static_cast<unsigned long long>(g_runtime_cycles), pc, channel,
        start_mode, source, destination, bytes, counts.used, counts.visible,
        counts.nonzero, counts.raw_x_ge_240, counts.raw_y_ge_160);
    ++g_oam_dma_records;
}

void reset_oam_trace_window() {
    g_oam_shadow_stats = {};
    g_oam_dma_stats = {};
    g_oam_attr0_stats = {};
    g_oam_attr0_provenance.fill({});
    g_oam_attr0_generation = 0;
    g_oam_shadow_slot_seen.fill(false);
    g_oam_shadow_slot_overwritten.fill(false);
    g_oam_shadow_records = 0u;
    g_oam_dma_records = 0u;
}

void get_oam_shadow_trace_stats(OamShadowTraceStats* out) {
    if (out) *out = g_oam_shadow_stats;
}

void get_oam_dma_trace_stats(OamDmaTraceStats* out) {
    if (out) *out = g_oam_dma_stats;
}

void get_oam_attr0_trace_stats(OamAttr0TraceStats* out) {
    if (out) *out = g_oam_attr0_stats;
}

bool get_oam_attr0_provenance(std::size_t slot, OamAttr0Provenance* out) {
    if (!out || slot >= kOamSlotCount) return false;
    *out = g_oam_attr0_provenance[slot];
    return true;
}

void begin_dma() { ++dma_depth(); }
void end_dma() { if (dma_depth() != 0u) --dma_depth(); }
bool dma_active() { return dma_depth() != 0u; }
bool iwram_store_trace_enabled() {
    return effect_trace_enabled() || oam_shadow_trace_enabled();
}

void set_default_enabled(bool enabled) {
    g_default_enabled = enabled ? 1 : 0;
}

void set_text_trace_directory(const char* directory) {
    flush_text_trace();
    if (g_text_trace_file) {
        std::fclose(g_text_trace_file);
        g_text_trace_file = nullptr;
    }
    g_text_trace_directory = directory ? directory : "";
    g_text_trace_enabled = !g_text_trace_directory.empty();
    g_text_trace_open_failed = false;
    g_text_trace_sequence = 0u;
    g_text_trace_pending_rows = 0u;
    g_text_trace_buffer.clear();
    if (g_text_trace_enabled && !g_text_trace_atexit_registered) {
        std::atexit(&flush_text_trace);
        g_text_trace_atexit_registered = true;
    }
    // Create the session artifact immediately so an enabled capture is
    // distinguishable from a run that simply produced no BG0 writes.
    if (g_text_trace_enabled) ensure_text_trace_file();
}

bool rearm_bounded_window() {
    if (g_trace_windows >= kMaxTraceWindows) return false;
    ++g_trace_windows;
    cpu_records() = 0u;
    dma_records() = 0u;
    reset_mode0_pc_budget();
    return true;
}

void set_dma_descriptor_observer(DmaDescriptorObserver observer) {
    g_dma_descriptor_observer = observer;
}

void set_video_write_observer(VideoWriteObserver observer) {
    g_video_write_observer = observer;
}

void note_palette_cpu_write(std::uint32_t pc, std::uint32_t address,
                            std::uint32_t size) {
    if (!g_video_write_observer || dma_active()) return;
    g_video_write_observer(pc, address, size, 0u, -1);
}

void set_oam_shadow_write_observer(OamShadowWriteObserver observer) {
    g_oam_shadow_write_observer = observer;
}

void set_oam_shadow_write_range_predicate(
    OamShadowWriteRangePredicate predicate) {
    g_oam_shadow_write_range_predicate = predicate;
}

}  // namespace gba::vram_trace
