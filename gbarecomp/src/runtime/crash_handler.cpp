#include "crash_handler.h"

#if !defined(_WIN32)

// POSIX (Linux): the same files as on Windows except the minidump.
// crash_report.txt holds the signal, the faulting address and a backtrace
// (execinfo); run_state.txt tells an external kill or hard hang apart from
// a crash on the next start. The handler runs on its own stack so a stack
// overflow can still be reported, and uses only write(2)-level output until
// the backtrace, which is best-effort.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace gbarecomp {
namespace {

bool g_installed = false;
char g_dir[1024];
char g_report_path[1100];
char g_run_state_path[1100];
CrashExtraWriter g_extra_writer = nullptr;
alignas(16) char g_alt_stack[64 * 1024];

void raw_write_file(const char* path, const char* data, size_t len) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    while (len > 0) {
        const ssize_t n = write(fd, data, len);
        if (n <= 0) break;
        data += n;
        len -= static_cast<size_t>(n);
    }
    close(fd);
}

void put(int fd, const char* s) {
    const size_t len = std::strlen(s);
    if (write(fd, s, len) < 0) {}
}

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGBUS:  return "SIGBUS (bus error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGFPE:  return "SIGFPE (arithmetic fault)";
        case SIGABRT: return "SIGABRT (abort)";
        default:      return "fatal signal";
    }
}

void write_report(const char* what, const void* address) {
    const int fd = open(g_report_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char line[256];
    put(fd, "Golden Sun Recompiled crash report\n\n");
    std::snprintf(line, sizeof(line), "What: %s\n", what);
    put(fd, line);
    std::snprintf(line, sizeof(line), "Address: %p\n\nBacktrace:\n", address);
    put(fd, line);
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fd);
    if (g_extra_writer) {
        if (const char* extra = g_extra_writer(g_dir)) {
            std::snprintf(line, sizeof(line), "\nAlso written: %s\n", extra);
            put(fd, line);
        }
    }
    close(fd);
    raw_write_file(g_run_state_path, "crashed\n", 8);
}

void fatal_signal(int sig, siginfo_t* info, void*) {
    write_report(signal_name(sig), info ? info->si_addr : nullptr);
    // Hand the signal back to the default action so the exit status and
    // any core dump stay what they would have been.
    std::signal(sig, SIG_DFL);
    raise(sig);
}

void terminate_handler() {
    write_report("std::terminate (uncaught C++ exception)", nullptr);
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

void check_previous_run_state() {
    char buf[64] = {};
    const int fd = open(g_run_state_path, O_RDONLY);
    if (fd < 0) return;
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0 && std::strncmp(buf, "live", 4) == 0 &&
        access(g_report_path, F_OK) != 0) {
        std::fprintf(stderr,
                     "previous run ended without a clean exit and without a "
                     "crash report -- externally terminated or hard hang\n");
    }
}

}  // namespace

void crash_handler_install(const char* log_dir) {
    if (g_installed) return;
    if (log_dir && log_dir[0]) {
        std::snprintf(g_dir, sizeof(g_dir), "%s", log_dir);
    } else {
        char exe[1024] = {};
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return;
        exe[n] = '\0';
        if (char* slash = std::strrchr(exe, '/')) *slash = '\0';
        std::snprintf(g_dir, sizeof(g_dir), "%s", exe);
    }
    std::snprintf(g_report_path, sizeof(g_report_path), "%s/crash_report.txt", g_dir);
    std::snprintf(g_run_state_path, sizeof(g_run_state_path), "%s/run_state.txt", g_dir);

    check_previous_run_state();

    stack_t alt{};
    alt.ss_sp = g_alt_stack;
    alt.ss_size = sizeof(g_alt_stack);
    sigaltstack(&alt, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = &fatal_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        sigaction(sig, &sa, nullptr);
    std::set_terminate(&terminate_handler);

    char live[64];
    const int len = std::snprintf(live, sizeof(live), "live pid=%d\n",
                                  static_cast<int>(getpid()));
    raw_write_file(g_run_state_path, live, len > 0 ? static_cast<size_t>(len) : 0);
    g_installed = true;
}

void crash_handler_mark_clean_exit() {
    if (!g_installed) return;
    raw_write_file(g_run_state_path, "clean\n", 6);
}

void crash_handler_set_extra_writer(CrashExtraWriter writer) {
    g_extra_writer = writer;
}

}  // namespace gbarecomp

#else  // _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <exception>

// ---------------------------------------------------------------------------
// dbghelp.dll is loaded DYNAMICALLY (LoadLibraryA/GetProcAddress), not linked.
// A machine/build missing it degrades to addresses-only backtraces and no
// minidump instead of failing to build or crashing the crash handler itself.
// The structs below mirror dbghelp's public ABI exactly (they are stable,
// documented layouts) so we don't need to include <dbghelp.h> or link
// dbghelp.lib at all.
// ---------------------------------------------------------------------------

namespace {

struct SymbolInfoLocal {
    ULONG SizeOfStruct;
    ULONG TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index;
    ULONG Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG Register;
    ULONG Scope;
    ULONG Tag;
    ULONG NameLen;
    ULONG MaxNameLen;
    CHAR Name[1];
};

struct ImageHlpLine64Local {
    DWORD SizeOfStruct;
    PVOID Key;
    DWORD LineNumber;
    PCSTR FileName;
    DWORD64 Address;
};

struct MiniDumpExceptionInformationLocal {
    DWORD ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL ClientPointers;
};

constexpr DWORD kMiniDumpWithIndirectlyReferencedMemory = 0x00000040u;
constexpr DWORD kMiniDumpScanMemory = 0x00000010u;

using PFN_SymInitialize = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using PFN_SymFromAddr = BOOL(WINAPI*)(HANDLE, DWORD64, DWORD64*, SymbolInfoLocal*);
using PFN_SymGetLineFromAddr64 =
    BOOL(WINAPI*)(HANDLE, DWORD64, DWORD*, ImageHlpLine64Local*);
using PFN_MiniDumpWriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD,
                                            MiniDumpExceptionInformationLocal*,
                                            void*, void*);

struct DbgHelp {
    HMODULE module = nullptr;
    PFN_SymInitialize SymInitialize = nullptr;
    PFN_SymFromAddr SymFromAddr = nullptr;
    PFN_SymGetLineFromAddr64 SymGetLineFromAddr64 = nullptr;
    PFN_MiniDumpWriteDump MiniDumpWriteDump = nullptr;
    bool sym_ready = false;
};

DbgHelp g_dbghelp;
bool g_dbghelp_load_attempted = false;

// GetProcAddress returns FARPROC (itself a function pointer type), so
// casting it straight to our differently-typed PFN_* aliases is a
// function-to-function reinterpret and trips -Wcast-function-type. Routing
// through void* first is the conventional way to load an arbitrary export
// without that warning; it is exactly what GetProcAddress is for.
template <typename Fn>
Fn load_proc(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// Loaded lazily, the first time a report is actually written, per the task's
// "load it dynamically at crash time" guidance — this keeps dbghelp.dll out
// of the process entirely on a run that never crashes.
void load_dbghelp_if_needed() {
    if (g_dbghelp_load_attempted) return;
    g_dbghelp_load_attempted = true;
    g_dbghelp.module = LoadLibraryA("dbghelp.dll");
    if (!g_dbghelp.module) return;
    g_dbghelp.SymInitialize =
        load_proc<PFN_SymInitialize>(g_dbghelp.module, "SymInitialize");
    g_dbghelp.SymFromAddr =
        load_proc<PFN_SymFromAddr>(g_dbghelp.module, "SymFromAddr");
    g_dbghelp.SymGetLineFromAddr64 = load_proc<PFN_SymGetLineFromAddr64>(
        g_dbghelp.module, "SymGetLineFromAddr64");
    g_dbghelp.MiniDumpWriteDump =
        load_proc<PFN_MiniDumpWriteDump>(g_dbghelp.module, "MiniDumpWriteDump");
    if (g_dbghelp.SymInitialize) {
        g_dbghelp.sym_ready =
            g_dbghelp.SymInitialize(GetCurrentProcess(), nullptr, TRUE) != 0;
    }
}

// ---------------------------------------------------------------------------
// State established once at crash_handler_install() time, in static storage
// (not the stack) so a stack-overflow crash later has nothing extra to pay
// for to reach these values.
// ---------------------------------------------------------------------------

bool g_installed = false;
char g_exe_path[MAX_PATH] = {};
char g_report_path[MAX_PATH + 32] = {};
char g_dump_path[MAX_PATH + 32] = {};
char g_run_state_path[MAX_PATH + 32] = {};
char g_dir[MAX_PATH] = {};
gbarecomp::CrashExtraWriter g_extra_writer = nullptr;
ULONGLONG g_start_tick = 0;
bool g_report_written = false;  // first fatal event wins; no clobbering

// Plain Win32 file write, no CRT buffering, no std::string/iostream. Safe to
// call from inside the exception filter.
void raw_write_file(const char* path, const char* data, size_t len) {
    if (!path || !path[0]) return;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, data, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(h);
}

// Fixed-capacity, non-allocating text buffer. Kept as a static (not a stack
// local) for the same stack-overflow-headroom reason as the paths above.
struct ReportBuf {
    char data[16384];
    size_t len = 0;
    void appendf(const char* fmt, ...) {
        if (len >= sizeof(data) - 1) return;
        va_list args;
        va_start(args, fmt);
        const int n = std::vsnprintf(data + len, sizeof(data) - len, fmt, args);
        va_end(args);
        if (n > 0) {
            len += static_cast<size_t>(n);
            if (len > sizeof(data) - 1) len = sizeof(data) - 1;
        }
    }
};
ReportBuf g_report_buf;

const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return "EXCEPTION_ACCESS_VIOLATION (access violation)";
        case EXCEPTION_STACK_OVERFLOW:
            return "EXCEPTION_STACK_OVERFLOW (stack overflow)";
        case EXCEPTION_BREAKPOINT:
            return "EXCEPTION_BREAKPOINT (breakpoint)";
        case EXCEPTION_SINGLE_STEP:
            return "EXCEPTION_SINGLE_STEP (single step)";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "EXCEPTION_INT_DIVIDE_BY_ZERO (integer divide by zero)";
        case EXCEPTION_INT_OVERFLOW:
            return "EXCEPTION_INT_OVERFLOW (integer overflow)";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "EXCEPTION_ILLEGAL_INSTRUCTION (illegal instruction)";
        case EXCEPTION_PRIV_INSTRUCTION:
            return "EXCEPTION_PRIV_INSTRUCTION (privileged instruction)";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "EXCEPTION_FLT_DIVIDE_BY_ZERO (float divide by zero)";
        case EXCEPTION_FLT_INVALID_OPERATION:
            return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:
            return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_UNDERFLOW:
            return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:
            return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_IN_PAGE_ERROR:
            return "EXCEPTION_IN_PAGE_ERROR (page-in failure)";
        case EXCEPTION_INVALID_DISPOSITION:
            return "EXCEPTION_INVALID_DISPOSITION";
        case 0x20474343u:
            // GCC's magic SEH exception code ("GCC" encoded), used by
            // libstdc++ on MinGW to propagate a C++ exception through
            // Windows' own unwinder. Reaching our filter with this code
            // means a C++ exception unwound with no matching catch anywhere
            // on the stack -- the same situation std::set_terminate's
            // handler is meant for, arriving here first because MinGW's
            // unwind-to-terminate path itself goes through SEH.
            return "GCC C++ EH (uncaught C++ exception, no matching catch)";
        default:
            return "unknown exception code";
    }
}

void append_backtrace(ReportBuf& buf, void** frames, USHORT count) {
    load_dbghelp_if_needed();
    char sym_storage[sizeof(SymbolInfoLocal) + 256] = {};
    SymbolInfoLocal* sym = reinterpret_cast<SymbolInfoLocal*>(sym_storage);
    for (USHORT i = 0; i < count; ++i) {
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        bool resolved = false;
        if (g_dbghelp.sym_ready && g_dbghelp.SymFromAddr) {
            std::memset(sym_storage, 0, sizeof(sym_storage));
            sym->SizeOfStruct = sizeof(SymbolInfoLocal);
            sym->MaxNameLen = 256;
            DWORD64 displacement = 0;
            if (g_dbghelp.SymFromAddr(GetCurrentProcess(), addr, &displacement,
                                      sym)) {
                ImageHlpLine64Local line{};
                line.SizeOfStruct = sizeof(line);
                DWORD line_disp = 0;
                if (g_dbghelp.SymGetLineFromAddr64 &&
                    g_dbghelp.SymGetLineFromAddr64(GetCurrentProcess(), addr,
                                                   &line_disp, &line)) {
                    buf.appendf("  #%2u 0x%p %s+0x%llx  (%s:%lu)\n",
                                static_cast<unsigned>(i), frames[i], sym->Name,
                                static_cast<unsigned long long>(displacement),
                                line.FileName,
                                static_cast<unsigned long>(line.LineNumber));
                } else {
                    buf.appendf("  #%2u 0x%p %s+0x%llx\n",
                                static_cast<unsigned>(i), frames[i], sym->Name,
                                static_cast<unsigned long long>(displacement));
                }
                resolved = true;
            }
        }
        if (!resolved) {
            HMODULE hmod = nullptr;
            char mod_path[MAX_PATH] = {};
            const char* mod_name = "?";
            std::uintptr_t offset = 0;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(frames[i]), &hmod) &&
                hmod) {
                GetModuleFileNameA(hmod, mod_path, MAX_PATH);
                mod_name = mod_path;
                for (const char* p = mod_path; *p; ++p) {
                    if (*p == '\\' || *p == '/') mod_name = p + 1;
                }
                offset = reinterpret_cast<std::uintptr_t>(frames[i]) -
                         reinterpret_cast<std::uintptr_t>(hmod);
            }
            buf.appendf("  #%2u 0x%p  %s+0x%zx\n", static_cast<unsigned>(i),
                        frames[i], mod_name, offset);
        }
    }
}

void write_minidump(EXCEPTION_POINTERS* ep) {
    load_dbghelp_if_needed();
    if (!g_dbghelp.MiniDumpWriteDump) return;
    HANDLE h = CreateFileA(g_dump_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    MiniDumpExceptionInformationLocal info{};
    MiniDumpExceptionInformationLocal* info_ptr = nullptr;
    if (ep) {
        info.ThreadId = GetCurrentThreadId();
        info.ExceptionPointers = ep;
        info.ClientPointers = FALSE;
        info_ptr = &info;
    }
    const DWORD dump_type =
        kMiniDumpWithIndirectlyReferencedMemory | kMiniDumpScanMemory;
    BOOL ok = g_dbghelp.MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                          h, dump_type, info_ptr, nullptr, nullptr);
    if (!ok && info_ptr) {
        // The local dbghelp.dll (observed: Windows' own, version 10.0.26100.x)
        // can fail ERROR_NOACCESS internally on the ExceptionParam path even
        // for a textbook-correct EXCEPTION_POINTERS/CONTEXT — reproduced in a
        // minimal repro outside this codebase, so it is a host dbghelp defect,
        // not a bug in the pointers we pass. Retry without exception info so a
        // run on an affected machine still gets a usable dump (missing only
        // the embedded exception record, which crash_report.txt already has)
        // instead of a truncated, empty .dmp.
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
        ok = g_dbghelp.MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                         h, dump_type, nullptr, nullptr, nullptr);
    }
    (void)ok;  // best-effort; a failed/degraded dump must not be fatal
    CloseHandle(h);
}

// The one report writer every fatal path (SEH, std::terminate, SIGABRT,
// pure-virtual) funnels through. `ep` is non-null only on the SEH path.
// Everything here uses stack/static buffers and raw Win32 calls — no
// std::string, no iostream, no heap allocation of our own.
void write_crash_report(const char* reason, EXCEPTION_POINTERS* ep) {
    if (!g_installed) return;      // never configured; nothing to write to
    if (g_report_written) return;  // first fatal event wins
    g_report_written = true;

    // Mark run_state as crashed (not just "live") so a later run doesn't
    // mistake THIS crash, which we did handle, for an external kill.
    raw_write_file(g_run_state_path, "crashed\n", 8);

    ReportBuf& buf = g_report_buf;
    buf.len = 0;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed_ms = (now >= g_start_tick) ? (now - g_start_tick) : 0;

    buf.appendf("Golden Sun Recompiled -- crash report\n");
    buf.appendf("Timestamp:  %04u-%02u-%02u %02u:%02u:%02u.%03u (local)\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds);
    buf.appendf("Executable: %s\n", g_exe_path);
    buf.appendf("Uptime:     %llu.%03llu s\n",
                static_cast<unsigned long long>(elapsed_ms / 1000),
                static_cast<unsigned long long>(elapsed_ms % 1000));
    buf.appendf("Reason:     %s\n\n", reason);

    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const DWORD code = er->ExceptionCode;
        buf.appendf("Exception code: 0x%08lX (%s)\n", static_cast<unsigned long>(code),
                    exception_name(code));
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
            er->NumberParameters >= 2) {
            const ULONG_PTR kind = er->ExceptionInformation[0];
            const ULONG_PTR addr = er->ExceptionInformation[1];
            const char* kind_str = (kind == 0) ? "read"
                                    : (kind == 1) ? "write"
                                    : (kind == 8) ? "data-execution-prevention (execute)"
                                                   : "unknown";
            buf.appendf("  %s access violation at address 0x%p\n", kind_str,
                        reinterpret_cast<void*>(addr));
        }
        buf.appendf("Faulting instruction address: 0x%p\n", er->ExceptionAddress);

        HMODULE hmod = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(er->ExceptionAddress), &hmod) &&
            hmod) {
            char mod_path[MAX_PATH] = {};
            GetModuleFileNameA(hmod, mod_path, MAX_PATH);
            const char* mod_name = mod_path;
            for (const char* p = mod_path; *p; ++p) {
                if (*p == '\\' || *p == '/') mod_name = p + 1;
            }
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(hmod);
            const std::uintptr_t offset =
                reinterpret_cast<std::uintptr_t>(er->ExceptionAddress) - base;
            buf.appendf("  Module: %s (base 0x%p, offset +0x%zx)\n", mod_name,
                        reinterpret_cast<void*>(base), offset);
        } else {
            buf.appendf("  Module: <could not resolve a loaded module for this address>\n");
        }
        buf.appendf("\n");
    }

    void* frames[64] = {};
    const USHORT frame_count = CaptureStackBackTrace(0, 64, frames, nullptr);
    buf.appendf("Backtrace (%u frames):\n", static_cast<unsigned>(frame_count));
    append_backtrace(buf, frames, frame_count);

    // The report goes to disk before the extra writer runs, so a second fault
    // while writing the extra files cannot lose it; it is rewritten with the
    // "Also written" line afterwards.
    raw_write_file(g_report_path, buf.data, buf.len);
    if (g_extra_writer) {
        const char* extra = g_extra_writer(g_dir);
        buf.appendf("\nAlso written: %s\n", extra ? extra : "(nothing)");
        raw_write_file(g_report_path, buf.data, buf.len);
    }
    write_minidump(ep);
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* ep) {
    write_crash_report("Unhandled SEH exception (SetUnhandledExceptionFilter)", ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

void terminate_handler() {
    char reason[512];
    std::snprintf(reason, sizeof(reason),
                  "std::terminate (uncaught C++ exception)");
    try {
        if (std::exception_ptr current = std::current_exception()) {
            std::rethrow_exception(current);
        }
    } catch (const std::exception& e) {
        std::snprintf(reason, sizeof(reason),
                      "std::terminate -- uncaught std::exception: %s", e.what());
    } catch (...) {
        std::snprintf(reason, sizeof(reason),
                      "std::terminate -- uncaught exception of unknown type");
    }
    write_crash_report(reason, nullptr);
    std::abort();
}

void abort_signal_handler(int) {
    write_crash_report("SIGABRT (abort() called)", nullptr);
    // Restore the default handler and re-raise so the process still exits
    // the way a plain abort() would without us in the picture.
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
}

#if defined(_MSC_VER)
// MSVC/UCRT route: pure virtual calls go through _purecall, which this hook
// intercepts.
void purecall_handler() {
    write_crash_report("pure virtual function call (_purecall)", nullptr);
    std::abort();
}
#else
// This toolchain builds with g++ (MinGW), which follows the Itanium C++ ABI:
// a pure virtual call goes through __cxa_pure_virtual, not MSVC's _purecall.
// libstdc++ defines __cxa_pure_virtual as a weak symbol precisely so it can
// be overridden; this is the correct hook for the compiler actually in use
// here, not the MSVC-only _set_purecall_handler the task text named.
extern "C" void __cxa_pure_virtual() {
    write_crash_report("pure virtual function call (__cxa_pure_virtual)", nullptr);
    std::abort();
}
#endif

void check_previous_run_state() {
    HANDLE h = CreateFileA(g_run_state_path, GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return;  // no prior run recorded
    char state_buf[64] = {};
    DWORD read = 0;
    ReadFile(h, state_buf, sizeof(state_buf) - 1, &read, nullptr);
    CloseHandle(h);
    state_buf[read < sizeof(state_buf) ? read : sizeof(state_buf) - 1] = '\0';

    const bool was_live = std::strncmp(state_buf, "live", 4) == 0;
    const DWORD report_attrs = GetFileAttributesA(g_report_path);
    const bool report_exists = report_attrs != INVALID_FILE_ATTRIBUTES;
    if (was_live && !report_exists) {
        std::fprintf(stderr,
            "previous run ended without a clean exit and without a crash "
            "report -- externally terminated or hard hang\n");
    }
}

}  // namespace

namespace gbarecomp {

void crash_handler_install(const char* log_dir) {
    if (g_installed) return;

    GetModuleFileNameA(nullptr, g_exe_path, MAX_PATH);

    char dir_buf[MAX_PATH] = {};
    const char* dir = log_dir;
    if (!dir || !dir[0]) {
        // Default: the directory the running executable lives in.
        std::snprintf(dir_buf, sizeof(dir_buf), "%s", g_exe_path);
        char* last_sep = nullptr;
        for (char* p = dir_buf; *p; ++p) {
            if (*p == '\\' || *p == '/') last_sep = p;
        }
        if (last_sep) *last_sep = '\0';
        else dir_buf[0] = '\0';
        dir = dir_buf;
    }

    std::snprintf(g_dir, sizeof(g_dir), "%s", dir);
    std::snprintf(g_report_path, sizeof(g_report_path), "%s\\crash_report.txt", dir);
    std::snprintf(g_dump_path, sizeof(g_dump_path), "%s\\crash_dump.dmp", dir);
    std::snprintf(g_run_state_path, sizeof(g_run_state_path), "%s\\run_state.txt", dir);

    check_previous_run_state();

    // Reserve extra stack for the calling thread so the exception filter has
    // room to run even when the fault IS a stack overflow. Best-effort: if
    // it fails, the filter still runs, just with less headroom.
    ULONG stack_guarantee = 128 * 1024;
    SetThreadStackGuarantee(&stack_guarantee);

    SetUnhandledExceptionFilter(&unhandled_exception_filter);
    std::set_terminate(&terminate_handler);
    std::signal(SIGABRT, &abort_signal_handler);
#if defined(_MSC_VER)
    _set_purecall_handler(&purecall_handler);
#endif

    g_start_tick = GetTickCount64();

    char live_state[96];
    const int live_len = std::snprintf(live_state, sizeof(live_state),
                                       "live pid=%lu\n",
                                       static_cast<unsigned long>(GetCurrentProcessId()));
    raw_write_file(g_run_state_path, live_state,
                   live_len > 0 ? static_cast<size_t>(live_len) : 0);

    g_installed = true;
}

void crash_handler_set_extra_writer(CrashExtraWriter writer) {
    g_extra_writer = writer;
}

void crash_handler_mark_clean_exit() {
    if (!g_installed) return;
    raw_write_file(g_run_state_path, "clean\n", 6);
}

}  // namespace gbarecomp

#endif  // _WIN32
