// One stderr line per distinct event, at most 80 in a run: the session log
// shows what a hook saw without flooding it. For a fault that only gameplay
// shows (22 November 2026: enemy skill costs and scrolling names in The Lost
// Age's battle list); remove the calls once it is understood.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <unordered_set>

namespace gsr {

inline void trace_once(std::uint64_t key, const char* format, ...) {
    static std::unordered_set<std::uint64_t> seen;
    if (seen.size() >= 80u || !seen.insert(key).second) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

}  // namespace gsr
