// pool_ldm_probe_test.cpp — CRASH-03 provenance seam coverage (synthetic,
// no ROM/BIOS). Proves the bounded diagnostic added to the pool-LDM probe:
//   1. probe arming emits exactly one payload-free [pool-ldm-crc] reason=arm
//      line whose CRC matches an independent IEEE CRC-32 of the tracked
//      window bytes;
//   2. per-slot depth-8 write rings retain an early (possibly poisoning)
//      writer PC after later carrier pushes, while staying depth-bounded;
//   3. every ring record carries the savestate g_runtime_state_epoch tag so
//      pre/post-load provenance is separable;
//   4. restore-boundary arming emits one restore CRC, resets stale host-side
//      evidence, preserves the new epoch, and suppresses a duplicate arm CRC;
//   5. runtime_note_pool_ldm_epoch_crc() reflects changed window contents.
//
// Drives the public extern "C" seams only: generated-store feed
// runtime_trace_event(RUNTIME_TRACE_MEM_WRITE,...), direct bus seam
// runtime_note_pool_ldm_bus_write, arm/snapshot
// runtime_pool_dispatch_history_record, restore-boundary arm, and the one-shot dump
// runtime_pool_dispatch_history_dump. Guest RAM identity is faked locally
// via g_fast_iwram pointing at a test-owned buffer; no guest state is
// patched outside this buffer.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gba_bus.h"
#include "gba_vram_trace.h"
#include "runtime_arm.h"

// Crash-dump seams live in runtime_arm.cpp but have no public header; they
// are part of this probe's tested contract.
extern "C" void runtime_pool_dispatch_history_record(uint32_t pc, int thumb);
extern "C" void runtime_pool_dispatch_history_dump(uint32_t invalid_pc);

#include "runtime_bus_bridge.h"  // g_runtime_vblank_starts frame key

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

constexpr uint32_t kMemWrite = 4u;  // RUNTIME_TRACE_MEM_WRITE
constexpr uint32_t kWindowBase = 0x03007E00u;
constexpr uint32_t kWindowEnd = 0x03007E40u;

uint8_t s_fake_iwram[0x8000];

void put32(uint32_t addr, uint32_t value) {
    uint8_t* p = s_fake_iwram + (addr & 0x7FFFu);
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
    p[2] = static_cast<uint8_t>(value >> 16);
    p[3] = static_cast<uint8_t>(value >> 24);
}

// Independent bitwise IEEE CRC-32 (poly 0xEDB88320) over raw bytes, used to
// cross-check the probe's word-wise implementation.
uint32_t ref_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string capture_path() { return "pool_ldm_probe_stderr.tmp"; }

struct FastIwramWriteEvent {
    uint32_t address = 0;
    uint32_t width = 0;
    uint32_t phase = 0;
    uint32_t value = 0;
};
FastIwramWriteEvent g_fast_iwram_events[4]{};
unsigned g_fast_iwram_event_count = 0;

unsigned g_slow_iwram_commit_count = 0;
uint32_t g_slow_iwram_commit_address = 0;
uint32_t g_slow_iwram_commit_width = 0;

void observe_slow_iwram_commit(uint32_t, uint32_t address, uint32_t width) {
    ++g_slow_iwram_commit_count;
    g_slow_iwram_commit_address = address;
    g_slow_iwram_commit_width = width;
}

void observe_fast_iwram_write(uint32_t address, uint32_t width,
                              uint32_t phase) {
    if (g_fast_iwram_event_count < 4u) {
        auto& event = g_fast_iwram_events[g_fast_iwram_event_count++];
        event = {address, width, phase, bus_read_u32(address & ~3u)};
    }
    if (phase == RUNTIME_FAST_IWRAM_WRITE_COMMITTED)
        runtime_note_pool_ldm_bus_write(address, width);
}

std::string slurp_capture() {
    std::fflush(stderr);
    FILE* f = std::fopen(capture_path().c_str(), "rb");
    if (!f) return "";
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

unsigned count_occurrences(const std::string& hay, const char* needle) {
    unsigned n = 0;
    for (size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + std::strlen(needle)))
        ++n;
    return n;
}

// True when some line beginning with |line_prefix| also contains |needle|
// before that line's newline.
bool line_contains(const std::string& hay, const char* line_prefix,
                   const char* needle) {
    size_t pos = 0;
    while ((pos = hay.find(line_prefix, pos)) != std::string::npos) {
        const size_t eol = hay.find('\n', pos);
        const size_t end = eol == std::string::npos ? hay.size() : eol;
        const size_t hit = hay.find(needle, pos);
        if (hit != std::string::npos && hit < end) return true;
        pos = end;
    }
    return false;
}

void test_arm_crc_and_ring_provenance() {
    // Deterministic known window contents.
    for (uint32_t i = 0; i < 16u; ++i)
        put32(kWindowBase + 4u * i, 0x5A000000u + i);

    // Route the probe's bus reads at our local buffer.
    g_fast_iwram = s_fake_iwram;

    std::remove(capture_path().c_str());
    if (!std::freopen(capture_path().c_str(), "w", stderr)) {
        std::printf("FAIL: cannot redirect stderr for capture\n");
        ++g_failures;
        return;
    }
#if defined(_WIN32)
    _putenv_s("GBARECOMP_VRAM_MAP_TRACE", "1");
#else
    setenv("GBARECOMP_VRAM_MAP_TRACE", "1", 1);
#endif

    // ── Phase A: arming emits exactly one CRC line ────────────────────────
    runtime_pool_dispatch_history_record(0x03006100u, 0);
    runtime_pool_dispatch_history_record(0x03006048u, 0);  // already armed
    std::string log = slurp_capture();
    check(count_occurrences(log, "[pool-ldm-crc] reason=arm") == 1,
          "arming must emit exactly one [pool-ldm-crc] reason=arm line");

    // Cross-check the printed arm CRC against an independent CRC-32 of the
    // same 64 window bytes.
    uint32_t expect =
        ref_crc32(s_fake_iwram + (kWindowBase & 0x7FFFu),
                  kWindowEnd - kWindowBase);
    size_t crc_pos = log.find("reason=arm");
    bool crc_ok = false;
    if (crc_pos != std::string::npos) {
        size_t tok = log.find("crc=0x", crc_pos);
        if (tok != std::string::npos && tok < crc_pos + 160) {
            uint32_t got = static_cast<uint32_t>(
                std::strtoul(log.c_str() + tok + 6, nullptr, 16));
            crc_ok = got == expect;
        }
    }
    check(crc_ok, "arm CRC must match independent IEEE CRC-32 of the window");

    // ── Phase B: restore boundary resets stale evidence and arms early ─────
    // Leave a stale pre-restore writer/history record behind. The restore
    // boundary must discard these host records without touching the fake RAM.
    g_runtime_state_epoch = 3;
    g_runtime_vblank_starts = 380;
    g_cpu.R[15] = 0x0BADF00Du;
    g_cpu.R[13] = 0x03007E34u;
    g_cpu.R[9] = 0x03006300u;
    runtime_trace_event(kMemWrite, 0x0BADF00Du, 0x03007E30u,
                        0xBAADF00Du, 4u);
    put32(0x03007E30u, 0xBAADF00Du);
    runtime_pool_dispatch_history_record(0x03006048u, 0);

    for (uint32_t i = 0; i < 16u; ++i)
        put32(kWindowBase + 4u * i, 0xA5000000u + i);
    const std::string restored_bytes(
        reinterpret_cast<const char*>(s_fake_iwram + (kWindowBase & 0x7FFFu)),
        kWindowEnd - kWindowBase);
    const uint32_t restore_expect = ref_crc32(
        reinterpret_cast<const uint8_t*>(restored_bytes.data()),
        restored_bytes.size());
    g_runtime_state_epoch = 4;
    g_runtime_vblank_starts = 395;
    runtime_pool_ldm_probe_restore_boundary();
    log = slurp_capture();
    check(count_occurrences(log, "[pool-ldm-crc] reason=restore") == 1,
          "restore boundary must emit exactly one restore CRC");
    check(count_occurrences(log, "[pool-ldm-crc] reason=arm") == 1,
          "restore boundary must not emit a duplicate arm CRC");
    check(line_contains(log, "[pool-ldm-crc] reason=restore",
                        "epoch=4 frame=395"),
          "restore CRC must carry the incremented state epoch and frame");
    size_t restore_pos = log.find("[pool-ldm-crc] reason=restore");
    bool restore_crc_ok = false;
    if (restore_pos != std::string::npos) {
        size_t tok = log.find("crc=0x", restore_pos);
        if (tok != std::string::npos && tok < restore_pos + 180) {
            uint32_t got = static_cast<uint32_t>(
                std::strtoul(log.c_str() + tok + 6, nullptr, 16));
            restore_crc_ok = got == restore_expect;
        }
    }
    check(restore_crc_ok,
          "restore CRC must match the restored synthetic window");

    // ── Phase C: post-restore poison writer retained across carriers ──────
    // Session shape mirrors session_20260826_122040: healthy r9, a
    // non-pool origin store of 0xDCEF0210 into 0x03007E30 through a mirrored
    // direct-bus address, then carrier prologue pushes from pool PCs whose r9
    // is already corrupt. This exercises the direct stack-window seam that
    // must not be limited to the two fixed slots.
    g_runtime_vblank_starts = 396;
    g_cpu.R[15] = 0x0833A4C8u;  // origin writer (non-pool CPU store)
    g_cpu.R[13] = 0x03007E34u;
    g_cpu.R[9] = 0x03006300u;
    constexpr uint32_t kPoisonAlias = 0x03107E30u;
    put32(kPoisonAlias, 0xDCEF0210u);
    runtime_note_pool_ldm_bus_write(kPoisonAlias, 4u);

    g_cpu.R[9] = 0xDCEF0210u;  // carriers push already-corrupt r9
    for (uint32_t k = 0; k < 3u; ++k) {
        g_runtime_vblank_starts = 397u + k;
        runtime_trace_event(kMemWrite, 0x030060C0u, 0x03007E30u,
                            0xDCEF0210u, 4u);
        put32(0x03007E30u, 0xDCEF0210u);
    }

    // Flood the neighbouring cell past the depth bound: oldest writers must
    // fall off, proving the ring stays depth-bounded.
    for (uint32_t k = 0; k < 12u; ++k) {
        g_runtime_vblank_starts = 400u + k;
        const uint32_t v = 0x11111110u + k;
        runtime_trace_event(kMemWrite, 0x03006080u, 0x03007E2Cu, v, 4u);
        put32(0x03007E2Cu, v);
    }

    // Fixed-slot path through the direct bus seam (post-write value read).
    put32(0x03007E24u, 0x02003050u);
    g_cpu.R[15] = 0x08000716u;
    g_cpu.R[9] = 0x03002020u;
    g_cpu.R[13] = 0x03007E38u;
    runtime_note_pool_ldm_bus_write(0x03007E24u, 4u);

    // Snapshot at LDM-entry geometry, then the one-shot crash dump.
    g_cpu.R[13] = 0x03007E2Cu;
    g_cpu.R[15] = 0x03006100u;
    g_runtime_vblank_starts = 420u;
    runtime_pool_dispatch_history_record(0x03006100u, 0);
    runtime_pool_dispatch_history_dump(0xDCEF03D0u);

    log = slurp_capture();

    const std::string post_restore_log = restore_pos == std::string::npos
        ? std::string() : log.substr(restore_pos);
    check(post_restore_log.find("pc=0x0BADF00D") == std::string::npos,
          "restore boundary must clear stale pre-restore writer records");
    check(post_restore_log.find("pool-history[0] pc=0x03006048") ==
              std::string::npos,
          "restore boundary must clear stale pre-restore dispatch history");
    check(log.find("pool-ldm load frame=420 state_epoch=") !=
              std::string::npos,
          "snapshot line must carry frame and state_epoch");
    // With snapshot SP=0x03007E2C, ring[0] tracks cell 0x03007E2C (r5) and
    // ring[1] tracks cell 0x03007E30 (r9): the poisoning write lands in
    // ring[1]. The dump must retain its non-carrier PC and own epoch.
    check(line_contains(
               log, "pool-ldm slot-ring[1]=0/4",
               "addr=0x03107E30 value=0xDCEF0210 width=4 pc=0x0833A4C8 "
               "epoch=4"),
           "direct stack-window seam must retain the poisoning writer "
           "(including a mirrored raw address) after restore despite later "
           "carrier pushes");
    check(line_contains(log, "pool-ldm slot-ring[1]=3/4",
                        "epoch=4 frame=399"),
          "newest ring entry must carry the restored epoch and frame");
    check(line_contains(log, "pool-ldm slot-ring[0]=0/8",
                        "addr=0x03007E2C value=0x11111114"),
          "flooded ring must stay depth-bounded (first visible entry "
          "skips evicted oldest writers)");
    check(count_occurrences(log, "value=0x11111110") == 0,
          "evicted oldest flooded write must not survive in the ring");
    check(line_contains(log, "pool-ldm slot=0x03007E2C live=0x1111111B ",
                        "last-write addr=0x03007E2C value=0x1111111B"),
          "newest-entry last-write line keeps its established shape");
    check(line_contains(log, "pool-ldm fixed-slot=0x03007E24 ",
                        "value=0x02003050 width=4 pc=0x08000716"),
          "fixed-slot bus-seam write recorded with its writer PC");
    check(log.find("pool-ldm fixed-slot-ring[0]=0/1") != std::string::npos,
          "fixed-slot ring exposes its single write");
    check(log.find("pool-ldm fixed-slot=0x03007E28 last-write=none") !=
              std::string::npos,
          "untouched fixed slot reports last-write=none");

    // ── Phase D: auth-epoch re-CRC seam tracks window changes ──────────────
    runtime_note_pool_ldm_epoch_crc();
    put32(0x03007E38u, 0x5A5A5A5Au);
    runtime_note_pool_ldm_epoch_crc();
    log = slurp_capture();
    check(count_occurrences(log, "reason=auth-epoch") == 2,
          "auth-epoch seam emits one CRC line per call");
    size_t a1 = log.find("reason=auth-epoch");
    size_t a2 = a1 == std::string::npos
                    ? std::string::npos
                    : log.find("reason=auth-epoch", a1 + 1);
    if (a1 != std::string::npos && a2 != std::string::npos) {
        size_t t1 = log.find("crc=0x", a1);
        size_t t2 = log.find("crc=0x", a2);
        if (t1 != std::string::npos && t2 != std::string::npos &&
            t1 < a2 && t2 < a2 + 200) {
            const uint32_t c1 = static_cast<uint32_t>(
                std::strtoul(log.c_str() + t1 + 6, nullptr, 16));
            const uint32_t c2 = static_cast<uint32_t>(
                std::strtoul(log.c_str() + t2 + 6, nullptr, 16));
            check(c1 != c2,
                  "re-CRC must change when window contents change");
        } else {
            check(false, "auth-epoch lines must each carry a crc field");
        }
    } else {
        check(false, "two auth-epoch lines required");
    }

    // ── Phase E: first-transition latch semantics ─────────────────────────
    // A restore that already contains poison is classified but must not
    // create a transition. After cleaning the word, two partial generated
    // stores reconstruct the full poison value; later carrier writes cannot
    // replace the latched first writer.
    const unsigned transitions_before =
        count_occurrences(slurp_capture(), "pool-ldm poison-transition pc=");
    put32(0x03007E30u, 0xDCEF0210u);
    g_runtime_state_epoch = 5;
    g_runtime_vblank_starts = 500;
    runtime_pool_ldm_probe_restore_boundary();
    log = slurp_capture();
    check(line_contains(log, "[pool-ldm-poison-arm] reason=restore",
                        "classification=target-poison-present"),
          "restore with poison must classify target-poison-present");
    put32(0x03007E30u, 0u);
    runtime_note_pool_ldm_bus_write(0x03007E30u, 4u);
    g_cpu.R[15] = 0x0833A500u;
    runtime_trace_event(kMemWrite, g_cpu.R[15], 0x03007E30u, 0x0210u, 2u);
    put32(0x03007E30u, 0x00000210u);
    g_cpu.R[15] = 0x0833A502u;
    runtime_trace_event(kMemWrite, g_cpu.R[15], 0x03007E32u, 0xDCEFu, 2u);
    put32(0x03007E30u, 0xDCEF0210u);
    for (uint32_t k = 0; k < 12u; ++k) {
        g_cpu.R[15] = 0x03006080u + k * 2u;
        runtime_trace_event(kMemWrite, g_cpu.R[15], 0x03107E30u,
                            0xDCEF0210u, 4u);
        put32(0x03107E30u, 0xDCEF0210u);
    }
    runtime_pool_dispatch_history_record(0x03006100u, 0);
    runtime_pool_dispatch_history_dump(0xDCEF0400u);
    log = slurp_capture();
    check(count_occurrences(log, "pool-ldm poison-transition pc=") ==
              transitions_before + 1u,
          "poison transition must latch only once across carriers");
    check(line_contains(log, "pool-ldm poison-transition pc=0x0833A502",
                        "state_epoch=5 frame=500 thumb=0 image_base=0x00000000 "
                        "seam=generated"),
          "partial generated stores must identify the first poison writer");

    // A new restore/epoch clears the old latch and permits one new transition
    // through a mirrored full bus write.
    put32(0x03007E30u, 0u);
    g_runtime_state_epoch = 6;
    g_runtime_vblank_starts = 600;
    runtime_pool_ldm_probe_restore_boundary();
    put32(0x03107E30u, 0xDCEF0210u);
    g_cpu.R[15] = 0x0833A600u;
    runtime_note_pool_ldm_bus_write(0x03107E30u, 4u);
    runtime_pool_dispatch_history_record(0x03006100u, 0);
    runtime_pool_dispatch_history_dump(0xDCEF0404u);
    log = slurp_capture();
    check(count_occurrences(log, "pool-ldm poison-transition pc=") ==
              transitions_before + 2u,
          "restore must reset the latch for a new epoch");
    check(line_contains(log, "pool-ldm poison-transition pc=0x0833A600",
                        "state_epoch=6 frame=600 thumb=0 image_base=0x00000000 "
                        "seam=bus"),
          "mirrored full bus transition must retain the new epoch");

    // ── Phase F: generated fast-IWRAM observer brackets one write ─────────
    put32(0x03007E30u, 0u);
    g_runtime_state_epoch = 7;
    g_runtime_vblank_starts = 700;
    runtime_pool_ldm_probe_restore_boundary();
    g_fast_iwram_event_count = 0;
    for (auto& event : g_fast_iwram_events) event = {};
    g_runtime_fast_iwram_write_observer = &observe_fast_iwram_write;
    g_cpu.R[15] = 0x0833A700u;
    bus_write_u32(0x03007E30u, 0xDCEF0210u);
    g_runtime_fast_iwram_write_observer = nullptr;
    check(g_fast_iwram_event_count == 2u,
          "fast-IWRAM write must emit exactly one start and committed event");
    if (g_fast_iwram_event_count == 2u) {
        check(g_fast_iwram_events[0].address == 0x03007E30u &&
              g_fast_iwram_events[0].width == 4u &&
              g_fast_iwram_events[0].phase == RUNTIME_FAST_IWRAM_WRITE_START &&
              g_fast_iwram_events[0].value == 0u,
              "fast-IWRAM start must retain exact address/size and pre-write bytes");
        check(g_fast_iwram_events[1].address == 0x03007E30u &&
              g_fast_iwram_events[1].width == 4u &&
              g_fast_iwram_events[1].phase ==
                  RUNTIME_FAST_IWRAM_WRITE_COMMITTED &&
              g_fast_iwram_events[1].value == 0xDCEF0210u,
              "fast-IWRAM committed event must see written bytes");
    }
    runtime_pool_dispatch_history_record(0x03006100u, 0);
    runtime_pool_dispatch_history_dump(0xDCEF0408u);
    log = slurp_capture();
    check(line_contains(log, "pool-ldm poison-transition pc=0x0833A700",
                        "state_epoch=7 frame=700 thumb=0 image_base=0x00000000 "
                        "seam=bus"),
          "fast-IWRAM observer must see the committed poison word");

    // The slow GbaBus path still owns its own start/commit pair. Verify that
    // falling back to it does not duplicate the committed callback.
    gba::GbaBus slow_bus;
    gbarecomp::set_active_bus(&slow_bus);
    g_fast_iwram = nullptr;
    g_slow_iwram_commit_count = 0;
    g_slow_iwram_commit_address = 0;
    g_slow_iwram_commit_width = 0;
    gba::vram_trace::reset_oam_trace_window();
    gba::vram_trace::set_default_enabled(true);
    gba::vram_trace::set_oam_shadow_write_observer(
        &observe_slow_iwram_commit);
    slow_bus.write32(0x03003480u, 0x12345678u);
    gba::vram_trace::OamShadowTraceStats slow_stats{};
    gba::vram_trace::get_oam_shadow_trace_stats(&slow_stats);
    check(slow_stats.write_calls == 1u &&
          g_slow_iwram_commit_count == 1u &&
          g_slow_iwram_commit_address == 0x03003480u &&
          g_slow_iwram_commit_width == 4u &&
          slow_bus.read32(0x03003480u) == 0x12345678u,
          "slow IWRAM path must emit one start/committed pair and write bytes");
    gba::vram_trace::set_oam_shadow_write_observer(nullptr);
    gba::vram_trace::set_default_enabled(false);
    gbarecomp::set_active_bus(nullptr);
}

}  // namespace

int main() {
    test_arm_crc_and_ring_provenance();
    if (g_failures == 0) std::printf("pool_ldm_probe_tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
