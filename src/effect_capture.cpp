#include "effect_capture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

namespace gsr {
namespace {

bool g_enabled = false;
CapturedEffectFrame g_pending;
EffectCanvas g_history;
std::uint32_t g_canvas = 0;
std::set<std::pair<std::uint32_t, std::uint32_t>> g_reported_unsupported;
std::uint64_t g_frame = std::numeric_limits<std::uint64_t>::max();
struct {
    unsigned long long entries = 0, kept = 0, unsupported = 0, invalid = 0;
    unsigned long long copies = 0, clears = 0;
    int max_width = 0, max_height = 0;
} g_counts;

// Allocator slots and generator template addresses: verified from Func_ed408
// and live gpu_frame_0041/0043, FACTS.md 2026-09-19. Template BYTES stay in the
// user-supplied ROM; none are copied into public source or assumed from a PC.
constexpr std::uint32_t kCanvasSlot = 0x03001EF0u;
constexpr std::uint32_t kStampSlot = 0x03001F08u;
// c1438 publishes the 128x128 canvas through this copy-and-fade wrapper.
// c1470 clears it with the relocated ROM 0x080008D4 routine (116 bytes).
constexpr std::uint32_t kCopyAndFade = 0x080054E4u;
constexpr std::uint32_t kCopyAndHalve = 0x08005534u;
constexpr std::uint32_t kPlainCopy = 0x03001388u;
constexpr std::uint32_t kClearCanvas = 0x03000164u;
constexpr std::uint32_t kFillCanvas = 0x03000168u;
constexpr std::size_t kCanvasBytes = 128 * 128;
constexpr std::uint32_t kHeaderTemplate = 0x080EDCC4u;
constexpr std::uint32_t kSaturationTemplate = 0x080EDCB8u;
constexpr std::uint32_t kFlipXTemplate = 0x080EDCD0u;
constexpr std::uint32_t kFlipYTemplate = 0x080EDCDCu;
constexpr std::uint32_t kFlipXYTemplate = 0x080EDCECu;
constexpr std::uint32_t kNoFlipTemplate = 0x080EDCF8u;
constexpr std::uint32_t kMaxTemplate = 0x080EDBE8u;
constexpr std::uint32_t kMaxReverseTemplate = 0x080EDBF8u;
constexpr std::uint32_t kAddTemplate = 0x080EDC88u;
constexpr std::uint32_t kAddReverseTemplate = 0x080EDCA0u;
// Func_ed408 mode 1: copy each non-zero source byte (stamp variant 13,
// flags 3 mode 1, measured 2026-09-23; session_20260923_124739 logged it as
// unsupported at 0x03006380).
constexpr std::uint32_t kReplaceTemplate = 0x080EDB00u;
constexpr std::uint32_t kReplaceReverseTemplate = 0x080EDB10u;

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
    // The three-instruction entry loads width/height from the caller's stack.
    if (!memory.matches(pc, kHeaderTemplate, 3 * 4)) return false;
    std::uint32_t at = pc + 3 * 4;
    const bool saturating = memory.matches(at, kSaturationTemplate, 3 * 4);
    if (saturating) at += 3 * 4;
    if (memory.matches(at, kFlipXTemplate, 3 * 4)) {
        stamp.flip_x = true;
        at += 3 * 4;
    } else if (memory.matches(at, kFlipYTemplate, 4 * 4)) {
        stamp.flip_y = true;
        at += 4 * 4;
    } else if (memory.matches(at, kFlipXYTemplate, 3 * 4)) {
        stamp.flip_x = stamp.flip_y = true;
        at += 3 * 4;
    } else if (memory.matches(at, kNoFlipTemplate, 4)) {
        at += 4;
    } else return false;

    // Identify the actual byte loop, stopping at this routine's ARM return,
    // never searching a subsequent allocation. Unknown operations are refused.
    const auto kernel = saturating
        ? (stamp.flip_x ? kAddReverseTemplate : kAddTemplate)
        : (stamp.flip_x ? kMaxReverseTemplate : kMaxTemplate);
    const std::size_t kernel_bytes = saturating ? 6 * 4 : 4 * 4;
    const auto replace =
        stamp.flip_x ? kReplaceReverseTemplate : kReplaceTemplate;
    bool matched = false, replaced = false;
    for (; at <= 0x030077FCu; at += 4) {
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

}  // namespace

void effect_capture_set_enabled(bool enabled) {
    g_enabled = enabled;
    if (!enabled) { g_pending = {}; g_history = {}; g_canvas = 0; }
    static bool report_registered = false;
    if (enabled && !report_registered) {
        report_registered = true;
        std::atexit([] {
            char report[256];
            effect_capture_report(report, sizeof report);
            std::fprintf(stderr, "[gsr] host effects session: %s\n", report);
        });
    }
}
bool effect_capture_enabled() { return g_enabled; }
bool effect_capture_observes(std::uint32_t pc) {
    return (pc >= 0x03002000u && pc <= 0x030077FCu) ||
           pc == kCopyAndFade || pc == kCopyAndHalve || pc == kPlainCopy ||
           pc == kClearCanvas || pc == kFillCanvas;
}

void effect_capture_on_entry(std::uint32_t pc, const std::uint32_t* r,
                             const std::uint8_t* ewram, const std::uint8_t* iwram,
                             const std::uint8_t* rom, std::size_t rom_bytes,
                             std::uint64_t frame) {
    if (!g_enabled || !r || !effect_capture_observes(pc)) return;
    const Memory memory{ewram, iwram, rom, rom_bytes};
    const auto canvas = memory.word(kCanvasSlot);
    if (canvas != g_canvas || frame < g_frame) {
        g_history = {};
        g_pending = {};
        g_canvas = canvas;
    }
    g_frame = frame;
    const bool plain_copy = pc == kPlainCopy;
    if (canvas == 0 || (plain_copy ? r[1] : r[0]) != canvas) return;
    if (pc == kClearCanvas || pc == kFillCanvas) {
        const bool clear = pc == kClearCanvas;
        if (r[1] == kCanvasBytes && (clear || r[2] == 0) &&
            memory.matches(pc, clear ? 0x080008D4u : 0x080008D8u,
                           clear ? 116 : 112)) {
            // Clearing the working canvas does not change the already
            // displayed VRAM copy. Keep that publication until the next copy.
            g_history = {};
            ++g_counts.clears;
        }
        return;
    }
    if (pc == kCopyAndFade || pc == kCopyAndHalve || plain_copy) {
        const auto destination = plain_copy ? r[0] : r[1];
        if (r[2] != kCanvasBytes || destination < 0x06000000u ||
            destination > 0x06018000u - kCanvasBytes ||
            (plain_copy && !memory.matches(pc, 0x08001AF8u, 120))) return;
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
            // Publish first, then fade the working copy, exactly as the game.
            if (!plain_copy) g_history.fade(pc == kCopyAndHalve);
        }
        ++g_counts.copies;
        return;
    }
    // Read LIVE pointers: the previous presented frame may belong to a
    // different spell, or a savestate may have replaced the whole allocation.
    if (pc != memory.word(kStampSlot) && pc != memory.word(kStampSlot + 4)) return;
    ++g_counts.entries;
    EffectSpark stamp;
    if (!stamp_operation(memory, pc, stamp)) {
        ++g_counts.unsupported;
        if (g_reported_unsupported.emplace(pc, r[14]).second) {
            std::fprintf(stderr,
                "[gsr] host effects: unsupported stamp pc=0x%08X caller=0x%08X "
                "width=%u height=%u frame=%llu\n", pc, r[14],
                memory.word(r[13]), memory.word(r[13] + 4),
                static_cast<unsigned long long>(frame));
        }
        return;
    }
    const auto stack = memory.span(r[13], 8);
    if (!stack) { ++g_counts.invalid; return; }
    const auto width = memory.word(r[13]), height = memory.word(r[13] + 4);
    const auto count = std::uint64_t(width) * height;
    const auto pixels = count <= std::numeric_limits<std::size_t>::max()
        ? memory.span(r[1], static_cast<std::size_t>(count)) : nullptr;
    if (!pixels || width == 0 || height == 0 ||
        width > std::uint32_t(std::numeric_limits<int>::max()) ||
        height > std::uint32_t(std::numeric_limits<int>::max())) {
        ++g_counts.invalid;
        return;
    }
    const std::int64_t x = static_cast<std::int32_t>(r[2]);
    const std::int64_t y = static_cast<std::int32_t>(r[3]);
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

CapturedEffectFrame effect_capture_take() {
    // VBlank/presentation frequency does not define the effect's lifetime.
    // A frame with no display copy must keep showing the preceding copy.
    return g_pending;
}
void effect_capture_report(char* out, std::size_t bytes) {
    if (!out || !bytes) return;
    std::snprintf(out, bytes,
        "stamp entries %llu, captured %llu, largest width/height %d/%d, "
        "unsupported operation %llu, invalid source/size %llu, copies %llu, clears %llu",
        g_counts.entries, g_counts.kept, g_counts.max_width, g_counts.max_height,
        g_counts.unsupported, g_counts.invalid, g_counts.copies, g_counts.clears);
}

}  // namespace gsr
