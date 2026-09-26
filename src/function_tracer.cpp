// function_tracer.cpp — see function_tracer.h.

#include "function_tracer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "frame_timing.h"
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

// ---- fixed-size open-addressing call table -----------------------------
// Sized generously so a whole capture window's worth of distinct functions
// fits without ever growing or allocating. No std::unordered_map on the
// hot path.
constexpr std::size_t kTableBits = 16;
constexpr std::size_t kTableSize = std::size_t{1} << kTableBits;  // 65536
constexpr std::uint32_t kTableMask =
    static_cast<std::uint32_t>(kTableSize - 1);

struct TracerSlot {
    std::uint32_t entry_pc = 0;
    std::uint32_t count = 0;  // 0 == slot free (see find_slot)
    std::uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r14 = 0;
};

TracerSlot g_table[kTableSize];

std::uint32_t hash_pc(std::uint32_t pc) {
    // Knuth multiplicative hash, folded down to kTableBits.
    return (pc * 2654435761u) >> (32 - kTableBits);
}

// A slot is free exactly when count == 0 (see the reset comment in
// close_and_reopen_window). Linear probing stops at the first free slot or
// the first slot already holding this pc.
TracerSlot& find_slot(std::uint32_t pc) {
    std::uint32_t idx = hash_pc(pc);
    for (;;) {
        TracerSlot& s = g_table[idx];
        if (s.count == 0 || s.entry_pc == pc) return s;
        idx = (idx + 1) & kTableMask;
    }
}

bool g_enabled = false;
bool g_text_record = false;

// ---- D4: write-failure tracking ------------------------------------------
// Sticky for the session (not cleared on the next successful write): a
// failure here usually means disk-full or permissions, which won't fix
// itself, and losing captures silently is the exact failure mode this
// tracer exists to avoid. Surfaced as a red line in draw_tracer_window();
// never aborts the run.
bool g_write_failed = false;
std::string g_write_failed_path;

void note_write_failure(const std::string& path) {
    g_write_failed = true;
    g_write_failed_path = path;
}

// ---- fingerprints (context recognition) ---------------------------------
// A fingerprint is a closed window's function set, kept for the repeat-
// matching machinery below (see merge_repeat_window()). F1: no longer used
// to gate screenshots -- that gate produced only 12 screenshots out of 699
// windows in a measured session, far too few to label anything, so
// maybe_save_screenshot() now saves unconditionally. Not the hot path:
// built once per window close.
constexpr std::size_t kMaxFingerprintPcs = 2000;
const char* const kFingerprintsPath = "logs/trace_fingerprints.txt";

struct Fingerprint {
    std::string label;
    std::vector<std::uint32_t> pcs;  // sorted ascending, size <= kMaxFingerprintPcs
};

// Distinct windows never merge here, even when they share a name prefix: a
// context with several visually different forms (e.g. two dungeon layouts)
// needs to keep matching against each of its own shapes.
std::vector<Fingerprint> g_fingerprints;

// Snapshot of the window currently in g_table as (entry_pc, count) pairs,
// keeping only the top kMaxFingerprintPcs by call count so memory stays
// bounded, sorted ascending by pc. Shared by build_fingerprint() (D2: the
// repeat-matching running intersection uses the same capped, sorted list --
// see merge_repeat_window()) so a window's pc set is bounded exactly once,
// not twice differently.
std::vector<std::pair<std::uint32_t, std::uint32_t>> capped_window_pcs() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> by_count;  // pc, count
    for (const TracerSlot& s : g_table) {
        if (s.count != 0) by_count.emplace_back(s.entry_pc, s.count);
    }
    if (by_count.size() > kMaxFingerprintPcs) {
        std::nth_element(by_count.begin(), by_count.begin() + kMaxFingerprintPcs,
                         by_count.end(),
                         [](const auto& a, const auto& b) {
                             return a.second > b.second;
                         });
        by_count.resize(kMaxFingerprintPcs);
    }
    std::sort(by_count.begin(), by_count.end());  // by pc, for set merges
    return by_count;
}

// Builds a fingerprint from an already-capped, pc-sorted snapshot (see
// capped_window_pcs()).
Fingerprint build_fingerprint(
    const std::string& label,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& capped_pcs) {
    Fingerprint fp;
    fp.label = label;
    fp.pcs.reserve(capped_pcs.size());
    for (const auto& kv : capped_pcs) fp.pcs.push_back(kv.first);
    return fp;
}

// Appends one fingerprint line to logs/trace_fingerprints.txt. Called
// whenever a window closes so a crash or restart never loses more than that
// window. D2: appends rather than rewriting the whole file -- the previous
// rewrite-on-every-close cost grew with total session fingerprints; this
// costs only this one fingerprint's own (capped) size.
void append_fingerprint(const Fingerprint& fp) {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    FILE* f = std::fopen(kFingerprintsPath, "a");
    if (!f) {
        note_write_failure(kFingerprintsPath);
        return;
    }
    std::fprintf(f, "%s ", fp.label.c_str());
    for (std::size_t i = 0; i < fp.pcs.size(); ++i) {
        if (i) std::fputc(',', f);
        std::fprintf(f, "0x%08X", fp.pcs[i]);
    }
    std::fputc('\n', f);
    std::fclose(f);
}

// ---- session-wide function-discovery tracking ----------------------------
// U3: distinct entry pcs ever recorded, all-time (seeded from loaded
// fingerprints at startup, then grows as this session discovers more) and
// this-session-only. Growth per window close is bounded by that window's
// distinct pc count, not by how many windows the session has closed so far.
std::unordered_set<std::uint32_t> g_all_seen_pcs;
std::unordered_set<std::uint32_t> g_session_seen_pcs;

// U2: feedback for the window that just closed, kept on screen until the
// next one closes (see close_and_reopen_window()).
bool g_has_last_close = false;
std::string g_last_close_label;
std::uint32_t g_last_close_distinct = 0;
std::size_t g_last_close_new = 0;

// Loads logs/trace_fingerprints.txt into g_fingerprints at startup. Missing
// or malformed lines are skipped rather than failing the run -- this file
// is a convenience cache, never load-bearing for the run itself.
void load_fingerprints() {
    g_fingerprints.clear();
    std::ifstream in(kFingerprintsPath);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const std::size_t sp = line.find(' ');
        if (sp == std::string::npos || sp == 0) continue;
        Fingerprint fp;
        fp.label = line.substr(0, sp);
        std::size_t pos = sp + 1;
        while (pos < line.size()) {
            const std::size_t comma = line.find(',', pos);
            const std::string tok = line.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!tok.empty()) {
                char* end = nullptr;
                const unsigned long v = std::strtoul(tok.c_str(), &end, 0);
                if (end != tok.c_str()) fp.pcs.push_back(static_cast<std::uint32_t>(v));
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        std::sort(fp.pcs.begin(), fp.pcs.end());
        for (std::uint32_t pc : fp.pcs) g_all_seen_pcs.insert(pc);
        g_fingerprints.push_back(std::move(fp));
    }
}

// ---- current capture window --------------------------------------------
std::uint32_t g_window_index = 0;
std::string g_window_label = "unlabeled";
std::uint64_t g_window_start_frame = 0;
std::uint32_t g_window_distinct = 0;
std::uint64_t g_window_total_calls = 0;
std::string g_session_dir;
bool g_session_ready = false;
// Overlay bank names seen during the current window (see
// gsr_current_overlay_name in runner_main.cpp), folded in every frame by
// check_hardware_boundaries() (see below). Usually holds one name; a
// window that straddles an overlay swap holds more.
std::vector<std::string> g_window_overlays;

// ---- screenshots (item 4) ------------------------------------------------
std::uint64_t g_screenshot_count = 0;

char g_label_buf[64] = "";

// D1: whether the tracer's ImGui window was focused as of its last draw.
// Updated once per draw_tracer_window() call; read by tracer_wants_keyboard()
// from the SDL event pump, which runs before this frame's draw -- so it
// reflects the previous frame's focus state, the same one-frame lag every
// other ImGui-driven input gate in this codebase already has.
bool g_window_focused = false;

std::string sanitize_label(const std::string& raw) {
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
    if (out.empty()) out = "unlabeled";
    return out;
}

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
    g_session_dir = std::string("logs/trace_") + stamp;
    std::error_code ec;
    std::filesystem::create_directories(g_session_dir, ec);
    g_session_ready = true;
}

// Item 5: current value of the unverified candidate room identity, read
// fresh (never cached) so every window's header/index line reflects the
// value at close time. NEVER used to drive behaviour -- see
// gsr_map_record_identity's declaration comment.
struct RoomIdentitySnapshot {
    bool valid = false;
    std::uint32_t room_ptr = 0, min_x = 0, min_y = 0, ext_x = 0, ext_y = 0;
};

RoomIdentitySnapshot read_room_identity() {
    RoomIdentitySnapshot s;
    s.valid = gsr_map_record_identity(&s.room_ptr, &s.min_x, &s.min_y,
                                      &s.ext_x, &s.ext_y);
    return s;
}

std::string format_room_identity(const RoomIdentitySnapshot& s) {
    char buf[160];
    if (s.valid) {
        std::snprintf(buf, sizeof(buf),
                     "room_ptr=0x%08X min_x=0x%08X min_y=0x%08X "
                     "ext_x=0x%08X ext_y=0x%08X",
                     s.room_ptr, s.min_x, s.min_y, s.ext_x, s.ext_y);
    } else {
        std::snprintf(buf, sizeof(buf), "room_ptr=unavailable");
    }
    return buf;
}

// F4: short stable tag for the current room, derived from the bounds tuple
// alone (min_x, min_y, ext_x, ext_y) -- NOT room_ptr, which was measured
// constant (0x02030CCC) across an entire session and so carries no signal.
// FNV-1a over the four raw words, rendered as 6 hex digits. Purely a label
// so captures from the same room share a visible tag in their window name
// and the index line; the full bounds stay in the header via
// format_room_identity() above. Returns empty when the identity isn't
// valid (no room loaded yet).
std::string room_signature_tag(const RoomIdentitySnapshot& s) {
    if (!s.valid) return std::string();
    std::uint32_t h = 2166136261u;
    const auto mix_u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<std::uint8_t>(v & 0xFFu);
            h *= 16777619u;
            v >>= 8;
        }
    };
    mix_u32(s.min_x);
    mix_u32(s.min_y);
    mix_u32(s.ext_x);
    mix_u32(s.ext_y);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06x", h & 0xFFFFFFu);
    return buf;
}

void write_index_line(std::uint32_t index, const std::string& label,
                       std::uint64_t start_frame, std::uint64_t end_frame,
                       const std::string& overlays,
                       const RoomIdentitySnapshot& room) {
    const std::string index_path = g_session_dir + "/index.txt";
    FILE* f = std::fopen(index_path.c_str(), "a");
    if (!f) {
        note_write_failure(index_path);
        return;
    }
    const std::uint64_t duration_frames =
        end_frame >= start_frame ? end_frame - start_frame : 0;
    const double duration_s =
        static_cast<double>(duration_frames) / gbarecomp::kGbaFrameHz;
    std::fprintf(f,
                 "%03u label=%s start_frame=%llu end_frame=%llu "
                 "duration_frames=%llu duration_seconds=%.2f overlays=%s "
                 "%s\n",
                 index, label.c_str(),
                 static_cast<unsigned long long>(start_frame),
                 static_cast<unsigned long long>(end_frame),
                 static_cast<unsigned long long>(duration_frames),
                 duration_s, overlays.c_str(),
                 format_room_identity(room).c_str());
    std::fclose(f);
}

// Joins a sorted, deduped name list with commas -- used for the overlay set
// both in per-window/index output and in the repeats file.
std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out.push_back(',');
        out += names[i];
    }
    return out;
}

// ---- repeat matching (isolating an action across repeated captures) -----
// D2: running per-label state, updated incrementally at each matching
// window close instead of recomputed from every WindowRecord ever stored --
// no per-window pc sets are retained beyond this merge, so both memory and
// window-close cost stay bounded by one (capped) window's size, not by how
// many windows the label has accumulated this session.
struct Band {
    std::uint32_t min_count;
    std::uint32_t max_count;
};
struct LabelRepeatState {
    std::size_t count = 0;  // windows closed under this label so far
    std::vector<std::pair<std::uint32_t, Band>> live;  // running intersection + bands
    std::vector<std::string> overlays;  // union across all windows, sorted+deduped
};
std::unordered_map<std::string, LabelRepeatState> g_label_repeat;

// Folds one just-closed window's capped (entry_pc, count) snapshot (see
// capped_window_pcs()) into the running intersection for `label`.
void merge_repeat_window(
    const std::string& label,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& capped_pcs,
    const std::vector<std::string>& overlays) {
    LabelRepeatState& st = g_label_repeat[label];
    if (st.count == 0) {
        st.live.reserve(capped_pcs.size());
        for (const auto& kv : capped_pcs)
            st.live.emplace_back(kv.first, Band{kv.second, kv.second});
    } else {
        std::vector<std::pair<std::uint32_t, Band>> merged;
        merged.reserve(st.live.size());
        std::size_t a = 0, b = 0;
        while (a < st.live.size() && b < capped_pcs.size()) {
            if (st.live[a].first < capped_pcs[b].first) {
                ++a;
            } else if (capped_pcs[b].first < st.live[a].first) {
                ++b;
            } else {
                Band band = st.live[a].second;
                band.min_count = std::min(band.min_count, capped_pcs[b].second);
                band.max_count = std::max(band.max_count, capped_pcs[b].second);
                merged.emplace_back(st.live[a].first, band);
                ++a;
                ++b;
            }
        }
        st.live = std::move(merged);
    }
    ++st.count;
    for (const std::string& name : overlays) {
        if (std::find(st.overlays.begin(), st.overlays.end(), name) ==
            st.overlays.end()) {
            st.overlays.push_back(name);
        }
    }
    std::sort(st.overlays.begin(), st.overlays.end());
}

// Rewrites logs/trace_<timestamp>/repeats/<label>.txt from the running
// state above. Only meaningful once a label has 2+ captures; a no-op
// before that, same as before.
void write_repeats_file(const std::string& label) {
    const auto it = g_label_repeat.find(label);
    if (it == g_label_repeat.end() || it->second.count < 2) return;
    const LabelRepeatState& st = it->second;

    ensure_session_dir();
    const std::string dir = g_session_dir + "/repeats";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/" + label + ".txt";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        note_write_failure(path);
        return;
    }
    std::fprintf(f, "repeats=%zu intersection=%zu overlays=%s\n", st.count,
                 st.live.size(), join_names(st.overlays).c_str());
    for (const auto& kv : st.live) {
        std::fprintf(f, "0x%08X,min=%u,max=%u\n", kv.first,
                     kv.second.min_count, kv.second.max_count);
    }
    std::fclose(f);
}

// Item 4: saves a screenshot for every window that closes (no newness
// gate -- measured over a 699-window session, gating on fingerprint novelty
// produced only 12 screenshots, far too few to label anything). Must be
// called BEFORE this window's own fingerprint is pushed (see
// close_and_reopen_window()) for ordering consistency with the fingerprint
// append, though the fingerprint match itself is no longer consulted here.
// Reads the most recently latched (VBlank-time) frame via
// gbarecomp::active_ppu() -- see the header comment on
// gba::GbaPpu::latched_framebuffer(). Not the hot path: runs once per
// window close.
void maybe_save_screenshot(const std::string& base_name) {
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!ppu || !ppu->has_latched_framebuffer()) return;

    ensure_session_dir();
    const std::string path = g_session_dir + "/" + base_name + ".png";
    if (gbarecomp::write_png(path, ppu->latched_framebuffer(),
                             ppu->render_width(), ppu->render_height())) {
        ++g_screenshot_count;
    } else {
        note_write_failure(path);
    }
}

// Writes the current window to disk under `label`, appends it to the
// session index, saves a screenshot if the context looks new (item 4),
// then resets the table and opens a fresh window. Called both from
// check_hardware_boundaries() (auto-detected boundaries) and from the
// manual "Mark window" button.
// Defined below with the text-speed probe. Flushed here, on every window
// close, so a marked Normal/Fast window's last letters survive whatever the
// session does next -- signals.csv lost its tail exactly this way in session
// 20260910_144421 (FACTS.md).
void flush_text_log();
void flush_text_budget_log();

void close_and_reopen_window(const std::string& label) {
    ensure_session_dir();
    flush_text_log();
    flush_text_budget_log();
    const std::string safe = sanitize_label(label);

    std::sort(g_window_overlays.begin(), g_window_overlays.end());
    const std::string overlays_joined = join_names(g_window_overlays);
    const RoomIdentitySnapshot room = read_room_identity();  // item 5

    // F4: tag the window's visible/on-disk name with the room signature so
    // captures from the same room share a spottable substring. Only the
    // display name changes -- `safe` itself stays the fingerprint/repeats
    // grouping key (unchanged), so a manual label typed in different rooms
    // still intersects as one action.
    const std::string room_tag = room_signature_tag(room);
    const std::string display_name =
        room_tag.empty() ? safe : (safe + "_room" + room_tag);

    char name[112];
    std::snprintf(name, sizeof(name), "/%03u_%s.txt", g_window_index,
                  display_name.c_str());
    const std::string window_path = g_session_dir + name;
    FILE* f = std::fopen(window_path.c_str(), "w");
    if (f) {
        std::fprintf(f, "# overlays=%s %s\n", overlays_joined.c_str(),
                     format_room_identity(room).c_str());
        for (const TracerSlot& s : g_table) {
            if (s.count == 0) continue;
            std::fprintf(f, "0x%08X,%u,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
                         s.entry_pc, s.count, s.r0, s.r1, s.r2, s.r3, s.r14);
        }
        std::fclose(f);
    } else {
        note_write_failure(window_path);
    }

    // U2/U3: fold this window's distinct pcs into the session/all-time seen
    // sets before the table resets, counting how many were never in either
    // set before now.
    std::size_t new_count = 0;
    for (const TracerSlot& s : g_table) {
        if (s.count == 0) continue;
        g_session_seen_pcs.insert(s.entry_pc);
        if (g_all_seen_pcs.insert(s.entry_pc).second) ++new_count;
    }
    g_has_last_close = true;
    g_last_close_label = display_name;
    g_last_close_distinct = g_window_distinct;
    g_last_close_new = new_count;

    const std::uint64_t end_frame = runtime_current_frame();
    if (g_text_record) {
        text_trace_window_closed(g_window_index, display_name.c_str(),
                                 g_window_start_frame, end_frame);
    }
    write_index_line(g_window_index, display_name, g_window_start_frame,
                     end_frame, overlays_joined, room);

    // D2: one capped, pc-sorted snapshot of this window feeds the
    // fingerprint and the repeat-matching merge below -- see
    // capped_window_pcs().
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> capped_pcs =
        capped_window_pcs();

    // Item 4: save BEFORE this window's own fingerprint is pushed, for
    // ordering consistency with the fingerprint append below.
    maybe_save_screenshot(display_name);

    // Fingerprint this window under its name before the table resets, then
    // append it -- see build_fingerprint()/append_fingerprint().
    g_fingerprints.push_back(build_fingerprint(safe, capped_pcs));
    append_fingerprint(g_fingerprints.back());

    // Repeat matching: fold this window into the running intersection for
    // its name, then rewrite that name's repeats file -- see
    // merge_repeat_window()/write_repeats_file(). Auto-generated names are
    // unique per window, so this only actually accumulates (2+) for the
    // manual "Mark window" label, which is the intended use (isolating a
    // repeated user action across several captures under the same typed
    // label).
    merge_repeat_window(safe, capped_pcs, g_window_overlays);
    write_repeats_file(safe);

    // Cheap reset: one memset of the fixed table rather than per-slot work.
    std::memset(g_table, 0, sizeof(g_table));
    g_window_distinct = 0;
    g_window_total_calls = 0;
    g_window_overlays.clear();
    ++g_window_index;
    g_window_label = "unlabeled";
    g_window_start_frame = end_frame;
    if (g_text_record)
        text_trace_window_opened(g_window_index, g_window_start_frame);
}

// Item 3: builds a filesystem-safe, sequence-first auto name from what
// triggered the boundary plus a short stable hash disambiguating windows
// that share a reason (e.g. two separate fades). The hash is FNV-1a over
// the reason string plus this window's own start frame and call count --
// real, already-tracked values, not a guessed/random suffix.
std::string generated_window_name(const char* reason) {
    std::uint32_t h = 2166136261u;
    const auto mix_byte = [&](std::uint8_t b) {
        h ^= b;
        h *= 16777619u;
    };
    for (const char* p = reason; *p; ++p) mix_byte(static_cast<std::uint8_t>(*p));
    const auto mix_u64 = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            mix_byte(static_cast<std::uint8_t>(v));
            v >>= 8;
        }
    };
    mix_u64(g_window_start_frame);
    mix_u64(g_window_total_calls);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s_%06x", reason, h & 0xFFFFFFu);
    return buf;
}

void close_and_reopen_window_auto(const char* reason) {
    close_and_reopen_window(generated_window_name(reason));
}

// ---- hardware boundary detection (item 2), sampled once per new guest
// frame from function_tracer_on_entry() ------------------------------------
// Fade: BLDY (io[0x54], low 5 bits = EVY). Leaving zero and returning to
// zero brackets one whole fade sequence (ramp out, optional hold, ramp
// back in -- or a single short pulse) as ONE boundary, fired on the
// return-to-zero edge; the rising edge alone never fires anything, so this
// cannot itself produce two windows for one fade. A short debounce guards
// only against BLDY momentarily reading 0 for a single frame mid-sequence.
// F2: measured over a 699-window/55-minute session, this rule alone fired
// 457 boundaries (median capture length 35 frames / 0.6s) -- Golden Sun
// uses brightness blending for ordinary effects, not just scene
// transitions, so a bare zero->nonzero->zero bracket fires constantly.
// Only a fade that actually reached near-full intensity is treated as a
// scene transition now: the peak BLDY seen during the bracket is tracked
// and checked against kFadeFullThreshold on the return-to-zero edge.
// TODO-EVIDENCE: a fade that ramps to black/white and never returns to
// zero (game cuts away while held) would not fire a boundary under this
// rule; no observed Golden Sun trace establishes whether that pattern
// occurs, so it is not separately instrumented rather than guessed at.
bool g_fade_in_progress = false;
std::uint8_t g_fade_peak_bldy = 0;  // F2: peak BLDY seen during current bracket
// F2: BLDY is a 5-bit field (0..31); a fade must peak at or above this to
// count as a scene transition rather than an ordinary blend effect. Named
// so it can be retuned from real capture data without hunting for a magic
// number in the branch below.
constexpr std::uint8_t kFadeFullThreshold = 28;
bool g_have_fade_boundary = false;
std::uint64_t g_last_fade_boundary_frame = 0;
constexpr std::uint64_t kFadeDebounceFrames = 30;  // ~0.5s at kGbaFrameHz

// Window registers ("window_open"/"window_close", F3 -- previously named
// "textbox_open"/"textbox_close"): DISPCNT (io[0x00..0x01]) window-enable
// bits 13-15, plus WIN0H/V/WIN1H/V (io[0x40..0x47]). Golden Sun reuses the
// GBA hardware window feature for both dialogue boxes and iris
// transitions -- a saved capture labelled "textbox_open" turned out to be
// a circular iris wipe, not dialogue, so the signal is genuinely useful but
// was misnamed. Any change fires "open" (enable-bit popcount increased, or
// bounds changed with popcount unchanged -- a new box on the same channel)
// or "close" (popcount decreased) as separate boundaries, per item 2. Not
// debounced: these are discrete register-state changes, not a multi-frame
// ramp.
bool g_win_state_valid = false;
// Window registers animate (a box sliding open, effects), so an
// undebounced fire would close a window every frame and produce thousands
// of captures and screenshots. Debounced like the fade path.
bool g_have_win_boundary = false;
std::uint64_t g_last_win_boundary_frame = 0;
constexpr std::uint64_t kWinDebounceFrames = 30;  // ~0.5s at kGbaFrameHz
std::uint8_t g_prev_win_enable = 0;
std::uint16_t g_prev_win0h = 0, g_prev_win0v = 0;
std::uint16_t g_prev_win1h = 0, g_prev_win1v = 0;

// F4: room bounds (min_x, min_y, ext_x, ext_y) via gsr_map_record_identity
// -- see that declaration's comment in function_tracer.h for what is and
// isn't verified about this data. Measured over the same 699-window
// session: the room pointer alongside these bounds was constant
// (0x02030CCC) throughout, useless as an identity, but the four bounds
// values took 23 distinct combinations, tracking real room changes. This
// is the one deliberate exception to "never branch on this value": the
// tracer is a diagnostic tool that only labels/buckets captures for a
// human afterwards, never feeds back into game state or rendering, so
// using an unverified-but-measured signal here to bucket captures carries
// none of the risk that branching on it in gameplay-affecting code would.
// Debounced the same way as the fade boundary above.
bool g_room_state_valid = false;
bool g_have_room_boundary = false;
std::uint64_t g_last_room_boundary_frame = 0;
constexpr std::uint64_t kRoomDebounceFrames = kFadeDebounceFrames;
std::uint32_t g_prev_room_min_x = 0, g_prev_room_min_y = 0;
std::uint32_t g_prev_room_ext_x = 0, g_prev_room_ext_y = 0;

// Overlay bank set (see gsr_overlay_name_at). Compared against last
// frame's set; kTransientCodeImages is a handful of rows (see its own
// "linear scan... is fine" comment), so scanning it every frame stays
// cheap.
bool g_overlay_state_valid = false;
std::vector<std::string> g_prev_overlay_set;

int popcount3(std::uint8_t bits) {
    return ((bits >> 0) & 1) + ((bits >> 1) & 1) + ((bits >> 2) & 1);
}

// ---- raw signal log (T2) --------------------------------------------------
// The fade threshold has now been guessed wrong in both directions (457
// boundaries, then 0 -- see kFadeFullThreshold's comment above), and the
// 56-minute single-capture gap this was added to diagnose (see
// check_hardware_boundaries' debounce fix above) showed the same problem
// for the window/room debounce windows: there is no real per-frame register
// data to tune any of these constants from. This writes one row per frame
// -- independent of whether a boundary actually fires that frame, and
// before check_hardware_boundaries' own early returns -- to
// logs/trace_<timestamp>/signals.csv, buffered in memory and flushed every
// kSignalFlushRows rows (never opened per frame). Overlay names are '|'-
// joined, not ','-joined like join_names() elsewhere, so a multi-overlay
// frame can't be mistaken for extra CSV columns.
std::string g_signal_buf;
std::size_t g_signal_buf_rows = 0;
bool g_signal_header_written = false;
constexpr std::size_t kSignalFlushRows = 300;  // ~5s at kGbaFrameHz

void flush_signal_log() {
    if (g_signal_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/signals.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        note_write_failure(path);
        g_signal_buf.clear();
        g_signal_buf_rows = 0;
        return;
    }
    std::fwrite(g_signal_buf.data(), 1, g_signal_buf.size(), f);
    std::fclose(f);
    g_signal_buf.clear();
    g_signal_buf_rows = 0;
}

// Previous frame's g_ws_obj_*_total / g_ws_expanded_diag[2] totals, so the
// signal log can report the per-frame delta instead of the session-running
// total those counters accumulate to (see the header comment below for why
// a delta is what a marked window needs).
std::uint64_t g_prev_obj_trusted_total = 0;
std::uint64_t g_prev_obj_untrusted_total = 0;
std::uint64_t g_prev_obj_culled_total = 0;

// Defined in runner_main.cpp, not gba_ppu.*: only golden_sun_obj_provider_
// provenance's usable() predicate knows *why* a candidate was rejected
// (no record, stale epoch, pending-frame mismatch, identity mismatch,
// attrs moved, or truncation mismatch), versus just that it was. Same
// once-per-frame-per-object gate and running-total-since-session-start
// shape as g_ws_obj_trusted_total/g_ws_obj_untrusted_total above.
// Indices 7..11 break down index 4 (attrs_moved) further: 7/8 split it by
// whether the held record is from this frame or an older one (recorder
// keeping up vs. not), and 9/10/11 flag which of ATTR0/1/2 actually differs
// (may overlap). See runner_main.cpp's golden_sun_obj_provider_provenance
// for the attribution rule; this file only reads the running totals.
extern "C" unsigned long long g_ws_obj_reject_totals[12];
std::uint64_t g_prev_obj_reject_totals[12] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// Defined in runner_main.cpp: per-OAM-upload tally of which sprite table an
// upload's source address belongs to (0 primary, 1 alt, 2 the 0x03002000
// address, 3 other). Same running-total-since-session-start shape as
// g_ws_obj_reject_totals above; the tracer reports the per-frame delta.
extern "C" unsigned long long g_ws_oam_src_totals[4];
std::uint64_t g_prev_oam_src_totals[4] = {0, 0, 0, 0};

// Defined in runner_main.cpp: per-object-per-frame tally of sprites resolved
// via the persistent position table (golden_sun_obj_track_lookup) rather
// than the record path or the raw-coordinate fallback. Same running-total-
// since-session-start shape as g_ws_obj_reject_totals above.
extern "C" unsigned long long g_ws_obj_from_track_total;
std::uint64_t g_prev_obj_from_track_total = 0;

// ---- OBJ parked-vs-room census (measurement only, no rendering change) ---
//
// Tests whether the ROOM rect (not the 240x160 hardware screen) separates
// Golden Sun's parked/hidden sprites from real ones when a sprite's world
// position is reconstructed from the camera plus its raw OAM coordinate,
// instead of from an authenticated provider record (see ROADMAP.md /
// FACTS.md 2026-09-11: authenticated records collapse to 0/frame during
// Move Psynergy, which is why a camera+raw reconstruction is the candidate
// replacement under test here). This does not feed rendering or the room
// buffer; it only counts, once per frame, into new signals.csv columns.
struct ObjParkCensus {
    std::uint32_t live = 0;
    std::uint32_t in_room = 0;
    std::uint32_t out_room = 0;
    std::uint32_t no_room = 0;
};

// Mirrors room_buffer.cpp's kEwramBase/read_u16 exactly (that file's helpers
// are anonymous-namespace-local and not reachable here, so the layout is
// re-read rather than the linkage altered).
constexpr std::uint32_t kCensusEwramBase = 0x02000000u;
constexpr std::uint32_t kCensusEwramMask = 0x0003FFFFu;
constexpr std::uint32_t kCensusRoomRect = 0x02030DC0u;  // min_x,max_x,min_y,max_y u16
constexpr std::uint32_t kCensusCamera = 0x02030DB0u;    // x,y, 16.16 fixed point

std::uint16_t census_read_u16(const std::uint8_t* ewram, std::uint32_t address) {
    const std::uint32_t off = (address - kCensusEwramBase) & kCensusEwramMask;
    return static_cast<std::uint16_t>(ewram[off] | (ewram[off + 1] << 8));
}

// Mirrors room_buffer.cpp's rect_is_room() exactly: excludes the all-zero
// between-rooms marker and the (0,1,8,256) world-map constant, and requires
// real extent of at least one 16px cell per axis.
bool census_rect_is_room(std::uint16_t min_x, std::uint16_t max_x,
                         std::uint16_t min_y, std::uint16_t max_y) {
    if (max_x == 1 && min_y == 8 && max_y == 256) return false;
    if (max_x <= min_x || max_y <= min_y) return false;
    return (max_x - min_x) >= 16 && (max_y - min_y) >= 16;
}

ObjParkCensus compute_obj_park_census() {
    ObjParkCensus c;
    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return c;
    const std::uint8_t* ewram = bus->ewram_ptr();
    const std::uint8_t* oam = bus->oam_ptr();
    if (!ewram || !oam) return c;

    const std::uint16_t min_x = census_read_u16(ewram, kCensusRoomRect);
    const std::uint16_t max_x = census_read_u16(ewram, kCensusRoomRect + 2);
    const std::uint16_t min_y = census_read_u16(ewram, kCensusRoomRect + 4);
    const std::uint16_t max_y = census_read_u16(ewram, kCensusRoomRect + 6);
    const bool room_valid = census_rect_is_room(min_x, max_x, min_y, max_y);

    // Camera is 16.16 fixed point; the integer part is the high u16 of each
    // 4-byte field (x at +0, y at +4), same as room_buffer.cpp.
    const int cam_x = static_cast<int>(census_read_u16(ewram, kCensusCamera + 2));
    const int cam_y = static_cast<int>(census_read_u16(ewram, kCensusCamera + 6));

    for (int idx = 0; idx < 128; ++idx) {
        const std::uint8_t* entry = oam + idx * 8;
        const std::uint16_t attr0 =
            static_cast<std::uint16_t>(entry[0] | (entry[1] << 8));
        const std::uint16_t attr1 =
            static_cast<std::uint16_t>(entry[2] | (entry[3] << 8));

        // Mirrors gba_ppu.cpp's OBJ loop skips exactly (~1601-1609), in the
        // same order, so these counts are comparable with obj_trusted/
        // obj_untrusted.
        const bool rot_scale = (attr0 & 0x0100u) != 0;
        const bool disable_or_double = (attr0 & 0x0200u) != 0;
        if (!rot_scale && disable_or_double) continue;
        const std::uint32_t obj_mode = (attr0 >> 10) & 0x3u;
        if (obj_mode == 2 || obj_mode == 3) continue;
        const std::uint32_t shape = (attr0 >> 14) & 0x3u;
        if (shape >= 3) continue;

        ++c.live;
        if (!room_valid) continue;  // no_room is set from live below.

        // Reconstruction under test: NOT the renderer's trusted path (which
        // prefers an authenticated provider position). This mirrors only
        // the untrusted-fallback arithmetic in gba_ppu.cpp (~1611-1617,
        // ~1653-1655): raw OAM coordinate, sign-extended, plus camera.
        const int raw_y = static_cast<int>(attr0 & 0xFFu);
        const int raw_x = static_cast<int>(attr1 & 0x1FFu);
        const int sy = (raw_y >= 160) ? raw_y - 256 : raw_y;
        const int sx = (raw_x & 0x100) ? raw_x - 0x200 : raw_x;
        const int world_x = sx + cam_x;
        const int world_y = sy + cam_y;

        if (world_x >= min_x && world_x < max_x && world_y >= min_y &&
            world_y < max_y) {
            ++c.in_room;
        } else {
            ++c.out_room;
        }
    }
    if (!room_valid) c.no_room = c.live;
    return c;
}

void log_raw_signals(std::uint64_t frame, const std::uint8_t* io) {
    if (!g_signal_header_written) {
        g_signal_buf +=
            "frame,bldy,win_enable,win0h,win0v,win1h,win1v,room_valid,"
            "min_x,min_y,ext_x,ext_y,overlays,"
            "field_sig,bg1_base,bg2_base,bg3_base,"
            "obj_untrusted,obj_trusted,obj_culled,"
            "obj_rej_no_record,obj_rej_epoch,obj_rej_pending,obj_rej_identity,"
            "obj_rej_attrs,obj_rej_trunc,obj_ok,"
            "obj_rej_attrs_this_frame,obj_rej_attrs_old_frame,obj_rej_attr0,"
            "obj_rej_attr1,obj_rej_attr2,"
            "obj_live,obj_in_room,obj_out_room,obj_no_room,"
            "oam_src_primary,oam_src_alt,oam_src_0x03002000,oam_src_other,"
            "obj_from_track\n";
        g_signal_header_written = true;
    }
    const std::uint8_t bldy = io[0x54] & 0x1Fu;
    const std::uint16_t dispcnt =
        static_cast<std::uint16_t>(io[0x00] | (io[0x01] << 8));
    const std::uint8_t win_enable =
        static_cast<std::uint8_t>((dispcnt >> 13) & 0x7u);
    const std::uint16_t win0h =
        static_cast<std::uint16_t>(io[0x40] | (io[0x41] << 8));
    const std::uint16_t win1h =
        static_cast<std::uint16_t>(io[0x42] | (io[0x43] << 8));
    const std::uint16_t win0v =
        static_cast<std::uint16_t>(io[0x44] | (io[0x45] << 8));
    const std::uint16_t win1v =
        static_cast<std::uint16_t>(io[0x46] | (io[0x47] << 8));
    const RoomIdentitySnapshot room = read_room_identity();
    std::string overlays;
    for (std::size_t i = 0;; ++i) {
        const char* name = gsr_overlay_name_at(i);
        if (!name) break;
        if (!overlays.empty()) overlays.push_back('|');
        overlays += name;
    }

    // Field screen-base signature (mirrors room_buffer.cpp's
    // is_field_signature(), which is anonymous-namespace-local to that TU
    // and not reachable here): BG3CNT/BG2CNT/BG1CNT screen-base fields
    // (bits 8-12 of IO 0x0E/0x0C/0x0A) must read 5/6/7 respectively for the
    // room buffer to accept the frame. A menu that repoints a screen base
    // makes the room buffer decline and the margin falls back to the
    // hardware's wrapped ring; the existing refusal counters are session
    // totals printed at exit and cannot say which frames, which is why
    // this is logged per frame instead.
    const std::uint16_t bg1cnt =
        static_cast<std::uint16_t>(io[0x0A] | (io[0x0B] << 8));
    const std::uint16_t bg2cnt =
        static_cast<std::uint16_t>(io[0x0C] | (io[0x0D] << 8));
    const std::uint16_t bg3cnt =
        static_cast<std::uint16_t>(io[0x0E] | (io[0x0F] << 8));
    const std::uint8_t bg1_base =
        static_cast<std::uint8_t>((bg1cnt >> 8) & 0x1Fu);
    const std::uint8_t bg2_base =
        static_cast<std::uint8_t>((bg2cnt >> 8) & 0x1Fu);
    const std::uint8_t bg3_base =
        static_cast<std::uint8_t>((bg3cnt >> 8) & 0x1Fu);
    const int field_sig =
        (bg3_base == 5u && bg2_base == 6u && bg1_base == 7u) ? 1 : 0;

    // OBJ trust/cull deltas since the previous row. obj_untrusted high with
    // obj_culled flat means objects never had an authenticated position;
    // obj_culled rising means they had one and the expanded cull box
    // rejected it -- that pair separates the two leads for the bottom-edge
    // culling defect. Totals are running session counters accumulated by
    // the PPU (gba_ppu.cpp) and never reset here.
    // gba_ppu.h declares these inside namespace gba, so they need the
    // qualification here even though they have C linkage.
    const std::uint64_t obj_trusted_total = gba::g_ws_obj_trusted_total;
    const std::uint64_t obj_untrusted_total = gba::g_ws_obj_untrusted_total;
    const std::uint64_t obj_culled_total = gba::g_ws_expanded_diag[2];
    const std::uint64_t obj_trusted_delta =
        obj_trusted_total - g_prev_obj_trusted_total;
    const std::uint64_t obj_untrusted_delta =
        obj_untrusted_total - g_prev_obj_untrusted_total;
    const std::uint64_t obj_culled_delta =
        obj_culled_total - g_prev_obj_culled_total;
    g_prev_obj_trusted_total = obj_trusted_total;
    g_prev_obj_untrusted_total = obj_untrusted_total;
    g_prev_obj_culled_total = obj_culled_total;

    // Per-reject-reason deltas, same shape as obj_trusted/obj_untrusted
    // above: distinguishes an untrusted object that never had a provenance
    // record (obj_rej_no_record) from one whose record went stale after the
    // sprite moved (obj_rej_epoch/pending/identity/attrs/trunc), which is
    // the question the two totals above can't answer on their own.
    // g_ws_obj_reject_totals is defined in runner_main.cpp, not gba_ppu.*,
    // so (unlike the gba:: symbols above) it is not namespace-qualified.
    std::uint64_t obj_reject_delta[12];
    for (int i = 0; i < 12; ++i) {
        const std::uint64_t total = g_ws_obj_reject_totals[i];
        obj_reject_delta[i] = total - g_prev_obj_reject_totals[i];
        g_prev_obj_reject_totals[i] = total;
    }

    // Per-frame (not running-total) OBJ parked-vs-room census; see
    // compute_obj_park_census() above.
    const ObjParkCensus census = compute_obj_park_census();

    // Per-OAM-upload source-table deltas, same shape as obj_reject_delta
    // above: g_ws_oam_src_totals is a session-running total, this reports
    // the per-frame delta.
    std::uint64_t oam_src_delta[4];
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t total = g_ws_oam_src_totals[i];
        oam_src_delta[i] = total - g_prev_oam_src_totals[i];
        g_prev_oam_src_totals[i] = total;
    }

    // obj_from_track delta, same shape as obj_trusted/obj_untrusted above;
    // appended last so the existing columns keep their positions.
    const std::uint64_t obj_from_track_total = g_ws_obj_from_track_total;
    const std::uint64_t obj_from_track_delta =
        obj_from_track_total - g_prev_obj_from_track_total;
    g_prev_obj_from_track_total = obj_from_track_total;

    // 420 covered the columns through obj_ok; the 5 appended attrs_moved
    // breakdown columns add up to 5 more "%llu," fields (<=20 digits each),
    // the 4 obj_* census columns add up to 4 more "%u," fields, the 4
    // oam_src_* columns add up to 4 more "%llu," fields, and obj_from_track
    // below adds one more "%llu" field, so grow the buffer with headroom
    // rather than trim it tight.
    char line[800];
    const int n = std::snprintf(line, sizeof(line),
        "%llu,%u,%u,%u,%u,%u,%u,%d,%08X,%08X,%08X,%08X,%s,"
        "%d,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,"
        "%u,%u,%u,%u,"
        "%llu,%llu,%llu,%llu,"
        "%llu\n",
        static_cast<unsigned long long>(frame), bldy, win_enable,
        win0h, win0v, win1h, win1v, room.valid ? 1 : 0,
        room.min_x, room.min_y, room.ext_x, room.ext_y, overlays.c_str(),
        field_sig, bg1_base, bg2_base, bg3_base,
        static_cast<unsigned long long>(obj_untrusted_delta),
        static_cast<unsigned long long>(obj_trusted_delta),
        static_cast<unsigned long long>(obj_culled_delta),
        static_cast<unsigned long long>(obj_reject_delta[0]),
        static_cast<unsigned long long>(obj_reject_delta[1]),
        static_cast<unsigned long long>(obj_reject_delta[2]),
        static_cast<unsigned long long>(obj_reject_delta[3]),
        static_cast<unsigned long long>(obj_reject_delta[4]),
        static_cast<unsigned long long>(obj_reject_delta[5]),
        static_cast<unsigned long long>(obj_reject_delta[6]),
        static_cast<unsigned long long>(obj_reject_delta[7]),
        static_cast<unsigned long long>(obj_reject_delta[8]),
        static_cast<unsigned long long>(obj_reject_delta[9]),
        static_cast<unsigned long long>(obj_reject_delta[10]),
        static_cast<unsigned long long>(obj_reject_delta[11]),
        census.live, census.in_room, census.out_room, census.no_room,
        static_cast<unsigned long long>(oam_src_delta[0]),
        static_cast<unsigned long long>(oam_src_delta[1]),
        static_cast<unsigned long long>(oam_src_delta[2]),
        static_cast<unsigned long long>(oam_src_delta[3]),
        static_cast<unsigned long long>(obj_from_track_delta));
    if (n > 0) {
        g_signal_buf.append(line, static_cast<std::size_t>(
            n < static_cast<int>(sizeof(line)) ? n : static_cast<int>(sizeof(line)) - 1));
    }

    if (++g_signal_buf_rows >= kSignalFlushRows) flush_signal_log();
}

// ---- text-speed probe (GSR_TEXT_RECORD) -----------------------------------
//
// FACTS.md "Display signals" (2026-09-10) left one thing unmeasured: the
// per-character delay state was found in source, but never observed running,
// so no Message-speed option can be tied to a number and no instant-text fix
// can be claimed. It names the join to make -- the delay state against the
// glyph uploads -- and this is it.
//
// Every PC below is an entry actually observed in that session's Normal and
// Fast windows (logs/trace_20260910_153228/014_Normal.txt, 015_Fast.txt),
// not a guessed function start:
//
//   0x080168F4  text processor, 252 Normal / 190 Fast entries. Its delay
//               check at 0x0801695E decrements a nonzero halfword and
//               returns before processing another character.
//   0x08016E80  entered with r2 = 0x08073808, the delay table itself, 32
//               entries in both windows.
//   0x08016EB0  entered with r2 = r3 = 1 under Normal and 2 under Fast --
//               the single strongest speed signal in that capture, but the
//               tracer keeps only the last call's arguments per window, so
//               it is one sample each and proves nothing on its own.
//   0x08018CAC  glyph routine, 35 Normal / 115 Fast, returning to the text
//               processor's call site (r14 = 0x08016E4D). One entry is one
//               letter drawn, so its frame spacing IS the text speed.
//
// One row per call rather than per window is the whole point: it turns those
// single end-of-window samples into a per-letter series, timestamped by frame
// and stamped with the current window label, so a Normal window and a Fast
// window of the same line can be compared directly. The store ledger below
// records the source's table index at 0x0200044C alongside the base byte at
// 0x02000240. The latter was the old recorder's mislabeled "table_index"
// column; retaining it makes the correction auditable in the next capture.
//
// Flushed every kTextFlushRows rows, unlike signals.csv: that file is flushed
// only every 300 rows and lost its tail in session 20260910_144421 (FACTS.md),
// which is exactly how a capture of the last few letters of a line would be
// lost here.
constexpr std::uint32_t kTextProcessorPc = 0x080168F4u;
constexpr std::uint32_t kTextDelayTablePc = 0x08016E80u;
constexpr std::uint32_t kTextDelayWaitPc = 0x08016EB0u;
constexpr std::uint32_t kTextGlyphPc = 0x08018CACu;
// 0x080168F4 seeds a stack-local counter at 0x08016920, may replace it at
// 0x08016942 from a context-dependent branch, and 0x08016EB0 consumes it at
// 0x08016F00. These are observed as writes so the effective budget is known
// before any behavior change is considered.
constexpr std::uint32_t kTextBudgetBasePc = 0x08016920u;
constexpr std::uint32_t kTextBudgetOverridePc = 0x08016942u;
constexpr std::uint32_t kTextBudgetDecrementPc = 0x08016F00u;
constexpr std::uint32_t kMessageSpeedOptionAddress = 0x0200044Cu;
constexpr std::uint32_t kEwramBaseAddress = 0x02000000u;
constexpr std::uint32_t kEwramMirrorMask = 0x0003FFFFu;  // 256 KiB, matches
                                                         // gba_memory.cpp
constexpr std::size_t kTextFlushRows = 32;

// Bytes of the text-context struct dumped beside every "proc" row.
//
// Measured (FACTS.md, 2026-09-10): Message speed sets how many letters the
// processor emits per frame -- 1 on Normal, ~3 on Fast -- so the control is a
// per-frame budget living in this struct, and source puts the delay state at
// context+0x22. Call counts cannot show a budget's value; the struct can.
// 64 bytes covers 0x00..0x3F, so the field is in range wherever inside the
// struct it turns out to sit, and a Normal capture diffed against a Fast one
// identifies it without guessing the offset up front.
//
// Every argument that looks like an EWRAM pointer is dumped, at every hooked
// entry, rather than one pointer at one entry.
//
// Session 20260910_183742 dumped r2 at the processor entry alone; session
// 20260910_185836 widened that to r0..r3 there. Both came back with no byte
// constant per setting and different between settings, across 518 samples of
// each pointer -- so the budget is not a settled field of either struct as
// seen from THAT sample point. Source stores the delay at R6+0x22 near
// 0x08016E5C and the check at 0x0801695E decrements it, so by the next
// processor entry it has already been spent and reads the same under both
// settings. Sampling at the other three hooked entries catches the struct at
// different points in that cycle, and 0x08016E80 in particular is entered
// with the delay table in r2 and a further EWRAM pointer in r1 that no dump
// has covered yet.
constexpr std::size_t kTextContextBytes = 64;
constexpr std::uint32_t kEwramLimit = 0x02040000u;

// The delay store is observed from runner_main.cpp, while this translation
// unit owns the labeled tracer windows and their session directory. Keep the
// store side bounded by aggregating one row per observed
// (table-index, base-byte, delay) triple in each window.
constexpr std::size_t kTextDelayPairLimit = 64;
struct TextDelayPair {
    std::uint32_t table_index = 0;
    std::uint32_t base_byte = 0;
    std::uint32_t delay = 0;
    std::uint64_t stores = 0;
    std::uint32_t first_context = 0;
    std::uint32_t last_context = 0;
    std::uint32_t context_changes = 0;
};

std::array<TextDelayPair, kTextDelayPairLimit> g_text_delay_pairs{};
std::size_t g_text_delay_pair_count = 0;
std::uint64_t g_text_delay_store_count = 0;
std::uint64_t g_text_delay_overflow_stores = 0;
std::uint32_t g_text_delay_window_index = 0;
std::uint64_t g_text_delay_window_start_frame = 0;
bool g_text_delay_window_active = false;
bool g_text_delay_header_written = false;

std::string g_text_buf;
std::size_t g_text_buf_rows = 0;
bool g_text_header_written = false;
std::string g_text_budget_buf;
std::size_t g_text_budget_buf_rows = 0;
bool g_text_budget_header_written = false;

void flush_text_log() {
    if (g_text_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/text_speed.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        note_write_failure(path);
        g_text_buf.clear();
        g_text_buf_rows = 0;
        return;
    }
    std::fwrite(g_text_buf.data(), 1, g_text_buf.size(), f);
    std::fclose(f);
    g_text_buf.clear();
    g_text_buf_rows = 0;
}

void flush_text_budget_log() {
    if (g_text_budget_buf.empty()) return;
    ensure_session_dir();
    const std::string path = g_session_dir + "/text_budget.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        note_write_failure(path);
        g_text_budget_buf.clear();
        g_text_budget_buf_rows = 0;
        return;
    }
    std::fwrite(g_text_budget_buf.data(), 1, g_text_budget_buf.size(), f);
    std::fclose(f);
    g_text_budget_buf.clear();
    g_text_budget_buf_rows = 0;
}

void reset_text_delay_window(std::uint32_t window_index,
                             std::uint64_t start_frame) {
    g_text_delay_pairs = {};
    g_text_delay_pair_count = 0;
    g_text_delay_store_count = 0;
    g_text_delay_overflow_stores = 0;
    g_text_delay_window_index = window_index;
    g_text_delay_window_start_frame = start_frame;
    g_text_delay_window_active = true;
}

void write_text_delay_window(std::uint32_t window_index,
                             const char* label,
                             std::uint64_t start_frame,
                             std::uint64_t end_frame) {
    ensure_session_dir();
    const std::string path = g_session_dir + "/text_delay.csv";
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) {
        note_write_failure(path);
        return;
    }
    if (!g_text_delay_header_written) {
        std::fprintf(f,
                     "window,label,start_frame,end_frame,total_stores,"
                     "overflow_stores,table_index,base_byte,delay,"
                     "pair_stores,context_first,context_last,context_changes\n");
        g_text_delay_header_written = true;
    }
    const char* safe_label = label && label[0] ? label : "unlabeled";
    if (g_text_delay_pair_count == 0) {
        std::fprintf(f, "%u,%s,%llu,%llu,%llu,%llu,0,0,0,0,0,0,0\n",
                     window_index, safe_label,
                     static_cast<unsigned long long>(start_frame),
                     static_cast<unsigned long long>(end_frame),
                     static_cast<unsigned long long>(g_text_delay_store_count),
                     static_cast<unsigned long long>(
                         g_text_delay_overflow_stores));
    } else {
        for (std::size_t i = 0; i < g_text_delay_pair_count; ++i) {
            const TextDelayPair& pair = g_text_delay_pairs[i];
            std::fprintf(
                f,
                "%u,%s,%llu,%llu,%llu,%llu,%u,%u,%u,%llu,0x%08X,"
                "0x%08X,%u\n",
                window_index, safe_label,
                static_cast<unsigned long long>(start_frame),
                static_cast<unsigned long long>(end_frame),
                static_cast<unsigned long long>(g_text_delay_store_count),
                static_cast<unsigned long long>(g_text_delay_overflow_stores),
                pair.table_index, pair.base_byte, pair.delay,
                static_cast<unsigned long long>(pair.stores),
                pair.first_context, pair.last_context,
                pair.context_changes);
        }
    }
    std::fclose(f);
}

void flush_text_delay_window_at_exit() {
    if (!g_text_record) return;
    flush_text_log();
    flush_text_budget_log();
    if (!g_text_delay_window_active) return;
    write_text_delay_window(g_text_delay_window_index, "exit",
                            g_text_delay_window_start_frame,
                            runtime_current_frame());
    g_text_delay_window_active = false;
}

// Called for every guest function entry while GSR_TEXT_RECORD is on. Four
// compares against constants on the miss path, so a normal text-record run
// pays a handful of instructions per entry and writes nothing.
void log_text_event(std::uint32_t entry_pc) {
    const char* kind = nullptr;
    switch (entry_pc) {
        case kTextProcessorPc: kind = "proc"; break;
        case kTextDelayTablePc: kind = "table"; break;
        case kTextDelayWaitPc: kind = "wait"; break;
        case kTextGlyphPc: kind = "glyph"; break;
        default: return;
    }

    // Unavailable before the bus exists; the setting is reported as -1 rather
    // than 0 so "not read" can never be mistaken for a real speed value.
    int speed = -1;
    if (const gba::GbaBus* bus = gbarecomp::active_bus()) {
        if (const std::uint8_t* ewram = bus->ewram_ptr()) {
            speed = ewram[(kMessageSpeedOptionAddress - kEwramBaseAddress) &
                          kEwramMirrorMask];
        }
    }

    if (!g_text_header_written) {
        g_text_buf += "frame,label,kind,speed,r0,r1,r2,r3,lr,"
                      "ctx0,ctx1,ctx2,ctx3\n";
        g_text_header_written = true;
    }

    std::string ctx[4];
    {
        if (const gba::GbaBus* bus = gbarecomp::active_bus()) {
            if (const std::uint8_t* ewram = bus->ewram_ptr()) {
                for (int reg = 0; reg < 4; ++reg) {
                    const std::uint32_t p = g_cpu.R[reg];
                    if (p < kEwramBaseAddress ||
                        p + kTextContextBytes > kEwramLimit) {
                        continue;
                    }
                    std::string& out = ctx[reg];
                    out.reserve(kTextContextBytes * 2);
                    const std::uint32_t off = p - kEwramBaseAddress;
                    for (std::size_t i = 0; i < kTextContextBytes; ++i) {
                        char hex[3];
                        std::snprintf(hex, sizeof(hex), "%02X", ewram[off + i]);
                        out.append(hex, 2);
                    }
                }
            }
        }
    }
    char line[832];
    const int n = std::snprintf(
        line, sizeof(line),
        "%llu,%s,%s,%d,%08X,%08X,%08X,%08X,%08X,%s,%s,%s,%s\n",
        static_cast<unsigned long long>(runtime_current_frame()),
        g_window_label.c_str(), kind, speed, g_cpu.R[0], g_cpu.R[1],
        g_cpu.R[2], g_cpu.R[3], g_cpu.R[14], ctx[0].c_str(), ctx[1].c_str(),
        ctx[2].c_str(), ctx[3].c_str());
    if (n > 0) {
        g_text_buf.append(line, static_cast<std::size_t>(
            n < static_cast<int>(sizeof(line)) ? n
                                               : static_cast<int>(sizeof(line)) - 1));
    }
    if (++g_text_buf_rows >= kTextFlushRows) flush_text_log();
}

void log_text_budget_store(std::uint32_t pc, std::uint32_t address,
                           std::uint32_t value, std::uint32_t context) {
    const char* phase = nullptr;
    switch (pc) {
        case kTextBudgetBasePc: phase = "base"; break;
        case kTextBudgetOverridePc: phase = "override"; break;
        case kTextBudgetDecrementPc: phase = "decrement"; break;
        default: return;
    }

    int speed = -1;
    if (const gba::GbaBus* bus = gbarecomp::active_bus()) {
        if (const std::uint8_t* ewram = bus->ewram_ptr()) {
            speed = ewram[(kMessageSpeedOptionAddress -
                           kEwramBaseAddress) & kEwramMirrorMask];
        }
    }

    if (!g_text_budget_header_written) {
        g_text_budget_buf +=
            "frame,window,phase,speed,pc,stack_addr,value,context\n";
        g_text_budget_header_written = true;
    }
    char line[192];
    const int n = std::snprintf(
        line, sizeof(line), "%llu,%u,%s,%d,0x%08X,0x%08X,%u,0x%08X\n",
        static_cast<unsigned long long>(runtime_current_frame()),
        g_window_index, phase, speed, pc, address, value, context);
    if (n > 0) {
        g_text_budget_buf.append(
            line, static_cast<std::size_t>(
                n < static_cast<int>(sizeof(line)) ? n
                                                   : static_cast<int>(sizeof(line)) - 1));
    }
    if (++g_text_budget_buf_rows >= kTextFlushRows)
        flush_text_budget_log();
}

// Returns true if a window was just closed (and reopened) this call --
// callers must not keep using slot pointers or window-scoped state from
// before the call.
bool check_hardware_boundaries(std::uint64_t frame) {
    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return false;
    const std::uint8_t* io = bus->io().raw();
    log_raw_signals(frame, io);

    // ---- fade --------------------------------------------------------
    const std::uint8_t bldy = io[0x54] & 0x1Fu;
    if (bldy != 0) {
        g_fade_in_progress = true;
        if (bldy > g_fade_peak_bldy) g_fade_peak_bldy = bldy;
    } else if (g_fade_in_progress) {
        g_fade_in_progress = false;
        // F2: only a fade that peaked near-full counts as a scene
        // transition -- see kFadeFullThreshold's comment above.
        const bool reached_full = g_fade_peak_bldy >= kFadeFullThreshold;
        g_fade_peak_bldy = 0;
        if (reached_full) {
            const bool debounced = g_have_fade_boundary &&
                (frame - g_last_fade_boundary_frame) < kFadeDebounceFrames;
            if (!debounced) {
                g_have_fade_boundary = true;
                g_last_fade_boundary_frame = frame;
                close_and_reopen_window_auto("fade");
                return true;
            }
        }
    }

    // ---- window registers (F3: was "text box") -------------------------
    const std::uint16_t dispcnt =
        static_cast<std::uint16_t>(io[0x00] | (io[0x01] << 8));
    const std::uint8_t win_enable =
        static_cast<std::uint8_t>((dispcnt >> 13) & 0x7u);
    const std::uint16_t win0h =
        static_cast<std::uint16_t>(io[0x40] | (io[0x41] << 8));
    const std::uint16_t win1h =
        static_cast<std::uint16_t>(io[0x42] | (io[0x43] << 8));
    const std::uint16_t win0v =
        static_cast<std::uint16_t>(io[0x44] | (io[0x45] << 8));
    const std::uint16_t win1v =
        static_cast<std::uint16_t>(io[0x46] | (io[0x47] << 8));
    if (g_win_state_valid &&
        (win_enable != g_prev_win_enable || win0h != g_prev_win0h ||
         win0v != g_prev_win0v || win1h != g_prev_win1h ||
         win1v != g_prev_win1v)) {
        const bool closing = popcount3(win_enable) < popcount3(g_prev_win_enable);
        const bool win_debounced = g_have_win_boundary &&
            (frame - g_last_win_boundary_frame) < kWinDebounceFrames;
        if (!win_debounced) {
            g_have_win_boundary = true;
            g_last_win_boundary_frame = frame;
            g_prev_win_enable = win_enable;
            g_prev_win0h = win0h;
            g_prev_win0v = win0v;
            g_prev_win1h = win1h;
            g_prev_win1v = win1v;
            close_and_reopen_window_auto(closing ? "window_close"
                                                 : "window_open");
            return true;
        }
        // T1: debounced -- leave g_prev_win_* at the last-fired baseline
        // rather than snapping it to this frame's (unreported) value. The
        // old code updated the baseline here unconditionally, so a state
        // that kept changing faster than kWinDebounceFrames apart would
        // never again produce a delta against a *stable* reference and
        // could go permanently silent; now the suppressed delta stays
        // visible and fires as soon as debounce elapses.
    } else {
        g_win_state_valid = true;
        g_prev_win_enable = win_enable;
        g_prev_win0h = win0h;
        g_prev_win0v = win0v;
        g_prev_win1h = win1h;
        g_prev_win1v = win1v;
    }

    // ---- room bounds (F4) -----------------------------------------------
    const RoomIdentitySnapshot room_now = read_room_identity();
    if (room_now.valid) {
        const bool changed = g_room_state_valid &&
            (room_now.min_x != g_prev_room_min_x ||
             room_now.min_y != g_prev_room_min_y ||
             room_now.ext_x != g_prev_room_ext_x ||
             room_now.ext_y != g_prev_room_ext_y);
        if (changed) {
            const bool room_debounced = g_have_room_boundary &&
                (frame - g_last_room_boundary_frame) < kRoomDebounceFrames;
            if (!room_debounced) {
                g_have_room_boundary = true;
                g_last_room_boundary_frame = frame;
                g_prev_room_min_x = room_now.min_x;
                g_prev_room_min_y = room_now.min_y;
                g_prev_room_ext_x = room_now.ext_x;
                g_prev_room_ext_y = room_now.ext_y;
                close_and_reopen_window_auto("room");
                return true;
            }
            // T1: debounced -- see the matching comment in the window-
            // register block above; leave the baseline at the last-fired
            // value instead of silently absorbing this delta.
        } else {
            g_prev_room_min_x = room_now.min_x;
            g_prev_room_min_y = room_now.min_y;
            g_prev_room_ext_x = room_now.ext_x;
            g_prev_room_ext_y = room_now.ext_y;
            g_room_state_valid = true;
        }
    }

    // ---- overlay bank set ------------------------------------------------
    std::vector<std::string> current_overlays;
    for (std::size_t i = 0;; ++i) {
        const char* name = gsr_overlay_name_at(i);
        if (!name) break;
        current_overlays.emplace_back(name);
        if (std::find(g_window_overlays.begin(), g_window_overlays.end(),
                      name) == g_window_overlays.end()) {
            g_window_overlays.emplace_back(name);
        }
    }
    std::sort(current_overlays.begin(), current_overlays.end());
    if (g_overlay_state_valid && current_overlays != g_prev_overlay_set) {
        g_prev_overlay_set = current_overlays;
        close_and_reopen_window_auto("overlay");
        return true;
    }
    g_overlay_state_valid = true;
    g_prev_overlay_set = std::move(current_overlays);
    return false;
}

// Savestate load (hotkey, TCP debug command, or --load-state): the load just
// moved runtime_current_frame() to whatever frame count that save recorded --
// a discontinuous jump, not one more tick of elapsed session time (see
// set_savestate_load_hook's comment in runtime_bus_bridge.h). Close the
// window that had been open across the jump (its reported duration will
// still include the jump itself, but future windows compute correctly
// because close_and_reopen_window() re-baselines g_window_start_frame to the
// post-load frame here) instead of leaving it open indefinitely: with the
// old behaviour, every later boundary computed its duration against a
// pre-load start_frame and the tracer's first capture of a session that
// loads a state near the start silently swallowed the entire session (see
// SESSION_HANDOFF notes / logs/trace_* "one giant capture" symptom).
void on_savestate_load() {
    if (!g_enabled) return;
    close_and_reopen_window_auto("savestate_load");
}

#ifdef GBARECOMP_HAVE_IMGUI

void draw_tracer_window() {
    if (!g_enabled) return;

    // Positioned to the right of the main viewport so real multi-viewport
    // (confirmed on: renderer=opengl) tears it into its own OS window; the
    // user can then move it, and ImGuiCond_FirstUseEver keeps that.
    const ImGuiViewport* main_vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(main_vp->WorkPos.x + main_vp->WorkSize.x + 24.0f,
               main_vp->WorkPos.y),
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380.0f, 360.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Function Tracer", nullptr,
                      ImGuiWindowFlags_NoDocking)) {
        g_window_focused = false;
        ImGui::End();
        return;
    }
    // D1: feeds tracer_wants_keyboard() -- see the field comment.
    g_window_focused = ImGui::IsWindowFocused();

    // D4: sticky, prominent -- a failed write loses a capture silently
    // otherwise, exactly the thing this tool exists to prevent.
    if (g_write_failed) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           "WRITE FAILED: %s", g_write_failed_path.c_str());
    }

    const std::uint64_t frame = runtime_current_frame();
    const std::uint64_t elapsed_frames =
        frame >= g_window_start_frame ? frame - g_window_start_frame : 0;
    const double elapsed_s =
        static_cast<double>(elapsed_frames) / gbarecomp::kGbaFrameHz;

    ImGui::Text("Window #%03u", g_window_index);
    ImGui::Text("Elapsed: %.1fs", elapsed_s);
    ImGui::Text("Distinct functions: %u", g_window_distinct);
    ImGui::Text("Total calls: %llu",
                static_cast<unsigned long long>(g_window_total_calls));
    ImGui::Text("Fingerprints loaded: %zu", g_fingerprints.size());
    ImGui::Text("Screenshots saved: %llu",
                static_cast<unsigned long long>(g_screenshot_count));
    // U3: progress meter toward mapping every function.
    ImGui::Text("Functions seen -- session: %zu, all-time: %zu",
               g_session_seen_pcs.size(), g_all_seen_pcs.size());
    if (g_text_record) {
        ImGui::TextWrapped(
            "Text recording: play a menu or the same NPC line, type its label "
            "in Label, then click Mark window. Repeat at each Message speed. "
            "text_speed.csv has per-call rows; text_delay.csv has counted "
            "halfword stores, and text_budget.csv records the effective "
            "dialogue counter writes.");
    }
    // U2: stays up until the next window closes.
    if (g_has_last_close) {
        ImGui::TextColored(
            ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s -- %u functions, %zu new",
            g_last_close_label.c_str(), g_last_close_distinct, g_last_close_new);
    }

    ImGui::InputText("Label", g_label_buf, sizeof(g_label_buf));
    if (g_label_buf[0] != '\0') {
        // Repeat matching (see merge_repeat_window()): a plain map lookup,
        // cheap enough to redo every ImGui frame -- the actual intersection
        // work happens only at window close.
        const std::string typed = sanitize_label(g_label_buf);
        const auto rit = g_label_repeat.find(typed);
        if (rit != g_label_repeat.end() && rit->second.count >= 2) {
            ImGui::Text("'%s': %zu repeats, intersection %zu functions",
                       typed.c_str(), rit->second.count, rit->second.live.size());
        } else {
            const std::size_t repeats =
                rit != g_label_repeat.end() ? rit->second.count : 0;
            ImGui::Text("'%s': %zu repeat(s) so far (need 2+ to intersect)",
                       typed.c_str(), repeats);
        }
    }
    if (ImGui::Button("Mark window")) {
        const std::string label = g_label_buf[0] ? g_label_buf : "manual";
        close_and_reopen_window(label);
        g_label_buf[0] = '\0';
    }

    // U4: nudge toward at least 3 captures per label -- 2 is the minimum for
    // an intersection at all, 3 is meaningfully more reliable. Only manual
    // labels ever repeat (auto-generated names are unique per window), so
    // this list is effectively the manual-label summary.
    if (!g_label_repeat.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Captures per label (3+ recommended):");
        std::vector<std::pair<std::string, std::size_t>> counts;
        counts.reserve(g_label_repeat.size());
        for (const auto& kv : g_label_repeat)
            counts.emplace_back(kv.first, kv.second.count);
        std::sort(counts.begin(), counts.end());
        for (const auto& kv : counts) {
            if (kv.second < 3) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s: %zu",
                                   kv.first.c_str(), kv.second);
            } else {
                ImGui::Text("%s: %zu", kv.first.c_str(), kv.second);
            }
        }
    }

    ImGui::End();
}

// D1: paired with g_config_ui_extra_draw as gbarecomp::g_config_ui_extra_wants_keyboard
// (see host_config_ui.h). True while the tracer's own text input must not
// also reach guest keyinput or a rebound hotkey: the tracer window has
// focus and ImGui itself currently wants the keyboard (i.e. the "Label"
// text box is active).
bool tracer_wants_keyboard() {
    if (!g_enabled) return false;
    return g_window_focused && ImGui::GetIO().WantCaptureKeyboard;
}
#else
void draw_tracer_window() {}
bool tracer_wants_keyboard() { return false; }
#endif

}  // namespace

void text_trace_window_opened(std::uint32_t window_index,
                              std::uint64_t start_frame) {
    if (!g_text_record) return;
    reset_text_delay_window(window_index, start_frame);
}

void text_trace_window_closed(std::uint32_t window_index, const char* label,
                              std::uint64_t start_frame,
                              std::uint64_t end_frame) {
    if (!g_text_record) return;
    if (!g_text_delay_window_active ||
        g_text_delay_window_index != window_index) {
        reset_text_delay_window(window_index, start_frame);
    }
    write_text_delay_window(window_index, label, start_frame, end_frame);
    g_text_delay_window_active = false;
}

void text_trace_delay_store(std::uint32_t table_index,
                            std::uint32_t base_byte,
                            std::uint32_t delay,
                            std::uint32_t context) {
    if (!g_text_record) return;
    if (!g_text_delay_window_active)
        reset_text_delay_window(0, runtime_current_frame());

    ++g_text_delay_store_count;
    for (std::size_t i = 0; i < g_text_delay_pair_count; ++i) {
        TextDelayPair& pair = g_text_delay_pairs[i];
        if (pair.table_index != table_index ||
            pair.base_byte != base_byte || pair.delay != delay) {
            continue;
        }
        ++pair.stores;
        if (pair.last_context != context) ++pair.context_changes;
        pair.last_context = context;
        return;
    }
    if (g_text_delay_pair_count == kTextDelayPairLimit) {
        ++g_text_delay_overflow_stores;
        return;
    }
    TextDelayPair& pair = g_text_delay_pairs[g_text_delay_pair_count++];
    pair.table_index = table_index;
    pair.base_byte = base_byte;
    pair.delay = delay;
    pair.stores = 1;
    pair.first_context = context;
    pair.last_context = context;
}

void text_trace_budget_store(std::uint32_t pc, std::uint32_t address,
                             std::uint32_t value, std::uint32_t context) {
    if (!g_text_record) return;
    log_text_budget_store(pc, address, value, context);
}

void function_tracer_init() {
    const char* e = std::getenv("GBARECOMP_FN_TRACER");
    g_enabled = e != nullptr && e[0] != '\0' && e[0] != '0';
    const char* text = std::getenv("GSR_TEXT_RECORD");
    g_text_record = text != nullptr && text[0] != '\0' && text[0] != '0';
    if (!g_enabled) return;

    // The tracer answers "what happened between frames"; the expanded view's
    // battle work needed the one thing it cannot show -- what the game changes
    // WITHIN a frame. Arm the renderer's row-state dump alongside it, bounded
    // to a handful of Mode 1 (battle) frames, so a single traced battle also
    // leaves logs/battle_rows.csv behind. Diagnostic only.
    gba::g_ws_row_state_dump_mode = 1;
    gba::g_ws_row_state_dump_frames = 16;  // full-row blocks, one per screen

    ensure_session_dir();
    if (g_text_record)
        gba::vram_trace::set_text_trace_directory(g_session_dir.c_str());
    load_fingerprints();
    g_window_label = "unlabeled";
    g_window_start_frame = runtime_current_frame();
    if (g_text_record) {
        text_trace_window_opened(g_window_index, g_window_start_frame);
        std::atexit(&flush_text_delay_window_at_exit);
    }
    gbarecomp::g_config_ui_extra_draw = &draw_tracer_window;
    gbarecomp::g_config_ui_extra_wants_keyboard = &tracer_wants_keyboard;
}

void function_tracer_on_savestate_load() { on_savestate_load(); }

void function_tracer_on_entry(std::uint32_t entry_pc) {
    if (!g_enabled) return;  // OFF: one predictable branch, nothing else.

    TracerSlot& slot = find_slot(entry_pc);
    if (slot.count == 0) {
        slot.entry_pc = entry_pc;
        ++g_window_distinct;
    }
    // D3: overwrite with THIS call's args every entry, not just the first --
    // a pc revisited later in the window (e.g. after menu navigation, then
    // the action being captured) must keep the most recent call's args, the
    // one relevant to what the user just did.
    slot.r0 = g_cpu.R[0];
    slot.r1 = g_cpu.R[1];
    slot.r2 = g_cpu.R[2];
    slot.r3 = g_cpu.R[3];
    slot.r14 = g_cpu.R[14];
    ++slot.count;
    ++g_window_total_calls;

    if (g_text_record) log_text_event(entry_pc);

    static std::uint64_t s_last_seen_frame = ~std::uint64_t{0};
    const std::uint64_t frame = runtime_current_frame();
    if (frame != s_last_seen_frame) {
        s_last_seen_frame = frame;
        // Item 2: hardware boundary check, once per new guest frame. May
        // close and reopen the window (see check_hardware_boundaries()),
        // which resets g_table -- safe here since `slot` above is no
        // longer touched after this point.
        check_hardware_boundaries(frame);
    }
}

}  // namespace gsr
