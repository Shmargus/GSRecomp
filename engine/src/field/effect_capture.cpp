// effect_capture.cpp -- the spell-trail capture shared by both games
// (effect_capture.h). Game addresses come from effect_capture_profile().
#include "effect_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

#include "game_exit.h"
#include "game_ram_layout.h"

namespace gsr {
namespace {

bool g_enabled = false;
CapturedEffectFrame g_pending;
EffectCanvas g_history;
std::uint32_t g_canvas = 0;
std::set<std::pair<std::uint32_t, std::uint32_t>> g_reported_unsupported;
std::uint64_t g_frame = std::numeric_limits<std::uint64_t>::max();
std::uint64_t g_last_copy_frame = 0;
struct {
    unsigned long long entries = 0, kept = 0, unsupported = 0, invalid = 0;
    unsigned long long copies = 0, clears = 0, skipped = 0;
    int max_width = 0, max_height = 0;
} g_counts;

// Func_cd260's canvas modes, the same block layout in both games (FACTS.md
// 2026-10-02): mode +0x7780, acting while +0x7824 is 1. Mode 1 is the plain
// copy followed by a fill of the canvas with the param.
constexpr std::uint32_t kCanvasModeOffset = 0x7780u;
constexpr std::uint32_t kCanvasActiveOffset = 0x7824u;
constexpr std::uint32_t kCanvasFillMode = 1u;
constexpr std::size_t kCanvasBytes = 128 * 128;

const EffectCaptureProfile& profile() {
    static const EffectCaptureProfile& p = effect_capture_profile();
    return p;
}

enum class Copy { None, Fade, Halve, Subtract, Add, Plain };
Copy copy_kind(std::uint32_t pc) {
    const auto& p = profile();
    if (pc == 0) return Copy::None;
    for (int i = 0; i < 2; ++i) {
        if (pc == p.copy_fade[i]) return Copy::Fade;
        if (pc == p.copy_halve[i]) return Copy::Halve;
        if (pc == p.copy_subtract[i]) return Copy::Subtract;
        if (pc == p.copy_add[i]) return Copy::Add;
    }
    return pc == p.plain_copy ? Copy::Plain : Copy::None;
}

// One address per routine: a ROM address in the part copied to IWRAM
// becomes its IWRAM copy's, and an entry at a resident routine's Thumb stub
// becomes the routine's.
std::uint32_t arm_entry(std::uint32_t pc) {
    const auto& p = profile();
    if (pc >= p.iwram_image_rom && pc < p.iwram_image_rom_end)
        pc = pc - p.iwram_image_rom + 0x03000000u;
    if (p.thumb_stub)
        for (const std::uint32_t r : {p.plain_copy, p.clear_canvas, p.fill_canvas})
            if (r && pc + p.thumb_stub == r) return r;
    return pc;
}

struct Memory {
    const std::uint8_t* ewram;
    const std::uint8_t* iwram;
    const std::uint8_t* rom;
    std::size_t rom_bytes;
    const std::uint8_t* span(std::uint32_t address, std::size_t bytes) const {
        const auto within = [&](std::uint32_t base, const std::uint8_t* data,
                                std::size_t size) -> const std::uint8_t* {
            if (!data || address < base) return nullptr;
            const std::size_t offset = address - base;
            return offset <= size && bytes <= size - offset ? data + offset : nullptr;
        };
        if (auto p = within(0x02000000u, ewram, 256 * 1024)) return p;
        if (auto p = within(0x03000000u, iwram, 32 * 1024)) return p;
        return within(0x08000000u, rom, rom_bytes);
    }
    std::uint32_t word(std::uint32_t address) const {
        const auto p = span(address, 4);
        return p ? std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                       (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24) : 0;
    }
    bool matches(std::uint32_t code, std::uint32_t source, std::size_t bytes) const {
        const auto a = span(code, bytes), b = span(source, bytes);
        return a && b && std::memcmp(a, b, bytes) == 0;
    }
};

bool stamp_operation(const Memory& memory, std::uint32_t pc, EffectSpark& stamp) {
    const auto& p = profile();
    // The three-instruction entry loads width/height from the caller's stack.
    if (!memory.matches(pc, p.header_template, 3 * 4)) return false;
    std::uint32_t at = pc + 3 * 4;
    const bool saturating = memory.matches(at, p.saturation_template, 3 * 4);
    if (saturating) at += 3 * 4;
    if (memory.matches(at, p.flip_x_template, 3 * 4)) {
        stamp.flip_x = true;
        at += 3 * 4;
    } else if (memory.matches(at, p.flip_y_template, 4 * 4)) {
        stamp.flip_y = true;
        at += 4 * 4;
    } else if (memory.matches(at, p.flip_xy_template, 3 * 4)) {
        stamp.flip_x = stamp.flip_y = true;
        at += 3 * 4;
    } else if (memory.matches(at, p.no_flip_template, 4)) {
        at += 4;
    } else return false;

    // Identify the actual byte loop, stopping at this routine's ARM return,
    // never searching a subsequent allocation. Unknown operations are refused.
    const auto kernel = saturating
        ? (stamp.flip_x ? p.add_reverse_template : p.add_template)
        : (stamp.flip_x ? p.max_reverse_template : p.max_template);
    const std::size_t kernel_bytes = saturating ? 6 * 4 : 4 * 4;
    const auto replace =
        stamp.flip_x ? p.replace_reverse_template : p.replace_template;
    bool matched = false, replaced = false;
    // Last word start in the game's IWRAM heap. An unset heap gives 0, so the
    // scan (which starts past pc + 12) does not run.
    const std::uint32_t last_word = g_game_ram_layout.iwram_heap_last >= 3u
        ? g_game_ram_layout.iwram_heap_last - 3u : 0u;
    for (; at <= last_word; at += 4) {
        if (memory.word(at) == 0xE12FFF1Eu) { // ARM BX LR
            if (matched == replaced) return false;
            stamp.blend = replaced ? EffectBlend::Replace
                : saturating ? EffectBlend::Add : EffectBlend::Maximum;
            return true;
        }
        if (memory.matches(at, kernel, kernel_bytes)) matched = true;
        if (!saturating && memory.matches(at, replace, 4 * 4)) replaced = true;
    }
    return false;
}

void capture_stamp(const Memory& memory, std::uint32_t pc, std::uint32_t caller,
                   std::uint32_t art, std::uint32_t x_word,
                   std::uint32_t y_word, std::uint32_t width,
                   std::uint32_t height, std::uint64_t frame);

}  // namespace

void effect_capture_set_enabled(bool enabled) {
    g_enabled = enabled;
    if (!enabled) { g_pending = {}; g_history = {}; g_canvas = 0; }
    static bool report_registered = false;
    if (enabled && !report_registered) {
        report_registered = true;
        gbarecomp::at_game_exit([] {
            char report[256];
            effect_capture_report(report, sizeof report);
            std::fprintf(stderr, "[gsr] host effects session: %s\n", report);
        });
    }
}
bool effect_capture_enabled() { return g_enabled; }
bool effect_capture_observes(std::uint32_t pc) {
    const auto& p = profile();
    pc = arm_entry(pc);
    const auto& heap = g_game_ram_layout;
    return (heap.iwram_heap_last >= 3u && pc >= heap.iwram_heap_first &&
            pc <= heap.iwram_heap_last - 3u) ||
           copy_kind(pc) != Copy::None ||
           (pc != 0 && (pc == p.clear_canvas || pc == p.fill_canvas));
}

void effect_capture_on_entry(std::uint32_t pc, const std::uint32_t* r,
                             const std::uint8_t* ewram, const std::uint8_t* iwram,
                             const std::uint8_t* rom, std::size_t rom_bytes,
                             std::uint64_t frame) {
    if (!g_enabled || !r || !effect_capture_observes(pc)) return;
    const auto& p = profile();
    pc = arm_entry(pc);
    const Memory memory{ewram, iwram, rom, rom_bytes};
    if (p.canvas_slot) {
        // The canvas is whatever its allocation slot names now.
        const auto live = memory.word(p.canvas_slot);
        if (live != g_canvas || frame < g_frame) {
            g_history = {};
            g_pending = {};
            g_canvas = live;
        }
    } else if (frame < g_frame) {  // a savestate load
        g_history = {};
        g_pending = {};
        g_canvas = 0;
    }
    g_frame = frame;
    const Copy kind = copy_kind(pc);
    const bool plain_copy = kind == Copy::Plain;
    const bool step_copy = kind == Copy::Subtract || kind == Copy::Add;
    if (!p.canvas_slot && kind != Copy::None) {
        // A 16 KB copy to video memory names the canvas.
        const auto destination = plain_copy ? r[0] : step_copy ? r[2] : r[1];
        const auto bytes = step_copy ? r[3] : r[2];
        const auto source = plain_copy ? r[1] : r[0];
        if (bytes == kCanvasBytes && destination >= 0x06000000u &&
            destination <= 0x06018000u - kCanvasBytes && source != g_canvas) {
            g_history = {};
            g_pending = {};
            g_canvas = source;
        }
    }
    const auto canvas = g_canvas;
    if (canvas == 0 || (plain_copy ? r[1] : r[0]) != canvas) return;
    if (pc != 0 && (pc == p.clear_canvas || pc == p.fill_canvas)) {
        const bool clear = pc == p.clear_canvas;
        // A fill with any value replaces everything drawn so far; the
        // renderer reads a tint back from the canvas edge.
        if (r[1] == kCanvasBytes &&
            memory.matches(pc, clear ? p.clear_source : p.fill_source,
                           clear ? p.clear_bytes : p.fill_bytes)) {
            // Clearing the working canvas does not change the already
            // displayed VRAM copy. Keep that publication until the next copy.
            g_history = {};
            ++g_counts.clears;
        }
        return;
    }
    if (kind != Copy::None) {
        const auto destination = plain_copy ? r[0] : step_copy ? r[2] : r[1];
        const auto bytes = step_copy ? r[3] : r[2];
        if (bytes != kCanvasBytes || destination < 0x06000000u ||
            destination > 0x06018000u - kCanvasBytes ||
            (plain_copy && !memory.matches(pc, p.plain_copy_source, 120))) return;
        g_pending = {};
        g_pending.canvas = canvas;
        if (!g_history.pixels.empty()) {
            EffectSpark completed;
            completed.x = g_history.width / 2 - g_history.origin_x;
            completed.y = g_history.height / 2 - g_history.origin_y;
            completed.width = g_history.width;
            completed.height = g_history.height;
            completed.art_offset = 0;
            completed.blend = EffectBlend::Maximum;
            g_pending.stamps.push_back(completed);
            g_pending.artwork = g_history.pixels;
            // Publish first, then change the working copy, exactly as the game.
            if (step_copy) {
                const unsigned step = r[1] & 0xFFu;
                for (auto& pixel : g_history.pixels)
                    pixel = static_cast<std::uint8_t>(kind == Copy::Add
                        ? std::min(63u, unsigned(pixel) + step)
                        : (pixel > step ? unsigned(pixel) - step : 0u));
            } else if (!plain_copy) {
                g_history.fade(kind == Copy::Halve);
            }
        }
        // Mode 1: the plain copy is followed by a fill of the whole canvas.
        if (plain_copy) {
            const auto block = memory.word(p.canvas_block_slot);
            if (block != 0 &&
                memory.word(block + kCanvasActiveOffset) == 1u &&
                memory.word(block + kCanvasModeOffset) == kCanvasFillMode) {
                g_history = {};
                ++g_counts.clears;
            }
        }
        ++g_counts.copies;
        g_last_copy_frame = frame;
        return;
    }
    const auto stack = memory.span(r[13], 8);
    if (!stack) { ++g_counts.invalid; return; }
    capture_stamp(memory, pc, r[14], r[1], r[2], r[3], memory.word(r[13]),
                  memory.word(r[13] + 4), frame);
}

void effect_capture_skipped_spark(std::uint32_t routine, std::uint32_t canvas,
                                  std::uint32_t art, std::int32_t x,
                                  std::int32_t y, std::uint32_t width,
                                  std::uint32_t height,
                                  const std::uint8_t* ewram,
                                  const std::uint8_t* iwram,
                                  const std::uint8_t* rom,
                                  std::size_t rom_bytes, std::uint64_t frame) {
    if (!g_enabled) return;
    const auto& p = profile();
    const Memory memory{ewram, iwram, rom, rom_bytes};
    if (p.canvas_slot) {
        // Only into the canvas the capture is already following this frame.
        if (canvas == 0 || canvas != g_canvas ||
            canvas != memory.word(p.canvas_slot) || frame != g_frame) return;
    } else {
        if (frame < g_frame) return;
        // Only into the canvas the capture is already following.
        if (canvas == 0 || canvas != g_canvas) return;
        g_frame = frame;
    }
    ++g_counts.skipped;
    capture_stamp(memory, routine, 0, art, static_cast<std::uint32_t>(x),
                  static_cast<std::uint32_t>(y), width, height, frame);
}

bool effect_capture_history_read(std::uint32_t canvas, int left, int top,
                                 std::uint8_t* out) {
    if (!g_enabled || canvas == 0 || canvas != g_canvas || !out) return false;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const int hx = left + x + g_history.origin_x;
            const int hy = top + y + g_history.origin_y;
            out[y * 128 + x] = g_history.in_bounds(hx, hy)
                ? g_history.pixels[std::size_t(hy) * g_history.width + hx] : 0;
        }
    return true;
}

void effect_capture_history_paint(std::uint32_t canvas, int left, int top,
                                  int width, int height,
                                  const std::uint8_t* pixels) {
    if (!g_enabled || canvas == 0 || canvas != g_canvas || !pixels ||
        width <= 0 || height <= 0) return;
    EffectSpark painted;
    painted.width = width;
    painted.height = height;
    painted.x = left + width / 2;
    painted.y = top + height / 2;
    painted.art_offset = 0;
    painted.blend = EffectBlend::Copy;
    stamp_spark_expanding(&g_history, painted, pixels,
                          std::size_t(width) * std::size_t(height));
}

namespace {

void capture_stamp(const Memory& memory, std::uint32_t pc, std::uint32_t caller,
                   std::uint32_t art, std::uint32_t x_word,
                   std::uint32_t y_word, std::uint32_t width,
                   std::uint32_t height, std::uint64_t frame) {
    const auto& p = profile();
    if (p.stamp_slot) {
        // Read LIVE pointers: the previous presented frame may belong to a
        // different spell, or a savestate may have replaced the whole
        // allocation.
        if (pc != memory.word(p.stamp_slot) && pc != memory.word(p.stamp_slot + 4))
            return;
    } else if (!memory.matches(pc, p.header_template, 3 * 4)) {
        // Known by its own LIVE bytes, entered at its Thumb stub or at the
        // ARM code after it.
        if (!p.thumb_stub ||
            !memory.matches(pc + p.thumb_stub, p.header_template, 3 * 4)) return;
        pc += p.thumb_stub;
    }
    ++g_counts.entries;
    EffectSpark stamp;
    if (!stamp_operation(memory, pc, stamp)) {
        ++g_counts.unsupported;
        if (g_reported_unsupported.emplace(pc, caller).second) {
            std::fprintf(stderr,
                "[gsr] host effects: unsupported stamp pc=0x%08X caller=0x%08X "
                "width=%u height=%u frame=%llu\n", pc, caller, width, height,
                static_cast<unsigned long long>(frame));
        }
        return;
    }
    const auto count = std::uint64_t(width) * height;
    const auto pixels = count <= std::numeric_limits<std::size_t>::max()
        ? memory.span(art, static_cast<std::size_t>(count)) : nullptr;
    if (!pixels || width == 0 || height == 0 ||
        width > std::uint32_t(std::numeric_limits<int>::max()) ||
        height > std::uint32_t(std::numeric_limits<int>::max())) {
        ++g_counts.invalid;
        return;
    }
    const std::int64_t x = static_cast<std::int32_t>(x_word);
    const std::int64_t y = static_cast<std::int32_t>(y_word);
    if (x + width > std::numeric_limits<int>::max() ||
        y + height > std::numeric_limits<int>::max()) {
        ++g_counts.invalid;
        return;
    }
    stamp.width = static_cast<int>(width);
    stamp.height = static_cast<int>(height);
    stamp.x = static_cast<int>(x + width / 2);
    stamp.y = static_cast<int>(y + height / 2);
    stamp.art_offset = 0;
    stamp_spark_expanding(&g_history, stamp, pixels, static_cast<std::size_t>(count));
    ++g_counts.kept;
    g_counts.max_width = std::max(g_counts.max_width, stamp.width);
    g_counts.max_height = std::max(g_counts.max_height, stamp.height);
}

}  // namespace

std::uint64_t effect_capture_last_copy_frame() { return g_last_copy_frame; }

CapturedEffectFrame effect_capture_take() {
    // VBlank/presentation frequency does not define the effect's lifetime.
    // A frame with no display copy must keep showing the preceding copy.
    return g_pending;
}
void effect_capture_report(char* out, std::size_t bytes) {
    if (!out || !bytes) return;
    std::snprintf(out, bytes,
        "stamp entries %llu, captured %llu, largest width/height %d/%d, "
        "unsupported operation %llu, invalid source/size %llu, copies %llu, clears %llu, "
        "skipped sparks added %llu",
        g_counts.entries, g_counts.kept, g_counts.max_width, g_counts.max_height,
        g_counts.unsupported, g_counts.invalid, g_counts.copies, g_counts.clears,
        g_counts.skipped);
}

}  // namespace gsr
