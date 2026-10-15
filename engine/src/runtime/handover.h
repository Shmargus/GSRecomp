#pragma once

// One window for both games (Windows). When The Broken Seal ends with a
// Seamless code (seamless.h), its program (player_main.cpp) keeps the window
// open and waits on a named pipe instead of exiting. The launcher's Broken
// Seal session asks it for the game's exit code (Query); the Lost Age session
// then sends The Lost Age's launch (Run) and the program runs it in the same
// window, as if the session had started it. Quit releases a waiting program.
// Whenever any step fails, both sides fall back to the old way: the program
// exits and the next game opens its own window.
//
// One request per connection: one byte, then for Run the Launch below. Reply:
// Query, the exit code (int32); Run, 1 when the game is starting, else 0.

#ifdef _WIN32
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace gbarecomp::handover {

// Set by the collection launcher for everything it starts: its process id.
// The pipe is named after it, and a waiting program gives up when the
// launcher is gone.
inline constexpr const wchar_t* kLauncherEnv = L"GSR_HANDOVER_LAUNCHER";

enum Request : char { kQuery = 'Q', kRun = 'R', kQuit = 'X' };

struct Launch {
    std::uint64_t stdout_handle = 0;  // already duplicated into the program; 0: none
    std::uint64_t stderr_handle = 0;
    std::wstring plugin;              // the engine plug-in, full path
    std::wstring command_line;        // as CreateProcess would get it
    std::wstring directory;           // working folder
    std::wstring environment;         // NAME=value\0 ... \0
};

inline DWORD launcher_pid() {
    wchar_t text[32];
    const DWORD n = GetEnvironmentVariableW(kLauncherEnv, text, 32);
    if (n == 0 || n >= 32) return 0;
    return static_cast<DWORD>(std::wcstoul(text, nullptr, 10));
}

inline std::wstring pipe_name(DWORD launcher) {
    return L"\\\\.\\pipe\\gsr-handover-" + std::to_wstring(launcher);
}

inline bool write_all(HANDLE pipe, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
        DWORD done = 0;
        if (!WriteFile(pipe, bytes, static_cast<DWORD>(size), &done, nullptr) || done == 0)
            return false;
        bytes += done;
        size -= done;
    }
    return true;
}

inline bool read_all(HANDLE pipe, void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
        DWORD done = 0;
        if (!ReadFile(pipe, bytes, static_cast<DWORD>(size), &done, nullptr) || done == 0)
            return false;
        bytes += done;
        size -= done;
    }
    return true;
}

inline bool write_text(HANDLE pipe, const std::wstring& text) {
    const std::uint32_t count = static_cast<std::uint32_t>(text.size());
    return write_all(pipe, &count, sizeof count) &&
           write_all(pipe, text.data(), count * sizeof(wchar_t));
}

inline bool read_text(HANDLE pipe, std::wstring& text) {
    std::uint32_t count = 0;
    if (!read_all(pipe, &count, sizeof count) || count > (1u << 24)) return false;
    text.assign(count, L'\0');
    return read_all(pipe, text.data(), count * sizeof(wchar_t));
}

inline bool write_launch(HANDLE pipe, const Launch& launch) {
    return write_all(pipe, &launch.stdout_handle, sizeof launch.stdout_handle) &&
           write_all(pipe, &launch.stderr_handle, sizeof launch.stderr_handle) &&
           write_text(pipe, launch.plugin) && write_text(pipe, launch.command_line) &&
           write_text(pipe, launch.directory) && write_text(pipe, launch.environment);
}

inline bool read_launch(HANDLE pipe, Launch& launch) {
    return read_all(pipe, &launch.stdout_handle, sizeof launch.stdout_handle) &&
           read_all(pipe, &launch.stderr_handle, sizeof launch.stderr_handle) &&
           read_text(pipe, launch.plugin) && read_text(pipe, launch.command_line) &&
           read_text(pipe, launch.directory) && read_text(pipe, launch.environment);
}

// ---- launcher side --------------------------------------------------------

// The waiting program's pipe, or INVALID_HANDLE_VALUE when none waits. With
// `expected_pid`, only that process's pipe. *server_pid gets the program's id.
inline HANDLE connect(DWORD expected_pid = 0, DWORD* server_pid = nullptr) {
    const DWORD launcher = launcher_pid();
    if (launcher == 0) return INVALID_HANDLE_VALUE;
    const std::wstring name = pipe_name(launcher);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (;;) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        // Busy: the program is between two requests. It fails at once when
        // no program waits any more.
        if (GetLastError() != ERROR_PIPE_BUSY ||
            !WaitNamedPipeW(name.c_str(), NMPWAIT_WAIT_FOREVER))
            return INVALID_HANDLE_VALUE;
    }
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid) || (expected_pid && pid != expected_pid)) {
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }
    if (server_pid) *server_pid = pid;
    return pipe;
}

// The ended game's exit code, when `player_pid` is waiting to run the next.
inline bool query(DWORD player_pid, std::int32_t* code) {
    HANDLE pipe = connect(player_pid);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    const char request = kQuery;
    const bool ok = write_all(pipe, &request, 1) && read_all(pipe, code, sizeof *code);
    CloseHandle(pipe);
    return ok;
}

// Lets a waiting program exit (nothing to hand over to).
inline void quit() {
    HANDLE pipe = connect();
    if (pipe == INVALID_HANDLE_VALUE) return;
    const char request = kQuit;
    write_all(pipe, &request, 1);
    CloseHandle(pipe);
}

// Runs `launch` in the waiting program. The handles are this process's own;
// they are duplicated into the program. On success *process is the program
// (SYNCHRONIZE and exit code access): wait on it as on a started game.
inline bool run(Launch launch, HANDLE out, HANDLE err, HANDLE* process, DWORD* process_id) {
    DWORD pid = 0;
    HANDLE pipe = connect(0, &pid);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    HANDLE player = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE |
                                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    bool ok = player != nullptr;
    const auto give = [&](HANDLE mine, std::uint64_t& theirs) {
        HANDLE remote = nullptr;
        if (!mine) return;
        if (DuplicateHandle(GetCurrentProcess(), mine, player, &remote, 0, FALSE,
                            DUPLICATE_SAME_ACCESS))
            theirs = reinterpret_cast<std::uintptr_t>(remote);
        else
            ok = false;
    };
    if (ok) { give(out, launch.stdout_handle); give(err, launch.stderr_handle); }
    std::int32_t started = 0;
    const char request = kRun;
    ok = ok && write_all(pipe, &request, 1) && write_launch(pipe, launch) &&
         read_all(pipe, &started, sizeof started) && started == 1;
    CloseHandle(pipe);
    if (!ok) {
        if (player) CloseHandle(player);
        return false;
    }
    *process = player;
    *process_id = pid;
    return true;
}

}  // namespace gbarecomp::handover
#endif  // _WIN32
