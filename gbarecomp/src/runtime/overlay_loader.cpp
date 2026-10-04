// overlay_loader.cpp — see overlay_loader.h.

#include "overlay_loader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "overlay_abi.h"
#include "overlay_compile.h"      // overlay_compile_one, HealBackend, heal_backend_name
#include "hot_queue_policy.h"
#include "runtime_arm.h"          // g_cpu, g_runtime_*, every runtime/bus/arm fn
#include "runtime_bus_bridge.h"   // active_bus
#include "host_prof_phase.h"      // host profiler phase marker
#include "../gba/gba_bus.h"       // rom_ptr / rom_size / iwram_ptr / ewram_ptr
#include "../gba/gba_bios.h"      // GbaBios snapshot
#include "../gba/crc32.h"         // gba::crc32

namespace fs = std::filesystem;

namespace gbarecomp {

namespace {

// ── Key: (pc & ~1) << 1 | thumb ────────────────────────────────────
inline uint64_t heal_key(uint32_t pc, bool thumb) {
    return (static_cast<uint64_t>(pc & ~1u) << 1) | (thumb ? 1u : 0u);
}

// True iff `addr` lives in a writable RAM region a self-assembled function
// could occupy (IWRAM or EWRAM). Forward-declared here so request_identity()
// (below) can use it; defined further down alongside its other RAM-address
// helpers.
bool is_ram_addr(uint32_t addr);

// ── Healed native function (game-thread-only) ──────────────────────
struct HealedEntry {
    uint32_t addr  = 0;
    bool     thumb = false;
    void   (*fn)(void) = nullptr;
    void*    module = nullptr;   // HMODULE of the loaded shard DLL
    uint32_t crc = 0;
    uint32_t end = 0;
    uint64_t native_calls = 0;
    // True iff `addr` lives in IWRAM/EWRAM: the bytes this entry was compiled
    // from can change under the game at any time, so overlay_resolve
    // re-verifies [addr, end) on every dispatch via heal_entry_crc_ok() and
    // only calls `fn` on a match. That re-verification is cheap in the common
    // case — see heal_entry_crc_ok()'s comment — but it is never skipped
    // outright. False (ROM/BIOS-backed) entries skip this — those images are
    // immutable for the run, so dispatch stays zero-overhead.
    bool     ram_backed = false;
    bool     relocatable = false;
    std::shared_ptr<const std::vector<uint8_t>> code_bytes;
    // LRU bookkeeping for HealSlot::alternates (see below). Meaningless for
    // an entry currently sitting in HealSlot::incumbent.
    uint64_t last_used = 0;
};

// ── Bounded per-key healed-code cache (multi-variant) ───────────────
// Golden Sun's sound-driver/sprite-blitter pool assembles code in place at a
// fixed RAM address; live memory there alternates among a SMALL, BOUNDED,
// RECURRING set of variants. A single (pc, thumb) -> HealedEntry slot makes
// every toggle look like a brand-new miss (CRC mismatch -> erase -> dispatch
// miss -> synchronous full-subtree interpretation), even though the exact
// bytes were already compiled moments earlier.
//
// HealSlot keeps ONE hot "incumbent" (serviced exactly like the old single
// entry — zero added cost on the common non-alternating path) plus a bounded
// set of OTHER resident variants keyed by content CRC. On an incumbent CRC
// miss, the alternates are checked (each re-verified with the SAME
// unconditional CRC32 check over the SAME byte range as the incumbent —
// nothing is ever entered on a stale or unverified basis); a match is
// promoted to incumbent and the displaced incumbent joins the alternates
// (never discarded, never unloaded — only the mapping/position changes).
// Only a genuinely new variant falls through to today's miss/bridge/re-enqueue.
// Keep enough resident variants for generated RAM code that has a modest
// multi-body working set, while retaining a finite per-key bound.  The old
// 8-entry bound churned when one address legitimately produced 10 CRCs.
constexpr std::size_t kMaxVariantsPerKey = 10;  // incumbent + up to 9 alternates

struct HealSlot {
    HealedEntry              incumbent;
    std::vector<HealedEntry> alternates;  // resident, non-incumbent variants
};

struct RequestIdentity {
    uint64_t base_key = 0;
    uint64_t digest = 0;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    // Number of leading bytes of *bytes (starting at pc, since a RAM
    // snapshot's byte 0 == pc) that `digest` and operator== actually compare.
    // For BIOS/ROM (bytes == nullptr) this is unused. For a RAM snapshot it
    // is EITHER the full snapshot size (unknown/first-seen extent) OR the
    // largest real compiled extent ever observed for this (pc,thumb) — see
    // request_identity(). Once an extent is known, compare exactly that
    // extent so unrelated bytes after a short body cannot defeat in-flight
    // deduplication. Narrowing this window can only ever cause an EXTRA
    // transient miss (two distinct bodies look like "the same request" and
    // one gets skipped) — never a wrong-body execution: the installed
    // entry's real [addr,end) is still re-verified against LIVE bytes by
    // heal_entry_crc_ok/heal_slot_resolve_ram on every dispatch, independent
    // of how the dedup key was computed.
    std::size_t window = 0;

    bool operator==(const RequestIdentity& o) const {
        if (base_key != o.base_key || digest != o.digest ||
            static_cast<bool>(bytes) != static_cast<bool>(o.bytes)) return false;
        if (!bytes) return true;  // immutable ROM/BIOS remain pc+mode keyed
        if (window != o.window) return false;  // different comparison scope
        if (bytes.get() == o.bytes.get()) return true;
        return std::memcmp(bytes->data(), o.bytes->data(), window) == 0;
    }
};

struct RequestIdentityHash {
    std::size_t operator()(const RequestIdentity& k) const {
        return static_cast<std::size_t>(k.base_key ^ k.digest ^
                                       (k.digest >> 32));
    }
};

// Game-thread-only, same invariant as g_healed/s_inflight/s_failed below:
// largest real compiled extent ever observed for a RAM-backed (pc,thumb)
// key, in bytes from pc. request_identity() only ever runs on the game
// thread (overlay_request_compile, overlay_mark_current_request_failed_for_test,
// and worker_main which now reuses the identity computed at enqueue time
// instead of recomputing it — see worker_main), so no locking is needed.
std::unordered_map<uint64_t, uint32_t> s_ram_extent_seen;

// Minimum RAM dedup identity window for a key whose real extent is not known
// yet (see request_identity). Once a compiled extent is known, the exact
// extent is used so unrelated trailing RAM cannot defeat deduplication. This
// is purely a dedup-effectiveness heuristic, never a correctness bound: a
// narrower window can only cost an extra transient miss (see the
// RequestIdentity::window comment); the dispatch-time CRC recheck is what
// actually gates execution either way.
constexpr std::size_t kMinRamIdentityWindow = 64;

RequestIdentity request_identity(const OverlayWorkItem& w) {
    RequestIdentity k;
    k.base_key = heal_key(w.pc, w.thumb);
    k.bytes = w.owned;
    if (k.bytes) {
        std::size_t window = k.bytes->size();
        if (is_ram_addr(w.pc)) {
            // Golden Sun's sound driver mutates unrelated bytes elsewhere in
            // the (up to 16 KB) RAM snapshot between two misses at the same
            // PC whose actual FUNCTION content is byte-identical; comparing
            // the whole snapshot (the old behavior) made those two requests
            // look different and defeated dedup. Once a real compiled extent
            // is known, compare exactly [pc, pc+extent), not a rounded-up
            // prefix that can include unrelated mutable bytes. First
            // sighting of a key (no recorded extent yet) keeps the bounded
            // minimum window so genuinely-distinct pending bodies remain
            // independently discoverable.
            const auto it = s_ram_extent_seen.find(k.base_key);
            if (it != s_ram_extent_seen.end()) {
                window = it->second;
                window = std::min(window, k.bytes->size());
            } else {
                // The first result may still be queued while the guest
                // mutates unrelated RAM. Bound the dedup prefix now too;
                // dispatch still verifies the complete compiled extent.
                window = std::min<std::size_t>(kMinRamIdentityWindow,
                                               k.bytes->size());
            }
        }
        k.window = window;
        // FNV narrows the hash table only. operator== always proves the
        // [0, window) prefix byte-for-byte, so collisions never authorize
        // suppression or poison another RAM variant.
        uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < window; ++i) {
            h ^= (*k.bytes)[i];
            h *= 1099511628211ull;
        }
        k.digest = h;
    }
    return k;
}

// Global monotonic tick used to timestamp HealedEntry::last_used so LRU
// eviction among a slot's alternates has a total order. Game-thread-only.
uint64_t s_lru_tick = 0;

// ── Worker → game-thread result ────────────────────────────────────
struct ReadyEntry {
    uint64_t key = 0;
    bool has_request = false;
    RequestIdentity request;
    bool     ok = false;
    OverlayCompiled c;
};

// ── Host arch token for the cache namespace ────────────────────────
// Keeps a Windows-x64 gcc DLL and a future Linux-arm64 tcc DLL for the same
// function in separate subdirs so a stale-arch artifact never wins.
const char* overlay_arch_abi() {
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64))
    return "windows-x64";
#elif defined(_WIN32)
    return "windows-x86";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

// ── State ──────────────────────────────────────────────────────────
bool                     s_active = false;       // feature on AND inited
bool                     s_ever_active = false;   // ever inited this session (sticky)
// Mutable IWRAM/EWRAM code is not safe to execute natively until its writer
// and image identity are proven.  Keep Stage-2 healing limited to immutable
// ROM/BIOS by default; opt in for experiments with GBARECOMP_SELFHEAL_RAM=1.
bool                     s_ram_heal_enabled = false;
std::string              s_cache_base;            // cache_root/<image_sha1>
std::string              s_active_cache_dir;      // base/<backend>/<arch> (worker writes here)
HealBackend              s_backend = HealBackend::Gcc;  // resolved producer
std::vector<uint8_t>     s_bios_bytes;            // 16 KB BIOS snapshot (base 0)
GbaOverlayCallbacks      g_callbacks{};

// Game-thread-only:
std::unordered_map<uint64_t, HealSlot> g_healed;
std::unordered_set<RequestIdentity, RequestIdentityHash> s_inflight;
std::unordered_set<RequestIdentity, RequestIdentityHash> s_failed;
uint64_t                                  s_native_calls_total = 0;
uint64_t                                  s_ram_revalidations = 0;      // CRC rechecks performed
uint64_t                                  s_ram_crc_mismatches = 0;     // rechecks that forced a re-heal
uint64_t                                  s_relocatable_alias_hits = 0;
uint64_t                                  s_relocatable_alias_misses = 0;

// ── TEMPORARY cost-attribution probe (GBARECOMP_COST_PROBE=1) ──────────────
// GS-011-perf follow-up: prices heal_entry_crc_ok — the RAM-identity re-check
// overlay_resolve performs on EVERY RAM-backed native call (ram_revalidations
// == native_calls, ~854/frame). Same env var and on/off idiom as
// runtime_bus_bridge.cpp's probe (reuses its g_cost_probe_enabled() rather
// than re-parsing the env var here); diagnostic-only, intended for removal.
// heal_entry_crc_ok is a LEAF: it calls only runtime_ram_code_guard (which
// itself calls only gba::crc32, on a cache miss) and never re-enters
// overlay_try_dispatch/heal_slot_resolve_ram, so — unlike the runtime_tick
// CostTimer that double-counted through IRQ re-entrancy — a clock read
// around it measures one real, non-overlapping span per call. Since
// runtime_ram_code_guard skips the CRC32 entirely on a cache hit, this probe
// now mostly prices the guard's cache lookup, not a hash. Still, at >1M
// calls/run the clock-read overhead itself is non-trivial; probe-on vs
// probe-off wall time is reported by the caller to quantify it.
extern "C" bool g_cost_probe_enabled(void);
uint64_t s_cost_ram_crc_ns    = 0;
uint64_t s_cost_ram_crc_calls = 0;
uint64_t s_cost_ram_crc_bytes = 0;
// Lazily initialized on first call (NOT a namespace-scope static-init lambda):
// g_cost_probe_enabled() itself is backed by a static-init lambda in
// runtime_bus_bridge.cpp (a different TU), and cross-TU static init order is
// unspecified — evaluating it from *this* TU's own static init could run
// before that flag's env-var read has happened, silently reading a
// zero-initialized false. Deferring to first real call (well after main())
// sidesteps the whole static-initialization-order question.
bool cost_ram_crc_probe_enabled() {
    static const bool on = [] {
        const bool on = g_cost_probe_enabled();
        if (on) {
            std::atexit([] {
                std::fprintf(stderr,
                    "[cost] ram_crc (heal_entry_crc_ok): %llu ns over %llu calls, "
                    "%llu bytes hashed (leaf, non-reentrant)\n",
                    static_cast<unsigned long long>(s_cost_ram_crc_ns),
                    static_cast<unsigned long long>(s_cost_ram_crc_calls),
                    static_cast<unsigned long long>(s_cost_ram_crc_bytes));
            });
        }
        return on;
    }();
    return on;
}

// Max bytes snapshotted from a mutable (IWRAM/EWRAM) region for one heal
// attempt. Bounded so the synchronous game-thread copy is cheap even for a
// pathological miss near the start of the region; real self-assembled
// blitters (the motivating case) are a few dozen instructions.
constexpr std::size_t kMaxRamSnapshotBytes = 16 * 1024;

// Cross-thread work/ready queues:
struct QueuedWork {
    OverlayWorkItem item;
    RequestIdentity request;
};

std::mutex                  s_work_mtx;
std::condition_variable     s_work_cv;
std::condition_variable     s_ready_cv;
std::deque<QueuedWork>      s_work;
std::unordered_set<RequestIdentity, RequestIdentityHash> s_queued;
std::unordered_set<RequestIdentity, RequestIdentityHash> s_hot_requested;
unsigned                    s_hot_burst = 0;
std::atomic<uint64_t>       s_hot_requests{0};
std::atomic<uint64_t>       s_hot_selected{0};
std::atomic<uint64_t>       s_fairness_pops{0};
std::thread                 s_worker;
std::atomic<bool>           s_stop{false};
// Number of queued (not currently executing) compile requests discarded by
// the most recent shutdown.  Shutdown only drops work still in the queue;
// the worker's active compile is always allowed to finish before join.
std::atomic<uint64_t>       s_shutdown_discarded_work{0};

std::mutex                  s_ready_mtx;
std::deque<ReadyEntry>      s_ready;
std::atomic<int>            s_ready_pending{0};

// Background warm-load thread (see the "Background warm-loader" block below).
// Shares s_stop/s_ready/s_ready_mtx with the compile worker: it never touches
// g_healed directly, only publishes finished entries through the SAME ready
// queue the compile worker uses, so overlay_drain_ready (game-thread-only)
// stays the single writer to g_healed.
std::thread                 s_warm_thread;
// Set once warm_load_background() has finished BOTH the ROM/BIOS and RAM
// scans. Read from overlay_request_compile (game thread) to decide whether
// the targeted single-key disk lookup below is still worth attempting;
// written once, from the warm thread, at the very end of
// warm_load_background(). Reset to false at the top of overlay_loader_init,
// before either background thread is spawned.
std::atomic<bool>           s_warm_load_done{false};

// ── Frame-tagged self-heal/SMC instrumentation (diagnostic) ─────────────
// See overlay_loader.h. Game-thread-only (every event source below —
// runtime_dispatch_miss, runtime_mutable_ram_code_miss, overlay_try_dispatch,
// overlay_request_compile, overlay_drain_ready — runs on the game thread), so
// no locking. Disabled (near-zero cost) unless GBARECOMP_FRAME_EVENTS is set.
struct FrameEventCounts {
    uint64_t frame            = 0;
    uint32_t dispatch_miss    = 0;  // runtime_dispatch_miss() invocations
    uint32_t compile_enqueued = 0;  // overlay_request_compile() actually queued
    uint32_t heal_installed   = 0;  // overlay_drain_ready() successful installs
    uint32_t ram_crc_mismatch = 0;  // overlay_try_dispatch() stale-RAM detections
    uint32_t ram_smc_fallback = 0;  // runtime_mutable_ram_code_miss() invocations
    uint64_t bridge_ns        = 0;  // wall time inside runtime_bridge_interpret
    uint32_t ready_pending_at_frame = 0; // ready entries seen at frame mark
    uint32_t ready_pending_after_drain = 0; // entries left after last drain
    uint32_t ready_drain_calls = 0;  // non-empty game-thread drain calls
    uint32_t ready_drain_entries = 0; // entries moved out of ready queue
    uint64_t ready_drain_ns = 0;  // wall time spent installing ready entries
    uint8_t warm_load_done = 0; // background cache scan completed at frame mark
};
struct BridgeMissEvent {
    uint64_t frame = 0;
    uint32_t pc = 0;
    bool thumb = false;
    uint64_t bridge_ns = 0;
    uint64_t interpreted_insns = 0;
    OverlayRequestOutcome outcome = OverlayRequestOutcome::Failed;
};
bool                           s_frame_events_enabled = false;
std::string                    s_frame_events_path;
std::vector<FrameEventCounts>  s_frame_events;
std::vector<BridgeMissEvent>   s_bridge_miss_events;

// RAM churn is deliberately a separate probe from the existing frame
// aggregates. It records only sparse, coalesced event rows, so a hot RAM
// dispatch cannot turn diagnostics into per-event I/O or an unbounded vector.
// The ring bounds retained history to the latest 16,384 guest frames and each
// frame to 64 unique PC/mode pairs. Rows are keyed by event + PC + mode + the
// small identity/cache labels below; counts, never guest bytes, are emitted.
struct RamChurnRow {
    uint32_t pc = 0;
    bool thumb = false;
    const char* event = nullptr;
    const char* identity = nullptr;
    const char* cache_kind = nullptr;
    uint32_t addr = 0;
    uint32_t pointer_target = 0;
    bool pointer_thumb = false;
    uint64_t count = 0;
};
struct RamChurnFrame {
    uint64_t frame = 0;
    std::vector<RamChurnRow> rows;
    bool pc_cap_reported = false;
    bool row_cap_reported = false;
};
constexpr std::size_t kRamChurnMaxFrames = 16384;
constexpr std::size_t kRamChurnMaxPcsPerFrame = 64;
constexpr std::size_t kRamChurnMaxRowsPerFrame = 256;
bool                           s_ram_churn_probe_enabled = false;
std::string                    s_ram_churn_path;
std::deque<RamChurnFrame>      s_ram_churn_frames;
uint64_t                       s_ram_churn_current_frame = 0;
uint64_t                       s_ram_churn_dropped_frames = 0;
uint64_t                       s_ram_churn_dropped_pcs = 0;
uint64_t                       s_ram_churn_dropped_rows = 0;

// Lifecycle evidence for the one measured 0x03005B0C image. This is kept
// separate from the sparse churn rows because each exact DMA completion is a
// distinct generation. CPU-writer completion and first dispatch are emitted
// as bounded metadata rows. No guest bytes are retained.
struct RamImageLifecycleRow {
    uint64_t generation = 0;
    uint64_t frame = 0;
    const char* event = nullptr;
    uint32_t writer_pc = 0;
    bool writer_thumb = false;
    uint32_t source = 0;
    uint32_t dest = 0;
    uint32_t size = 0;
    uint32_t crc32 = 0;
    uint32_t channel = 0;
    uint64_t first_dispatch_frame = 0;
    uint32_t first_dispatch_pc = 0;
    bool first_dispatch_thumb = false;
    uint32_t cpu_writer_first_pc = 0;
    uint32_t cpu_writer_last_pc = 0;
    bool cpu_writer_thumb = false;
    uint32_t cpu_writer_first_addr = 0;
    uint32_t cpu_writer_last_end = 0;
    uint32_t cpu_writer_stores = 0;
    uint32_t early_dispatches = 0;
    uint32_t late_writes = 0;
};
constexpr std::size_t kRamImageLifecycleMaxRows = 4096;
constexpr std::size_t kNoRamImageLifecycleRow = static_cast<std::size_t>(-1);
std::string                    s_ram_image_lifecycle_path;
std::deque<RamImageLifecycleRow> s_ram_image_lifecycle_rows;
uint64_t                       s_ram_image_generation = 0;
std::size_t                    s_ram_image_dma_row = kNoRamImageLifecycleRow;
std::size_t                    s_ram_image_writer_row = kNoRamImageLifecycleRow;
uint32_t                       s_ram_image_writer_call_depth = 0;
bool                           s_ram_image_writer_started = false;
std::size_t                    s_ram_image_pending_row = kNoRamImageLifecycleRow;

const char* ram_churn_outcome_name(OverlayRequestOutcome outcome) {
    switch (outcome) {
    case OverlayRequestOutcome::Alias:    return "alias";
    case OverlayRequestOutcome::Queued:   return "queued";
    case OverlayRequestOutcome::Inflight: return "inflight";
    case OverlayRequestOutcome::Failed:   return "failed";
    }
    return "unknown";
}

RamChurnFrame& ram_churn_frame_bucket(uint64_t frame) {
    if (s_ram_churn_frames.empty() || s_ram_churn_frames.back().frame != frame) {
        s_ram_churn_frames.push_back({});
        s_ram_churn_frames.back().frame = frame;
        if (s_ram_churn_frames.size() > kRamChurnMaxFrames) {
            s_ram_churn_frames.pop_front();
            ++s_ram_churn_dropped_frames;
        }
    }
    return s_ram_churn_frames.back();
}

void ram_churn_record_fields(uint64_t frame, const char* event, uint32_t pc,
                             bool thumb, const char* identity,
                             const char* cache_kind, uint32_t addr,
                             uint32_t pointer_target, bool pointer_thumb) {
    if (!s_ram_churn_probe_enabled) return;
    RamChurnFrame& bucket = ram_churn_frame_bucket(frame);
    for (RamChurnRow& row : bucket.rows) {
        if (row.pc == pc && row.thumb == thumb && row.event == event &&
            row.identity == identity && row.cache_kind == cache_kind &&
            row.addr == addr && row.pointer_target == pointer_target &&
            row.pointer_thumb == pointer_thumb) {
            ++row.count;
            return;
        }
    }

    bool known_pc = false;
    for (const RamChurnRow& row : bucket.rows) {
        if (row.pc == pc && row.thumb == thumb) {
            known_pc = true;
            break;
        }
    }
    if (!known_pc) {
        std::size_t unique = 0;
        for (std::size_t i = 0; i < bucket.rows.size(); ++i) {
            bool first = true;
            for (std::size_t j = 0; j < i; ++j) {
                if (bucket.rows[j].pc == bucket.rows[i].pc &&
                    bucket.rows[j].thumb == bucket.rows[i].thumb) {
                    first = false;
                    break;
                }
            }
            if (first) ++unique;
        }
        if (unique >= kRamChurnMaxPcsPerFrame) {
            ++s_ram_churn_dropped_pcs;
            if (!bucket.pc_cap_reported) {
                bucket.pc_cap_reported = true;
                std::fprintf(stderr,
                    "[ram-churn] frame=%llu unique-pc cap=%zu; "
                    "additional PCs are coalesced\n",
                    static_cast<unsigned long long>(frame),
                    kRamChurnMaxPcsPerFrame);
            }
            return;
        }
    }
    if (bucket.rows.size() >= kRamChurnMaxRowsPerFrame) {
        ++s_ram_churn_dropped_rows;
        if (!bucket.row_cap_reported) {
            bucket.row_cap_reported = true;
            std::fprintf(stderr,
                "[ram-churn] frame=%llu row cap=%zu; additional event "
                "kinds are coalesced\n",
                static_cast<unsigned long long>(frame),
                kRamChurnMaxRowsPerFrame);
        }
        return;
    }
    bucket.rows.push_back({pc, thumb, event, identity, cache_kind,
                           addr, pointer_target, pointer_thumb, 1});
}

void ram_churn_record(uint64_t frame, const char* event, uint32_t pc,
                      bool thumb, const char* identity,
                      const char* cache_kind) {
    ram_churn_record_fields(frame, event, pc, thumb, identity, cache_kind,
                            0u, 0u, false);
}

void ram_churn_record_ram_pointer_write(
    uint64_t frame, uint32_t dest, uint32_t writer_pc, uint8_t writer_thumb,
    uint32_t pointer_target, uint8_t pointer_thumb) {
    ram_churn_record_fields(frame, "ram_pointer_write", writer_pc,
                            writer_thumb != 0u, "target-image", "pointer",
                            dest, pointer_target, pointer_thumb != 0u);
}

constexpr uint32_t kRamImageStart = 0x03005B0Cu;
constexpr uint32_t kRamImageEnd = 0x03005D5Cu;

void ram_image_lifecycle_drop_oldest() {
    if (s_ram_image_lifecycle_rows.empty()) return;
    s_ram_image_lifecycle_rows.pop_front();
    auto adjust = [](std::size_t& index) {
        if (index == kNoRamImageLifecycleRow) return;
        index = index == 0u ? kNoRamImageLifecycleRow : index - 1u;
    };
    adjust(s_ram_image_dma_row);
    adjust(s_ram_image_writer_row);
    adjust(s_ram_image_pending_row);
}

std::size_t ram_image_lifecycle_append(const RamImageLifecycleRow& row) {
    if (s_ram_image_lifecycle_rows.size() >= kRamImageLifecycleMaxRows)
        ram_image_lifecycle_drop_oldest();
    s_ram_image_lifecycle_rows.push_back(row);
    return s_ram_image_lifecycle_rows.size() - 1u;
}

uint32_t ram_image_lifecycle_crc32() {
    std::array<uint8_t, kRamImageEnd - kRamImageStart> image{};
    for (std::size_t i = 0; i < image.size(); ++i)
        image[i] = bus_read_u8(kRamImageStart + static_cast<uint32_t>(i));
    return gba::crc32(image.data(), image.size());
}

void ram_image_lifecycle_dma(
    uint64_t frame, uint8_t completed, uint32_t source, uint32_t dest,
    uint32_t size, uint32_t writer_pc, uint8_t writer_thumb, uint32_t crc32,
    uint32_t channel) {
    if (!s_ram_churn_probe_enabled) return;
    if (!completed) {
        ++s_ram_image_generation;
        s_ram_image_dma_row = ram_image_lifecycle_append({
            s_ram_image_generation, frame, "trigger", writer_pc,
            writer_thumb != 0u, source, dest, size, 0u, channel,
            0u, 0u, false});
        s_ram_image_writer_row = kNoRamImageLifecycleRow;
        s_ram_image_writer_call_depth = 0;
        s_ram_image_writer_started = false;
        s_ram_image_pending_row = kNoRamImageLifecycleRow;
        return;
    }
    if (s_ram_image_dma_row == kNoRamImageLifecycleRow) return;
    s_ram_image_writer_row = ram_image_lifecycle_append({
        s_ram_image_generation, frame, "complete", writer_pc,
        writer_thumb != 0u, source, dest, size, crc32, channel,
        0u, 0u, false});
    s_ram_image_writer_started = false;
    s_ram_image_pending_row = kNoRamImageLifecycleRow;
}

void ram_image_lifecycle_write(uint64_t frame, uint32_t pc, uint8_t thumb,
                               uint32_t addr, uint32_t width) {
    if (!s_ram_churn_probe_enabled) return;
    if (s_ram_image_writer_row == kNoRamImageLifecycleRow) {
        if (s_ram_image_pending_row != kNoRamImageLifecycleRow) {
            RamImageLifecycleRow& row =
                s_ram_image_lifecycle_rows[s_ram_image_pending_row];
            if (row.late_writes != UINT32_MAX) ++row.late_writes;
            s_ram_image_pending_row = kNoRamImageLifecycleRow;
        }
        return;
    }
    RamImageLifecycleRow& row =
        s_ram_image_lifecycle_rows[s_ram_image_writer_row];
    if (!row.event || std::strcmp(row.event, "complete") != 0) return;
    if (!s_ram_image_writer_started) {
        s_ram_image_writer_started = true;
        s_ram_image_writer_call_depth = runtime_call_stack_depth();
        row.cpu_writer_first_pc = pc;
        row.cpu_writer_thumb = thumb != 0u;
        row.cpu_writer_first_addr = addr;
    }
    row.cpu_writer_last_pc = pc;
    row.cpu_writer_last_end = addr + width;
    if (row.cpu_writer_stores != UINT32_MAX) ++row.cpu_writer_stores;
    (void)frame;
}

void ram_image_lifecycle_boundary(uint8_t outer, uint32_t return_pc,
                                  uint32_t call_stack_depth) {
    if (!s_ram_churn_probe_enabled || !s_ram_image_writer_started ||
        s_ram_image_writer_row == kNoRamImageLifecycleRow) return;
    if (outer == 0u &&
        (s_ram_image_writer_call_depth == 0u ||
         call_stack_depth >= s_ram_image_writer_call_depth)) return;

    RamImageLifecycleRow& complete =
        s_ram_image_lifecycle_rows[s_ram_image_writer_row];
    RamImageLifecycleRow final = complete;
    final.event = "writer_complete";
    final.frame = s_ram_churn_current_frame;
    final.crc32 = ram_image_lifecycle_crc32();
    final.first_dispatch_frame = 0u;
    final.first_dispatch_pc = 0u;
    final.first_dispatch_thumb = false;
    final.late_writes = 0u;
    s_ram_image_pending_row = ram_image_lifecycle_append(final);
    s_ram_image_writer_row = kNoRamImageLifecycleRow;
    s_ram_image_dma_row = kNoRamImageLifecycleRow;
    s_ram_image_writer_started = false;
    s_ram_image_writer_call_depth = 0u;
    (void)return_pc;
}

void ram_image_lifecycle_dispatch(uint64_t frame, uint32_t pc,
                                  uint8_t thumb) {
    if (!s_ram_churn_probe_enabled ||
        pc < kRamImageStart || pc >= kRamImageEnd) {
        return;
    }
    if (s_ram_image_writer_row != kNoRamImageLifecycleRow) {
        ++s_ram_image_lifecycle_rows[s_ram_image_writer_row].early_dispatches;
        return;
    }
    if (s_ram_image_pending_row == kNoRamImageLifecycleRow) return;
    RamImageLifecycleRow& row =
        s_ram_image_lifecycle_rows[s_ram_image_pending_row];
    row.first_dispatch_frame = frame;
    row.first_dispatch_pc = pc;
    row.first_dispatch_thumb = thumb != 0u;
    s_ram_image_pending_row = kNoRamImageLifecycleRow;
}

void dump_ram_image_lifecycle() {
    if (!s_ram_churn_probe_enabled || s_ram_image_lifecycle_path.empty()) return;
    std::FILE* f = std::fopen(s_ram_image_lifecycle_path.c_str(), "w");
    if (!f) return;
    std::fprintf(f,
        "generation,frame,event,writer_pc,writer_mode,source,dest,size,"
        "crc32,channel,first_dispatch_frame,first_dispatch_pc,"
        "first_dispatch_mode,cpu_writer_first_pc,cpu_writer_last_pc,"
        "cpu_writer_mode,cpu_writer_first_addr,cpu_writer_last_end,"
        "cpu_writer_stores,early_dispatches,late_writes\n");
    for (const RamImageLifecycleRow& row : s_ram_image_lifecycle_rows) {
        std::fprintf(f, "%llu,%llu,%s,%08X,%s,%08X,%08X,%u,%08X,%u,%llu,"
                        "%08X,%s,%08X,%08X,%s,%08X,%08X,%u,%u,%u\n",
            static_cast<unsigned long long>(row.generation),
            static_cast<unsigned long long>(row.frame), row.event,
            row.writer_pc, row.writer_thumb ? "thumb" : "arm", row.source,
            row.dest, row.size, row.crc32, row.channel,
            static_cast<unsigned long long>(row.first_dispatch_frame),
            row.first_dispatch_pc,
            row.first_dispatch_thumb ? "thumb" : "arm",
            row.cpu_writer_first_pc, row.cpu_writer_last_pc,
            row.cpu_writer_thumb ? "thumb" : "arm",
            row.cpu_writer_first_addr, row.cpu_writer_last_end,
            row.cpu_writer_stores, row.early_dispatches, row.late_writes);
    }
    std::fclose(f);
    std::fprintf(stderr, "[ram-image-lifecycle] dumped %zu rows -> %s\n",
                 s_ram_image_lifecycle_rows.size(),
                 s_ram_image_lifecycle_path.c_str());
    std::fflush(stderr);
}

void dump_ram_churn() {
    if (!s_ram_churn_probe_enabled || s_ram_churn_path.empty()) return;
    std::FILE* f = std::fopen(s_ram_churn_path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "frame,event,pc,mode,count,identity_kind,cache_kind,"
                    "dest,pointer_target,pointer_mode\n");
    uint64_t rows = 0;
    for (const RamChurnFrame& bucket : s_ram_churn_frames) {
        for (const RamChurnRow& row : bucket.rows) {
            std::fprintf(f, "%llu,%s,%08X,%s,%llu,%s,%s,%08X,%08X,%s\n",
                static_cast<unsigned long long>(bucket.frame), row.event,
                row.pc, row.thumb ? "thumb" : "arm",
                static_cast<unsigned long long>(row.count), row.identity,
                row.cache_kind, row.addr, row.pointer_target,
                row.pointer_thumb ? "thumb" : "arm");
            ++rows;
        }
    }
    std::fclose(f);
    std::fprintf(stderr,
        "[ram-churn] dumped %llu rows -> %s (dropped_frames=%llu "
        "dropped_pcs=%llu dropped_rows=%llu)\n",
        static_cast<unsigned long long>(rows), s_ram_churn_path.c_str(),
        static_cast<unsigned long long>(s_ram_churn_dropped_frames),
        static_cast<unsigned long long>(s_ram_churn_dropped_pcs),
        static_cast<unsigned long long>(s_ram_churn_dropped_rows));
    std::fflush(stderr);
}

FrameEventCounts& frame_event_bucket() {
    if (s_frame_events.empty()) s_frame_events.push_back({});
    return s_frame_events.back();
}

void dump_frame_events() {
    if (!s_frame_events_enabled || s_frame_events_path.empty()) return;
    std::FILE* f = std::fopen(s_frame_events_path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "frame,dispatch_miss,compile_enqueued,heal_installed,"
                    "ram_crc_mismatch,ram_smc_fallback,bridge_us,"
                    "ready_pending_at_frame,ready_pending_after_drain,"
                    "ready_drain_calls,ready_drain_entries,ready_drain_us,"
                    "warm_load_done\n");
    for (const FrameEventCounts& c : s_frame_events) {
        std::fprintf(f, "%llu,%u,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%llu,%u\n",
            static_cast<unsigned long long>(c.frame), c.dispatch_miss,
            c.compile_enqueued, c.heal_installed, c.ram_crc_mismatch,
            c.ram_smc_fallback,
            static_cast<unsigned long long>(c.bridge_ns / 1000ull),
            c.ready_pending_at_frame, c.ready_pending_after_drain,
            c.ready_drain_calls, c.ready_drain_entries,
            static_cast<unsigned long long>(c.ready_drain_ns / 1000ull),
            static_cast<unsigned>(c.warm_load_done));
    }
    std::fclose(f);
    const fs::path miss_path = fs::path(s_frame_events_path).string() + ".misses.csv";
    f = std::fopen(miss_path.string().c_str(), "w");
    if (f) {
        std::fprintf(f, "frame,pc,mode,bridge_us,interpreted_insns,request\n");
        static const char* outcomes[] = {"alias", "queued", "inflight", "failed"};
        for (const BridgeMissEvent& e : s_bridge_miss_events) {
            std::fprintf(f, "%llu,%08X,%s,%llu,%llu,%s\n",
                static_cast<unsigned long long>(e.frame), e.pc,
                e.thumb ? "thumb" : "arm",
                static_cast<unsigned long long>(e.bridge_ns / 1000ull),
                static_cast<unsigned long long>(e.interpreted_insns),
                outcomes[static_cast<unsigned>(e.outcome)]);
        }
        std::fclose(f);
    }
    std::fprintf(stderr, "[frame-events] dumped %zu frames -> %s\n",
                 s_frame_events.size(), s_frame_events_path.c_str());
    std::fflush(stderr);
}

// The cache subdir a given backend writes to / is scanned from:
//   recomp_cache/<image_sha1>/<gcc|tcc>/<os-arch>/<pc>_<crc>_<a|t>.dll
std::string cache_dir_for(HealBackend b) {
    return (fs::path(s_cache_base) / heal_backend_name(b) / overlay_arch_abi())
        .string();
}

// ── Toolchain probe: is a real g++/gcc reachable on PATH? ───────────
// Determines the `auto` backend: a dev/production box (gcc present) produces
// optimized gcc DLLs; a toolchain-less player box falls back to bundled tcc.
bool gcc_toolchain_available() {
    static int s_cached = -1;
    if (s_cached >= 0) return s_cached != 0;
    int found = 0;
    if (const char* path = std::getenv("PATH"); path && *path) {
#ifdef _WIN32
        const char sep = ';';
        static const char* exes[] = {"g++.exe", "gcc.exe", "cc.exe", "clang.exe"};
#else
        const char sep = ':';
        static const char* exes[] = {"g++", "gcc", "cc", "clang"};
#endif
        const char* p = path;
        while (*p && !found) {
            const char* e = std::strchr(p, sep);
            std::size_t dlen = e ? static_cast<std::size_t>(e - p) : std::strlen(p);
            if (dlen > 0 && dlen < 480) {
                for (const char* exe : exes) {
                    char cand[512];
                    std::snprintf(cand, sizeof(cand), "%.*s/%s",
                                  static_cast<int>(dlen), p, exe);
                    if (std::FILE* f = std::fopen(cand, "rb")) {
                        std::fclose(f);
                        found = 1;
                        break;
                    }
                }
            }
            if (!e) break;
            p = e + 1;
        }
    }
    s_cached = found;
    return found != 0;
}

// Resolve the production heal backend: GBARECOMP_HEAL_BACKEND = gcc | tcc |
// auto (default auto). `auto` prefers gcc when a real toolchain is reachable
// (the dev/release-quality producer), else the bundled, toolchain-free tcc.
HealBackend resolve_backend() {
    const char* be = std::getenv("GBARECOMP_HEAL_BACKEND");
    if (be && be[0]) {
        if (std::strcmp(be, "gcc") == 0) return HealBackend::Gcc;
        if (std::strcmp(be, "tcc") == 0) return HealBackend::Tcc;
        // auto-no-gcc: pretend this is a toolchain-less player box even though
        // gcc is present — force tcc + the bundled include (heal_simulate_shipped
        // in overlay_compile.cpp). Mirrors psxrecomp's OVERLAY_BACKEND_AUTO_NO_GCC.
        if (std::strcmp(be, "auto-no-gcc") == 0) return HealBackend::Tcc;
        // anything else (incl. "auto") → auto-resolve below
    }
    // The release's bundled g++ (GBARECOMP_HEAL_TOOLCHAIN) counts as gcc.
    if (!heal_toolchain_root().empty()) return HealBackend::Gcc;
    return gcc_toolchain_available() ? HealBackend::Gcc : HealBackend::Tcc;
}

// True iff `addr` lives in a writable RAM region a self-assembled function
// could occupy (IWRAM or EWRAM). Shared by region_bytes (snapshot-on-heal)
// and the dispatch-time CRC recheck (live-bytes-on-call).
bool is_ram_addr(uint32_t addr) {
    return (addr >= 0x03000000u && addr < 0x03008000u) ||   // IWRAM 32 KiB
           (addr >= 0x02000000u && addr < 0x02040000u);     // EWRAM 256 KiB
}

// Resolve a live pointer to `len` bytes at `addr` inside IWRAM/EWRAM, or
// nullptr if `addr` isn't in a RAM region or the range would run off its end.
// Used ONLY by the game-thread dispatch-time CRC recheck (never cached).
const uint8_t* ram_live_ptr(gba::GbaBus* bus, uint32_t addr, uint32_t len) {
    if (!bus) return nullptr;
    if (addr >= 0x03000000u && addr < 0x03008000u) {
        const uint32_t off = addr - 0x03000000u;
        if (static_cast<uint64_t>(off) + len > 32u * 1024u) return nullptr;
        return bus->iwram_ptr() + off;
    }
    if (addr >= 0x02000000u && addr < 0x02040000u) {
        const uint32_t off = addr - 0x02000000u;
        if (static_cast<uint64_t>(off) + len > 256u * 1024u) return nullptr;
        return bus->ewram_ptr() + off;
    }
    return nullptr;
}

// Re-verify `e` against LIVE RAM bytes using the exact [addr, end) range it
// was compiled from. `*live_out` (if non-null) receives the resolved live
// pointer (or nullptr if the region itself couldn't be resolved) regardless
// of whether the CRC matched, so callers can distinguish "content changed"
// from "region shrank/vanished under us".
//
// The actual match test delegates to runtime_ram_code_guard() (runtime_arm),
// which caches the CRC32 result per (addr,end,crc) and only re-hashes when
// the page-epoch counters for [addr,end) show a write since the last check.
// Those counters are bumped by every write path that can touch guest RAM —
// generated-code fast stores and the interpreter's slow-store fallback
// (note_ram_write_for_identity, armv4t/runtime_arm.h), GbaBus::write8/16/32
// (note_ram_code_write_bus, gba/gba_bus.cpp — the same chokepoint DMA and
// timed DMA transfers use), and savestate load (GbaBus::deserialize bumps
// every page unconditionally). So this is not a weaker check than the old
// unconditional gba::crc32() call — it is the same check, skipped only when
// nothing could have changed the bytes since it last ran.
bool heal_entry_crc_ok(const HealedEntry& e, gba::GbaBus* bus,
                       const uint8_t** live_out) {
    const uint32_t len = e.end - e.addr;
    const uint8_t* live = bus ? ram_live_ptr(bus, e.addr, len) : nullptr;
    if (live_out) *live_out = live;
    if (!live) return false;
    const bool cost_probe = cost_ram_crc_probe_enabled();
    if (!cost_probe) return runtime_ram_code_guard(e.addr, e.end, e.crc) != 0;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = runtime_ram_code_guard(e.addr, e.end, e.crc) != 0;
    const uint64_t elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
    if (cost_probe) {
        s_cost_ram_crc_ns += elapsed;
        ++s_cost_ram_crc_calls;
        s_cost_ram_crc_bytes += len;
    }
    return ok;
}

// Insert `h` as the new incumbent of `slot`, displacing any current
// incumbent into `alternates` (never discarding it — see the HealSlot
// comment above). Evicts the least-recently-used alternate first if that
// would exceed kMaxVariantsPerKey resident variants. Eviction only drops the
// cached HealedEntry from the map — the underlying loaded module is never
// unloaded anywhere in this file (overlay_loader_shutdown leaves every DLL
// loaded for the process lifetime), so a dropped alternate can never
// invalidate a pointer still referenced elsewhere; the next sighting of that
// content simply behaves exactly like a fresh miss.
void heal_slot_promote(HealSlot& slot, HealedEntry h) {
    h.last_used = ++s_lru_tick;
    // A worker can finish the same live RAM variant more than once while the
    // game thread is draining misses.  Do not retain duplicate alternates:
    // duplicate entries evict real variants and turn a stable pool into an
    // endless recompile/interpreter storm on map re-entry.
    for (auto it = slot.alternates.begin(); it != slot.alternates.end();) {
        if (it->crc == h.crc && it->end == h.end)
            it = slot.alternates.erase(it);
        else
            ++it;
    }
    if (slot.incumbent.fn) {
        if (slot.incumbent.crc == h.crc && slot.incumbent.end == h.end) {
            // Re-installing the content already resident as incumbent
            // (e.g. a re-scan) — nothing to displace.
            slot.incumbent = h;
            return;
        }
        // The alternates set is keyed by content identity (crc, end) — drop
        // any existing alternate that is about to become a duplicate of the
        // displaced incumbent (can only happen via a racing async compile;
        // see overlay_drain_ready) so the cap always bounds DISTINCT variants.
        for (std::size_t i = 0; i < slot.alternates.size(); ++i) {
            if (slot.alternates[i].crc == slot.incumbent.crc &&
                slot.alternates[i].end == slot.incumbent.end) {
                slot.alternates.erase(slot.alternates.begin() + i);
                break;
            }
        }
        if (slot.alternates.size() >= kMaxVariantsPerKey - 1) {
            std::size_t victim = 0;
            for (std::size_t i = 1; i < slot.alternates.size(); ++i) {
                if (slot.alternates[i].last_used < slot.alternates[victim].last_used)
                    victim = i;
            }
            slot.alternates.erase(slot.alternates.begin() + victim);
        }
        slot.alternates.push_back(slot.incumbent);
    }
    slot.incumbent = h;
}

// Outcome of a dispatch-time RAM-backed resolution attempt.
enum class HealResolveResult {
    kMatched,      // *out_match points at the (possibly newly promoted) incumbent
    kMismatch,     // no resident variant (incumbent or alternate) matched live bytes
    kRegionGone,   // the incumbent's own [addr,end) no longer resolves at all
};

// Dispatch-time resolution for a RAM-backed slot: check the incumbent first
// (hot path, no scan). On a miss, walk the bounded alternates set looking for
// one whose CRC verifies against the CURRENT live bytes; a match is promoted
// to incumbent (swap — the displaced incumbent joins the alternates) and
// returned. Every candidate is re-verified with heal_entry_crc_ok before it
// is ever considered dispatch-eligible.
HealResolveResult heal_slot_resolve_ram(HealSlot& slot, gba::GbaBus* bus,
                                        HealedEntry** out_match) {
    const uint8_t* live = nullptr;
    if (heal_entry_crc_ok(slot.incumbent, bus, &live)) {
        *out_match = &slot.incumbent;
        return HealResolveResult::kMatched;
    }
    if (!live) return HealResolveResult::kRegionGone;

    for (std::size_t i = 0; i < slot.alternates.size(); ++i) {
        HealedEntry cand = slot.alternates[i];  // copy: the vector mutates below
        const uint8_t* cand_live = nullptr;
        if (!heal_entry_crc_ok(cand, bus, &cand_live)) continue;

        HealedEntry old_incumbent = slot.incumbent;
        slot.alternates.erase(slot.alternates.begin() + i);
        slot.alternates.push_back(old_incumbent);
        cand.last_used = ++s_lru_tick;
        slot.incumbent = cand;
        *out_match = &slot.incumbent;
        return HealResolveResult::kMatched;
    }
    return HealResolveResult::kMismatch;
}

// ── Region resolution: the code image a PC lives in ─────────────────
// BIOS and ROM are immutable for the run — zero-copy, pointer straight into
// the backing image. IWRAM/EWRAM are mutable, so this takes a bounded,
// SYNCHRONOUS snapshot on the calling (game) thread and hands the async
// worker an owned, private copy it can safely read without a data race.
// `base` is set to `pc` for a RAM snapshot so the finder's `pc - base`
// arithmetic is exact (the snapshot's byte 0 IS pc).
bool region_bytes(uint32_t pc, OverlayWorkItem* w) {
    if (pc < 0x00004000u) {
        if (s_bios_bytes.empty()) return false;
        w->bytes = s_bios_bytes.data();
        w->size  = s_bios_bytes.size();
        w->base  = 0u;
        w->owned.reset();
        return true;
    }
    if (pc >= 0x08000000u) {
        gba::GbaBus* bus = active_bus();
        if (!bus || !bus->rom_ptr() || bus->rom_size() == 0) return false;
        const uint64_t rom_end = 0x08000000ull + bus->rom_size();
        if (pc >= rom_end) return false;  // past the actual cart image
        w->bytes = bus->rom_ptr();
        w->size  = bus->rom_size();
        w->base  = 0x08000000u;
        w->owned.reset();
        return true;
    }

    gba::GbaBus* bus = active_bus();
    if (!bus) return false;

    const uint8_t* region_ptr = nullptr;
    uint32_t region_base = 0, region_end = 0;
    if (pc >= 0x03000000u && pc < 0x03008000u) {
        region_ptr  = bus->iwram_ptr();
        region_base = 0x03000000u;
        region_end  = 0x03008000u;
    } else if (pc >= 0x02000000u && pc < 0x02040000u) {
        region_ptr  = bus->ewram_ptr();
        region_base = 0x02000000u;
        region_end  = 0x02040000u;
    } else {
        // Not BIOS, ROM, IWRAM, or EWRAM — no code image to heal from.
        return false;
    }

    const std::size_t avail = static_cast<std::size_t>(region_end - pc);
    const std::size_t take  = std::min(avail, kMaxRamSnapshotBytes);
    const uint8_t* src = region_ptr + (pc - region_base);
    auto snapshot = std::make_shared<std::vector<uint8_t>>(src, src + take);

    w->owned = snapshot;
    w->bytes = w->owned->data();
    w->size  = w->owned->size();
    w->base  = pc;  // snapshot byte 0 == pc, so pc - base == 0 exactly
    return true;
}

// ── DLL callback table ─────────────────────────────────────────────
// runtime_should_yield returns bool (C++); the ABI field is int. Adapt rather
// than reinterpret_cast a differing return type.
int ovl_should_yield(void) { return runtime_should_yield() ? 1 : 0; }

void fill_callbacks() {
    g_callbacks.abi_version = GBA_OVERLAY_ABI_VERSION;

    g_callbacks.cpu                = &g_cpu;
    g_callbacks.runtime_insn_trace = &g_runtime_insn_trace;
    g_callbacks.runtime_cycles     = &g_runtime_cycles;
    g_callbacks.runtime_break_pc   = &g_runtime_break_pc;
    g_callbacks.runtime_fn_entry_hook = &g_runtime_fn_entry_hook;
    g_callbacks.runtime_ram_code_guard = &runtime_ram_code_guard;
    g_callbacks.runtime_mutable_ram_code_miss =
        &runtime_mutable_ram_code_miss;

    g_callbacks.bus_read_u32  = bus_read_u32;
    g_callbacks.bus_read_u16  = bus_read_u16;
    g_callbacks.bus_read_u8   = bus_read_u8;
    g_callbacks.bus_write_u32 = bus_write_u32;
    g_callbacks.bus_write_u16 = bus_write_u16;
    g_callbacks.bus_write_u8  = bus_write_u8;

    g_callbacks.arm_cond_passes   = arm_cond_passes;
    g_callbacks.arm_shift_lsl     = arm_shift_lsl;
    g_callbacks.arm_shift_lsr     = arm_shift_lsr;
    g_callbacks.arm_shift_asr     = arm_shift_asr;
    g_callbacks.arm_shift_ror     = arm_shift_ror;
    g_callbacks.arm_set_nz        = arm_set_nz;
    g_callbacks.arm_set_nzc_logic = arm_set_nzc_logic;
    g_callbacks.arm_set_nzcv_add  = arm_set_nzcv_add;
    g_callbacks.arm_set_nzcv_adc  = arm_set_nzcv_adc;
    g_callbacks.arm_set_nzcv_sub  = arm_set_nzcv_sub;
    g_callbacks.arm_set_nzcv_sbc  = arm_set_nzcv_sbc;

    g_callbacks.runtime_dispatch               = runtime_dispatch;
    g_callbacks.runtime_dispatch_with_exchange = runtime_dispatch_with_exchange;
    g_callbacks.runtime_call_push_return       = runtime_call_push_return;
    g_callbacks.runtime_call_should_return     = runtime_call_should_return;
    g_callbacks.runtime_call_cancel_return     = runtime_call_cancel_return;

    g_callbacks.runtime_tick       = runtime_tick;
    g_callbacks.runtime_should_yield = ovl_should_yield;
    g_callbacks.runtime_mem_cycles = runtime_mem_cycles;
    g_callbacks.runtime_mul_cycles = runtime_mul_cycles;
    g_callbacks.runtime_idle_backedge = runtime_idle_backedge;
    g_callbacks.runtime_image_base = &g_runtime_image_base;

    g_callbacks.runtime_swi                  = runtime_swi;
    g_callbacks.runtime_irq                  = runtime_irq;
    g_callbacks.runtime_mrs_cpsr             = runtime_mrs_cpsr;
    g_callbacks.runtime_mrs_spsr             = runtime_mrs_spsr;
    g_callbacks.runtime_msr_cpsr             = runtime_msr_cpsr;
    g_callbacks.runtime_msr_spsr             = runtime_msr_spsr;
    g_callbacks.runtime_read_user_reg        = runtime_read_user_reg;
    g_callbacks.runtime_write_user_reg       = runtime_write_user_reg;
    g_callbacks.runtime_exception_return     = runtime_exception_return;
    g_callbacks.runtime_restore_cpsr_from_spsr = runtime_restore_cpsr_from_spsr;

    g_callbacks.runtime_insn_fp          = runtime_insn_fp;
    g_callbacks.runtime_trace_event      = runtime_trace_event;
    g_callbacks.runtime_unimplemented_op = runtime_unimplemented_op;
}

// ── Worker thread ──────────────────────────────────────────────────
// The game thread NEVER compiles. Every miss hands the immutable region to the
// worker, which emits the overlay C and runs the resolved compiler (gcc or
// tcc) to a cached DLL; the game thread installs it into g_healed at a frame
// boundary (overlay_drain_ready). Keeps the 60 fps loop + audio stall-free.
void worker_main() {
    for (;;) {
        QueuedWork queued;
        {
            std::unique_lock<std::mutex> lk(s_work_mtx);
            s_work_cv.wait(lk, [] { return s_stop.load() || !s_work.empty(); });
            if (s_stop.load() && s_work.empty()) return;
            const bool fairness_due = s_hot_burst >= kHotQueueBurstLimit;
            const std::size_t index = hot_queue_pick_index(
                s_work, s_hot_burst, [](const QueuedWork& q) {
                    return s_hot_requested.count(q.request) != 0;
                });
            auto it = s_work.begin() + static_cast<std::ptrdiff_t>(index);
            queued = std::move(*it);
            s_work.erase(it);
            s_queued.erase(queued.request);
            const bool was_hot = s_hot_requested.erase(queued.request) != 0;
            if (was_hot && index != 0) {
                s_hot_selected.fetch_add(1, std::memory_order_relaxed);
            } else if (fairness_due) {
                s_fairness_pops.fetch_add(1, std::memory_order_relaxed);
            }
        }
        OverlayWorkItem& w = queued.item;

        ReadyEntry r;
        r.key = heal_key(w.pc, w.thumb);
        r.has_request = true;
        // Reuse the identity computed on the game thread at enqueue time
        // (queued.request) rather than recomputing request_identity(w) here:
        // request_identity now reads s_ram_extent_seen, which is
        // game-thread-only (see its declaration) and would race if touched
        // from this worker thread. The two are identical by construction —
        // queued.request was built from this same w via request_identity in
        // overlay_request_compile before w was moved into QueuedWork.
        r.request = queued.request;
        std::string err;
        r.ok = overlay_compile_one(w, s_active_cache_dir, &g_callbacks,
                                   /*compile_if_missing=*/true, s_backend,
                                   &r.c, &err);
        if (r.ok) {
            std::fprintf(stderr,
                "self_heal: HEALED 0x%08X (%s) -> native via %s (crc=%08X, "
                "[0x%08X,0x%08X)); the interpreter bridge stops for this PC.\n",
                w.pc, w.thumb ? "thumb" : "arm", heal_backend_name(s_backend),
                r.c.crc, w.pc, r.c.end);
        } else {
            std::fprintf(stderr,
                "self_heal: compile FAILED for 0x%08X (%s): %s — staying on the "
                "interpreter bridge this session.\n",
                w.pc, w.thumb ? "thumb" : "arm", err.c_str());
        }

        {
            std::lock_guard<std::mutex> lk(s_ready_mtx);
            s_ready.push_back(r);
            s_ready_pending.fetch_add(1, std::memory_order_release);
        }
        s_ready_cv.notify_all();
    }
}

// ── Background warm-loader (init-time cache reload, off the game thread) ──
// Runs on its own thread (s_warm_thread), concurrently with BIOS boot and the
// first frames, so it never delays the frame loop starting. Two independent
// handling paths, split by is_ram_addr(pc):
//
//   ROM/BIOS-backed (immutable): the SAME algorithm as the old synchronous
//   warm load, just moved off the game thread. The cart ROM / BIOS snapshot
//   never change for the run, so recomputing the extent + CRC32 against LIVE
//   bytes from this background thread is exactly as safe as doing it on the
//   game thread (no concurrent writer exists for that memory). This is the
//   ONLY verification a ROM/BIOS entry ever gets — overlay_try_dispatch's
//   ROM/BIOS path is zero-overhead and never re-checks at call time — so it
//   must not be skipped or weakened.
//
//   RAM-backed (mutable): re-deriving against LIVE bytes at process start is
//   both pointless (IWRAM/EWRAM holds whatever pre-boot state is sitting
//   there, essentially never one of the cached variants — that's WHY the old
//   code warm-loaded almost nothing) and unsafe to attempt from a
//   non-game thread (the game thread writes IWRAM/EWRAM with no lock on that
//   path, by design — it's the dispatch hot path). Instead: trust the cache
//   filename's pc/thumb/crc (literally how THIS SAME compiler named the file
//   — not a guess) and the persisted ".c" sidecar's header comment for `end`
//   (also literally what THIS SAME compiler discovered when it produced that
//   file — not a guess, not re-derived from possibly-different live bytes).
//   No GBA memory is read for this path at all. This does not weaken safety:
//   unlike ROM/BIOS, overlay_try_dispatch's RAM path ALREADY re-verifies
//   every candidate against LIVE bytes with the same unconditional CRC32
//   check on EVERY dispatch attempt before ever calling its native fn
//   (heal_entry_crc_ok) — installing a not-yet-live-verified RAM entry here
//   is exactly as safe as a resident alternate that was installed from a
//   PAST live match and is now being reconsidered against NEW live bytes;
//   the entry-time check is what actually gates execution, always.
//
// Both paths publish through the SAME s_ready queue the compile worker uses
// — neither ever touches g_healed directly — so overlay_drain_ready (called
// once per frame on the game thread) remains the single writer to g_healed
// and the dispatch hot path stays lock-free.

// Parse "<pc:08X>_<crc:08X>_<a|t>.dll" (23 chars). false for anything that
// doesn't match the exact shape written by overlay_compile_one.
bool parse_cache_filename(const std::string& fn, uint32_t* pc, uint32_t* crc,
                          bool* thumb) {
#ifdef _WIN32
    if (fn.size() != 23 || fn.compare(19, 4, ".dll") != 0) return false;
#else
    // Linux writes "<pc>_<crc>_<mode>.so" (22 chars).
    if (fn.size() != 22 || fn.compare(19, 3, ".so") != 0) return false;
#endif
    if (fn[8] != '_' || fn[17] != '_') return false;
    const char mode = fn[18];
    if (mode != 'a' && mode != 't') return false;
    char* endp = nullptr;
    *pc = static_cast<uint32_t>(std::strtoul(fn.substr(0, 8).c_str(), &endp, 16));
    if (!endp || *endp != '\0') return false;
    *crc = static_cast<uint32_t>(std::strtoul(fn.substr(9, 8).c_str(), &endp, 16));
    if (!endp || *endp != '\0') return false;
    *thumb = (mode == 't');
    return true;
}

// Read the ground-truth `end` out of the ".c" sidecar overlay_emit.cpp wrote
// next to the DLL at ORIGINAL compile time: "// function 0xPPPPPPPP
// mode=(thumb|arm) end=0xEEEEEEEE". Cross-checks pc/thumb against the
// filename's own record (both were written by that same compile) and refuses
// — no guess — on any parse failure, mismatch, or missing sidecar.
bool read_end_from_sidecar(const fs::path& c_path, uint32_t want_pc,
                           bool want_thumb, uint32_t* out_end) {
    std::FILE* f = std::fopen(c_path.string().c_str(), "rb");
    if (!f) return false;
    char buf[256];
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    const char* p = std::strstr(buf, "// function 0x");
    if (!p) return false;
    unsigned parsed_pc = 0, parsed_end = 0;
    char mode[8] = {};
    if (std::sscanf(p, "// function 0x%x mode=%7s end=0x%x",
                    &parsed_pc, mode, &parsed_end) != 3)
        return false;
    if (parsed_pc != want_pc) return false;
    const bool thumb = (std::strcmp(mode, "thumb") == 0);
    if (thumb != want_thumb) return false;
    *out_end = parsed_end;
    return true;
}

// ROM/BIOS half. `seen` dedups across the gcc and tcc namespaces so the
// higher-priority one (scanned first) wins — consumption is producer-blind.
int warm_scan_rom_bios(const std::string& dir, HealBackend backend,
                       std::unordered_set<uint64_t>& seen) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    int queued = 0;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec || s_stop.load(std::memory_order_relaxed)) break;
        if (!de.is_regular_file()) continue;
        uint32_t pc = 0, crc_hint = 0;
        bool thumb = false;
        if (!parse_cache_filename(de.path().filename().string(), &pc, &crc_hint, &thumb))
            continue;
        if (is_ram_addr(pc)) continue;  // RAM keys: handled by warm_scan_ram
        const uint64_t key = heal_key(pc, thumb);
        if (!seen.insert(key).second) continue;  // already covered (gcc wins)

        OverlayWorkItem w;
        w.pc = pc;
        w.thumb = thumb;
        if (!region_bytes(pc, &w)) continue;

        ReadyEntry r;
        r.key = key;
        std::string err;
        r.ok = overlay_compile_one(w, dir, &g_callbacks,
                                   /*compile_if_missing=*/false, backend, &r.c, &err);
        if (!r.ok) continue;  // stale/mismatched on disk — heals normally at runtime
        {
            std::lock_guard<std::mutex> lk(s_ready_mtx);
            s_ready.push_back(r);
            s_ready_pending.fetch_add(1, std::memory_order_release);
        }
        ++queued;
    }
    return queued;
}

struct RamFileCandidate {
    uint32_t pc = 0, crc = 0;
    bool thumb = false;
    fs::path dll, cfile, picfile;
    fs::file_time_type mtime{};
};

// Filesystem-only scan (no GBA memory touched): every "<pc>_<crc>_<mode>.dll"
// under `dir` becomes one candidate, keyed by (pc,thumb,crc) so the SAME
// variant seen in both the gcc and tcc namespaces is only considered once
// (gcc scanned first == gcc wins, same priority as everywhere else in this
// file).
void collect_ram_candidates(
    const std::string& dir,
    std::unordered_map<uint64_t, std::vector<RamFileCandidate>>& by_key,
    std::unordered_set<uint64_t>& seen_variant) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec || s_stop.load(std::memory_order_relaxed)) break;
        if (!de.is_regular_file()) continue;
        uint32_t pc = 0, crc = 0;
        bool thumb = false;
        if (!parse_cache_filename(de.path().filename().string(), &pc, &crc, &thumb))
            continue;
        if (!is_ram_addr(pc)) continue;
        const uint64_t key = heal_key(pc, thumb);
        const uint64_t variant_id =
            key ^ (static_cast<uint64_t>(crc) * 0x9E3779B97F4A7C15ull);
        if (!seen_variant.insert(variant_id).second) continue;

        RamFileCandidate c;
        c.pc = pc; c.crc = crc; c.thumb = thumb;
        c.dll = de.path();
        c.cfile = de.path();
        c.cfile.replace_extension(".c");
        c.picfile = de.path();
        c.picfile.replace_extension(".pic");
        std::error_code mec;
        c.mtime = fs::last_write_time(de.path(), mec);
        by_key[key].push_back(std::move(c));
    }
}

// RAM half: pure-disk metadata only (filename + sidecar comment). Caps each
// key at kMaxVariantsPerKey, preferring the most-recently-modified files on
// disk when more exist. Publishes through s_ready like everything else here.
int warm_scan_ram() {
    std::unordered_map<uint64_t, std::vector<RamFileCandidate>> by_key;
    std::unordered_set<uint64_t> seen_variant;
    collect_ram_candidates(cache_dir_for(HealBackend::Gcc), by_key, seen_variant);
    collect_ram_candidates(cache_dir_for(HealBackend::Tcc), by_key, seen_variant);

    int queued = 0;
    for (auto& kv : by_key) {
        if (s_stop.load(std::memory_order_relaxed)) break;
        std::vector<RamFileCandidate>& cands = kv.second;
        // Oldest first, newest last: pushed to s_ready in this order so the
        // game thread's heal_slot_promote (LRU-ticked at drain time) makes
        // the most-recently-modified-on-disk file the final incumbent.
        std::sort(cands.begin(), cands.end(),
                 [](const RamFileCandidate& a, const RamFileCandidate& b) {
                     return a.mtime < b.mtime;
                 });
        if (cands.size() > kMaxVariantsPerKey) {
            cands.erase(cands.begin(),
                       cands.end() - static_cast<std::ptrdiff_t>(kMaxVariantsPerKey));
        }
        for (const RamFileCandidate& c : cands) {
            uint32_t end = 0;
            if (!read_end_from_sidecar(c.cfile, c.pc, c.thumb, &end)) continue;

            ReadyEntry r;
            r.key = kv.first;
            std::string err;
            r.ok = overlay_load_cached(c.dll.string(), c.pc, c.thumb, c.crc, end,
                                       &g_callbacks, &r.c, &err);
            if (!r.ok) continue;
            // Exact bytes are optional acceleration metadata. ABI/schema,
            // filename fields, extent, mode, CRC, and EOF are all checked;
            // malformed/missing metadata leaves the old address-keyed entry
            // intact but ineligible for cross-address binding.
            overlay_read_relocatable_metadata(
                c.picfile.string(), c.pc, c.thumb, c.crc, end,
                &r.c.code_bytes);
            {
                std::lock_guard<std::mutex> lk(s_ready_mtx);
                s_ready.push_back(r);
                s_ready_pending.fetch_add(1, std::memory_order_release);
            }
            ++queued;
        }
    }
    return queued;
}

// ── Targeted immutable-image warm lookup (WARM-A-01 defect 2) ─────
// warm_load_background() (below) can still be mid-scan when a dispatch miss
// for an immutable key it hasn't reached yet arrives at
// overlay_request_compile. ROM/BIOS keys are cheap to find: the scan only
// loads an exact matching image, normally one file. RAM misses use the normal
// worker path instead. A targeted RAM lookup would rebuild the index of every
// RAM DLL in both cache directories on each miss, blocking the game thread
// while the background scanner already does the same work.
//
// This lookup publishes through the SAME s_ready queue as everything else in
// this file, so the caller must still drive overlay_drain_ready() to install
// it — it never touches g_healed directly.

bool warm_load_rom_bios_key_now(uint32_t pc, bool want_thumb, uint64_t key) {
    bool any = false;
    for (HealBackend backend : {HealBackend::Gcc, HealBackend::Tcc}) {
        const std::string dir = cache_dir_for(backend);
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        for (const auto& de : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!de.is_regular_file()) continue;
            uint32_t fpc = 0, fcrc = 0;
            bool fthumb = false;
            if (!parse_cache_filename(de.path().filename().string(), &fpc,
                                      &fcrc, &fthumb))
                continue;
            if (fpc != pc || fthumb != want_thumb) continue;

            OverlayWorkItem w;
            w.pc = pc; w.thumb = want_thumb;
            if (!region_bytes(pc, &w)) continue;

            ReadyEntry r;
            r.key = key;
            std::string err;
            r.ok = overlay_compile_one(w, dir, &g_callbacks,
                                       /*compile_if_missing=*/false, backend,
                                       &r.c, &err);
            if (!r.ok) continue;  // stale/mismatched on disk — heals normally
            {
                std::lock_guard<std::mutex> lk(s_ready_mtx);
                s_ready.push_back(r);
                s_ready_pending.fetch_add(1, std::memory_order_release);
            }
            any = true;
        }
    }
    return any;
}

// Entry point for s_warm_thread. Runs concurrently with BIOS boot + the first
// frames; never blocks the game thread. Skips the RAM half entirely when RAM
// healing isn't enabled (those entries could never dispatch anyway — see
// overlay_try_dispatch's `!s_ram_heal_enabled` gate — so scanning for them,
// reading sidecars, and LoadLibrary-ing their DLLs would be pure waste).
void warm_load_background() {
    const auto t0 = std::chrono::steady_clock::now();
    std::unordered_set<uint64_t> seen;
    int rom_bios = 0;
    rom_bios += warm_scan_rom_bios(cache_dir_for(HealBackend::Gcc), HealBackend::Gcc, seen);
    rom_bios += warm_scan_rom_bios(cache_dir_for(HealBackend::Tcc), HealBackend::Tcc, seen);
    const int ram = s_ram_heal_enabled ? warm_scan_ram() : 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr,
        "self_heal: warm-load (background) done: rom_bios=%d ram=%d in %lldms\n",
        rom_bios, ram, static_cast<long long>(ms));
    std::fflush(stderr);
    // Published AFTER both scans (and their s_ready pushes) are queued, so
    // any game-thread miss that observes s_warm_load_done==true is
    // guaranteed the full on-disk cache is at least published to s_ready
    // (drained lazily via the normal per-frame overlay_drain_ready) — the
    // targeted lookup above becomes a pure no-op from that point on.
    s_warm_load_done.store(true, std::memory_order_release);
}

}  // namespace

void overlay_loader_init(const std::string& cache_root,
                         const std::string& image_sha1,
                         const gba::GbaBios* bios) {
    g_runtime_ram_pointer_write_probe = nullptr;
    g_runtime_ram_image_dma_probe = nullptr;
    g_runtime_ram_image_write_probe = nullptr;
    g_runtime_ram_image_boundary_probe = nullptr;
    g_runtime_ram_image_dispatch_probe = nullptr;
    // Acceptance/CI mode: prove that the linked static corpus is sufficient.
    // Do not even inspect the overlay cache, because a warm shard would hide a
    // missing generated dispatch entry before runtime_dispatch_miss can fail.
    const char* fe_env = std::getenv("GBARECOMP_FRAME_EVENTS");
    s_frame_events_enabled = fe_env && fe_env[0] != '\0';
    s_frame_events_path = s_frame_events_enabled ? fe_env : std::string();
    const char* churn_env = std::getenv("GSR_RAM_CHURN_PROBE");
    s_ram_churn_probe_enabled = churn_env && churn_env[0] != '\0' &&
        std::strcmp(churn_env, "0") != 0 &&
        std::strcmp(churn_env, "false") != 0 &&
        std::strcmp(churn_env, "off") != 0;
    s_ram_churn_path = s_ram_churn_probe_enabled
        ? (s_frame_events_path.empty()
            ? std::string("ram-churn.csv")
            : s_frame_events_path + ".ram-churn.csv")
        : std::string();
    s_ram_image_lifecycle_path = s_ram_churn_probe_enabled
        ? s_ram_churn_path + ".lifecycle.csv" : std::string();
    s_ram_churn_frames.clear();
    s_ram_churn_current_frame = 0;
    s_ram_churn_dropped_frames = 0;
    s_ram_churn_dropped_pcs = 0;
    s_ram_churn_dropped_rows = 0;
    s_ram_image_lifecycle_rows.clear();
    s_ram_image_generation = 0;
    s_ram_image_dma_row = kNoRamImageLifecycleRow;
    s_ram_image_writer_row = kNoRamImageLifecycleRow;
    s_ram_image_writer_call_depth = 0;
    s_ram_image_writer_started = false;
    s_ram_image_pending_row = kNoRamImageLifecycleRow;
    if (s_ram_churn_probe_enabled)
        g_runtime_ram_pointer_write_probe =
            &ram_churn_record_ram_pointer_write;
    if (s_ram_churn_probe_enabled) {
        g_runtime_ram_image_dma_probe = &ram_image_lifecycle_dma;
        g_runtime_ram_image_write_probe = &ram_image_lifecycle_write;
        g_runtime_ram_image_boundary_probe = &ram_image_lifecycle_boundary;
        g_runtime_ram_image_dispatch_probe = &ram_image_lifecycle_dispatch;
    }
    const char* strict_env = std::getenv("GBARECOMP_STRICT_STATIC");
    const bool strict_static =
        strict_env && strict_env[0] != '\0' && strict_env[0] != '0';
    if (strict_static) {
        s_active = false;
        std::printf("strict_static=ENABLED self_heal_recompile=DISABLED "
                    "cache_load=DISABLED interpreter_bridge=ABORT\n");
        return;
    }

    const char* ram_env = std::getenv("GBARECOMP_SELFHEAL_RAM");
    s_ram_heal_enabled = ram_env &&
        (std::strcmp(ram_env, "1") == 0 || std::strcmp(ram_env, "true") == 0 ||
         std::strcmp(ram_env, "on") == 0);
    std::printf("self_heal_ram=%s (mutable RAM native overlays are %s)\n",
                s_ram_heal_enabled ? "ENABLED" : "DISABLED",
                s_ram_heal_enabled ? "opt-in" : "interpreter-only");

    // Self-improving native healing is ON by default so released games heal
    // interpreter misses to native AND persist them across launches (the cached
    // DLLs warm-load next session). Opt out for pure-interpreter runs (oracle
    // diff / cycle-accurate trace) with GBARECOMP_SELFHEAL_RECOMPILE=0.
    const char* sh_env = std::getenv("GBARECOMP_SELFHEAL_RECOMPILE");
    const bool sh_off = sh_env && (std::strcmp(sh_env, "0") == 0 ||
                                   std::strcmp(sh_env, "false") == 0 ||
                                   std::strcmp(sh_env, "off") == 0);
    if (sh_off) {
        s_active = false;
        std::printf("self_heal_recompile=DISABLED "
                    "(GBARECOMP_SELFHEAL_RECOMPILE=0 — pure Stage-1 interpreter "
                    "bridge this session)\n");
        return;
    }

    const char* root_env = std::getenv("GBARECOMP_HEAL_CACHE");
    const std::string root =
        (root_env && root_env[0]) ? root_env
                                  : (cache_root.empty() ? "recomp_cache"
                                                        : cache_root);
    s_cache_base = (fs::path(root) / image_sha1).string();

    // Resolve the producer (env > auto) and create its namespaced cache dir.
    s_backend = resolve_backend();
    s_active_cache_dir = cache_dir_for(s_backend);
    std::error_code ec;
    fs::create_directories(s_active_cache_dir, ec);

    // Snapshot the 16 KB BIOS as the immutable code image for BIOS heals.
    s_bios_bytes.clear();
    if (bios && bios->loaded()) {
        s_bios_bytes.resize(gba::GbaBios::kSize);
        for (std::size_t i = 0; i < gba::GbaBios::kSize; ++i) {
            s_bios_bytes[i] = bios->read8(static_cast<uint32_t>(i));
        }
    }

    fill_callbacks();
    s_active = true;
    s_ever_active = true;  // sticky: survives shutdown for the exit report

    // Warm-loading the on-disk cache used to run synchronously right here,
    // before the frame loop started — a real LoadLibrary + CRC check per
    // cached file, scaling with total accumulated cache size (measured
    // ~1.7s of pure startup cost against a 372-file/13MB cache). It now runs
    // on its own background thread, concurrently with BIOS boot and the
    // first frames: overlay_try_dispatch simply misses (today's already-
    // correct path) for any key not yet warm-loaded, and the frame loop
    // never waits on it.
    s_stop.store(false);
    s_shutdown_discarded_work.store(0, std::memory_order_release);
    s_warm_load_done.store(false, std::memory_order_release);
    s_worker = std::thread(worker_main);
    s_warm_thread = std::thread(warm_load_background);

    std::printf("self_heal_recompile=ENABLED backend=%s cache=\"%s\" "
                "warm_load=background (completion logged to stderr)\n",
                heal_backend_name(s_backend), s_active_cache_dir.c_str());
}

void overlay_loader_shutdown() {
    if (s_worker.joinable() || s_warm_thread.joinable()) {
        // Do not make clean exit wait for every cold miss accumulated during
        // the session.  Work already removed by worker_main is allowed to
        // finish (overlay_compile_one uses temp files + atomic rename), but
        // queued work is no longer useful once the game is exiting.  Clear it
        // while holding the same mutex the worker uses, so no new queued item
        // can race with the discard.  This preserves cache correctness and
        // keeps the worker join bounded by at most one active compile.
        std::size_t discarded = 0;
        {
            std::lock_guard<std::mutex> lk(s_work_mtx);
            s_stop.store(true, std::memory_order_release);
            discarded = s_work.size();
            s_work.clear();
            s_queued.clear();
            s_hot_requested.clear();
            s_hot_burst = 0;
        }
        s_shutdown_discarded_work.store(
            static_cast<uint64_t>(discarded), std::memory_order_release);
        if (discarded) {
            std::fprintf(stderr,
                "self_heal: shutdown discarded %zu queued compile request(s); "
                "active compile(s) still joined\n", discarded);
        }
        s_work_cv.notify_all();
        if (s_worker.joinable()) s_worker.join();
        if (s_warm_thread.joinable()) s_warm_thread.join();
    }
    // DLLs are left loaded (immutable code, process-lifetime); the OS reclaims
    // them at exit. Drain any last results so counters/banner are accurate.
    overlay_drain_ready();
    dump_frame_events();
    dump_ram_churn();
    dump_ram_image_lifecycle();
    g_runtime_ram_pointer_write_probe = nullptr;
    g_runtime_ram_image_dma_probe = nullptr;
    g_runtime_ram_image_write_probe = nullptr;
    g_runtime_ram_image_boundary_probe = nullptr;
    g_runtime_ram_image_dispatch_probe = nullptr;
    if (s_relocatable_alias_hits || s_relocatable_alias_misses) {
        std::fprintf(stderr,
            "self_heal: relocatable_alias hits=%llu misses=%llu\n",
            static_cast<unsigned long long>(s_relocatable_alias_hits),
            static_cast<unsigned long long>(s_relocatable_alias_misses));
    }
    const uint64_t hot_requests = s_hot_requests.load(std::memory_order_relaxed);
    if (hot_requests) {
        std::fprintf(stderr,
            "self_heal: hot_queue requests=%llu promoted=%llu fairness=%llu\n",
            static_cast<unsigned long long>(hot_requests),
            static_cast<unsigned long long>(
                s_hot_selected.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(
                s_fairness_pops.load(std::memory_order_relaxed)));
    }
    s_active = false;
}

// ── Test-only state reset ────────────────────────────────────────────────
// Clears every in-memory heal structure (g_healed, inflight/failed sets,
// counters, the LRU tick, diagnostic rings, and any queued-but-undrained
// ready entries)
// WITHOUT touching the on-disk cache. Lets a single test process simulate a
// fresh session's warm-load against a cache directory populated by an
// earlier overlay_loader_init/shutdown cycle in the SAME process — exactly
// what a real second process launch would see, without actually spawning
// one. Call only between overlay_loader_shutdown() and the next
// overlay_loader_init(). Never used by production code.
void overlay_loader_reset_state_for_test() {
    g_runtime_ram_pointer_write_probe = nullptr;
    g_runtime_ram_image_dma_probe = nullptr;
    g_runtime_ram_image_write_probe = nullptr;
    g_runtime_ram_image_boundary_probe = nullptr;
    g_runtime_ram_image_dispatch_probe = nullptr;
    g_healed.clear();
    s_inflight.clear();
    s_failed.clear();
    s_ram_extent_seen.clear();
    s_native_calls_total = 0;
    s_ram_revalidations = 0;
    s_ram_crc_mismatches = 0;
    s_relocatable_alias_hits = 0;
    s_relocatable_alias_misses = 0;
    // The test reset models a fresh loader session in the same process. Keep
    // diagnostic sidecars session-local too; otherwise frame-events from
    // earlier cycles outlive the RAM-churn ring and the two CSVs disagree.
    s_frame_events.clear();
    s_bridge_miss_events.clear();
    s_ram_churn_frames.clear();
    s_ram_churn_current_frame = 0;
    s_ram_churn_dropped_frames = 0;
    s_ram_churn_dropped_pcs = 0;
    s_ram_churn_dropped_rows = 0;
    s_ram_image_lifecycle_rows.clear();
    s_ram_image_generation = 0;
    s_ram_image_dma_row = kNoRamImageLifecycleRow;
    s_ram_image_writer_row = kNoRamImageLifecycleRow;
    s_ram_image_writer_call_depth = 0;
    s_ram_image_writer_started = false;
    s_ram_image_pending_row = kNoRamImageLifecycleRow;
    s_lru_tick = 0;
    {
        std::lock_guard<std::mutex> lk(s_work_mtx);
        s_work.clear();
        s_queued.clear();
        s_hot_requested.clear();
        s_hot_burst = 0;
    }
    s_hot_requests.store(0, std::memory_order_relaxed);
    s_hot_selected.store(0, std::memory_order_relaxed);
    s_fairness_pops.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(s_ready_mtx);
        s_ready.clear();
    }
    s_ready_pending.store(0, std::memory_order_release);
    s_shutdown_discarded_work.store(0, std::memory_order_release);
}

uint64_t overlay_shutdown_discarded_work_for_test() {
    return s_shutdown_discarded_work.load(std::memory_order_acquire);
}

bool overlay_mark_current_request_failed_for_test(uint32_t pc, bool thumb) {
    OverlayWorkItem w;
    w.pc = pc & ~1u;
    w.thumb = thumb;
    if (!region_bytes(w.pc, &w)) return false;
    s_failed.insert(request_identity(w));
    return true;
}

bool overlay_ready_pending_for_test() {
    return s_ready_pending.load(std::memory_order_acquire) != 0;
}

bool overlay_ready_contains_for_test(uint32_t pc, bool thumb) {
    std::lock_guard<std::mutex> lk(s_ready_mtx);
    pc &= ~1u;
    for (const ReadyEntry& r : s_ready) {
        if (r.ok && r.c.pc == pc && r.c.thumb == thumb) return true;
    }
    return false;
}

bool try_bind_completed_relocatable(const OverlayWorkItem& w, uint64_t key) {
    if (!is_ram_addr(w.pc) || !w.bytes || !w.size) return false;
    const std::size_t off = static_cast<std::size_t>(w.pc - w.base);
    if (off > w.size) {
        ++s_relocatable_alias_misses;
        return false;
    }
    const uint8_t* live = w.bytes + off;
    HealedEntry found;
    std::size_t found_len = 0;
    auto select = [&](const HealedEntry& candidate) {
        if (!candidate.relocatable || !candidate.ram_backed ||
            candidate.thumb != w.thumb || !candidate.code_bytes)
            return false;
        const std::size_t len = candidate.code_bytes->size();
        if (!len || len <= found_len || len > w.size - off) return false;
        if (candidate.crc != gba::crc32(live, len) ||
            std::memcmp(candidate.code_bytes->data(), live, len) != 0)
            return false;
        found = candidate;
        found_len = len;
        return true;
    };
    for (const auto& kv : g_healed) {
        select(kv.second.incumbent);
        for (const HealedEntry& alt : kv.second.alternates) select(alt);
    }
    if (found_len) {
        // Several proven bodies may share a prefix. Prefer the longest exact
        // match, whose retained bytes cover the largest already-verified CFG.
        // No finder, emitter, or compiler work is allowed on this game thread.
        found.addr = w.pc;
        found.end = w.pc + static_cast<uint32_t>(found_len);
        found.native_calls = 0;
        heal_slot_promote(g_healed[key], found);
        ++s_relocatable_alias_hits;
        return true;
    }
    ++s_relocatable_alias_misses;
    return false;
}

OverlayRequestOutcome overlay_request_compile(uint32_t pc, bool thumb) {
    if (!s_active) return OverlayRequestOutcome::Failed;
    pc &= ~1u;
    if (!s_ram_heal_enabled && is_ram_addr(pc)) return OverlayRequestOutcome::Failed;
    const uint64_t key = heal_key(pc, thumb);
    // ROM/BIOS-backed content is immutable for the run, so a key already
    // present in g_healed never needs re-requesting. A RAM-backed key CAN
    // legitimately already be present (a resident incumbent + bounded
    // alternates set — see HealSlot) and still need a fresh compile here:
    // overlay_try_dispatch/runtime_mutable_ram_code_miss only reach this
    // call after none of those resident variants matched the CURRENT live
    // bytes, i.e. this is genuinely new content for the key. The existing
    // resident variants are left untouched — heal_slot_promote (called from
    // overlay_drain_ready once this compile lands) folds the new content in
    // as the incumbent without discarding any of them.
    if (!is_ram_addr(pc) && g_healed.count(key)) return OverlayRequestOutcome::Alias;

    // Avoid a game-thread RAM cache scan while the background warm loader is
    // still running. The old targeted lookup rebuilt an index of every RAM
    // cache file in both backend directories for each RAM miss. The worker's
    // normal path computes this snapshot's exact cache key and reuses its DLL
    // if present; the background scan still loads its bounded set of cached
    // variants. Keep the immutable-image lookup for this rare path.
    if (!is_ram_addr(pc) &&
        !s_warm_load_done.load(std::memory_order_acquire)) {
        if (warm_load_rom_bios_key_now(pc, thumb, key)) {
            overlay_drain_ready();
            if (g_healed.count(key)) return OverlayRequestOutcome::Alias;
        }
    }

    // Resolves BIOS/ROM zero-copy, or takes a bounded synchronous snapshot of
    // IWRAM/EWRAM RIGHT HERE on the game thread (never on the worker) so the
    // async compile never reads bytes the game could be mutating concurrently.
    OverlayWorkItem w;
    w.pc = pc; w.thumb = thumb;
    if (!region_bytes(pc, &w)) {
        // No code image at all (neither BIOS, ROM, IWRAM, nor EWRAM). Don't
        // retry.
        RequestIdentity bad;
        bad.base_key = key;
        s_failed.insert(bad);
        return OverlayRequestOutcome::Failed;
    }
    const RequestIdentity request = request_identity(w);
    const auto inflight = s_inflight.find(request);
    if (inflight != s_inflight.end()) {
        // Repeated execution makes this exact immutable RAM snapshot more
        // valuable than cold one-shot variants. Mark it for bounded worker-
        // side promotion; the game thread still never compiles or waits.
        if (is_ram_addr(pc)) {
            std::lock_guard<std::mutex> lk(s_work_mtx);
            if (s_queued.count(*inflight) &&
                s_hot_requested.insert(*inflight).second) {
                s_hot_requests.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return OverlayRequestOutcome::Inflight;
    }
    if (s_failed.count(request)) return OverlayRequestOutcome::Failed;

    // A completed PIC RAM body may already exist at another guest address.
    // Bind only after same-mode, candidate-length, byte-for-byte proof.
    if (try_bind_completed_relocatable(w, key)) return OverlayRequestOutcome::Alias;

    // Production rule: the game thread NEVER compiles. Hand the region to the
    // worker thread; the game thread keeps interpreting this PC until the shard
    // is installed at a frame boundary (overlay_drain_ready). First hit
    // enqueues — it does not compile.
    if (s_frame_events_enabled) ++frame_event_bucket().compile_enqueued;
    ram_churn_record(s_ram_churn_current_frame, "compile_enqueued", pc,
                     thumb,
                     is_ram_addr(pc) ? "ram-snapshot" : "immutable-image",
                     "worker");
    s_inflight.insert(request);
    {
        std::lock_guard<std::mutex> lk(s_work_mtx);
        s_queued.insert(request);
        s_work.push_back(QueuedWork{std::move(w), request});
    }
    s_work_cv.notify_one();
    return OverlayRequestOutcome::Queued;
}

void overlay_drain_ready() {
    const uint32_t pending_before = static_cast<uint32_t>(std::max(
        0, s_ready_pending.load(std::memory_order_acquire)));
    if (pending_before == 0) return;
    const auto drain_t0 = s_frame_events_enabled
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    std::deque<ReadyEntry> local;
    {
        std::lock_guard<std::mutex> lk(s_ready_mtx);
        local.swap(s_ready);
        s_ready_pending.store(0, std::memory_order_release);
    }
    for (const ReadyEntry& r : local) {
        if (r.has_request) s_inflight.erase(r.request);
        if (r.ok) {
            // Game-thread install: publish the worker-produced code into the
            // dispatch map. This validates + installs only — it never compiles.
            // The new content becomes the incumbent (it's the live content
            // that just missed); any previous incumbent for this key is
            // preserved as an alternate, not discarded.
            HealedEntry h;
            h.addr = r.c.pc; h.thumb = r.c.thumb; h.fn = r.c.fn;
            h.module = r.c.module; h.crc = r.c.crc; h.end = r.c.end;
            h.ram_backed = is_ram_addr(r.c.pc);
            h.relocatable = r.c.relocatable;
            h.code_bytes = r.c.code_bytes;
            if (h.ram_backed) {
                // One line per completed RAM-code compile (not per frame,
                // not per dispatch — compiles are rare). Always on, never
                // gated behind crash tracing/GBARECOMP_TRACE: the whole
                // point is to have this data from a long normal-speed play
                // session without needing the slow tracing mode. "reason" is
                // read-only, computed from state already in scope here
                // (no new cross-thread state, no behaviour change): no prior
                // HealSlot for this key at all ("no-variant"), a slot whose
                // alternates set is already at kMaxVariantsPerKey and would
                // evict on this promote ("cap-full"), or a slot with room
                // whose resident variants just didn't match live bytes
                // ("content-miss").
                static uint64_t s_ram_compile_seq = 0;
                ++s_ram_compile_seq;
                const auto existing = g_healed.find(r.key);
                const char* reason = "content-miss";
                if (existing == g_healed.end()) {
                    reason = "no-variant";
                } else if (existing->second.incumbent.fn &&
                           existing->second.alternates.size() >=
                               kMaxVariantsPerKey - 1) {
                    reason = "cap-full";
                }
                std::fprintf(stderr,
                             "[ram-compile] seq=%llu pc=0x%08X crc=0x%08X "
                             "addr=0x%08X end=0x%08X len=%u reason=%s\n",
                             static_cast<unsigned long long>(s_ram_compile_seq),
                             h.addr, h.crc, h.addr, h.end, h.end - h.addr,
                             reason);
                // Record the real extent this compile discovered so future
                // request_identity() calls for this key can narrow their
                // dedup comparison window — never below any extent we've
                // actually seen (monotonic max, never shrinks).
                uint32_t& seen = s_ram_extent_seen[r.key];
                const uint32_t len = h.end - h.addr;
                if (len > seen) seen = len;
            }
            heal_slot_promote(g_healed[r.key], h);
            if (s_frame_events_enabled) ++frame_event_bucket().heal_installed;
            ram_churn_record(
                s_ram_churn_current_frame, "heal_install", h.addr, h.thumb,
                h.ram_backed ? "ram-crc" : "immutable-image",
                h.relocatable ? "relocatable-overlay" : "fixed-overlay");
        } else {
            if (r.has_request) s_failed.insert(r.request);
        }
    }
    if (s_frame_events_enabled) {
        const auto drain_t1 = std::chrono::steady_clock::now();
        FrameEventCounts& c = frame_event_bucket();
        ++c.ready_drain_calls;
        const uint64_t entries = static_cast<uint64_t>(c.ready_drain_entries) +
            static_cast<uint64_t>(local.size());
        c.ready_drain_entries = entries > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(entries);
        c.ready_drain_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                drain_t1 - drain_t0).count());
        c.ready_pending_after_drain = static_cast<uint32_t>(std::max(
            0, s_ready_pending.load(std::memory_order_acquire)));
    }
}

void (*overlay_wait_resolve(uint32_t pc, bool thumb,
                            uint32_t timeout_ms))(void) {
    // The game thread blocking on the compile worker: host profiler phase.
    gbarecomp::HostProfPhaseScope _hp_wait(gbarecomp::kHpCompileWait);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    for (;;) {
        overlay_drain_ready();
        if (RuntimeGuestFn fn = overlay_resolve(pc, thumb ? 1 : 0)) return fn;
        if (std::chrono::steady_clock::now() >= deadline) return nullptr;
        std::unique_lock<std::mutex> lk(s_ready_mtx);
        s_ready_cv.wait_until(lk, deadline, [] {
            return s_ready_pending.load(std::memory_order_acquire) > 0;
        });
    }
}

bool overlay_query(uint32_t pc, bool thumb, uint64_t* native_calls) {
    auto it = g_healed.find(heal_key(pc, thumb));
    if (it == g_healed.end()) return false;
    HealSlot& slot = it->second;
    if (!slot.incumbent.ram_backed) {
        // Immutable ROM/BIOS content: presence alone is the answer, exactly
        // as before.
        if (native_calls) *native_calls = slot.incumbent.native_calls;
        return true;
    }
    // RAM-backed: "healed" means the CURRENT live bytes actually resolve to
    // some resident variant right now (mirrors overlay_try_dispatch — a
    // stale incumbent left over from a still-in-flight recompile of newer
    // content must not report true just because the key exists).
    HealedEntry* match = nullptr;
    if (heal_slot_resolve_ram(slot, active_bus(), &match) !=
        HealResolveResult::kMatched) {
        return false;
    }
    if (native_calls) *native_calls = match->native_calls;
    return true;
}

uint64_t overlay_game_thread_compile_ns() {
    // The game thread never compiles any more (both gcc and tcc run on the
    // worker thread). Always 0 — kept so the coverage banner's stall metric
    // stays wired and reads as a clean zero.
    return 0;
}

void overlay_counters(uint64_t* healed_native, uint64_t* native_calls_total,
                      uint64_t* inflight, uint64_t* failed,
                      uint64_t* ram_healed, uint64_t* ram_revalidations,
                      uint64_t* ram_crc_mismatches) {
    if (healed_native)      *healed_native      = g_healed.size();
    if (native_calls_total) *native_calls_total = s_native_calls_total;
    if (inflight)           *inflight           = s_inflight.size();
    if (failed)             *failed             = s_failed.size();
    if (ram_healed) {
        uint64_t n = 0;
        for (const auto& kv : g_healed) {
            if (kv.second.incumbent.ram_backed) ++n;
        }
        *ram_healed = n;
    }
    if (ram_revalidations)   *ram_revalidations   = s_ram_revalidations;
    if (ram_crc_mismatches)  *ram_crc_mismatches  = s_ram_crc_mismatches;
}

bool overlay_enabled() { return s_active; }

bool overlay_alias_relocatable_for_test(uint32_t from_pc, uint32_t to_pc,
                                        bool thumb) {
    auto it = g_healed.find(heal_key(from_pc, thumb));
    if (it == g_healed.end() || !it->second.incumbent.fn ||
        !it->second.incumbent.ram_backed ||
        !it->second.incumbent.relocatable) {
        return false;
    }
    HealedEntry alias = it->second.incumbent;
    const uint32_t len = alias.end - alias.addr;
    alias.addr = to_pc;
    alias.end = to_pc + len;
    alias.native_calls = 0;
    heal_slot_promote(g_healed[heal_key(to_pc, thumb)], alias);
    return true;
}

void overlay_relocatable_alias_counters(uint64_t* hits, uint64_t* misses) {
    if (hits) *hits = s_relocatable_alias_hits;
    if (misses) *misses = s_relocatable_alias_misses;
}

// Sticky: true once the heal feature initialized this session, and stays true
// after overlay_loader_shutdown() so the exit coverage report (which runs
// after shutdown drains/joins the worker) honestly states the feature was on.
bool overlay_was_enabled() { return s_ever_active; }

void overlay_note_frame(uint64_t frame) {
    s_ram_churn_current_frame = frame;
    if (!s_frame_events_enabled && !s_ram_churn_probe_enabled) return;
    if (s_frame_events_enabled) {
        if (s_frame_events.empty() || s_frame_events.back().frame != frame) {
            s_frame_events.push_back({});
            s_frame_events.back().frame = frame;
        }
        FrameEventCounts& c = s_frame_events.back();
        c.ready_pending_at_frame = static_cast<uint32_t>(std::max(
            0, s_ready_pending.load(std::memory_order_acquire)));
        c.ready_pending_after_drain = c.ready_pending_at_frame;
        c.warm_load_done = s_warm_load_done.load(std::memory_order_acquire)
            ? 1u : 0u;
    }
}

void overlay_note_dispatch_miss(uint32_t pc, bool thumb,
                                OverlayRequestOutcome outcome) {
    if (s_frame_events_enabled) ++frame_event_bucket().dispatch_miss;
    ram_churn_record(
        s_ram_churn_current_frame, "dispatch_miss", pc, thumb,
        is_ram_addr(pc) ? "ram-request" : "immutable-pc",
        ram_churn_outcome_name(outcome));
}

void overlay_note_smc_fallback(uint32_t pc, bool thumb) {
    if (s_frame_events_enabled) ++frame_event_bucket().ram_smc_fallback;
    ram_churn_record(s_ram_churn_current_frame, "ram_smc_fallback", pc,
                     thumb, "static-guard-crc", "aot-guard");
}

void overlay_note_bridge_miss(uint32_t pc, bool thumb, uint64_t ns,
                              uint64_t interpreted_insns,
                              OverlayRequestOutcome outcome) {
    if (!s_frame_events_enabled) return;
    frame_event_bucket().bridge_ns += ns;
    BridgeMissEvent e;
    e.frame = frame_event_bucket().frame;
    e.pc = pc;
    e.thumb = thumb;
    e.bridge_ns = ns;
    e.interpreted_insns = interpreted_insns;
    e.outcome = outcome;
    s_bridge_miss_events.push_back(e);
}

void overlay_note_ram_crc_mismatch(uint32_t pc, bool thumb) {
    ram_churn_record(s_ram_churn_current_frame, "ram_crc_mismatch", pc,
                     thumb, "resident-crc", "ram-variant");
}

}  // namespace gbarecomp

// ── Hot-path resolve tier ────────────────────────────────────────────────
// Resolve/bookkeep here, but leave the actual invoke to runtime_dispatch's
// tail-transfer site. In particular, do not restore g_runtime_image_base after
// a relocatable hit: post-call work would force a host call frame. Generated
// PIC bodies snapshot their own base on entry, and the emitter resynchronizes
// the global before later transfers.
namespace gbarecomp {
extern "C" void (*overlay_resolve(uint32_t pc, int thumb))(void) {
    if (!gbarecomp::s_active) return nullptr;
    auto it = gbarecomp::g_healed.find(
        gbarecomp::heal_key(pc, thumb != 0));
    if (it == gbarecomp::g_healed.end() || !it->second.incumbent.fn) {
        // Never healed at all yet for this (pc,thumb) — the only branch with
        // no prior crc to report.
        std::fprintf(stderr,
            "[dispatch-miss] reason=not-healed pc=0x%08X crc=0x00000000\n", pc);
        return nullptr;
    }
    gbarecomp::HealSlot& slot = it->second;
    if (slot.incumbent.ram_backed && !gbarecomp::s_ram_heal_enabled)
        return nullptr;

    // ROM/BIOS-backed entries are immutable for the run: zero overhead,
    // unchanged from before.
    if (!slot.incumbent.ram_backed) {
        gbarecomp::HealedEntry& e = slot.incumbent;
        ++e.native_calls;
        ++gbarecomp::s_native_calls_total;
        return e.fn;
    }

    // RAM-backed entries have no write-path hook telling us the bytes changed
    // out from under the compiled native code, so soundness comes from
    // content verification AT ENTRY instead: re-hash the exact range the
    // incumbent was compiled from and only call it on a match — this is the
    // hot, non-alternating path and costs exactly what it always has: no
    // added scan. On an incumbent mismatch, check the bounded set of OTHER
    // resident variants for this key (Golden Sun's self-assembled sound-
    // driver/sprite-blitter pool toggles among a few recurring bodies at the
    // same address) before paying for an interpreter bridge; every candidate
    // is re-verified with the SAME unconditional CRC32 check, so nothing is
    // ever entered on a stale or unverified basis. Only a genuinely new
    // variant falls through to today's behavior: miss and re-enqueue —
    // runtime_dispatch_miss re-enqueues a heal for the new content (the
    // on-disk cache is already keyed by CRC, so a previously-seen-on-disk
    // variant is a cache hit, not a fresh compile). The slot's incumbent and
    // alternates are left exactly as they are: none of them just became
    // wrong, they simply don't match THIS content, and a later return to any
    // of them must not have to re-heal. (overlay_request_compile's guard
    // deliberately allows re-enqueuing for a RAM-backed key that is already
    // present in g_healed, precisely so this path is reachable.)
    gba::GbaBus* bus = gbarecomp::active_bus();
    gbarecomp::HealedEntry* match = nullptr;
    const gbarecomp::HealResolveResult result =
        gbarecomp::heal_slot_resolve_ram(slot, bus, &match);
    if (result == gbarecomp::HealResolveResult::kRegionGone) {
        // region shrank/vanished under us — treat as a miss. crc is the
        // incumbent's own recorded value (what it expected, never re-verified).
        std::fprintf(stderr,
            "[dispatch-miss] reason=region-gone pc=0x%08X crc=0x%08X\n", pc,
            slot.incumbent.crc);
        return nullptr;
    }
    ++gbarecomp::s_ram_revalidations;
    if (result == gbarecomp::HealResolveResult::kMismatch) {
        ++gbarecomp::s_ram_crc_mismatches;
        if (gbarecomp::s_frame_events_enabled)
            ++gbarecomp::frame_event_bucket().ram_crc_mismatch;
        gbarecomp::overlay_note_ram_crc_mismatch(pc, thumb != 0);
        // crc is the incumbent's resident value — the one that just failed
        // to match live bytes. Compare against the crc on the [ram-compile]
        // line this triggers to see whether the recompile rediscovers the
        // exact same body.
        std::fprintf(stderr,
            "[dispatch-miss] reason=content-mismatch pc=0x%08X crc=0x%08X\n",
            pc, slot.incumbent.crc);
        gbarecomp::overlay_request_compile(pc, thumb != 0);
        return nullptr;
    }

    gbarecomp::HealedEntry& e = *match;
    ++e.native_calls;
    ++gbarecomp::s_native_calls_total;
    if (e.relocatable) g_runtime_image_base = e.addr;
    return e.fn;
}
}  // namespace gbarecomp

// Invoke-and-status wrapper retained for miss/fallback callers that need to
// continue in C after the native function returns. The generic runtime uses
// overlay_resolve() directly for the hot tail-dispatch path.
extern "C" int overlay_try_dispatch(uint32_t pc, int thumb) {
    const uint32_t saved_image_base = g_runtime_image_base;
    if (void (*fn)(void) = gbarecomp::overlay_resolve(pc, thumb)) {
        fn();
        // This ABI explicitly continues in the C caller after the native
        // body returns. Preserve its caller's PIC context; the hot generic
        // runtime_dispatch() path uses overlay_resolve() directly and tail-
        // transfers without this post-call work.
        g_runtime_image_base = saved_image_base;
        return 1;
    }
    return 0;
}
