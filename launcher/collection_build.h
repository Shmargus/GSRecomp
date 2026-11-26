#pragma once
#include "collection_session.h"
#include "collection_preferences.h"
#include "launcher_common.h"
#include "build_resources.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __ANDROID__
#include "android_host.h"
#endif
#endif

namespace gsr_collection {
namespace fs = std::filesystem;
struct BuildSnapshot {
    bool running = false, finished = false, succeeded = false;
    int done = 0, total = 0;
    std::string stage, error;
    // Feedback while it runs; build_feedback() below puts it into words.
    // Times are seconds. "quiet" is since the build last printed anything,
    // "idle" since it last used the processor (-1: cannot tell here).
    double elapsed = 0, step_elapsed = 0, quiet = 0, idle = -1;
    double longest_quiet = 0;      // longest pause so far in this build
    // The previous successful build of this game on this PC (-1: none).
    double last_total = -1, last_step = -1, last_step_quiet = -1;
    int cpu_percent = -1;          // the build's share of the whole PC
    std::uint64_t free_ram = 0, total_ram = 0;
    std::string activity;          // its latest output line (developer builds)
    std::string other_build;       // a build program running outside this build
    fs::path log;
};

// How long each step of the last successful build took on this PC and its
// longest pause, so a slow or silent step can be compared with a measured
// run instead of a guessed limit.
struct BuildStepTime { std::string step; double seconds = 0, quiet = 0; };
struct BuildTimes { double total = -1; std::vector<BuildStepTime> steps; };
inline std::string build_step_key(std::string stage) {
    // "Compiling the game (144 files)" is the same step with another count.
    if (const auto open = stage.find(" ("); open != std::string::npos) stage.erase(open);
    for (char& c : stage) if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return stage;
}
inline BuildTimes load_build_times(const fs::path& file) {
    BuildTimes times;
    std::ifstream in(file, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream fields(line);
        std::string kind;
        std::getline(fields, kind, '\t');
        if (kind == "total") fields >> times.total;
        else if (kind == "step") {
            BuildStepTime step;
            std::string seconds, quiet;
            if (std::getline(fields, seconds, '\t') && std::getline(fields, quiet, '\t') &&
                std::getline(fields, step.step)) {
                try { step.seconds = std::stod(seconds); step.quiet = std::stod(quiet); }
                catch (const std::exception&) { continue; }
                times.steps.push_back(std::move(step));
            }
        }
    }
    return times;
}
inline void save_build_times(const fs::path& file, const BuildTimes& times) {
    std::ostringstream out;
    out << "total\t" << times.total << "\n";
    for (const auto& step : times.steps)
        out << "step\t" << step.seconds << "\t" << step.quiet << "\t" << step.step << "\n";
    const auto text = out.str();
    std::string ignored;
    gba::replace_file(file, text.data(), text.size(), &ignored);
}

inline std::string build_clock(double seconds) {
    const long long s = std::max(0LL, static_cast<long long>(seconds + 0.5));
    char text[32];
    if (s >= 3600) std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else std::snprintf(text, sizeof(text), "%lld:%02lld", s / 60, s % 60);
    return text;
}

// One wording for every launcher, plain enough for a player.
// `summary`: the time so far against the last build here. `detail`: what it
// is doing now or, after a pause longer than any before, a verdict first
// ("Still working", "Slower than last time", "Looks stuck") and then why.
// `level`: 0 normal, 1 slower than last time, 2 looks stuck (no output and
// no processor use for that long).
struct BuildFeedback { std::string summary, detail; int level = 0; };
inline BuildFeedback build_feedback(const BuildSnapshot& b) {
    BuildFeedback out;
    out.summary = "Building for " + build_clock(b.elapsed);
    if (b.last_total > 0) out.summary += " (last time: " + build_clock(b.last_total) + " in total)";
    if (b.last_step > 0)
        out.summary += ". This step: " + build_clock(b.step_elapsed) + " (last time: " + build_clock(b.last_step) + ")";
    // A pause counts as long once it beats every pause of this build so far
    // and this step's longest pause in the last build. The one-minute floor
    // is NOT measured: it keeps the first pauses of a build (before any
    // longer pause has been seen) from being called stuck. Replace it with
    // a measured value once real stalls have been recorded.
    constexpr double unmeasured_floor = 60;
    const double usual = std::max({b.longest_quiet, b.last_step_quiet, unmeasured_floor});
    if (b.quiet <= usual) {
        if (!b.activity.empty()) out.detail = "Now: " + b.activity;
        return out;
    }
    const std::string quiet = build_clock(b.quiet);
    const std::string other = b.other_build.empty() ? "" :
        " Another build is running at the same time (" + b.other_build + "), which slows both down.";
    char memory[64] = "";
    if (b.total_ram)
        std::snprintf(memory, sizeof(memory), " Free memory: %.1f of %.1f GB.",
                      b.free_ram / 1073741824.0, b.total_ram / 1073741824.0);
    if (b.idle >= 0 && b.idle > usual) {
        out.level = 2;
        out.detail = "Looks stuck: nothing has happened for " + quiet +
                     " and the build is not using the processor." + other + memory +
                     " Click here to see the build log, or cancel and build again.";
        return out;
    }
    const std::string working = b.cpu_percent >= 0
        ? "the processor is " + std::to_string(b.cpu_percent) + "% busy with it"
        : std::string("it is still running");
    if (b.last_step_quiet >= 1 && b.quiet > b.last_step_quiet) {
        out.level = 1;
        out.detail = "Slower than last time: no news for " + quiet + " (last time at most " +
                     build_clock(b.last_step_quiet) + "), but " + working + ", so let it run." +
                     other + memory + " Click here to see the build log.";
        return out;
    }
    out.detail = "Still working: no news for " + quiet + ", but " + working +
                 ". Some steps run quietly; let it run." + other;
    if (!b.activity.empty()) out.detail += " Last: " + b.activity;
    return out;
}

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
    std::map<int, std::uint64_t> cpu_by_process_;  // clock ticks per build process
#endif
    // Feedback (BuildSnapshot): when things last happened, this build's
    // steps and the last successful build's, all under mutex_.
    using Clock = std::chrono::steady_clock;
    Clock::time_point started_, step_started_, last_output_, last_cpu_, last_sample_, last_other_check_;
    double step_quiet_ = 0;
    bool cpu_known_ = false, compiled_ = false;
    std::uint64_t cpu_time_ = 0;
    BuildTimes previous_, current_;
    fs::path times_file_;
    static double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
    double step_done_before_locked() const {
        const auto key = build_step_key(state_.stage);
        for (const auto& step : current_.steps) if (step.step == key) return step.seconds;
        return 0;
    }
    // Developer builds switch between steps (each target links), so a step
    // that comes back adds to its earlier time.
    void close_step_locked(Clock::time_point now) {
        if (state_.stage.empty()) return;
        const auto key = build_step_key(state_.stage);
        const double time = seconds(now - step_started_);
        for (auto& step : current_.steps)
            if (step.step == key) { step.seconds += time; step.quiet = std::max(step.quiet, step_quiet_); return; }
        current_.steps.push_back({key, time, step_quiet_});
    }
    void stage_locked(std::string stage, Clock::time_point now) {
        if (stage == state_.stage) return;
        close_step_locked(now);
        state_.stage = std::move(stage);
        step_started_ = now; step_quiet_ = 0;
        state_.last_step = state_.last_step_quiet = -1;
        const auto key = build_step_key(state_.stage);
        for (const auto& step : current_.steps) if (step.step == key) step_quiet_ = step.quiet;
        for (const auto& step : previous_.steps)
            if (step.step == key) { state_.last_step = step.seconds; state_.last_step_quiet = step.quiet; }
    }
    void output_locked(Clock::time_point now) {
        const double gap = seconds(now - last_output_);
        state_.longest_quiet = std::max(state_.longest_quiet, gap);
        step_quiet_ = std::max(step_quiet_, gap);
        last_output_ = now;
    }
    // The build's processor use and the PC's free memory, sampled at most
    // once a second while a launcher window asks for the state.
    void measure_locked(Clock::time_point now) {
        const double wall = seconds(now - last_sample_);
        last_sample_ = now;
        const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (job_ && QueryInformationJobObject(job_, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr)) {
            const auto cpu = static_cast<std::uint64_t>(info.TotalUserTime.QuadPart + info.TotalKernelTime.QuadPart);
            if (cpu_known_ && wall > 0)
                state_.cpu_percent = static_cast<int>(std::min(100.0, (cpu - cpu_time_) / 1e7 / (wall * cpus) * 100 + 0.5));
            if (!cpu_known_ || cpu > cpu_time_) last_cpu_ = now;
            cpu_time_ = cpu; cpu_known_ = true;
        }
        MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) { state_.free_ram = memory.ullAvailPhys; state_.total_ram = memory.ullTotalPhys; }
#else
        if (pid_) {
            // Every process in the build's process group (start() makes one).
            std::uint64_t used = 0;
            std::map<int, std::uint64_t> seen;
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator("/proc", ec)) {
                const auto name = entry.path().filename().string();
                if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
                std::ifstream in(entry.path() / "stat");
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const auto close = text.rfind(')');
                if (close == std::string::npos) continue;
                // After the name: state ppid pgrp ... utime (12th) stime (13th).
                std::istringstream fields(text.substr(close + 1));
                std::string field;
                std::uint64_t value[13] = {};
                bool ok = true;
                for (int i = 0; i < 13 && ok; ++i) {
                    ok = static_cast<bool>(fields >> field);
                    if (ok && i) try { value[i] = std::stoull(field); } catch (const std::exception&) { ok = false; }
                }
                if (!ok || static_cast<pid_t>(value[2]) != pid_) continue;
                const int process = std::stoi(name);
                const std::uint64_t ticks = value[11] + value[12];
                const auto before = cpu_by_process_.find(process);
                used += before == cpu_by_process_.end() ? ticks : ticks - std::min(ticks, before->second);
                seen[process] = ticks;
            }
            cpu_by_process_ = std::move(seen);
            const long tick = sysconf(_SC_CLK_TCK);
            if (cpu_known_ && wall > 0 && tick > 0)
                state_.cpu_percent = static_cast<int>(std::min(100.0, double(used) / tick / (wall * cpus) * 100 + 0.5));
            if (!cpu_known_ || used) last_cpu_ = now;
            cpu_known_ = true;
        }
        std::ifstream meminfo("/proc/meminfo");
        for (std::string key; meminfo >> key;) {
            std::uint64_t kb = 0;
            meminfo >> kb;
            meminfo.ignore(64, '\n');
            if (key == "MemTotal:") state_.total_ram = kb * 1024;
            else if (key == "MemAvailable:") state_.free_ram = kb * 1024;
        }
#endif
    }
#ifdef _WIN32
    // A compiler or build tool running outside this build's job: another
    // build (a terminal, a second launcher) competing for the same PC.
    void find_other_build_locked() {
        state_.other_build.clear();
        if (!job_) return;
        HANDLE list = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (list == INVALID_HANDLE_VALUE) return;
        static const wchar_t* const tools[] = {L"ninja.exe", L"cmake.exe", L"cc1plus.exe", L"cc1.exe",
            L"lto1.exe", L"ld.exe", L"ld.lld.exe", L"lld-link.exe", L"clang.exe", L"clang++.exe",
            L"gsr_builder.exe"};
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(list, &entry); more && state_.other_build.empty();
             more = Process32NextW(list, &entry)) {
            for (const auto* tool : tools) {
                if (_wcsicmp(entry.szExeFile, tool) != 0) continue;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                BOOL inside = TRUE;
                if (process && IsProcessInJob(process, job_, &inside) && !inside)
                    for (const wchar_t* c = tool; *c; ++c) state_.other_build += static_cast<char>(*c);
                if (process) CloseHandle(process);
                break;
            }
        }
        CloseHandle(list);
    }
#endif
    void sample_locked() {
        if (!state_.running) return;
        const auto now = Clock::now();
        if (now - last_sample_ >= std::chrono::seconds(1)) measure_locked(now);
        state_.elapsed = seconds(now - started_);
        state_.step_elapsed = step_done_before_locked() + seconds(now - step_started_);
        state_.quiet = seconds(now - last_output_);
        state_.idle = cpu_known_ ? seconds(now - last_cpu_) : -1;
#ifdef _WIN32
        // Looked for only during a long pause, when it would be shown.
        if (state_.quiet > state_.longest_quiet && now - last_other_check_ >= std::chrono::seconds(5)) {
            last_other_check_ = now;
            find_other_build_locked();
        }
#endif
    }
    // The latest developer build line in a few words: ninja's
    // "[3/9] Building CXX object CMakeFiles/x.dir/src/a.cpp.obj" becomes
    // "Compiling a.cpp".
    void set_activity_locked(const std::string& value) {
        const auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) return;
        std::string text = value.substr(first);
        if (text.front() == '[') {
            if (const auto end = text.find("] "); end != std::string::npos) text.erase(0, end + 2);
            for (const char* verb : {"Building CXX object ", "Building C object ", "Linking CXX ", "Linking C "}) {
                if (text.rfind(verb, 0) != 0) continue;
                auto file = text.substr(text.find_last_of("/\\ ") + 1);
                for (const std::string suffix : {".obj", ".o"})
                    if (file.size() > suffix.size() && file.ends_with(suffix)) { file.resize(file.size() - suffix.size()); break; }
                text = std::string(verb[0] == 'B' ? "Compiling " : "Linking ") + file;
                break;
            }
        }
        if (text.size() > 120) {
            std::size_t cut = 117;
            while (cut && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
            text = text.substr(0, cut) + "...";
        }
        state_.activity = std::move(text);
    }
    void finish(bool success, std::string error) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.running = false; state_.finished = true;
        state_.succeeded = success && !cancelled_;
        state_.error = cancelled_ ? "Build cancelled." : std::move(error);
        // Only builds that compiled something become the times to compare with.
        if (state_.succeeded && compiled_ && !times_file_.empty()) {
            const auto now = Clock::now();
            close_step_locked(now);
            current_.total = seconds(now - started_);
            save_build_times(times_file_, current_);
        }
    }
    void run(fs::path collection, fs::path executable, fs::path root,
             fs::path rom, int choice, bool player, bool reduced) {
        const fs::path directory = player ? executable.parent_path() : collection;
        std::ofstream log;
        bool done = false;
        std::string reported, pending;
        auto line = [&](std::string value) {
            if (!value.empty() && value.back() == '\r') value.pop_back();
            std::lock_guard<std::mutex> lock(mutex_);
            const auto now = Clock::now();
            output_locked(now);
            if (value.rfind("@stage ", 0) == 0) {
                stage_locked(value.substr(7), now); state_.done = state_.total = 0;
                if (state_.stage.find("Compiling") != std::string::npos) compiled_ = true;
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
                    state_.done = d; state_.total = t; compiled_ = true;
                    stage_locked(value.find("Linking") != std::string::npos ? "Linking game" : "Building game", now);
                }
                set_activity_locked(value);
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
        auto complete = [&](bool exited_ok) {
            // A final protocol line may arrive without its terminating newline.
            if (!pending.empty() && !discarding) line(pending);
            if (!exited_ok || (player && !done)) {
#ifndef _WIN32
                if (reported.empty() && reduced)
                    reported = "The limited build failed. It needs systemd 254+ with user CPU and memory controls. See local/collection-build.log.";
#endif
                finish(false, reported.empty() ? "The build failed. See local/collection-build.log." : std::move(reported));
                return;
            }
            if (player && !cancelled_) {
                try {
                    const auto& game = gsr_launcher::kGames[choice];
                    std::string record_error;
                    if (!gsr_launcher::finish_player_build_record(root, game, rom, &record_error)) {
                        finish(false, std::move(record_error));
                        return;
                    }
                    const auto fingerprint = gsr_launcher::release_fingerprint(root, game);
                    if (!gba::replace_file(root / game.release_stamp, fingerprint.data(),
                                           fingerprint.size(), &record_error)) {
                        finish(false, std::move(record_error));
                        return;
                    }
                } catch (const std::exception& e) {
                    finish(false, e.what());
                    return;
                }
            }
            finish(true, {});
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
        if (player) {
            cmd += L" --rom " + quote(rom.wstring());
            if (reduced) cmd += L" --low-resources";
        }
        else cmd += L" -NoProfile -ExecutionPolicy Bypass -File " +
            quote((collection / "scripts" / "build_dev.ps1").wstring()) +
            L" -Game " + (choice == 0 ? std::wstring(L"BrokenSeal") : std::wstring(L"LostAge")) +
            L" -LauncherProcessId " + std::to_wstring(GetCurrentProcessId()) + L" -SkipLauncher" +
            L" -BuildRom " + quote(rom.wstring());
        if (!player && reduced) cmd += L" -CpuPercent 50 -RamPercent 50 -CpuCapInherited";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input; startup.hStdOutput = startup.hStdError = writer;
        PROCESS_INFORMATION process{};
        // "Skip the ROM check" (any_rom.h): the builder inherits it from here,
        // as it does from a game session.
        if (player)
            SetEnvironmentVariableA(gbarecomp::kAnyRomEnv,
                gsr_session::load_launcher_audio_settings(root).any_rom ? "1" : "0");
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (reduced) {
            const auto resources = build_resources();
            if (!resources.ram || !resources.cpus) {
                if (job) CloseHandle(job);
                CloseHandle(reader); CloseHandle(writer); CloseHandle(input); release();
                finish(false, "Could not read CPU and RAM limits. The build was not started."); return;
            }
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
            limits.JobMemoryLimit = SIZE_T(resources.ram / 2);
        }
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu.CpuRate = 50 * 100;
        const bool configured = job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            (!reduced || SetInformationJobObject(job, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu)));
        bool started = configured &&
            CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(), &startup, &process);
        CloseHandle(writer); CloseHandle(input);
        if (!started || !AssignProcessToJobObject(job, process.hProcess)) {
            if (process.hProcess) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, INFINITE); CloseHandle(process.hProcess); CloseHandle(process.hThread); }
            if (job) CloseHandle(job);
            CloseHandle(reader); release();
            finish(false, "Windows could not start the build with its required limits."); return;
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
        complete(code == 0);
        release();
#else
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
        const auto resources = reduced ? build_resources() : BuildResources{};
        if (reduced && (!resources.ram || !resources.cpus)) {
            close(pipefd[0]); close(pipefd[1]); close(serial);
            finish(false, "Could not read CPU and RAM limits. The build was not started."); return;
        }
        const std::string quota = "CPUQuota=" + std::to_string(std::uint64_t(resources.cpus) * 50) + "%";
        const std::string memory = "MemoryMax=" + std::to_string(resources.ram / 2);
        const pid_t parent = getpid();
        // "Skip the ROM check" (any_rom.h): the builder inherits it from here,
        // as it does from a game session.
        const bool any_rom = player && gsr_session::load_launcher_audio_settings(root).any_rom;
#ifdef __ANDROID__
        // The builder runs from the app's library folder, so it cannot find
        // its parts beside itself: every path is given (android_host.cpp
        // unpacked and linked them into the game folder).
        const fs::path lib = gsr_android::native_library_dir();
        std::vector<std::string> android_args = {program,
            "--rom", romarg,
            "--data", (root / "builder" / "data").string(),
            "--engine", (root / "builder" / "engine").string(),
            "--toolchain", (root / "builder" / "toolchain").string(),
            "--recompiler", (root / "builder" / "gba_recompile").string(),
            "--work", (root / "builder" / "work").string(),
            "--out", root.string(),
            "--llvm", (lib / "libllvm.so").string(),
            "--engine-library", (lib / gsr_launcher::kGames[choice].engine).string()};
        std::vector<char*> android_argv;
        for (auto& argument : android_args) android_argv.push_back(argument.data());
        android_argv.push_back(nullptr);
#endif
        pid_t child = fork();
        if (child == 0) {
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) _exit(127);
            setpgid(0, 0); close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO); dup2(pipefd[1], STDERR_FILENO); close(pipefd[1]);
            if (player) setenv(gbarecomp::kAnyRomEnv, any_rom ? "1" : "0", 1);
            if (chdir(cwd.c_str()) == 0) {
                if (reduced) execlp("systemd-run", "systemd-run", "--user", "--scope", "--quiet", "--collect",
                    "--no-ask-password", "--expand-environment=no", "--property", quota.c_str(),
                    "--property", memory.c_str(), "--", program.c_str(), "--rom", romarg.c_str(),
                    "--low-resources", static_cast<char*>(nullptr));
#ifdef __ANDROID__
                else execv(program.c_str(), android_argv.data());
#else
                else execl(program.c_str(), program.c_str(), "--rom", romarg.c_str(), static_cast<char*>(nullptr));
#endif
            }
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
        complete(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        close(serial);
#endif
    }
public:
    // A player package builds with its builder; the source tree builds with
    // the developer scripts.
    static bool is_player(const fs::path& game_root) {
        std::error_code ec;
        return game_root.parent_path().filename() == "games" ||
               fs::is_directory(game_root / "builder", ec);
    }
    CollectionBuild() = default;
    CollectionBuild(const CollectionBuild&) = delete;
    CollectionBuild& operator=(const CollectionBuild&) = delete;
    ~CollectionBuild() { stop(); }
    BuildSnapshot snapshot() { std::lock_guard<std::mutex> lock(mutex_); sample_locked(); return state_; }
    BuildSnapshot poll() {
        std::lock_guard<std::mutex> lock(mutex_);
        sample_locked();
        auto state = state_;
        state_.finished = false;
        return state;
    }
    void stop() {
        cancel();
        if (worker_.joinable()) worker_.join();
    }
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
    bool start(const fs::path& collection, int choice, const fs::path& rom, std::string& error, bool reduced = false) {
        error.clear();
        if (snapshot().running) { error = "A build is already running."; return false; }
        if (worker_.joinable()) worker_.join();
        try {
            fs::path root = gsr_session::game_root(collection, choice);
            std::error_code ec;
            const bool player = is_player(root);
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
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = {}; state_.running = true; cancelled_ = false;
                state_.log = collection / "local" / "collection-build.log";
                // Compared only with builds of the same game, kind and limits.
                times_file_ = collection / "local" / (std::string("build-times-") +
                    (choice ? "the-lost-age" : "the-broken-seal") + (player ? "-player" : "-developer") +
                    (reduced ? "-limited" : "") + ".txt");
                previous_ = load_build_times(times_file_); current_ = {};
                state_.last_total = previous_.total;
                started_ = step_started_ = last_output_ = last_cpu_ = last_sample_ = last_other_check_ = Clock::now();
                step_quiet_ = 0; cpu_time_ = 0; cpu_known_ = compiled_ = false;
#ifndef _WIN32
                cpu_by_process_.clear();
#endif
                stage_locked(player ? "Starting build" : "Building game", started_);
            }
            worker_ = std::thread([this, collection, executable, root, rom, choice, player, reduced] {
#ifdef __ANDROID__
                gsr_android::keep_awake(true);
#endif
                try { run(collection, executable, root, rom, choice, player, reduced); }
                catch (const std::exception& e) { finish(false, e.what()); }
#ifdef __ANDROID__
                gsr_android::keep_awake(false);
#endif
            });
            return true;
        } catch (const std::exception& e) { error = e.what(); finish(false, error); return false; }
    }
};

// "Skip the ROM check" lets a player play a patched ROM, but the game code is
// always built from a clean one (the translator refuses any other). With it
// on, every build (player or developer) asks for:
//   1. the clean ROM, then
//   2. the ROM to play, unless it is already chosen (`ask_play`).
inline bool skip_rom_check_on(const fs::path& collection, int choice) {
    return gsr_session::load_launcher_audio_settings(
        gsr_session::game_root(collection, choice)).any_rom;
}
inline std::string clean_build_rom_message(const fs::path& collection, int choice,
                                           bool ask_play) {
    const auto& game = gsr_launcher::kGames[choice];
    const int revision = rom_language(collection, choice);
    const std::string rom = std::string(game.title) + " (" + game.rom_names[revision] + ")";
    if (!gsr_launcher::rom_is_language(game, revision)) {
        // A supported patch (Reloaded) is built from its own ROM.
        const std::string text = "\"Skip the ROM check\" is on. " + rom +
                                 " is built from its own ROM, not a clean one.\n\n";
        if (!ask_play) return text + "Next, choose your " + rom + " ROM.";
        return text + "Two file windows open next, in this order:\n\n"
                      "1. Choose your " + rom + " ROM. The game is built from it.\n"
                      "2. Choose the ROM you want to PLAY (the same one).";
    }
    std::string text = "\"Skip the ROM check\" is on: you can play a patched ROM, but the "
                       "game is always built from a clean, unpatched ROM.\n\n";
    if (!ask_play)
        return text + "Next, choose your CLEAN " + rom + " ROM. The game is built from it; "
                      "you will still play the ROM you already chose.";
    return text + "Two file windows open next, in this order:\n\n"
                  "1. Choose your CLEAN " + rom + " ROM. The game is built from it.\n"
                  "2. Choose the ROM you want to PLAY, such as your patched ROM.";
}
inline bool check_clean_build_rom(const fs::path& rom, const fs::path& collection, int choice,
                                  std::string& error) {
    return gsr_launcher::validate_rom(rom, &error, gsr_launcher::kGames[choice], false,
                                      rom_language(collection, choice));
}
} // namespace gsr_collection
