#pragma once
#include "collection_session.h"
#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gsr_collection {
namespace fs = std::filesystem;
struct BuildSnapshot {
    bool running = false, finished = false, succeeded = false;
    int done = 0, total = 0;
    std::string stage, error;
};

class CollectionBuild {
    std::mutex mutex_;
    BuildSnapshot state_;
    std::thread worker_;
    std::atomic<bool> cancelled_{false};
#ifdef _WIN32
    HANDLE job_ = nullptr;
    static std::wstring quote(const std::wstring& s) {
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (wchar_t c : s) {
            if (c == L'\\') { ++slashes; continue; }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0; out += c;
        }
        out.append(slashes * 2, L'\\');
        return out + L'"';
    }
#else
    pid_t pid_ = 0;
#endif
    void finish(bool success, std::string error) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.running = false; state_.finished = true;
        state_.succeeded = success && !cancelled_;
        state_.error = cancelled_ ? "Build cancelled." : std::move(error);
    }
    void run(fs::path collection, fs::path executable, fs::path directory,
             fs::path rom, int choice, bool player) {
        std::ofstream log;
        bool done = false;
        std::string reported, pending;
        auto line = [&](std::string value) {
            if (!value.empty() && value.back() == '\r') value.pop_back();
            std::lock_guard<std::mutex> lock(mutex_);
            if (value.rfind("@stage ", 0) == 0) {
                state_.stage = value.substr(7); state_.done = state_.total = 0;
            } else if (value.rfind("@progress ", 0) == 0) {
                int d = 0, t = 0;
                if (std::sscanf(value.c_str() + 10, "%d %d", &d, &t) == 2 && d >= 0 && t >= 0) {
                    state_.done = d; state_.total = t;
                }
            } else if (value == "@done") done = true;
            else if (value.rfind("@error ", 0) == 0) reported = value.substr(7);
            else if (!player) {
                int d = 0, t = 0;
                if (std::sscanf(value.c_str(), "[%d/%d]", &d, &t) == 2 && d >= 0 && t > 0) {
                    state_.done = d; state_.total = t;
                    state_.stage = value.find("Linking") != std::string::npos ? "Linking game" : "Building game";
                }
            }
        };
        bool discarding = false;
        auto consume = [&](const char* bytes, size_t count) {
            log.write(bytes, static_cast<std::streamsize>(count)); log.flush();
            for (size_t i = 0; i < count; ++i) {
                if (bytes[i] == '\n') {
                    if (!discarding) line(pending);
                    pending.clear(); discarding = false;
                } else if (!discarding) {
                    if (pending.size() < 65536) pending += bytes[i];
                    else { pending.clear(); discarding = true; }
                }
            }
        };
#ifdef _WIN32
        HANDLE serial = nullptr;
        if (player) {
            serial = CreateMutexW(nullptr, FALSE, L"Local\\GoldenSunCollectionBuild");
            DWORD result = serial ? WaitForSingleObject(serial, 0) : WAIT_FAILED;
            if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
                if (serial) CloseHandle(serial);
                finish(false, "Another collection build is running."); return;
            }
        }
        auto release = [&] { if (serial) { ReleaseMutex(serial); CloseHandle(serial); } };
        log.open(collection / "local" / "collection-build.log", std::ios::binary | std::ios::trunc);
        if (!log) { release(); finish(false, "Could not open local/collection-build.log."); return; }
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE reader = nullptr, writer = nullptr;
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (!CreatePipe(&reader, &writer, &sa, 0) ||
            !SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0) || input == INVALID_HANDLE_VALUE) {
            if (reader) CloseHandle(reader);
            if (writer) CloseHandle(writer);
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            release(); finish(false, "Could not create the build output pipe."); return;
        }
        std::wstring cmd = quote(executable.wstring());
        if (player) cmd += L" --rom " + quote(rom.wstring());
        else cmd += L" -NoProfile -ExecutionPolicy Bypass -File " +
            quote((collection / "scripts" / "build_dev.ps1").wstring()) +
            L" -Game " + (choice == 0 ? std::wstring(L"BrokenSeal") : std::wstring(L"LostAge")) +
            L" -LauncherProcessId " + std::to_wstring(GetCurrentProcessId()) + L" -SkipLauncher";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input; startup.hStdOutput = startup.hStdError = writer;
        PROCESS_INFORMATION process{};
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        bool started = job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(), &startup, &process);
        CloseHandle(writer); CloseHandle(input);
        if (!started || !AssignProcessToJobObject(job, process.hProcess)) {
            if (process.hProcess) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, INFINITE); CloseHandle(process.hProcess); CloseHandle(process.hThread); }
            if (job) CloseHandle(job);
            CloseHandle(reader); release();
            finish(false, "Windows could not start or contain the build process."); return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = job;
            if (cancelled_) TerminateJobObject(job, 1);
            else if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) TerminateJobObject(job, 1);
        }
        CloseHandle(process.hThread);
        char buffer[4096]; DWORD count = 0;
        while (ReadFile(reader, buffer, sizeof(buffer), &count, nullptr) && count) consume(buffer, count);
        CloseHandle(reader); WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
        { std::lock_guard<std::mutex> lock(mutex_); job_ = nullptr; CloseHandle(job); }
        release();
        bool ok = code == 0 && (!player || done);
#else
        (void)choice; (void)player;
        int serial = open((collection / "local" / "collection-build.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (serial < 0 || flock(serial, LOCK_EX | LOCK_NB) != 0) {
            if (serial >= 0) close(serial);
            finish(false, "Another collection build is running or the build lock is unavailable."); return;
        }
        log.open(collection / "local" / "collection-build.log", std::ios::binary | std::ios::trunc);
        if (!log) { close(serial); finish(false, "Could not open local/collection-build.log."); return; }
        int pipefd[2];
        if (pipe(pipefd) != 0) { close(serial); finish(false, "Could not create the build output pipe."); return; }
        if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) < 0) {
            close(pipefd[0]); close(pipefd[1]); close(serial);
            finish(false, "Could not secure the build output pipe."); return;
        }
        const std::string program = executable.string(), cwd = directory.string(), romarg = rom.string();
        pid_t child = fork();
        if (child == 0) {
            setpgid(0, 0); close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO); close(pipefd[1]);
            if (chdir(cwd.c_str()) == 0) execl(program.c_str(), program.c_str(), "--rom", romarg.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(pipefd[1]);
        if (child < 0) { close(pipefd[0]); close(serial); finish(false, "Could not start the build process."); return; }
        setpgid(child, child);
        { std::lock_guard<std::mutex> lock(mutex_); pid_ = child; if (cancelled_) kill(-child, SIGKILL); }
        char buffer[4096]; ssize_t count;
        while ((count = read(pipefd[0], buffer, sizeof(buffer))) != 0) {
            if (count > 0) consume(buffer, static_cast<size_t>(count));
            else if (errno != EINTR) break;
        }
        close(pipefd[0]); int status = 0; pid_t waited;
        do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
        { std::lock_guard<std::mutex> lock(mutex_); pid_ = 0; kill(-child, SIGKILL); }
        close(serial);
        bool ok = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && done;
#endif
        if (!pending.empty() && !discarding) line(pending);
        // A final protocol line may arrive without its terminating newline.
#ifdef _WIN32
        ok = code == 0 && (!player || done);
#else
        ok = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && done;
#endif
        finish(ok, ok ? std::string() : (reported.empty() ? "The build failed. See local/collection-build.log." : reported));
    }
public:
    CollectionBuild() = default;
    CollectionBuild(const CollectionBuild&) = delete;
    CollectionBuild& operator=(const CollectionBuild&) = delete;
    ~CollectionBuild() { cancel(); if (worker_.joinable()) worker_.join(); }
    BuildSnapshot snapshot() { std::lock_guard<std::mutex> lock(mutex_); return state_; }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!state_.running) return;
        cancelled_ = true;
#ifdef _WIN32
        if (job_) TerminateJobObject(job_, 1);
#else
        if (pid_) kill(-pid_, SIGKILL);
#endif
    }
    bool start(const fs::path& collection, int choice, const fs::path& rom, std::string& error) {
        error.clear();
        if (snapshot().running) { error = "A build is already running."; return false; }
        if (worker_.joinable()) worker_.join();
        try {
            fs::path root = gsr_session::game_root(collection, choice);
            std::error_code ec;
            bool player = root.parent_path().filename() == "games" || fs::is_directory(root / "builder", ec);
#ifdef _WIN32
            fs::path executable;
            if (player) executable = root / "builder" / "gsr_builder.exe";
            else {
                wchar_t system[MAX_PATH]; UINT length = GetSystemDirectoryW(system, MAX_PATH);
                if (!length || length >= MAX_PATH) { error = "Could not find Windows PowerShell."; return false; }
                executable = fs::path(system) / "WindowsPowerShell" / "v1.0" / "powershell.exe";
                if (!fs::is_regular_file(collection / "scripts" / "build_dev.ps1", ec)) { error = "scripts/build_dev.ps1 is missing."; return false; }
            }
#else
            if (!player) { error = "Developer builds from the collection launcher are supported on Windows only."; return false; }
            fs::path executable = root / "builder" / "gsr_builder";
#endif
            if (!fs::is_regular_file(executable, ec)) { error = "The game builder is missing."; return false; }
            if (player && !fs::is_regular_file(rom, ec)) { error = "Choose a valid game ROM."; return false; }
            fs::create_directories(collection / "local");
            { std::lock_guard<std::mutex> lock(mutex_); state_ = {}; state_.running = true; state_.stage = player ? "Starting build" : "Building game"; cancelled_ = false; }
            worker_ = std::thread([this, collection, executable, root, rom, choice, player] {
                try { run(collection, executable, player ? executable.parent_path() : collection, rom, choice, player); }
                catch (const std::exception& e) { finish(false, e.what()); }
            });
            return true;
        } catch (const std::exception& e) { error = e.what(); finish(false, error); return false; }
    }
};
} // namespace gsr_collection
