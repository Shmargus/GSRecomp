// crash_handler.h — Host-side, always-on crash reporting.
//
// Platform infrastructure, not game code: installed once from the game's
// entry point, before anything else initializes (SDL, the guest bus, the
// ROM/BIOS loaders). It must never be read by the guest or influence
// emulation in any way — it only observes the host process from the outside
// and writes plain files next to the executable when that process dies
// abnormally.
//
// On a fatal Windows exception (access violation, stack overflow, ...), an
// uncaught C++ exception reaching std::terminate, abort()/SIGABRT, or a
// pure-virtual call, it writes:
//   - crash_report.txt   plain-text: what happened, where, and a backtrace
//   - crash_dump.dmp     a MiniDumpWriteDump minidump, if dbghelp is available
// both next to the running executable.
//
// It also distinguishes "the process crashed" from "the process was
// externally killed" (Stop-Process -Force / TerminateProcess), because the
// latter cannot run any handler at all — see crash_handler_install()'s doc
// comment below for how that is surfaced.
//
// This header is platform-free on purpose: everything Windows-specific is
// hidden in the .cpp, and on a non-Windows build every entry point here
// compiles down to a no-op so callers never need an #ifdef of their own.

#pragma once

namespace gbarecomp {

// Install the crash handler. Call this as the FIRST thing in main(), before
// any other subsystem (SDL, the guest bus, argument parsing that could
// itself misbehave) so as much of the run as possible is covered.
//
// `log_dir` is the directory crash_report.txt / crash_dump.dmp / run_state.txt
// are written to. Pass the directory the executable lives in (next to the
// .exe) so the files are easy to find; a null or empty `log_dir` disables
// the handler entirely (used by tests that don't want it installed).
//
// On startup, if the PREVIOUS run's run_state.txt says that run was still
// "live" and no crash_report.txt exists for it, this logs one line via
// std::fprintf(stderr, ...) naming the situation explicitly:
//   "previous run ended without a clean exit and without a crash report —
//    externally terminated or hard hang"
// That is the only way to tell a real crash apart from an external kill,
// because an external kill cannot run our handler at all.
//
// Safe to call more than once; later calls are a no-op. On non-Windows
// platforms this only manages run_state.txt bookkeeping (see below) — there
// is no SEH, so mid-crash reporting is Windows-only.
void crash_handler_install(const char* log_dir);

// Call once, right before a normal, successful process exit (after the main
// loop returns cleanly, not from inside an exception handler). Marks this
// run's run_state.txt "clean" so crash_handler_install() on the NEXT run
// does not mistake it for an externally-terminated one.
//
// Safe to call even if crash_handler_install() was never called or failed;
// it is then a no-op.
void crash_handler_mark_clean_exit();

}  // namespace gbarecomp
