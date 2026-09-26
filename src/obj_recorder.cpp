// obj_recorder.cpp — see obj_recorder.h.

#include "obj_recorder.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

namespace gsr {
namespace {

namespace ws = ::gsr::widescreen;

constexpr std::size_t kOutcomeCount =
    static_cast<std::size_t>(ws::GoldenSunObjPlacementOutcome::Count);

bool g_enabled = false;

// ---- session directory ---------------------------------------------------
// Separate from map_recorder's logs/maprec_<stamp>/ for the same reason that
// one is separate from the tracer's: different diagnostic, different output
// shape. Same stamping approach.
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
    g_session_dir = std::string("logs/objrec_") + stamp;
    std::error_code ec;
    std::filesystem::create_directories(g_session_dir, ec);
    g_session_ready = true;
}

// Sticky write-failure flag, same rationale as map_recorder.cpp's: a
// silently-lost capture must show up in the summary rather than vanish.
bool g_write_failed = false;
std::string g_write_failed_path;

void note_write_failure(const std::string& path) {
    if (!g_write_failed) {
        g_write_failed = true;
        g_write_failed_path = path;
    }
}

// ---- session totals ------------------------------------------------------
std::uint64_t g_total_committed = 0;
std::array<std::uint64_t, kOutcomeCount> g_total_by_outcome{};
// Split the two exact outcomes by body/shadow: "do shadows come through as
// well as bodies" is exactly the question asked of this run, and a combined
// total cannot answer it.
std::uint64_t g_total_body_exact = 0;
std::uint64_t g_total_shadow_exact = 0;
std::uint64_t g_total_body_committed = 0;
std::uint64_t g_total_shadow_committed = 0;
std::uint64_t g_total_affine_committed = 0;
std::uint64_t g_total_affine_exact = 0;
// Largest paired offset seen, and how many came within reach of the
// truncated field's half-range -- the only way the paired recovery can be
// wrong. Anything but a comfortable margin here invalidates it.
int g_paired_offset_max_x = 0;
int g_paired_offset_max_y = 0;
std::uint64_t g_paired_offset_near_limit = 0;
// Per sprite table, so "is the second table being seen at all?" is a
// number rather than an assumption.
constexpr std::size_t kTableLimit = 4;
struct TableTally {
    std::uint32_t base = 0;
    std::uint64_t committed = 0;
    std::uint64_t exact = 0;
};
std::array<TableTally, kTableLimit> g_tables{};
std::uint64_t g_frames_seen = 0;

// ---- per-frame row (coverage.csv) ----------------------------------------
//
// One row per guest frame that committed at least one sprite. Buffered and
// flushed in blocks, then re-flushed from the exit summary, so closing the
// game window mid-session keeps everything already written.
std::uint64_t g_frame = ~std::uint64_t{0};
bool g_frame_open = false;
std::uint64_t g_frame_committed = 0;
std::array<std::uint64_t, kOutcomeCount> g_frame_by_outcome{};

std::string g_coverage_buf;
std::size_t g_coverage_rows = 0;
bool g_coverage_header_written = false;
// 300 rows matches map_recorder.cpp's kFramesFlushRows, which is itself the
// cadence function_tracer.cpp established for a per-frame CSV. Reused rather
// than picked fresh.
constexpr std::size_t kCoverageFlushRows = 300;

void flush_coverage() {
    if (g_coverage_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/coverage.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        note_write_failure(path);
        g_coverage_buf.clear();
        g_coverage_rows = 0;
        return;
    }
    std::fwrite(g_coverage_buf.data(), 1, g_coverage_buf.size(), f);
    std::fclose(f);
    g_coverage_buf.clear();
    g_coverage_rows = 0;
}

std::string g_lifetime_buf;
std::size_t g_lifetime_rows = 0;
bool g_lifetime_header = false;
std::uint64_t g_lifetime_sequence = 0;
void flush_lifetime() {
    if (g_lifetime_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/lifetime.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) { note_write_failure(path); }
    else {
        if (std::fwrite(g_lifetime_buf.data(), 1, g_lifetime_buf.size(), f) !=
            g_lifetime_buf.size()) note_write_failure(path);
        if (std::fclose(f) != 0) note_write_failure(path);
    }
    g_lifetime_buf.clear();
    g_lifetime_rows = 0;
}

std::string g_shadow_write_buf;
std::size_t g_shadow_write_rows = 0;
bool g_shadow_write_header = false;
std::uint64_t g_shadow_write_sequence = 0;
void flush_shadow_writes() {
    if (g_shadow_write_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/shadow_writes.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) { note_write_failure(path); }
    else {
        if (std::fwrite(g_shadow_write_buf.data(), 1,
                        g_shadow_write_buf.size(), f) !=
            g_shadow_write_buf.size()) note_write_failure(path);
        if (std::fclose(f) != 0) note_write_failure(path);
    }
    g_shadow_write_buf.clear();
    g_shadow_write_rows = 0;
}

std::string g_ewram_write_buf;
std::size_t g_ewram_write_rows = 0;
bool g_ewram_write_header = false;
std::uint64_t g_ewram_write_sequence = 0;
std::uint64_t g_total_ewram_writes = 0;
void flush_ewram_writes() {
    if (g_ewram_write_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/ewram_writes.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) { note_write_failure(path); }
    else {
        if (std::fwrite(g_ewram_write_buf.data(), 1,
                        g_ewram_write_buf.size(), f) !=
            g_ewram_write_buf.size()) note_write_failure(path);
        if (std::fclose(f) != 0) note_write_failure(path);
    }
    g_ewram_write_buf.clear();
    g_ewram_write_rows = 0;
}

std::string g_boulder_trace_buf;
std::size_t g_boulder_trace_rows = 0;
bool g_boulder_trace_header = false;
std::uint64_t g_boulder_trace_sequence = 0;
std::uint64_t g_total_boulder_trace_rows = 0;
void flush_boulder_trace() {
    if (g_boulder_trace_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/boulder_trace.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) { note_write_failure(path); }
    else {
        if (std::fwrite(g_boulder_trace_buf.data(), 1,
                        g_boulder_trace_buf.size(), f) !=
            g_boulder_trace_buf.size()) note_write_failure(path);
        if (std::fclose(f) != 0) note_write_failure(path);
    }
    g_boulder_trace_buf.clear();
    g_boulder_trace_rows = 0;
}

std::string g_camera_buf;
std::size_t g_camera_rows = 0;
bool g_camera_header = false;
std::uint64_t g_camera_sequence = 0;
std::uint64_t g_total_camera_samples = 0;
void flush_camera() {
    if (g_camera_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/object_camera.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) { note_write_failure(path); }
    else {
        if (std::fwrite(g_camera_buf.data(), 1, g_camera_buf.size(), f) !=
            g_camera_buf.size()) note_write_failure(path);
        if (std::fclose(f) != 0) note_write_failure(path);
    }
    g_camera_buf.clear();
    g_camera_rows = 0;
}

void close_frame() {
    if (!g_frame_open) return;
    g_frame_open = false;
    if (g_frame_committed == 0) return;
    ++g_frames_seen;
    if (!g_coverage_header_written) {
        g_coverage_header_written = true;
        g_coverage_buf += "frame,committed";
        for (std::size_t i = 0; i < kOutcomeCount; ++i) {
            g_coverage_buf += ',';
            g_coverage_buf += ws::golden_sun_obj_placement_outcome_name(
                static_cast<ws::GoldenSunObjPlacementOutcome>(i));
        }
        g_coverage_buf += '\n';
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%llu,%llu",
                  static_cast<unsigned long long>(g_frame),
                  static_cast<unsigned long long>(g_frame_committed));
    g_coverage_buf += buf;
    for (std::size_t i = 0; i < kOutcomeCount; ++i) {
        std::snprintf(buf, sizeof(buf), ",%llu",
                      static_cast<unsigned long long>(g_frame_by_outcome[i]));
        g_coverage_buf += buf;
    }
    g_coverage_buf += '\n';
    if (++g_coverage_rows >= kCoverageFlushRows) flush_coverage();
}

// ---- distinct call sites (sites.csv) -------------------------------------
//
// The actionable output. A sprite that ends SourceUnavailable was emitted by
// some guest code path we have not hooked; grouping by (writer_pc,
// return_pc, depth) names those paths directly, instead of leaving a
// percentage with nowhere to go. Every outcome is tallied per site, not just
// the failures, so a site that is 90% covered is distinguishable from one
// that is never covered.
struct CallSite {
    bool valid = false;
    std::uint32_t writer_pc = 0;
    std::uint32_t return_pc = 0;
    std::uint32_t depth = 0;
    std::uint64_t total = 0;
    std::array<std::uint64_t, kOutcomeCount> by_outcome{};
    // A representative unsourced sprite, so the site can be recognised in a
    // disassembly without cross-referencing another file. First one wins;
    // later ones only bump the counts.
    std::uint16_t sample_attr0 = 0;
    std::uint16_t sample_attr1 = 0;
    std::uint16_t sample_attr2 = 0;
    std::uint32_t sample_staging = 0;
    int sample_slot = -1;
};

// Bounded so a long session cannot grow this without limit. A handful of
// guest routines commit sprites; 512 distinct call sites is far beyond that,
// and the overflow count in the summary says so if it is ever wrong.
constexpr std::size_t kSiteLimit = 512;
std::array<CallSite, kSiteLimit> g_sites{};
std::uint64_t g_sites_overflow = 0;

CallSite* find_or_add_site(const ObjPlacementSample& s) {
    CallSite* free_slot = nullptr;
    for (auto& site : g_sites) {
        if (!site.valid) {
            if (!free_slot) free_slot = &site;
            continue;
        }
        if (site.writer_pc == s.writer_pc && site.return_pc == s.return_pc &&
            site.depth == s.depth) {
            return &site;
        }
    }
    if (!free_slot) {
        ++g_sites_overflow;
        return nullptr;
    }
    free_slot->valid = true;
    free_slot->writer_pc = s.writer_pc;
    free_slot->return_pc = s.return_pc;
    free_slot->depth = s.depth;
    return free_slot;
}

// ---- actor records (records.csv) -----------------------------------------
//
// golden_sun_obj_record_identity admits any pointer shaped like an actor
// record: base 0x03002000, stride 0x38, body at +0x00 and its paired shadow
// at +0x0C, bounded only by IWRAM. It used to stop at 0x030022E0 plus one
// hand-authenticated body (0x03002348), which left every actor past the
// audited prefix without a trusted position. Admission is not proof: callers
// retain the same-frame, epoch, complete-ATTR and hardware-truncation
// provenance gates, which is what actually authenticates a record.
constexpr std::size_t kRecordLimit = 64;
struct RecordTally {
    bool valid = false;
    std::uint32_t record_base = 0;
    std::uint64_t body_committed = 0;
    std::uint64_t body_exact = 0;
    std::uint64_t shadow_committed = 0;
    std::uint64_t shadow_exact = 0;
};
std::array<RecordTally, kRecordLimit> g_records{};
std::uint64_t g_records_overflow = 0;

RecordTally* find_or_add_record(std::uint32_t record_base) {
    RecordTally* free_slot = nullptr;
    for (auto& record : g_records) {
        if (!record.valid) {
            if (!free_slot) free_slot = &record;
            continue;
        }
        if (record.record_base == record_base) return &record;
    }
    if (!free_slot) {
        ++g_records_overflow;
        return nullptr;
    }
    free_slot->valid = true;
    free_slot->record_base = record_base;
    return free_slot;
}

// ---- vertical-wrap exposure ----------------------------------------------
//
// The measurement that ties this census back to what Jimmy actually sees on
// screen. A sprite is exposed to the 8-bit wrap when its committed byte is
// one of the values two view rows share -- and only when it has no exact
// placement to fall back on. Counting those separately answers "how many of
// the sprites that can jump are ones we could already have placed?".
//
// The ambiguous band is derived, not assumed: with a 40px top margin the
// negative reading covers rows -56..-1 (bytes 200..255), so any row at or
// below -57 folds onto bytes 152..199, which are also real bottom-margin
// rows. Recorded as a raw byte range so the analysis can re-derive it for a
// different view size without re-running the session.
constexpr std::uint32_t kWrapAmbiguousFirstByte = 152;
constexpr std::uint32_t kWrapAmbiguousLastByte = 199;
std::uint64_t g_wrap_band_committed = 0;
std::uint64_t g_wrap_band_exact = 0;
std::uint64_t g_wrap_band_unresolved = 0;

// ---- unsourced detail (unsourced.csv) ------------------------------------
//
// Bounded per-sprite detail for the failures only. Deduped on the fields
// that identify a sprite rather than its position, so a single NPC walking
// across a room contributes one row instead of one per frame.
struct UnsourcedKey {
    bool valid = false;
    std::uint32_t return_pc = 0;
    std::uint16_t attr2 = 0;
    int slot = -1;
};
constexpr std::size_t kUnsourcedLimit = 2048;
std::array<UnsourcedKey, kUnsourcedLimit> g_unsourced_keys{};
std::size_t g_unsourced_count = 0;
std::uint64_t g_unsourced_dropped = 0;
std::string g_unsourced_buf;
bool g_unsourced_header_written = false;

void flush_unsourced() {
    if (g_unsourced_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/unsourced.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        note_write_failure(path);
        g_unsourced_buf.clear();
        return;
    }
    std::fwrite(g_unsourced_buf.data(), 1, g_unsourced_buf.size(), f);
    std::fclose(f);
    g_unsourced_buf.clear();
}

bool unsourced_is_new(const ObjPlacementSample& s) {
    for (std::size_t i = 0; i < g_unsourced_count; ++i) {
        const auto& key = g_unsourced_keys[i];
        if (key.valid && key.return_pc == s.return_pc &&
            key.attr2 == s.attr2 && key.slot == s.slot) {
            return false;
        }
    }
    if (g_unsourced_count >= kUnsourcedLimit) {
        ++g_unsourced_dropped;
        return false;
    }
    g_unsourced_keys[g_unsourced_count++] = {true, s.return_pc, s.attr2,
                                             s.slot};
    return true;
}

void log_unsourced(const ObjPlacementSample& s) {
    if (!unsourced_is_new(s)) return;
    if (!g_unsourced_header_written) {
        g_unsourced_header_written = true;
        g_unsourced_buf +=
            "frame,slot,slot_address,writer_pc,return_pc,depth,staging,"
            "record_base,shadow,affine,attr0,attr1,attr2,raw_x,raw_y,"
            "outcome\n";
    }
    char buf[384];
    std::snprintf(
        buf, sizeof(buf),
        "%llu,%d,0x%08x,0x%08x,0x%08x,%u,0x%08x,0x%08x,%d,%d,0x%04x,"
        "0x%04x,0x%04x,%u,%u,%s\n",
        static_cast<unsigned long long>(s.frame), s.slot, s.slot_address,
        s.writer_pc, s.return_pc, s.depth, s.staging, s.record_base,
        s.shadow ? 1 : 0, s.affine ? 1 : 0, s.attr0, s.attr1, s.attr2,
        s.raw_x, s.raw_y,
        ws::golden_sun_obj_placement_outcome_name(s.outcome));
    g_unsourced_buf += buf;
    if (g_unsourced_buf.size() >= 32768) flush_unsourced();
}

// ---- summary -------------------------------------------------------------
//
// Rewritten in full on every call rather than appended, so it always
// describes the whole session so far. Called from the flush points and from
// atexit, because the normal way this session ends is Jimmy closing the game
// window, not a clean shutdown.
void write_summary() {
    if (!g_enabled) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/summary.txt";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        note_write_failure(path);
        return;
    }
    const auto pct = [](std::uint64_t n, std::uint64_t d) {
        return d == 0 ? 0.0 : (100.0 * static_cast<double>(n) /
                               static_cast<double>(d));
    };
    const std::uint64_t exact =
        g_total_by_outcome[static_cast<std::size_t>(
            ws::GoldenSunObjPlacementOutcome::Exact)] +
        g_total_by_outcome[static_cast<std::size_t>(
            ws::GoldenSunObjPlacementOutcome::PairedBody)];

    std::fprintf(f, "sprite placement census\n");
    std::fprintf(f, "frames with commits : %llu\n",
                 static_cast<unsigned long long>(g_frames_seen));
    std::fprintf(f, "committed sprites   : %llu\n",
                 static_cast<unsigned long long>(g_total_committed));
    std::fprintf(f, "full-precision      : %llu (%.2f%%)\n",
                 static_cast<unsigned long long>(exact),
                 pct(exact, g_total_committed));
    std::fprintf(f, "\nby outcome\n");
    for (std::size_t i = 0; i < kOutcomeCount; ++i) {
        std::fprintf(f, "  %-22s %10llu  %6.2f%%\n",
                     ws::golden_sun_obj_placement_outcome_name(
                         static_cast<ws::GoldenSunObjPlacementOutcome>(i)),
                     static_cast<unsigned long long>(g_total_by_outcome[i]),
                     pct(g_total_by_outcome[i], g_total_committed));
    }

    // Bodies and shadows reported apart: "NPCs and objects always render
    // correctly WITH their shadows" is the goal, and a shadow that loses its
    // placement wraps on its own even when its body does not.
    std::fprintf(f, "\nbody vs shadow\n");
    std::fprintf(f, "  body   committed %llu, full-precision %llu (%.2f%%)\n",
                 static_cast<unsigned long long>(g_total_body_committed),
                 static_cast<unsigned long long>(g_total_body_exact),
                 pct(g_total_body_exact, g_total_body_committed));
    std::fprintf(f, "  shadow committed %llu, full-precision %llu (%.2f%%)\n",
                 static_cast<unsigned long long>(g_total_shadow_committed),
                 static_cast<unsigned long long>(g_total_shadow_exact),
                 pct(g_total_shadow_exact, g_total_shadow_committed));

    // Golden Sun scales sprites constantly, so this is not a corner case:
    // affine sprites were half the committed population in
    // objrec_20260906_110334, and refusing to place them was what left them
    // on the wrapping byte.
    std::fprintf(f, "\naffine (rotation/scaling) sprites\n");
    std::fprintf(f, "  committed %llu, full-precision %llu (%.2f%%)\n",
                 static_cast<unsigned long long>(g_total_affine_committed),
                 static_cast<unsigned long long>(g_total_affine_exact),
                 pct(g_total_affine_exact, g_total_affine_committed));

    std::fprintf(f, "\nsprite tables observed\n");
    for (const auto& table : g_tables) {
        if (!table.base) continue;
        std::fprintf(f, "  0x%08x  committed %llu, full-precision %llu (%.2f%%)\n",
                     table.base,
                     static_cast<unsigned long long>(table.committed),
                     static_cast<unsigned long long>(table.exact),
                     pct(table.exact, table.committed));
    }

    std::fprintf(f, "\npaired shadow recovery\n");
    std::fprintf(f, "  largest offset from body: x %d, y %d\n",
                 g_paired_offset_max_x, g_paired_offset_max_y);
    std::fprintf(f,
                 "  offsets within reach of the wrap limit: %llu\n",
                 static_cast<unsigned long long>(
                     g_paired_offset_near_limit));
    std::fprintf(f,
                 "  (x must stay under 256 and y under 128; a nonzero\n"
                 "   count above means the recovery is not safe)\n");

    std::fprintf(f,
                 "\nvertical-wrap exposure (committed OAM Y byte in %u..%u,\n"
                 "the band where a bottom-margin row and a row above the view\n"
                 "share a byte at a 40px top margin)\n",
                 kWrapAmbiguousFirstByte, kWrapAmbiguousLastByte);
    std::fprintf(f, "  in band            : %llu\n",
                 static_cast<unsigned long long>(g_wrap_band_committed));
    std::fprintf(f, "  ...with placement  : %llu (%.2f%%) -- these are fixable\n",
                 static_cast<unsigned long long>(g_wrap_band_exact),
                 pct(g_wrap_band_exact, g_wrap_band_committed));
    std::fprintf(f, "  ...without         : %llu (%.2f%%) -- these still jump\n",
                 static_cast<unsigned long long>(g_wrap_band_unresolved),
                 pct(g_wrap_band_unresolved, g_wrap_band_committed));

    std::fprintf(f, "\nmeasured EWRAM object-source writes\n");
    std::fprintf(f, "  recorded            : %llu\n",
                 static_cast<unsigned long long>(g_total_ewram_writes));
    std::fprintf(f, "  camera joins        : %llu\n",
                 static_cast<unsigned long long>(g_total_camera_samples));
    std::fprintf(f, "  boulder trace rows  : %llu\n",
                 static_cast<unsigned long long>(g_total_boulder_trace_rows));

    std::fprintf(f, "\ntable limits\n");
    std::fprintf(f, "  call sites overflow    : %llu\n",
                 static_cast<unsigned long long>(g_sites_overflow));
    std::fprintf(f, "  actor records overflow : %llu\n",
                 static_cast<unsigned long long>(g_records_overflow));
    std::fprintf(f, "  unsourced rows dropped : %llu\n",
                 static_cast<unsigned long long>(g_unsourced_dropped));
    if (g_write_failed) {
        std::fprintf(f, "\nWRITE FAILED for %s -- this session is incomplete\n",
                     g_write_failed_path.c_str());
    }
    std::fclose(f);
}

void write_sites() {
    if (!g_enabled) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/sites.csv";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        note_write_failure(path);
        return;
    }
    std::fprintf(f, "writer_pc,return_pc,depth,total");
    for (std::size_t i = 0; i < kOutcomeCount; ++i) {
        std::fprintf(f, ",%s",
                     ws::golden_sun_obj_placement_outcome_name(
                         static_cast<ws::GoldenSunObjPlacementOutcome>(i)));
    }
    std::fprintf(f, ",sample_slot,sample_staging,sample_attr0,sample_attr1,"
                    "sample_attr2\n");
    for (const auto& site : g_sites) {
        if (!site.valid) continue;
        std::fprintf(f, "0x%08x,0x%08x,%u,%llu", site.writer_pc,
                     site.return_pc, site.depth,
                     static_cast<unsigned long long>(site.total));
        for (std::size_t i = 0; i < kOutcomeCount; ++i) {
            std::fprintf(f, ",%llu",
                         static_cast<unsigned long long>(site.by_outcome[i]));
        }
        std::fprintf(f, ",%d,0x%08x,0x%04x,0x%04x,0x%04x\n", site.sample_slot,
                     site.sample_staging, site.sample_attr0, site.sample_attr1,
                     site.sample_attr2);
    }
    std::fclose(f);
}

void write_records() {
    if (!g_enabled) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/records.csv";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        note_write_failure(path);
        return;
    }
    std::fprintf(f,
                 "record_base,body_committed,body_exact,shadow_committed,"
                 "shadow_exact\n");
    for (const auto& record : g_records) {
        if (!record.valid) continue;
        std::fprintf(f, "0x%08x,%llu,%llu,%llu,%llu\n", record.record_base,
                     static_cast<unsigned long long>(record.body_committed),
                     static_cast<unsigned long long>(record.body_exact),
                     static_cast<unsigned long long>(record.shadow_committed),
                     static_cast<unsigned long long>(record.shadow_exact));
    }
    std::fclose(f);
}

void write_all() {
    flush_lifetime();
    flush_shadow_writes();
    flush_ewram_writes();
    flush_boulder_trace();
    flush_camera();
    close_frame();
    flush_coverage();
    flush_unsourced();
    write_sites();
    write_records();
    write_summary();
}

}  // namespace

void obj_recorder_init() {
    const char* e = std::getenv("GSR_OBJ_RECORD");
    g_enabled = e != nullptr && e[0] != '\0' && e[0] != '0';
    if (!g_enabled) return;
    ensure_session_dir();
    std::atexit(write_all);
}

bool obj_recorder_enabled() { return g_enabled; }

void obj_recorder_note_lifetime(const ObjLifetimeSample& s) {
    if (!g_enabled) return;
    if (!g_lifetime_header) {
        g_lifetime_header = true;
        g_lifetime_buf += "sequence,event,reason,frame,epoch,dma,slot,source,staging,context_frame,body_frame,body_epoch,checks,attr0,attr1,attr2,provenance_frame,provenance_epoch,target,expected0,expected1,expected2,entry_state,entry_frame,entry_epoch,entry_pc,entry_source,entry_target,entry_depth,entry_return_pc,entry_attr0,entry_attr1,entry_attr2,entry_flags,entry_stage_source,entry_stage_frame,entry_stage_epoch,entry_stage_x,entry_stage_y\n";
    }
    g_lifetime_buf += std::to_string(++g_lifetime_sequence);
    g_lifetime_buf += ','; g_lifetime_buf += s.event;
    g_lifetime_buf += ','; g_lifetime_buf += s.reason;
    const std::uint64_t values[] = {s.frame, s.epoch, s.dma,
        static_cast<std::uint64_t>(s.slot), s.source, s.staging,
        s.context_frame, s.body_frame, s.body_epoch, s.checks,
        s.attr0, s.attr1, s.attr2, s.provenance_frame, s.provenance_epoch,
        s.target, s.expected0, s.expected1, s.expected2};
    for (auto v : values) {
        g_lifetime_buf += ','; g_lifetime_buf += std::to_string(v);
    }
    g_lifetime_buf += ','; g_lifetime_buf += s.entry.state;
    const std::uint64_t entry_values[] = {
        s.entry.frame, s.entry.epoch, s.entry.pc, s.entry.source,
        s.entry.target, s.entry.depth, s.entry.return_pc,
        s.entry.attr0, s.entry.attr1, s.entry.attr2, s.entry.flags,
        s.entry.stage_source, s.entry.stage_frame, s.entry.stage_epoch};
    for (auto v : entry_values) {
        g_lifetime_buf += ','; g_lifetime_buf += std::to_string(v);
    }
    // Keep negative full-precision coordinates signed in the CSV.
    g_lifetime_buf += ','; g_lifetime_buf += std::to_string(s.entry.stage_x);
    g_lifetime_buf += ','; g_lifetime_buf += std::to_string(s.entry.stage_y);
    g_lifetime_buf += '\n';
    // Reuse the existing flush cadence; no cap that expires before gameplay.
    if (++g_lifetime_rows >= kCoverageFlushRows) flush_lifetime();
}

void obj_recorder_note_shadow_write(const ObjShadowWriteSample& s) {
    if (!g_enabled) return;
    if (!g_shadow_write_header) {
        g_shadow_write_header = true;
        g_shadow_write_buf +=
            "sequence,event,frame,epoch,dma,slot,writer_pc,address,size,"
            "source,destination,control,start_mode,attr_mask,attr0,attr1,attr2,"
            "return_pc,caller_return_pc\n";
    }
    g_shadow_write_buf += std::to_string(++g_shadow_write_sequence);
    g_shadow_write_buf += ','; g_shadow_write_buf += s.event;
    const std::uint64_t values[] = {
        s.frame, s.epoch, s.dma, static_cast<std::uint64_t>(s.slot),
        s.writer_pc, s.address, s.size, s.source, s.destination,
        s.control, static_cast<std::uint64_t>(s.start_mode), s.attr_mask, s.attr0,
        s.attr1, s.attr2, s.return_pc, s.caller_return_pc};
    for (auto v : values) {
        g_shadow_write_buf += ','; g_shadow_write_buf += std::to_string(v);
    }
    g_shadow_write_buf += '\n';
    if (++g_shadow_write_rows >= kCoverageFlushRows) flush_shadow_writes();
}

void obj_recorder_note_ewram_write(const ObjEwramWriteSample& s) {
    if (!g_enabled) return;
    if (!g_ewram_write_header) {
        g_ewram_write_header = true;
        g_ewram_write_buf +=
            "sequence,frame,epoch,region,writer_pc,address,offset,size\n";
    }
    char buf[256];
    std::snprintf(
        buf, sizeof(buf), "%llu,%llu,%llu,%s,0x%08x,0x%08x,0x%04x,%u\n",
        static_cast<unsigned long long>(++g_ewram_write_sequence),
        static_cast<unsigned long long>(s.frame),
        static_cast<unsigned long long>(s.epoch), s.region, s.writer_pc,
        s.address, s.offset, s.size);
    g_ewram_write_buf += buf;
    ++g_total_ewram_writes;
    if (++g_ewram_write_rows >= kCoverageFlushRows) flush_ewram_writes();
}

void obj_recorder_note_boulder_trace(const ObjBoulderTraceSample& s) {
    if (!g_enabled) return;
    if (!g_boulder_trace_header) {
        g_boulder_trace_header = true;
        g_boulder_trace_buf +=
            "sequence,event,source_state,outcome,frame,epoch,slot,source,"
            "source_offset,target,writer_pc,return_pc,depth,entry_pc,"
            "entry_depth,entry_return_pc,r0,r1,r2,r3,r5,r6,r7,r9,r10,r11,"
            "store_value,field_a_value,field_b_value,candidate_fields_valid,"
            "attr0,attr1,attr2,raw_x,raw_y,resolved_x,resolved_y\n";
    }
    const char* event = s.event ? s.event : "unknown";
    const char* source_state = s.source_state ? s.source_state : "unknown";
    const char* outcome = s.outcome ? s.outcome : "unknown";
    char buf[1600];
    std::snprintf(
        buf, sizeof(buf),
        "%llu,%s,%s,%s,%llu,%llu,%d,0x%08x,0x%08x,0x%08x,0x%08x,"
        "0x%08x,%u,0x%08x,%u,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,"
        "0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,"
        "0x%08x,%u,0x%04x,0x%04x,0x%04x,%u,%u,%d,%d\n",
        static_cast<unsigned long long>(++g_boulder_trace_sequence), event,
        source_state, outcome, static_cast<unsigned long long>(s.frame),
        static_cast<unsigned long long>(s.epoch), s.slot, s.source,
        s.source_offset, s.target, s.writer_pc, s.return_pc, s.depth,
        s.entry_pc, s.entry_depth, s.entry_return_pc, s.r0, s.r1, s.r2,
        s.r3, s.r5, s.r6, s.r7, s.r9, s.r10, s.r11, s.store_value,
        s.field_a_value, s.field_b_value, s.candidate_fields_valid ? 1u : 0u,
        static_cast<unsigned>(s.attr0), static_cast<unsigned>(s.attr1),
        static_cast<unsigned>(s.attr2), s.raw_x, s.raw_y, s.resolved_x,
        s.resolved_y);
    g_boulder_trace_buf += buf;
    ++g_total_boulder_trace_rows;
    if (++g_boulder_trace_rows >= kCoverageFlushRows) flush_boulder_trace();
}

void obj_recorder_note_camera_sample(const ObjCameraSample& s) {
    if (!g_enabled) return;
    if (!g_camera_header) {
        g_camera_header = true;
        g_camera_buf +=
            "sequence,event,reason,source_state,source_region,frame,epoch,"
            "slot,writer_pc,target,source,entry_frame,entry_epoch,entry_pc,"
            "entry_depth,entry_return_pc,attr0,attr1,attr2,dispcnt,"
            "bg0_hofs,bg0_vofs,bg1_hofs,bg1_vofs,bg2_hofs,bg2_vofs,"
            "bg3_hofs,bg3_vofs,field_a_offset,field_a_value,field_b_offset,"
            "field_b_value,candidate_fields_valid\n";
    }
    g_camera_buf += std::to_string(++g_camera_sequence);
    g_camera_buf += ','; g_camera_buf += s.event;
    g_camera_buf += ','; g_camera_buf += s.reason;
    g_camera_buf += ','; g_camera_buf += s.source_state;
    g_camera_buf += ','; g_camera_buf += s.source_region;
    const std::uint64_t values[] = {
        s.frame, s.epoch, static_cast<std::uint64_t>(s.slot), s.writer_pc,
        s.target, s.source, s.entry_frame, s.entry_epoch, s.entry_pc,
        s.entry_depth, s.entry_return_pc, s.attr0, s.attr1, s.attr2,
        s.dispcnt, s.bg0_hofs, s.bg0_vofs, s.bg1_hofs, s.bg1_vofs,
        s.bg2_hofs, s.bg2_vofs, s.bg3_hofs, s.bg3_vofs, s.field_a_offset,
        s.field_a_value, s.field_b_offset, s.field_b_value,
        s.candidate_fields_valid ? 1u : 0u};
    for (auto v : values) {
        g_camera_buf += ','; g_camera_buf += std::to_string(v);
    }
    g_camera_buf += '\n';
    ++g_total_camera_samples;
    if (++g_camera_rows >= kCoverageFlushRows) flush_camera();
}

void obj_recorder_note_placement(const ObjPlacementSample& s) {
    if (!g_enabled) return;  // OFF: one predictable branch, nothing else.

    if (s.frame != g_frame) {
        close_frame();
        g_frame = s.frame;
        g_frame_open = true;
        g_frame_committed = 0;
        g_frame_by_outcome = {};
    }

    const auto outcome_index = static_cast<std::size_t>(s.outcome);
    if (outcome_index >= kOutcomeCount) return;
    const bool exact = ws::golden_sun_obj_placement_is_exact(s.outcome);

    ++g_frame_committed;
    ++g_frame_by_outcome[outcome_index];
    ++g_total_committed;
    ++g_total_by_outcome[outcome_index];

    if (s.shadow) {
        ++g_total_shadow_committed;
        if (exact) ++g_total_shadow_exact;
    } else {
        ++g_total_body_committed;
        if (exact) ++g_total_body_exact;
    }
    if (s.affine) {
        ++g_total_affine_committed;
        if (exact) ++g_total_affine_exact;
    }

    const int abs_x = s.paired_offset_x < 0 ? -s.paired_offset_x
                                            : s.paired_offset_x;
    const int abs_y = s.paired_offset_y < 0 ? -s.paired_offset_y
                                            : s.paired_offset_y;
    if (abs_x > g_paired_offset_max_x) g_paired_offset_max_x = abs_x;
    if (abs_y > g_paired_offset_max_y) g_paired_offset_max_y = abs_y;
    // 96 of 128 rows, 192 of 256 columns: close enough to the wrap limit
    // that the reading could be the wrong one.
    if (abs_y >= 96 || abs_x >= 192) ++g_paired_offset_near_limit;

    for (auto& table : g_tables) {
        if (table.base && table.base != s.table_base) continue;
        table.base = s.table_base;
        ++table.committed;
        if (exact) ++table.exact;
        break;
    }

    const std::uint32_t raw_y = s.raw_y & 0xFFu;
    if (raw_y >= kWrapAmbiguousFirstByte && raw_y <= kWrapAmbiguousLastByte) {
        ++g_wrap_band_committed;
        if (exact) {
            ++g_wrap_band_exact;
        } else {
            ++g_wrap_band_unresolved;
        }
    }

    if (auto* site = find_or_add_site(s)) {
        ++site->total;
        ++site->by_outcome[outcome_index];
        if (site->sample_slot < 0) {
            site->sample_slot = s.slot;
            site->sample_staging = s.staging;
            site->sample_attr0 = s.attr0;
            site->sample_attr1 = s.attr1;
            site->sample_attr2 = s.attr2;
        }
    }

    if (s.record_identified) {
        if (auto* record = find_or_add_record(s.record_base)) {
            if (s.shadow) {
                ++record->shadow_committed;
                if (exact) ++record->shadow_exact;
            } else {
                ++record->body_committed;
                if (exact) ++record->body_exact;
            }
        }
    }

    if (!exact) log_unsourced(s);
}

}  // namespace gsr
