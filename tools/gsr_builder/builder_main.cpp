// gsr_builder -- builds GoldenSunGame.dll on the player's PC from their ROM.
//
// The release ships only our own code: the engine (GoldenSunRecomp.exe),
// this builder, the translator (gba_recompile.exe), a trimmed compiler and
// the configuration data. Everything from Golden Sun, code included, comes
// from the player's ROM, here:
//
//   1. check the ROM (SHA-1),
//   2. translate it into C++ (config/usa/game_code_plan.txt: the main
//      program, the RAM-copied routines, the overlays, which the game's own
//      decompressor unpacks via guest_call.h, the synthesized variants and
//      the particle stamps),
//   3. compile that with the bundled GCC and link GoldenSunGame.dll against
//      the engine's import library.
//
// The launcher runs it and reads its progress lines from stdout:
//   @stage <text>
//   @progress <done> <total>
//   @done
//   @error <text>
//
// Layout (all overridable): <root>/builder/gsr_builder.exe, with data/,
// engine/, mingw64/ (the compiler), gba_recompile.exe beside it; the DLL
// goes to <root>.
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "block_timing.h"
#include "guest_call.h"
#include "sha1.h"

namespace fs = std::filesystem;
using gsr::builder::GuestMachine;

namespace {

constexpr const char* kRomSha1 = "5c4695205413df7db52b9a184815a07783999971";
constexpr const char* kBuilderVersion = "2";  // 2: block timing (block_timing.cpp)

// File names that differ between the Windows and Linux releases.
#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
constexpr const char* kGameLibrary = "GoldenSunGame.dll";
constexpr const char* kEngineExe = "GoldenSunRecomp.exe";
#else
constexpr const char* kExeSuffix = "";
constexpr const char* kGameLibrary = "libGoldenSunGame.so";
constexpr const char* kEngineExe = "GoldenSunRecomp";
#endif

// ---------------------------------------------------------------- output

std::mutex g_out_mutex;
std::ofstream g_log;

void emit(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_out_mutex);
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_log) g_log << buf << '\n' << std::flush;
}

void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_out_mutex);
    if (g_log) g_log << s << '\n' << std::flush;
}

// ---------------------------------------------------------------- files

std::vector<std::uint8_t> read_bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// The end of a failed step's own log, copied into build-log.txt so that one
// file is enough to see why a build failed.
void log_tail(const fs::path& log, std::size_t max_lines = 30) {
    std::istringstream in(read_text(log));
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    const std::size_t first = lines.size() > max_lines ? lines.size() - max_lines : 0;
    log_line("---- " + log.filename().string() + (first ? " (last lines)" : "") + " ----");
    for (std::size_t i = first; i < lines.size(); ++i) log_line(lines[i]);
    log_line("----");
}

bool write_bytes(const fs::path& p, const void* data, std::size_t n) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

bool write_text(const fs::path& p, const std::string& s) {
    return write_bytes(p, s.data(), s.size());
}

std::string sha1_hex(const void* p, std::size_t n) {
    return gba::sha1(p, n).hex();
}

std::string hex(std::uint32_t v, int width, bool upper) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), upper ? "%0*X" : "%0*x", width, v);
    return buf;
}

// ---------------------------------------------------------------- processes

#ifdef _WIN32
HANDLE g_job = nullptr;  // kills every child if the builder is stopped

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') { q.append(backslashes * 2 + 1, L'\\'); q.push_back(L'"'); backslashes = 0; continue; }
        q.append(backslashes, L'\\');
        backslashes = 0;
        q.push_back(c);
    }
    q.append(backslashes * 2, L'\\');
    q.push_back(L'"');
    return q;
}

// Environment for child processes: this one's, with PATH replaced so the
// bundled toolchain (and nothing else on the PC) is found.
std::wstring g_child_env;

void set_child_path(const fs::path& toolchain_bin) {
    std::wstring env;
    LPWCH block = GetEnvironmentStringsW();
    for (LPWCH p = block; *p; p += wcslen(p) + 1) {
        std::wstring var(p);
        if (_wcsnicmp(var.c_str(), L"PATH=", 5) == 0) continue;
        env += var;
        env.push_back(L'\0');
    }
    FreeEnvironmentStringsW(block);
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    env += L"PATH=" + toolchain_bin.wstring() + L";" + sysdir;
    env.push_back(L'\0');
    env.push_back(L'\0');
    g_child_env = env;
}

// Run exe with args; stdout/stderr go to `log` (appended). Returns the exit
// code, or -1 if it could not start.
int run_process(const fs::path& exe, const std::vector<std::string>& args,
                const fs::path& log, const fs::path& cwd = {}) {
    std::wstring cmd = quote(exe.wstring());
    for (const std::string& a : args) cmd += L" " + quote(widen(a));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.wstring().c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    const BOOL ok = CreateProcessW(
        nullptr, buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        g_child_env.empty() ? nullptr : g_child_env.data(),
        cwd.empty() ? nullptr : cwd.wstring().c_str(), &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok) return -1;
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
#else
// Children find the bundled toolchain first, then the system's basics. The
// bundled compiler's own libraries (GMP, MPFR, ISL, BFD...) are in
// toolchain/hostlib, which few distros have at the right versions.
void set_child_path(const fs::path& toolchain_bin) {
    const std::string path = toolchain_bin.string() + ":/usr/bin:/bin";
    setenv("PATH", path.c_str(), 1);
    const fs::path hostlib = toolchain_bin.parent_path() / "hostlib";
    if (fs::is_directory(hostlib)) setenv("LD_LIBRARY_PATH", hostlib.c_str(), 1);
}

// Run exe with args; stdout/stderr go to `log` (appended). Returns the exit
// code, or -1 if it could not start. Every child is killed if the builder
// dies (PR_SET_PDEATHSIG), as the Windows job object does.
int run_process(const fs::path& exe, const std::vector<std::string>& args,
                const fs::path& log, const fs::path& cwd = {}) {
    // Everything the child needs is built before fork: the builder runs
    // compilers from several threads, and after fork only async-signal-safe
    // calls are allowed.
    const std::string exe_s = exe.string(), log_s = log.string(),
                      cwd_s = cwd.string();
    std::vector<std::string> owned;
    owned.push_back(exe_s);
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(127);
        const int fd = open(log_s.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        if (!cwd_s.empty() && chdir(cwd_s.c_str()) != 0) _exit(127);
        execv(exe_s.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        return code == 127 ? -1 : code;
    }
    return -1;
}
#endif

// Run `tasks` on `jobs` threads, reporting progress; stops handing out new
// tasks after the first failure and returns its message.
bool run_parallel(const std::vector<std::function<bool(std::string*)>>& tasks,
                  int jobs, std::string* error) {
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<bool> failed{false};
    std::mutex err_mutex;
    const std::size_t total = tasks.size();
    emit("@progress 0 %zu", total);
    auto worker = [&] {
        for (;;) {
            if (failed) return;
            const std::size_t i = next++;
            if (i >= total) return;
            std::string e;
            if (!tasks[i](&e)) {
                std::lock_guard<std::mutex> lock(err_mutex);
                if (!failed.exchange(true)) *error = e;
                return;
            }
            emit("@progress %zu %zu", ++done, total);
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < std::max(1, jobs); ++t) threads.emplace_back(worker);
    for (auto& t : threads) t.join();
    return !failed;
}

// ---------------------------------------------------------------- setup

struct Setup {
    fs::path rom;
    fs::path data;        // main.toml, transient-*.toml, overlays/, plan
    fs::path engine;      // include/, libGoldenSunRecomp_api.a
    fs::path toolchain;   // bin/g++.exe
    fs::path recompiler;  // gba_recompile.exe
    fs::path work;        // generated sources, objects, logs
    fs::path out_dir;     // where GoldenSunGame.dll goes
    int jobs = 0;
    bool keep_work = false;
    bool generate_only = false;
};

fs::path gen_dir(const Setup& s) { return s.work / "gen"; }
fs::path input_dir(const Setup& s) { return s.work / "inputs"; }
fs::path logs_dir(const Setup& s) { return s.work / "logs"; }

bool recompile(const Setup& s, const std::vector<std::string>& args,
               const std::string& name, std::string* error) {
    std::vector<std::string> a = args;
    a.push_back("--slim");
    const fs::path log = logs_dir(s) / (name + ".log");
    std::error_code ec;
    fs::remove(log, ec);
    const int rc = run_process(s.recompiler, a, log);
    if (rc != 0) {
        log_tail(log);
        *error = "translating " + name + " failed (" + std::to_string(rc) +
                 "); see " + log.string();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- plan

struct PlanStep {
    std::string kind;
    std::vector<std::string> args;
};

std::vector<PlanStep> read_plan(const fs::path& p) {
    std::vector<PlanStep> steps;
    std::istringstream in(read_text(p));
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        PlanStep step;
        if (!(words >> step.kind)) continue;
        std::string w;
        while (words >> w) step.args.push_back(w);
        steps.push_back(step);
    }
    return steps;
}

struct Overlay {
    std::string id;
    std::uint32_t rom_offset;
    std::uint32_t start;
    std::uint32_t text_end;
    std::string text_sha1;
};

std::vector<Overlay> read_registry(const fs::path& p) {
    std::vector<Overlay> out;
    const std::regex re(
        R"re(^GSR_OVERLAY\((rom_([0-9a-f]+)), (0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u, "([0-9a-f]+)"\))re");
    std::istringstream in(read_text(p));
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (!std::regex_search(line, m, re)) continue;
        out.push_back({m[1], static_cast<std::uint32_t>(std::stoul(m[2], nullptr, 16)),
                       static_cast<std::uint32_t>(std::stoul(m[3], nullptr, 16)),
                       static_cast<std::uint32_t>(std::stoul(m[4], nullptr, 16)), m[5]});
    }
    return out;
}

// ---------------------------------------------------------------- overlays

// Func_2fb0's two steps, run from the ROM: the decompressor at 0x08002544
// (r0 = compressed source, r1 = destination; returns the size), then the
// BL fix-up at 0x08002D5C over the result.
bool unpack_overlay(const std::vector<std::uint8_t>& rom, const Overlay& o,
                    std::vector<std::uint8_t>* image, std::string* error) {
    GuestMachine m(rom);
    const std::uint32_t dest = o.start;
    std::uint32_t size = 0;
    const std::uint32_t unpack_args[4] = {0x08000000u + o.rom_offset, dest, 0, 0};
    if (!m.call(0x08002544u, unpack_args, &size, error)) return false;
    std::uint32_t ignored = 0;
    const std::uint32_t fix_args[4] = {dest, size, 0, 0};
    if (!m.call(0x08002D5Cu, fix_args, &ignored, error)) return false;
    const std::size_t off = dest - 0x02000000u;
    if (off + size > m.ewram().size()) {
        *error = "overlay " + o.id + " is larger than expected";
        return false;
    }
    image->assign(m.ewram().begin() + static_cast<std::ptrdiff_t>(off),
                  m.ewram().begin() + static_cast<std::ptrdiff_t>(off + size));
    const std::uint32_t text_off = o.start - dest;
    if (o.text_end <= o.start || text_off + (o.text_end - o.start) > image->size() ||
        sha1_hex(image->data() + text_off, o.text_end - o.start) != o.text_sha1) {
        *error = "overlay " + o.id + " did not unpack to the expected code";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- synth

constexpr std::uint32_t kRomBase = 0x08000000u;
constexpr std::uint32_t kRuntimeBase = 0x03000000u;
constexpr std::uint32_t kCopySource = 0x08000770u;
std::uint32_t rom_address(std::uint32_t runtime) { return kCopySource + (runtime - kRuntimeBase); }
std::uint32_t rom_offset(std::uint32_t runtime) { return rom_address(runtime) - kRomBase; }

struct Variant {
    std::string dir, prefix;
    std::vector<std::uint8_t> rom;
    std::string toml;
};

// tools/build_synthesized_dc8_variant.py
Variant synth_dc8(const std::vector<std::uint8_t>& rom, std::uint32_t tmpl) {
    constexpr std::uint32_t kEntry = 0x03000828u, kEnd = 0x030008CCu;
    Variant v;
    v.rom = rom;
    const std::uint32_t source = rom_offset(tmpl);
    const std::uint32_t dest = rom_offset(kEntry);
    for (int it = 0; it < 4; ++it) {
        const std::uint32_t base = dest + static_cast<std::uint32_t>(it) * 0x24u;
        std::memcpy(&v.rom[base], &rom[source], 8);
        std::memcpy(&v.rom[base + 0x0C], &rom[source + 8], 16);
    }
    const std::string t = hex(tmpl, 8, false);
    const std::string patched = sha1_hex(v.rom.data(), v.rom.size());
    const std::string image = sha1_hex(&v.rom[rom_offset(kEntry)], kEnd - kEntry);
    v.dir = "synth_dc8_" + t;
    v.prefix = "gsr_synth_dc8_" + t + "_";
    v.toml =
        "# Private generated input; do not commit.\n"
        "# Ring event #3295783 proves ARM writer 0x03000800 and template 0x" + t + ".\n"
        "[program]\n"
        "name = \"Golden Sun synthesized Func_dc8 variant 0x" + t + "\"\n"
        "id = \"golden_sun_usa_synth_dc8_" + t + "\"\n"
        "load_address = 0x08000000\n"
        "size = 0x00800000\n"
        "entry_pc = 0x03000820\n"
        "speculative_literal_harvest = false\n"
        "codegen_shards = 1\n"
        "\n"
        "[identity]\n"
        "sha1 = \"" + patched + "\"\n"
        "\n"
        "[[code_copy]]\n"
        "runtime_start = 0x03000820\n"
        "source_start = 0x08000f90\n"
        "size = 0x000000ac\n"
        "name = \"synthesized_Func_dc8_0x" + t + "\"\n"
        "note = \"24-byte template repeated into four code holes; complete executable SHA-1 " + image + "\"\n"
        "\n"
        "[[extra_func]]\n"
        "addr = 0x03000820\n"
        "mode = \"arm\"\n"
        "name = \"Func_dc8_synth_0x" + t + "\"\n"
        "note = \"ARM loop header immediately preceding the synthesized span; backedge from 0x030008c4\"\n"
        "\n"
        "[[resume_range]]\n"
        "start = 0x03000820\n"
        "end = 0x030008cc\n"
        "mode = \"arm\"\n"
        "note = \"Whole loop including its 0x03000820 header; valid after writer completes until its next pass or IWRAM reset\"\n";
    return v;
}

// tools/build_synthesized_dc8_slot2_variant.py
Variant synth_dc8b(const std::vector<std::uint8_t>& rom, int variant) {
    constexpr std::uint32_t kLoopStart = 0x03000A58u, kLoopEnd = 0x03000A98u;
    constexpr std::uint32_t kTable = 0x030009C4u;
    constexpr std::uint32_t kHoles[4] = {0x03000A5Cu, 0x03000A68u, 0x03000A74u, 0x03000A80u};
    Variant v;
    v.rom = rom;
    const std::uint32_t source = rom_offset(kTable) + static_cast<std::uint32_t>(variant) * 8u;
    for (int i = 0; i < 4; ++i)
        std::memcpy(&v.rom[rom_offset(kHoles[i])], &rom[source + static_cast<std::uint32_t>(i) * 8u], 8);
    const std::string n = std::to_string(variant);
    const std::string patched = sha1_hex(v.rom.data(), v.rom.size());
    const std::string image = sha1_hex(&v.rom[rom_offset(kLoopStart)], kLoopEnd - kLoopStart);
    v.dir = "synth_dc8b_" + n;
    v.prefix = "gsr_synth_dc8b_" + n + "_";
    v.toml =
        "# Private generated input; do not commit.\n"
        "# Writer 0x03000a20..0x03000a50 copies table 0x030009c4 + " + n + " * 8.\n"
        "[program]\n"
        "name = \"Golden Sun synthesized Func_dc8 slot 2 variant " + n + "\"\n"
        "id = \"golden_sun_usa_synth_dc8b_" + n + "\"\n"
        "load_address = 0x08000000\n"
        "size = 0x00800000\n"
        "entry_pc = 0x" + hex(kLoopStart, 8, false) + "\n"
        "speculative_literal_harvest = false\n"
        "codegen_shards = 1\n"
        "\n"
        "[identity]\n"
        "sha1 = \"" + patched + "\"\n"
        "\n"
        "[[code_copy]]\n"
        "runtime_start = 0x" + hex(kLoopStart, 8, false) + "\n"
        "source_start = 0x" + hex(rom_address(kLoopStart), 8, false) + "\n"
        "size = 0x" + hex(kLoopEnd - kLoopStart, 8, false) + "\n"
        "name = \"synthesized_Func_dc8_slot2_" + n + "\"\n"
        "note = \"Eight table words in four holes; complete executable SHA-1 " + image + "\"\n"
        "\n"
        "[[extra_func]]\n"
        "addr = 0x" + hex(kLoopStart, 8, false) + "\n"
        "mode = \"arm\"\n"
        "name = \"Func_dc8_slot2_synth_" + n + "\"\n"
        "note = \"Mixer loop head; backedge from 0x03000a94, falls through to 0x03000a98\"\n"
        "\n"
        "[[resume_range]]\n"
        "start = 0x" + hex(kLoopStart, 8, false) + "\n"
        "end = 0x" + hex(kLoopEnd, 8, false) + "\n"
        "mode = \"arm\"\n"
        "note = \"Whole loop; valid after the writer's pass until its next pass or an IWRAM reset\"\n";
    return v;
}

// tools/build_synthesized_dc8_slot3_variant.py
Variant synth_dc8c(const std::vector<std::uint8_t>& rom, int variant) {
    constexpr std::uint32_t kSlotStart = 0x03000BD8u, kSlotEnd = 0x03000C70u;
    constexpr std::uint32_t kTable = 0x08000404u;
    constexpr std::uint32_t kSize = kSlotEnd - kSlotStart;
    Variant v;
    v.rom = rom;
    std::memcpy(&v.rom[rom_offset(kSlotStart)],
                &rom[kTable - kRomBase + static_cast<std::uint32_t>(variant) * kSize], kSize);
    const std::string n = std::to_string(variant);
    const std::string image = sha1_hex(&v.rom[rom_offset(kSlotStart)], kSize);
    const std::string patched = sha1_hex(v.rom.data(), v.rom.size());
    v.dir = "synth_dc8c_" + n;
    v.prefix = "gsr_synth_dc8c_" + n + "_";
    v.toml =
        "# Private generated input; do not commit.\n"
        "# 0x080037d4 copies table 0x08000404 + " + n + " * 0x98 over 0x03000bd8.\n"
        "[program]\n"
        "name = \"Golden Sun synthesized Func_dc8 slot 3 variant " + n + "\"\n"
        "id = \"golden_sun_usa_synth_dc8c_" + n + "\"\n"
        "load_address = 0x08000000\n"
        "size = 0x00800000\n"
        "entry_pc = 0x" + hex(kSlotStart, 8, false) + "\n"
        "speculative_literal_harvest = false\n"
        "codegen_shards = 1\n"
        "\n"
        "[identity]\n"
        "sha1 = \"" + patched + "\"\n"
        "\n"
        "[[code_copy]]\n"
        "runtime_start = 0x" + hex(kSlotStart, 8, false) + "\n"
        "source_start = 0x" + hex(rom_address(kSlotStart), 8, false) + "\n"
        "size = 0x" + hex(kSize, 8, false) + "\n"
        "name = \"synthesized_Func_dc8_slot3_" + n + "\"\n"
        "note = \"ROM block " + n + " of table 0x08000404; complete executable SHA-1 " + image + "\"\n"
        "\n"
        "[[extra_func]]\n"
        "addr = 0x" + hex(kSlotStart, 8, false) + "\n"
        "mode = \"arm\"\n"
        "name = \"Func_dc8_slot3_synth_" + n + "\"\n"
        "note = \"Entered by fall-through from 0x03000bd4; leaves to 0x03000c70\"\n"
        "\n"
        "[[resume_range]]\n"
        "start = 0x" + hex(kSlotStart, 8, false) + "\n"
        "end = 0x" + hex(kSlotEnd, 8, false) + "\n"
        "mode = \"arm\"\n"
        "note = \"Whole block; valid until the next 0x080037d4 copy or an IWRAM reset\"\n";
    return v;
}

// ---------------------------------------------------------------- stamps

struct StampVariant {
    std::vector<std::uint8_t> bytes;
    std::vector<std::array<int, 4>> inputs;
    std::uint32_t offset = 0;
    std::string sha1;
};

// tools/build_stamp_variants.py + emulate_stamp_builder.py: every routine
// Func_ed408 (ROM 0x080ED408, Thumb) builds, run with its two measured
// stand-ins (the slot allocator 0x080048B0 returns the destination; DMA3
// copies at once).
bool build_stamps(const std::vector<std::uint8_t>& rom, std::vector<StampVariant>* out,
                  std::string* error) {
    constexpr std::uint32_t kBuilder = 0x080ED408u, kAllocator = 0x080048B0u;
    constexpr std::uint32_t kDest = 0x03006000u, kSp = 0x03007E00u;
    GuestMachine m(rom);
    m.stub_returning(kAllocator, kDest);
    m.set_instant_dma3(true);
    std::map<std::vector<std::uint8_t>, std::size_t> seen;
    for (int w : {7, 8}) {
        for (int h : {7, 8}) {
            for (int flags = 0; flags < 16; ++flags) {
                for (int mode = 0; mode < 4; ++mode) {
                    m.clear_memory();
                    m.poke32(kSp, static_cast<std::uint32_t>(mode));
                    const std::uint32_t args[4] = {0, static_cast<std::uint32_t>(w),
                                                   static_cast<std::uint32_t>(h),
                                                   static_cast<std::uint32_t>(flags)};
                    std::uint32_t r0 = 0;
                    if (!m.call(kBuilder | 1u, args, &r0, error, kSp, 2000000ull)) {
                        *error = "stamp builder: " + *error;
                        return false;
                    }
                    const auto& iw = m.iwram();
                    std::size_t end = 0x6800;
                    while (end > 0x6000 && iw[end - 1] == 0) --end;
                    std::vector<std::uint8_t> image(iw.begin() + 0x6000, iw.begin() + static_cast<std::ptrdiff_t>(end));
                    while (image.size() % 4) image.push_back(0);
                    auto it = seen.find(image);
                    if (it == seen.end()) {
                        seen[image] = out->size();
                        StampVariant v;
                        v.bytes = image;
                        v.inputs.push_back({w, h, flags, mode});
                        out->push_back(std::move(v));
                    } else {
                        (*out)[it->second].inputs.push_back({w, h, flags, mode});
                    }
                }
            }
        }
    }
    std::uint32_t offset = 0;
    for (StampVariant& v : *out) {
        v.offset = offset;
        v.sha1 = sha1_hex(v.bytes.data(), v.bytes.size());
        offset += static_cast<std::uint32_t>(v.bytes.size());
    }
    return true;
}

std::string stamps_toml(const std::vector<StampVariant>& vs, const std::string& image_sha1,
                        std::size_t size) {
    std::string s =
        "# Private generated input; do not commit. tools/build_stamp_variants.py\n"
        "# Every routine Func_ed408 can build, packed at a generation origin.\n"
        "[program]\n"
        "name = \"Golden Sun particle stamp routines\"\n"
        "id = \"golden_sun_usa_stamps\"\n"
        "load_address = 0x02000000\n"
        "size = 0x" + hex(static_cast<std::uint32_t>(size), 8, false) + "\n"
        "entry_pc = 0x02000000\n"
        "speculative_literal_harvest = false\n"
        "codegen_shards = 4\n"
        "\n"
        "[identity]\n"
        "sha1 = \"" + image_sha1 + "\"\n";
    for (std::size_t i = 0; i < vs.size(); ++i) {
        const auto& v = vs[i];
        const std::uint32_t start = 0x02000000u + v.offset;
        const std::uint32_t end = start + static_cast<std::uint32_t>(v.bytes.size());
        char idx[8];
        std::snprintf(idx, sizeof(idx), "%03zu", i);
        const auto& in = v.inputs[0];
        s += "\n[[extra_func]]\n"
             "addr = 0x" + hex(start, 8, false) + "\n"
             "mode = \"arm\"\n"
             "name = \"stamp_" + std::string(idx) + "\"\n"
             "note = \"Func_ed408 output for r1=" + std::to_string(in[0]) +
             " r2=" + std::to_string(in[1]) + " r3=" + std::to_string(in[2]) +
             " mode=" + std::to_string(in[3]) + " and " +
             std::to_string(v.inputs.size() - 1) + " other input(s)\"\n"
             "\n[[resume_range]]\n"
             "start = 0x" + hex(start, 8, false) + "\n"
             "end = 0x" + hex(end, 8, false) + "\n"
             "mode = \"arm\"\n"
             "note = \"whole routine; every instruction is an entry\"\n";
    }
    return s;
}

std::string stamps_registry(const std::vector<StampVariant>& vs) {
    std::string rows;
    for (std::size_t i = 0; i < vs.size(); ++i) {
        if (i) rows += "\n";
        rows += "    {0x" + hex(vs[i].offset, 5, true) + "u, 0x" +
                hex(static_cast<std::uint32_t>(vs[i].bytes.size()), 3, true) + "u, \"" +
                vs[i].sha1 + "\"},";
    }
    return "// AUTO-GENERATED by tools/build_stamp_variants.py. DO NOT EDIT.\n"
           "// Func_ed408 stamp routines packed at 0x02000000; offsets are from the\n"
           "// image origin, lengths in bytes, SHA-1 over the routine's bytes.\n"
           "#include <cstdint>\n"
           "\n"
           "struct GsrStampVariant {\n"
           "    std::uint32_t offset;\n"
           "    std::uint32_t length;\n"
           "    const char* sha1;\n"
           "};\n"
           "\n"
           "extern \"C\" const GsrStampVariant gsr_stamps_kVariants[] = {\n" +
           rows + "\n};\n"
           "extern \"C\" const unsigned gsr_stamps_kVariantCount = " +
           std::to_string(vs.size()) + "u;\n";
}

// ---------------------------------------------------------------- generate

bool generate(const Setup& s, const std::vector<std::uint8_t>& rom, std::string* error) {
    const auto plan = read_plan(s.data / "game_code_plan.txt");
    if (plan.empty()) {
        *error = "the build plan is missing: " + (s.data / "game_code_plan.txt").string();
        return false;
    }
    const fs::path gen = gen_dir(s), inputs = input_dir(s);
    fs::create_directories(gen);
    fs::create_directories(inputs);
    fs::create_directories(logs_dir(s));
    const std::string rom_path = s.rom.string();

    std::vector<std::function<bool(std::string*)>> tasks;
    for (const PlanStep& step : plan) {
        const auto& a = step.args;
        if (step.kind == "main" && a.size() == 3) {
            tasks.push_back([&s, a, rom_path, gen](std::string* e) {
                return recompile(s, {"--rom", rom_path, "--config", (s.data / a[1]).string(),
                                     "--out", (gen / a[0]).string(), "--max-functions", a[2]},
                                 a[0], e);
            });
        } else if (step.kind == "transient" && (a.size() == 3 || a.size() == 4)) {
            tasks.push_back([&s, a, rom_path, gen](std::string* e) {
                std::vector<std::string> args{"--rom", rom_path, "--config",
                                              (s.data / a[1]).string(), "--out",
                                              (gen / a[0]).string(), "--symbol-prefix", a[2],
                                              "--no-symbol-map"};
                if (a.size() == 4) {
                    args.push_back("--relocatable-image");
                    args.push_back(a[3]);
                }
                return recompile(s, args, a[0], e);
            });
        } else if (step.kind == "overlays") {
            for (const Overlay& o : read_registry(s.data / "overlay-registry.inc")) {
                tasks.push_back([&s, o, &rom, gen, inputs](std::string* e) {
                    std::vector<std::uint8_t> image;
                    if (!unpack_overlay(rom, o, &image, e)) return false;
                    const fs::path bin = inputs / ("overlay_" + o.id + ".bin");
                    if (!write_bytes(bin, image.data(), image.size())) {
                        *e = "cannot write " + bin.string();
                        return false;
                    }
                    return recompile(s, {"--rom", bin.string(), "--rom-base",
                                         "0x" + hex(o.start, 8, true), "--config",
                                         (s.data / "overlays" / (o.id + ".toml")).string(),
                                         "--out", (gen / ("overlay_" + o.id)).string(),
                                         "--symbol-prefix", "gsr_overlay_" + o.id + "_",
                                         "--no-symbol-map"},
                                     "overlay_" + o.id, e);
                });
            }
        } else if ((step.kind == "synth_dc8" || step.kind == "synth_dc8b" ||
                    step.kind == "synth_dc8c") && a.size() == 1) {
            tasks.push_back([&s, step, &rom, gen, inputs](std::string* e) {
                const std::string& arg = step.args[0];
                Variant v = step.kind == "synth_dc8"
                    ? synth_dc8(rom, static_cast<std::uint32_t>(std::stoul(arg, nullptr, 16)))
                    : step.kind == "synth_dc8b" ? synth_dc8b(rom, std::stoi(arg))
                                                : synth_dc8c(rom, std::stoi(arg));
                const fs::path vrom = inputs / (v.dir + ".gba");
                const fs::path vtoml = inputs / (v.dir + ".toml");
                if (!write_bytes(vrom, v.rom.data(), v.rom.size()) || !write_text(vtoml, v.toml)) {
                    *e = "cannot write " + vrom.string();
                    return false;
                }
                return recompile(s, {"--rom", vrom.string(), "--config", vtoml.string(), "--out",
                                     (gen / v.dir).string(), "--symbol-prefix", v.prefix,
                                     "--no-symbol-map"},
                                 v.dir, e);
            });
        } else if (step.kind == "stamps") {
            tasks.push_back([&s, &rom, gen, inputs](std::string* e) {
                std::vector<StampVariant> vs;
                if (!build_stamps(rom, &vs, e)) return false;
                std::vector<std::uint8_t> image;
                for (const auto& v : vs) image.insert(image.end(), v.bytes.begin(), v.bytes.end());
                const std::string image_sha1 = sha1_hex(image.data(), image.size());
                const fs::path bin = inputs / "stamps.bin", toml = inputs / "stamps.toml";
                if (!write_bytes(bin, image.data(), image.size()) ||
                    !write_text(toml, stamps_toml(vs, image_sha1, image.size()))) {
                    *e = "cannot write " + bin.string();
                    return false;
                }
                const fs::path out = gen / "stamps";
                if (!recompile(s, {"--rom", bin.string(), "--rom-base", "0x02000000", "--config",
                                   toml.string(), "--out", out.string(), "--symbol-prefix",
                                   "gsr_stamps_", "--relocatable-image",
                                   "0x02000000:0x" + hex(static_cast<std::uint32_t>(image.size()), 1, true),
                                   "--no-symbol-map"},
                               "stamps", e))
                    return false;
                if (!write_text(out / "stamp_registry.cpp", stamps_registry(vs))) {
                    *e = "cannot write the stamp registry";
                    return false;
                }
                return true;
            });
        } else {
            *error = "unknown build plan line: " + step.kind;
            return false;
        }
    }
    // The main program is by far the largest job: start it first.
    return run_parallel(tasks, s.jobs, error);
}

// ---------------------------------------------------------------- compile

bool has_prefix(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// The sources the game build compiles from each generated folder (the
// CMakeLists.txt globs): recompiled*.cpp and dispatch_table.cpp everywhere,
// plus symbol_map.cpp for the main program and stamp_registry.cpp for the
// stamps.
std::vector<fs::path> game_sources(const fs::path& gen) {
    std::vector<fs::path> out;
    for (const auto& dir : fs::directory_iterator(gen)) {
        if (!dir.is_directory()) continue;
        const std::string dname = dir.path().filename().string();
        for (const auto& f : fs::directory_iterator(dir.path())) {
            const std::string n = f.path().filename().string();
            const bool take =
                (has_prefix(n, "recompiled") && f.path().extension() == ".cpp") ||
                n == "dispatch_table.cpp" ||
                (dname == "main" && n == "symbol_map.cpp") ||
                (dname == "stamps" && n == "stamp_registry.cpp");
            if (take) out.push_back(f.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The tables the engine reads from the DLL, by name (CMakeLists.txt's
// GoldenSunGame.def): every `extern "C" const ... NAME[] =` in
// dispatch_table*.cpp and stamp_registry.cpp.
std::string make_def(const std::vector<fs::path>& sources) {
    std::string def = "LIBRARY GoldenSunGame.dll\nEXPORTS\n";
    const std::regex name_re(R"(([A-Za-z_][A-Za-z0-9_]*)(\[\])? =)");
    for (const fs::path& p : sources) {
        const std::string n = p.filename().string();
        if (!has_prefix(n, "dispatch_table") && n != "stamp_registry.cpp") continue;
        std::istringstream in(read_text(p));
        std::string line;
        while (std::getline(in, line)) {
            if (!has_prefix(line, "extern \"C\" const ")) continue;
            std::smatch m;
            if (std::regex_search(line, m, name_re)) def += "    " + m[1].str() + " DATA\n";
        }
    }
    return def;
}

// The block-timing rewrite, over every generated .cpp (block_timing.cpp):
// the same game code the developer build compiles.
bool apply_block_timing(const Setup& s, std::string* error) {
    emit("@stage Preparing the game code");
    gsr::BlockTimingStats st;
    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(gen_dir(s))) {
        if (!e.is_regular_file() || e.path().extension() != ".cpp") continue;
        std::string out;
        if (!gsr::block_timing_transform(read_text(e.path()), &out, st)) {
            *error = "block timing: " + e.path().string() +
                     " has no #include \"recompiled.h\"";
            return false;
        }
        if (!write_text(e.path(), out)) {
            *error = "block timing: cannot write " + e.path().string();
            return false;
        }
        ++files;
    }
    log_line("block timing: " + std::to_string(files) + " files, " +
             std::to_string(st.merged) + " instructions merged, " +
             std::to_string(st.kept) + " kept, " +
             std::to_string(st.mem_cycles_inline) + " memory timings inline, " +
             std::to_string(st.cond_inline) + " conditions inline");
    if (st.merged == 0) {
        *error = "block timing matched nothing in the generated code";
        return false;
    }
    return true;
}

bool compile_and_link(const Setup& s, std::string* error) {
    const fs::path gen = gen_dir(s), obj = s.work / "obj";
    fs::create_directories(obj);
    const auto sources = game_sources(gen);
    if (sources.empty()) {
        *error = "no game code was generated";
        return false;
    }
    const fs::path gxx = s.toolchain / "bin" / (std::string("g++") + kExeSuffix);
    std::vector<std::string> base = {
        "-O2", "-g", "-DNDEBUG", "-std=c++20", "-Og", "-g0",
        // A broken setup fails in seconds instead of printing errors for
        // an hour per file.
        "-fmax-errors=20",
        "-DGBARECOMP_OUTLINE_BUS=1",
#ifndef _WIN32
        // A shared library on Linux must be position-independent; calls
        // between game functions bind inside it (with -Bsymbolic below).
        "-fPIC", "-fno-semantic-interposition",
#endif
        "-I" + (gen / "main").string(),
        "-I" + (s.engine / "include").string()};
#ifndef _WIN32
    // The bundled compiler brings the C library headers and link stubs it
    // needs (a Steam Deck has none installed); a system compiler has its own.
    const fs::path sysroot = s.toolchain / "sysroot";
    if (fs::is_directory(sysroot)) base.push_back("--sysroot=" + sysroot.string());
#endif

    emit("@stage Compiling the game (%zu files)", sources.size());
    std::vector<fs::path> objects;
    std::vector<std::function<bool(std::string*)>> tasks;
    for (const fs::path& src : sources) {
        const std::string name = src.parent_path().filename().string() + "__" +
                                 src.stem().string();
        const fs::path o = obj / (name + ".o");
        objects.push_back(o);
        tasks.push_back([&s, base, gxx, src, o, name](std::string* e) {
            std::vector<std::string> args = base;
            args.insert(args.end(), {"-c", src.string(), "-o", o.string()});
            const fs::path log = logs_dir(s) / ("cc_" + name + ".log");
            std::error_code ec;
            fs::remove(log, ec);
            const int rc = run_process(gxx, args, log);
            if (rc != 0) {
                log_tail(log);
                *e = "compiling " + src.filename().string() + " failed (" +
                     std::to_string(rc) + "); see " + log.string();
                return false;
            }
            fs::remove(log, ec);  // keep only the logs of failures
            return true;
        });
    }
    if (!run_parallel(tasks, s.jobs, error)) return false;

    emit("@stage Linking %s", kGameLibrary);
    const fs::path def = s.work / "GoldenSunGame.def";
    const fs::path rsp = s.work / "objects.rsp";
    write_text(def, make_def(sources));
    std::string list;
    for (const fs::path& o : objects) {
        std::string p = o.generic_string();
        list += "\"" + p + "\"\n";
    }
    write_text(rsp, list);
    const fs::path dll_tmp = s.work / kGameLibrary;
    const fs::path log = logs_dir(s) / "link.log";
    std::error_code ec;
    fs::remove(log, ec);
#ifdef _WIN32
    const int rc = run_process(gxx, {"-shared", "-o", dll_tmp.string(), "@" + rsp.string(),
                                     def.string(),
                                     (s.engine / "libGoldenSunRecomp_api.a").string()},
                               log);
#else
    // ELF needs no export or import list: the library exports every table,
    // and what it calls in the engine is found in GoldenSunRecomp when the
    // game loads it (the linker exports each engine symbol the build's own
    // copy of this library called, and this one calls the same).
    std::vector<std::string> link = {"-shared", "-Wl,-Bsymbolic", "-o", dll_tmp.string(),
                                     "@" + rsp.string()};
    if (fs::is_directory(sysroot)) link.push_back("--sysroot=" + sysroot.string());
    const int rc = run_process(gxx, link, log);
#endif
    if (rc != 0) {
        log_tail(log);
        *error = "linking failed (" + std::to_string(rc) + "); see " + log.string();
        return false;
    }
    const fs::path dll = s.out_dir / kGameLibrary;
    fs::remove(dll, ec);
    fs::copy_file(dll_tmp, dll, ec);
    if (ec) {
        *error = "cannot place " + dll.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- main

fs::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

int usage() {
    std::fprintf(stderr,
        "gsr_builder --rom <Golden Sun ROM> [--out <dir>] [--work <dir>]\n"
        "            [--data <dir>] [--engine <dir>] [--toolchain <dir>]\n"
        "            [--recompiler <gba_recompile.exe>] [--jobs N]\n"
        "            [--keep-work] [--generate-only]\n");
    return 2;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> argv;
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
        argv.push_back(s);
    }
#else
int main(int argc, char** cargv) {
    std::vector<std::string> argv(cargv, cargv + argc);
#endif
    const fs::path here = exe_dir();
    Setup s;
    s.data = here / "data";
    s.engine = here / "engine";
#ifdef _WIN32
    // GCC finds its Windows headers at bin/../../mingw64/include, so the
    // bundled toolchain folder must be called mingw64.
    s.toolchain = here / "mingw64";
#else
    s.toolchain = here / "toolchain";
#endif
    s.recompiler = here / (std::string("gba_recompile") + kExeSuffix);
    s.work = here / "work";
    s.out_dir = here.parent_path();
    for (std::size_t i = 1; i < argv.size(); ++i) {
        auto next = [&]() -> fs::path {
            return i + 1 < argv.size() ? fs::u8path(argv[++i]) : fs::path();
        };
        const std::string& a = argv[i];
        if (a == "--rom") s.rom = next();
        else if (a == "--out") s.out_dir = next();
        else if (a == "--work") s.work = next();
        else if (a == "--data") s.data = next();
        else if (a == "--engine") s.engine = next();
        else if (a == "--toolchain") s.toolchain = next();
        else if (a == "--recompiler") s.recompiler = next();
        else if (a == "--jobs" && i + 1 < argv.size()) s.jobs = std::stoi(argv[++i]);
        else if (a == "--keep-work") s.keep_work = true;
        else if (a == "--generate-only") s.generate_only = true;
        else return usage();
    }
    if (s.rom.empty()) return usage();
    for (fs::path* p : {&s.rom, &s.data, &s.engine, &s.toolchain, &s.recompiler, &s.work,
                        &s.out_dir}) {
        *p = fs::absolute(*p).lexically_normal().make_preferred();
    }
    if (s.jobs <= 0) {
        // One job per core, but at least ~700 MB of RAM each.
#ifdef _WIN32
        MEMORYSTATUSEX mem{sizeof(mem)};
        GlobalMemoryStatusEx(&mem);
        const unsigned long long total_ram = mem.ullTotalPhys;
#else
        const unsigned long long total_ram =
            static_cast<unsigned long long>(sysconf(_SC_PHYS_PAGES)) *
            static_cast<unsigned long long>(sysconf(_SC_PAGE_SIZE));
#endif
        const int by_ram = static_cast<int>(total_ram / (700ull << 20));
        s.jobs = std::max(1, std::min<int>(static_cast<int>(std::thread::hardware_concurrency()), by_ram));
    }

#ifdef _WIN32
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }
#endif
    set_child_path(s.toolchain / "bin");

    std::error_code ec;
    fs::remove_all(s.work, ec);
    fs::create_directories(s.work);
    fs::create_directories(logs_dir(s));
    g_log.open(s.work / "build-log.txt", std::ios::trunc);

    auto fail = [&](const std::string& msg) {
        emit("@error %s", msg.c_str());
        return 1;
    };

    emit("@stage Checking your ROM");
    const auto rom = read_bytes(s.rom);
    if (rom.empty()) return fail("Could not read the ROM file.");
    const std::string rom_sha1 = sha1_hex(rom.data(), rom.size());
    if (rom_sha1 != kRomSha1)
        return fail("This is not the Golden Sun (USA, Europe) ROM this release supports.");
#ifdef _WIN32
    const fs::path engine_link_input = s.engine / "libGoldenSunRecomp_api.a";
#else
    const fs::path engine_link_input = s.engine / "include" / "runtime_arm.h";
#endif
    for (const fs::path& need : {s.recompiler,
                                 s.toolchain / "bin" / (std::string("g++") + kExeSuffix),
                                 engine_link_input,
                                 s.data / "game_code_plan.txt"}) {
        if (!fs::exists(need)) return fail("Missing part of the release: " + need.string());
    }

    std::string error;
    emit("@stage Translating the game from your ROM");
    if (!generate(s, rom, &error)) return fail(error);
    if (!s.generate_only) {
        if (!apply_block_timing(s, &error)) return fail(error);
        if (!compile_and_link(s, &error)) return fail(error);
        // The launcher rebuilds when any of these change: a different ROM,
        // a new builder, or a new engine (an update may change what the
        // game code calls).
        const auto engine = read_bytes(s.out_dir / kEngineExe);
        const std::string stamp = std::string("rom_sha1=") + rom_sha1 +
                                  "\nbuilder=" + kBuilderVersion +
                                  "\nengine_sha1=" +
                                  (engine.empty() ? std::string("none")
                                                  : sha1_hex(engine.data(), engine.size())) +
                                  "\n";
        write_text(s.out_dir / "GoldenSunGame.build.txt", stamp);
        if (!s.keep_work) {
            g_log.close();
            fs::remove_all(gen_dir(s), ec);
            fs::remove_all(input_dir(s), ec);
            fs::remove_all(s.work / "obj", ec);
        }
    }
    emit("@done");
    return 0;
}
