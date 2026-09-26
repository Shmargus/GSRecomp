// function_tracer.h — GBARECOMP_FN_TRACER function-call tracer.
//
// Counts every distinct guest function entry into "windows", auto-detects
// boundaries from measured hardware state (fades, overlay bank swaps and
// window-register changes -- the latter is not proven to mean dialogue), and
// auto-names each window from
// what triggered it, and saves a screenshot when the closing window's
// function set doesn't already match a known fingerprint. Also offers a
// floating ImGui window (its own OS window via multi-viewport, see
// host_config_ui.h) with a manual "Mark window" free-text label for
// actions hardware can't detect. GSR_TEXT_RECORD is a launcher alias that
// enables this tracer, its text-capture guidance, and the session's
// payload-free BG0 write metadata CSV.
//
// Golden Sun-specific tooling: lives entirely in src/, drawn through the
// one generic extension point host_config_ui.cpp exposes
// (gbarecomp::g_config_ui_extra_draw). No game logic lives in gbarecomp
// itself.
#pragma once

#include <cstddef>
#include <cstdint>

// Defined in runner_main.cpp: index-based iterator over the "overlay_rom_*"
// EWRAM overlay banks (kTransientCodeImages there) currently verified as
// matched in guest RAM -- see g_verified_identity_cache there. Small IWRAM
// identity-gate stub rows in that same table are not overlay banks and are
// skipped. Call with i = 0, 1, 2, ... until it returns nullptr; several
// banks can be matched at once if a window straddles a bank swap. Extern
// "C" (rather than a gsr:: declaration) so the symbol resolves across the
// runner_main.cpp / function_tracer.cpp translation-unit boundary
// regardless of the anonymous namespace runner_main.cpp keeps that cache
// in. Not the hot path in the usual sense, but now sampled once per guest
// frame (see check_hardware_boundaries in function_tracer.cpp) rather than
// once per 30-frame sample -- kTransientCodeImages is a handful of rows
// (see the "linear scan... is fine" comment at its own definition site).
extern "C" const char* gsr_overlay_name_at(std::size_t i);

// Defined in runner_main.cpp: current value of the UNVERIFIED candidate
// room-identity pointer + bounds (golden_sun_map_record_read_identity;
// see docs/issues/WIDE-MARGIN-ATTEMPTS.md, "Room bounds exist in memory").
// Returns false (leaving outputs untouched) before the first room has
// loaded or if the pointer doesn't look plausible. The tracer records
// whatever this returns in every window's header for later comparison, and
// (F4) fires a "room" boundary when the bounds tuple changes while the
// identity reads valid, and folds a short hash of the bounds into every
// window's name/index line as a room tag. This is a deliberate, narrow
// exception to treating the value as inert: the tracer only ever uses it
// to label/bucket diagnostic captures for a human afterwards, never to
// alter game state, rendering, or control flow -- it must still NEVER be
// used to drive gameplay-affecting behaviour anywhere else (unverified
// beyond one room; see the doc above).
extern "C" bool gsr_map_record_identity(std::uint32_t* room_ptr,
                                         std::uint32_t* min_x,
                                         std::uint32_t* min_y,
                                         std::uint32_t* ext_x,
                                         std::uint32_t* ext_y);

namespace gsr {

// Caches the GBARECOMP_FN_TRACER env flag, and -- only when it is set --
// wires the tracer's draw call into host_config_ui.cpp. Call once at
// startup, before gbarecomp::run_game().
void function_tracer_init();

// Savestate-load handler. The runner owns gbarecomp's single savestate-load
// hook and calls this from it; a no-op while the tracer is off.
void function_tracer_on_savestate_load();

// Hot path: called from golden_sun_function_entry_observer for EVERY guest
// function entry. When GBARECOMP_FN_TRACER is unset this is a single
// cached-flag branch and nothing else.
void function_tracer_on_entry(std::uint32_t entry_pc);

// Text-delay store diagnostics. The runner observes the game-owned halfword
// store; the tracer owns the current labeled window and writes the bounded
// per-window ledger beside text_speed.csv.
void text_trace_window_opened(std::uint32_t window_index,
                              std::uint64_t start_frame);
void text_trace_window_closed(std::uint32_t window_index, const char* label,
                              std::uint64_t start_frame,
                              std::uint64_t end_frame);
void text_trace_delay_store(std::uint32_t table_index,
                            std::uint32_t base_byte,
                            std::uint32_t delay,
                            std::uint32_t context);

// Dialogue-budget writes observed at the three generated stores that seed
// and consume the processor's per-call stack counter. Diagnostic only; the
// observer never changes the guest value.
void text_trace_budget_store(std::uint32_t pc, std::uint32_t address,
                             std::uint32_t value, std::uint32_t context);

}  // namespace gsr
