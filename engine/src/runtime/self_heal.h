// self_heal.h: bookkeeping for self-healing, the engine's fallback for guest
// code the ahead-of-time translation did not cover.
//
// A dispatch miss is a jump to a guest address with no translated function.
// Stage 1: runtime_dispatch_miss runs the call in the reference interpreter
// and returns to generated code (stop-address rule in
// runtime_arm_default_aborts.cpp). Stage 2: the function is compiled into a
// native overlay at run time and cached on disk (overlay_loader.h), so later
// calls run natively. Every miss is logged, counted in the coverage report at
// exit, and written to a TOML proposal that a person reviews before it joins
// the game's configuration; proposals are never merged automatically. A run
// that needed either stage is reported as not fully static.
//
// A codegen gap (an instruction form the translator cannot lower) is not a
// miss: it always aborts loudly (runtime_unimplemented_op).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp {

// Reset all self-heal bookkeeping. Called once per machine bring-up
// (run_game) so a fresh process starts with a clean coverage tally.
void self_heal_reset();

// True if any dispatch miss was bridged this session.
bool self_heal_any_misses();

// Distinct missed PCs bridged this session.
std::uint32_t self_heal_distinct_misses();

// Total guest instructions executed in the interpreter bridge this session.
std::uint64_t self_heal_interpreted_insns();

// Print coverage to stdout. If there were misses and frag_path is set,
// write proposed [[extra_func]] entries there. Tight clusters get a note
// for possible [[jump_table]] review (see self_heal_cluster_misses).
void self_heal_write_report(const char* frag_path);

// Set the running program's identity (title, 4-char game code, ROM SHA-1).
// Stamped into the frag header and the coverage JSON so the persisted logs
// are unambiguous about which game produced them. Call once at startup.
void self_heal_set_program_identity(const char* title, const char* code,
                                    const char* sha1);

// ── Jump-table-aware proposal clustering ───────────────────────────
// One bridged miss, for the proposal/coverage report.
struct SelfHealMiss {
    std::uint32_t pc    = 0;
    bool          thumb = false;
    std::uint64_t count = 0;  // times bridged this session
};

// A maximal run of adjacent misses in a PC-sorted list. When
// `jump_table_candidate`, the run's members are tightly-spaced same-mode
// misses that look like the case targets of a computed-jump switch the
// finder could not size (e.g. nine fragments in 0x08062894..0x08062920 in
// another game): review should prefer ONE sized [[jump_table]]
// over merging the individual [[extra_func]] entries.
struct SelfHealCluster {
    std::size_t begin = 0;   // [begin, end) indices into the sorted input
    std::size_t end   = 0;
    bool jump_table_candidate = false;
};

// Group a PC-SORTED miss list into adjacency clusters. A run of at least
// `min_run` same-mode misses, each within `max_gap` bytes of its
// predecessor, is flagged jump_table_candidate. Every input index lands in
// exactly one returned cluster (singletons included). Pure / no I/O; unit
// tested (engine/tests/selfheal/cluster_test.cpp) independently of the runtime.
std::vector<SelfHealCluster> self_heal_cluster_misses(
    const std::vector<SelfHealMiss>& sorted_misses,
    std::uint32_t max_gap, std::size_t min_run);

// Coverage status, counters and per-PC healing results for the TCP `misses`
// command and the exit report. Reading it doesn't arm tracing.
std::string self_heal_misses_json();

// Write self_heal_misses_json() to `path` (+ a trailing newline). Unlike the
// frag proposal, this is written on EVERY exit so the build loop / CI can
// read one machine-readable file to confirm FULLY_STATIC or drive the merge
// loop. Returns false if the path can't be opened. NEVER touches game.toml.
bool self_heal_write_coverage_json(const char* path);

}  // namespace gbarecomp
