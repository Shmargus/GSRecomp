// player_main.cpp -- the game program: loads the game's engine plug-in from
// beside it (GSR_ENGINE_PLUGIN, game_module.cpp) and runs it. Each game
// builds it under its old program name, so the launcher starts it as before.
// Windows only; Linux still builds the engine as the program itself.
//
// Seamless mode (handover.h): when The Broken Seal ends with a Seamless code
// and the collection launcher started this program, the window stays open
// and the program waits for the launcher to send The Lost Age's launch, then
// runs it in the same window.
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "handover.h"
#include "seamless.h"

#ifndef GSR_ENGINE_PLUGIN
#error "GSR_ENGINE_PLUGIN names the engine plug-in, e.g. L\"TLAEngine.dll\""
#endif

namespace {

namespace handover = gbarecomp::handover;
using GameMain = int (*)(int, char**, void*, void**);

GameMain load_game(const std::wstring& plugin) {
    // Altered search path: the plug-in's own game library is found beside it,
    // not beside this program.
    HMODULE engine = LoadLibraryExW(plugin.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!engine) {
        std::fprintf(stderr, "[player] could not load %ls (Windows error %lu)\n",
                     plugin.c_str(), GetLastError());
        return nullptr;
    }
    const auto game_main = reinterpret_cast<GameMain>(
        reinterpret_cast<void*>(GetProcAddress(engine, "gsr_game_main")));
    if (!game_main)
        std::fprintf(stderr, "[player] %ls has no gsr_game_main\n", plugin.c_str());
    return game_main;
}

// Points C's stdout (fd 1) or stderr (fd 2) and Windows' standard handle at
// `handle`, closing what they pointed at before. Takes `handle` over.
void set_output(int fd, DWORD which, HANDLE handle) {
    std::fflush(fd == 1 ? stdout : stderr);
    const int opened = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_TEXT);
    if (opened < 0) return;
    _dup2(opened, fd);
    _close(opened);
    SetStdHandle(which, reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
}

// The launch's environment replaces this process's, for Windows and for C's
// getenv (the games read their settings through both).
void set_environment(const std::wstring& block) {
    std::vector<std::wstring> names;
    if (LPWCH current = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = current; *entry; entry += std::wcslen(entry) + 1) {
            const std::wstring text(entry);
            const std::size_t equals = text.find(L'=', 1);
            if (text[0] != L'=' && equals != std::wstring::npos)
                names.push_back(text.substr(0, equals));
        }
        FreeEnvironmentStringsW(current);
    }
    for (const std::wstring& name : names) {
        SetEnvironmentVariableW(name.c_str(), nullptr);
        _wputenv_s(name.c_str(), L"");
    }
    for (std::size_t at = 0; at < block.size() && block[at] != L'\0';) {
        const std::wstring text(block.c_str() + at);
        at += text.size() + 1;
        const std::size_t equals = text.find(L'=', 1);
        if (text[0] == L'=' || equals == std::wstring::npos) continue;
        const std::wstring name = text.substr(0, equals);
        const std::wstring value = text.substr(equals + 1);
        SetEnvironmentVariableW(name.c_str(), value.c_str());
        _wputenv_s(name.c_str(), value.c_str());
    }
}

// The launch's command line as a C main() gets it (the system code page).
std::vector<std::string> arguments_of(const std::wstring& command_line) {
    std::vector<std::string> result;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(command_line.c_str(), &count);
    if (!wide) return result;
    for (int i = 0; i < count; ++i) {
        const int size = WideCharToMultiByte(CP_ACP, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text(size > 0 ? size - 1 : 0, '\0');
        if (size > 1)
            WideCharToMultiByte(CP_ACP, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        result.push_back(std::move(text));
    }
    LocalFree(wide);
    return result;
}

// Waits for the launcher with the window open. Returns the next game's exit
// code, or `ended_code` when there is nothing to hand over to.
int wait_and_hand_over(int ended_code, void* window) {
    const DWORD launcher = handover::launcher_pid();
    HANDLE launcher_process = OpenProcess(SYNCHRONIZE, FALSE, launcher);
    if (!launcher_process) return ended_code;
    // The pipe exists before this program's output closes: the session sees
    // its log end, finds the pipe and asks for the code.
    const std::wstring name = handover::pipe_name(launcher);
    HANDLE pipe = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        CloseHandle(launcher_process);
        return ended_code;
    }
    std::fprintf(stderr, "[player] hand-over: window kept, waiting for the next game\n");
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE nul2 = nullptr;
    DuplicateHandle(GetCurrentProcess(), nul, GetCurrentProcess(), &nul2, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    set_output(1, STD_OUTPUT_HANDLE, nul);
    set_output(2, STD_ERROR_HANDLE, nul2);

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    int result = ended_code;
    for (;;) {
        ResetEvent(overlapped.hEvent);
        bool connected = ConnectNamedPipe(pipe, &overlapped) != FALSE;
        if (!connected) {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                const HANDLE waits[2] = {overlapped.hEvent, launcher_process};
                if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    CancelIo(pipe);
                    break;  // the launcher is gone
                }
                DWORD unused = 0;
                connected = GetOverlappedResult(pipe, &overlapped, &unused, FALSE) != FALSE;
            }
        }
        if (!connected) break;
        // Requests are small and sent at once; read them with plain waits.
        const auto read = [&](void* data, std::size_t size) {
            auto* bytes = static_cast<char*>(data);
            while (size > 0) {
                DWORD done = 0;
                ResetEvent(overlapped.hEvent);
                if (!ReadFile(pipe, bytes, static_cast<DWORD>(size), nullptr, &overlapped) &&
                    GetLastError() != ERROR_IO_PENDING) return false;
                if (!GetOverlappedResult(pipe, &overlapped, &done, TRUE) || done == 0) return false;
                bytes += done;
                size -= done;
            }
            return true;
        };
        const auto write = [&](const void* data, std::size_t size) {
            DWORD done = 0;
            ResetEvent(overlapped.hEvent);
            if (!WriteFile(pipe, data, static_cast<DWORD>(size), nullptr, &overlapped) &&
                GetLastError() != ERROR_IO_PENDING) return false;
            return GetOverlappedResult(pipe, &overlapped, &done, TRUE) && done == size;
        };
        const auto read_text = [&](std::wstring& text) {
            std::uint32_t count = 0;
            if (!read(&count, sizeof count) || count > (1u << 24)) return false;
            text.assign(count, L'\0');
            return read(text.data(), count * sizeof(wchar_t));
        };
        char request = 0;
        if (!read(&request, 1)) { DisconnectNamedPipe(pipe); continue; }
        if (request == handover::kQuery) {
            const std::int32_t code = ended_code;
            write(&code, sizeof code);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
            continue;
        }
        if (request != handover::kRun) break;  // Quit, or nonsense

        handover::Launch launch;
        bool ok = read(&launch.stdout_handle, sizeof launch.stdout_handle) &&
                  read(&launch.stderr_handle, sizeof launch.stderr_handle) &&
                  read_text(launch.plugin) && read_text(launch.command_line) &&
                  read_text(launch.directory) && read_text(launch.environment);
        GameMain game_main = ok ? load_game(launch.plugin) : nullptr;
        std::vector<std::string> arguments = arguments_of(launch.command_line);
        ok = game_main && !arguments.empty() && SetCurrentDirectoryW(launch.directory.c_str());
        const std::int32_t started = ok ? 1 : 0;
        write(&started, sizeof started);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        if (!ok) break;
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
        CloseHandle(overlapped.hEvent);
        CloseHandle(launcher_process);

        set_environment(launch.environment);
        if (launch.stdout_handle)
            set_output(1, STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(launch.stdout_handle));
        if (launch.stderr_handle)
            set_output(2, STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(launch.stderr_handle));
        std::vector<char*> argv;
        for (std::string& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        std::fprintf(stderr, "[player] hand-over: running %ls in the kept window\n",
                     launch.plugin.c_str());
        return game_main(static_cast<int>(arguments.size()), argv.data(), window, nullptr);
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    CloseHandle(overlapped.hEvent);
    CloseHandle(launcher_process);
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        std::fprintf(stderr, "[player] could not find this program's folder\n");
        return 1;
    }
    std::wstring plugin(path, length);
    plugin.resize(plugin.find_last_of(L"\\/") + 1);
    plugin += GSR_ENGINE_PLUGIN;
    const GameMain game_main = load_game(plugin);
    if (!game_main) return 1;
    // Started by the collection launcher: the game keeps its window when it
    // ends, in case it hands over.
    const bool collection = handover::launcher_pid() != 0;
    void* window = nullptr;
    const int code = game_main(argc, argv, nullptr, collection ? &window : nullptr);
    if (!collection || !window || gbarecomp::seamless::slot_of_exit_code(code) < 0)
        return code;
    return wait_and_hand_over(code, window);
}
