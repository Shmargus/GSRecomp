// Diagnostic move probe; see move_probe.h. Evidence (FACTS.md, 2026-09-30):
// every battle move's presentation calls 0x080B9D34 with R0 = the battle
// action record; +0x4C is the move id (1 = Attack), +0x58 the effect word
// (bit 0x4000 = weapon swing).
#include "move_probe.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "gba_bus.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" unsigned long long runtime_current_frame();

namespace gsr {
namespace {

constexpr std::uint32_t kMoveEntryPc = 0x080B9D34u;
constexpr std::uint32_t kEwramBase = 0x02000000u;
constexpr std::uint32_t kEwramSize = 0x40000u;
constexpr std::uint32_t kAbilityOffset = 0x4C;
constexpr std::uint32_t kEffectOffset = 0x58;
constexpr std::uint32_t kRecordEnd = 0x5C;
constexpr unsigned long long kWatchFrames = 300;
constexpr int kLineCap = 8000;

bool enabled() {
    static const bool on = [] {
        const char* v = std::getenv("GSR_MOD_FIELD_TEST");
        return v && v[0] && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

// Written by the guest-entry hook, read by the present hook. In the normal
// windowed run both are the same thread; atomics cover the debug-server mode
// where the game runs on its own thread.
std::atomic<bool> g_have_start{false};
std::atomic<std::uint64_t> g_start_frame{0};
std::atomic<std::uint32_t> g_ability{0}, g_effect{0}, g_action{0};
std::atomic<int> g_lines{0};
std::atomic<bool> g_cap_said{false};

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

// True when a line may be printed; prints the cap notice once.
bool take_line() {
    if (g_lines.fetch_add(1) < kLineCap) return true;
    if (!g_cap_said.exchange(true))
        std::fprintf(stderr, "[move-probe] line cap reached\n");
    return false;
}

}  // namespace

void move_probe_on_entry(std::uint32_t entry_pc) {
    if (entry_pc != kMoveEntryPc || !enabled()) return;
    const std::uint32_t action = g_cpu.R[0];
    if (action < kEwramBase || action - kEwramBase + kRecordEnd > kEwramSize)
        return;
    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !bus->ewram_ptr()) return;
    const std::uint8_t* rec = bus->ewram_ptr() + (action & 0x3FFFFu);
    const std::uint32_t ability = read_u32(rec + kAbilityOffset);
    const std::uint32_t effect = read_u32(rec + kEffectOffset);
    const unsigned long long frame = runtime_current_frame();
    g_ability = ability;
    g_effect = effect;
    g_action = action;
    g_start_frame = frame;
    g_have_start = true;
    if (take_line())
        std::fprintf(stderr,
                     "[move-probe] start frame=%llu ability=%u effect=0x%X "
                     "action=0x%08X\n",
                     frame, ability, effect, action);
}

void move_probe_on_sparks(std::uint64_t frame, int effect_layer,
                          const std::vector<EffectSpark>& stamps,
                          const SceneLayer* layer) {
    if (!enabled() || !g_have_start.load()) return;
    static std::uint64_t last_logged = ~0ull;
    const std::uint64_t start = g_start_frame.load();
    if (frame < start || frame - start > kWatchFrames) return;
    if (frame == last_logged) return;  // one line per frame
    if (!take_line()) return;
    last_logged = frame;

    char line[768];
    int n = std::snprintf(line, sizeof line,
                          "[move-probe] f=%llu dt=%llu layer=%d n=%zu",
                          static_cast<unsigned long long>(frame),
                          static_cast<unsigned long long>(frame - start),
                          effect_layer, stamps.size());
    auto add = [&](const char* fmt, auto... a) {
        if (n > 0 && n < static_cast<int>(sizeof line))
            n += std::snprintf(line + n, sizeof line - n, fmt, a...);
    };
    if (!stamps.empty()) {
        int x0 = stamps[0].x, x1 = x0, y0 = stamps[0].y, y1 = y0;
        int w0 = effect_stamp_width(stamps[0]), w1 = w0;
        int add_n = 0, max_n = 0, rep_n = 0;
        for (const EffectSpark& s : stamps) {
            x0 = std::min(x0, s.x); x1 = std::max(x1, s.x);
            y0 = std::min(y0, s.y); y1 = std::max(y1, s.y);
            const int w = effect_stamp_width(s);
            w0 = std::min(w0, w); w1 = std::max(w1, w);
            if (s.blend == EffectBlend::Add) ++add_n;
            else if (s.blend == EffectBlend::Maximum) ++max_n;
            else ++rep_n;
        }
        add(" bbox=%d,%d..%d,%d", x0, y0, x1, y1);
        add(" sizes=%d..%d", w0, w1);
        add(" add=%d max=%d rep=%d", add_n, max_n, rep_n);
    }
    if (layer) {
        add(" scroll=%d,%d affine=%d wraps=%d size_code=%u",
            layer->scroll_x, layer->scroll_y, layer->affine ? 1 : 0,
            layer->wraps ? 1 : 0, layer->size_code);
        if (layer->affine)
            add(" pa=%d pb=%d pc=%d pd=%d ref=%d,%d", layer->affine_pa,
                layer->affine_pb, layer->affine_pc, layer->affine_pd,
                static_cast<int>(layer->affine_ref_x),
                static_cast<int>(layer->affine_ref_y));
    }
    std::fprintf(stderr, "%s\n", line);
}

}  // namespace gsr
