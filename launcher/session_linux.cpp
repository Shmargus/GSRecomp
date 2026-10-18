// Internal Linux game session, embedded in the collection launcher.
//
// The Windows session (session_windows.cpp) is Win32 and GDI+; this is its
// Linux counterpart for the player release, drawn with SDL2 and Dear ImGui
// (the toolkit the in-game F1 menu already uses), so it needs nothing the
// game does not. It does what the Windows player launcher does:
//
//   * Pick ROM: choose the game's ROM (USA, Europe), checked by SHA-1.
//   * First start (or after an update): run builder/gsr_builder, which
//     translates the ROM and compiles the game library (lib*.so) beside the
//     game, with its progress on screen.
//   * Start the game program with the player release's settings, its output
//     captured into logs/session_<time>.log.
//   * Afterwards, if F12 was pressed or the game crashed, pack a bug report
//     into logs/bug_reports and offer the report form. Nothing is sent.
//
// Layout beside this program: the game program, its library (built
// on first start), builder/ (gsr_builder, gba_recompile, data/, engine/,
// toolchain/).
#include <SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "launcher_png.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "crash_handler.h"
#include "launcher_common.h"
#include "launcher_build_progress.h"
#include "launcher_logo.h"
#include "collection_session.h"
#include "collection_preferences.h"
#include "launcher_online.h"
#include "collection_storage.h"
#include "collection_settings.h"
#include "session_environment.h"
#include "launcher_widescreen_diagnostics_policy.h"
#include "../engine/src/runtime/seamless.h"
#include "../engine/src/runtime/soft_reset.h"

namespace fs = std::filesystem;
using namespace gsr_launcher;

namespace {

bool g_collection_play = false;
bool g_collection_tests_override = false;
gsr_collection::GameSettings g_collection_settings;
bool g_collection_update_checked = false;
bool g_collection_activation_pending = false;
// Forward Seamless handovers to the collection window; Soft Reset stays here.
int g_session_exit_code = 0;

// Where players can still upload a report by hand when sending fails: the
// Google Form the project page links; only the developer sees the answers.
constexpr const char* kBugReportUrl =
    "https://docs.google.com/forms/d/e/"
    "1FAIpQLScTmnmH6_BXYsYVfIKtjDsN-gLx59mv4pVjYx4ITbH2YGktBw/viewform";

fs::path g_root;
fs::path g_collection_root;
gsr_collection::ManagedFiles g_managed;

// ---------------------------------------------------------------- files

void write_text(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// logs/launcher.log: what the launcher itself did, one timestamped line per
// step (ROM picked and checked, build started and how it ended, game started
// and how it exited), as the Windows launcher does. Started fresh past 256 KB.
fs::path g_launcher_log_path;

void launcher_log(std::string line) {
    if (g_launcher_log_path.empty()) return;
    for (char& c : line)
        if (c == '\r' || c == '\n') c = ' ';
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S  ", std::localtime(&now));
    std::ofstream out(g_launcher_log_path, std::ios::binary | std::ios::app);
    if (out) out << stamp << line << "\n";
}

void open_launcher_log() {
    std::error_code ec;
    fs::create_directories(g_root / "logs", ec);
    g_launcher_log_path = g_root / "logs" / "launcher.log";
    const auto size = fs::file_size(g_launcher_log_path, ec);
    if (!ec && size > 256 * 1024) fs::remove(g_launcher_log_path, ec);
    launcher_log(std::string("---- Launcher ") + gsr_online::kReleaseVersion + " started (built " +
                 __DATE__ + " " + __TIME__ + ") in " + g_root.string());
}


std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// ---------------------------------------------------------------- the logo

// ---------------------------------------------------------------- the ROM

fs::path rom_cache_path() { return g_root / "local" / "launcher-rom.txt"; }
fs::path auto_start_path() { return g_root / "local" / "launcher-autostart.txt"; }
constexpr Uint32 kAutoStartMaxWaitMs = 10000;  // for the update check

// The ROM picked last time, when it is still there and still the right ROM;
// empty otherwise. The launcher then plays it without asking again.
std::string remembered_rom() {
    const std::string cached = trim(read_text(rom_cache_path()));
    if (cached.empty()) return {};
    std::string error;
    if (validate_rom(cached, &error)) return cached;
    launcher_log("Remembered ROM " + cached + " can no longer be used: " + error);
    return {};
}

bool have_program(const char* name) {
    const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    return q + "'";
}

// The desktop's own file chooser: zenity (GNOME and most others) or kdialog
// (KDE, the Steam Deck's desktop). Returns "" when cancelled; sets
// *no_dialog when neither exists, so the launcher can ask for a typed path.
std::string pick_rom_with_dialog(const std::string& start_dir, bool* no_dialog) {
    *no_dialog = false;
    std::string cmd;
    if (have_program("zenity")) {
        cmd = "zenity --file-selection --title=" +
              shell_quote(session_game().rom_prompt) +
              " --file-filter=" + shell_quote("Golden Sun ROM (*.gba) | *.gba *.GBA") +
              " --file-filter=" + shell_quote("All files | *");
        if (!start_dir.empty()) cmd += " --filename=" + shell_quote(start_dir + "/");
    } else if (have_program("kdialog")) {
        cmd = "kdialog --title " + shell_quote(session_game().rom_prompt) +
              " --getopenfilename " +
              shell_quote(start_dir.empty() ? std::string(".") : start_dir) +
              " " + shell_quote("*.gba *.GBA|Golden Sun ROM (*.gba)\n*|All files");
    } else {
        *no_dialog = true;
        return {};
    }
    cmd += " 2>/dev/null";
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
    }
    return trim(out);
}

// ---------------------------------------------------------------- game code

// The builder runs in the background; the window shows its progress lines.
struct BuildState {
    std::mutex mutex;
    bool running = false;
    bool finished = false;
    bool succeeded = false;
    std::string stage;
    int done = 0, total = 0;
    std::string error;
    pid_t pid = 0;
} g_build;
std::thread g_build_thread;
std::string g_build_rom;

void show_message(const std::string& title, const std::string& message, bool error);

void start_game_build(const std::string& rom) {
    const fs::path builder = g_root / "builder" / "gsr_builder";
    std::error_code exists_ec;
    if (!fs::is_regular_file(builder, exists_ec)) {
        launcher_log("Build: builder/gsr_builder is missing.");
        show_message("Part of Golden Sun Recompiled is missing",
                     "builder/gsr_builder was not found.\n\nPlease unpack the whole "
                     "download again.", true);
        return;
    }
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
        launcher_log(std::string("Build: no pipe for the builder: ") + std::strerror(errno));
        show_message("The game builder could not start", std::strerror(errno), true);
        return;
    }
    const std::string builder_s = builder.string(), dir_s = builder.parent_path().string();
    std::string rom_arg = rom;
    std::vector<char*> argv = {const_cast<char*>(builder_s.c_str()),
                               const_cast<char*>("--rom"), rom_arg.data(), nullptr};
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid < 0) {
        const std::string why = std::strerror(errno);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        launcher_log("Build: the builder could not start: " + why);
        show_message("The game builder could not start", why, true);
        return;
    }
    if (pid == 0) {
        // Closing the launcher mid-build stops the builder (and it stops
        // its compilers the same way).
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(127);
        dup2(pipe_fd[1], 1);
        dup2(pipe_fd[1], 2);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        if (chdir(dir_s.c_str()) != 0) _exit(127);
        execv(builder_s.c_str(), argv.data());
        _exit(127);
    }
    close(pipe_fd[1]);
    launcher_log("Build: started (log: builder/work/build-log.txt).");
    {
        std::lock_guard<std::mutex> lock(g_build.mutex);
        g_build.running = true;
        g_build.finished = false;
        g_build.succeeded = false;
        g_build.stage = "Starting";
        g_build.done = g_build.total = 0;
        g_build.error.clear();
        g_build.pid = pid;
    }
    g_build_rom = rom;
    if (g_build_thread.joinable()) g_build_thread.join();
    g_build_thread = std::thread([read_fd = pipe_fd[0], pid] {
        std::string pending;
        char buf[4096];
        bool done = false;
        std::string error;
        ssize_t n;
        while ((n = read(read_fd, buf, sizeof(buf))) > 0 ||
               (n < 0 && errno == EINTR)) {
            if (n < 0) continue;
            pending.append(buf, static_cast<std::size_t>(n));
            std::size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                const std::string line = trim(pending.substr(0, nl));
                pending.erase(0, nl + 1);
                std::lock_guard<std::mutex> lock(g_build.mutex);
                if (line.rfind("@stage ", 0) == 0) {
                    g_build.stage = line.substr(7);
                    g_build.done = g_build.total = 0;
                } else if (line.rfind("@progress ", 0) == 0) {
                    std::sscanf(line.c_str() + 10, "%d %d", &g_build.done,
                                &g_build.total);
                } else if (line == "@done") {
                    done = true;
                } else if (line.rfind("@error ", 0) == 0) {
                    error = line.substr(7);
                }
            }
        }
        close(read_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        const bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::lock_guard<std::mutex> lock(g_build.mutex);
        g_build.running = false;
        g_build.finished = true;
        g_build.succeeded = done && exited_ok;
        g_build.error = error.empty() && !g_build.succeeded
                            ? std::string("The build stopped unexpectedly.")
                            : error;
        g_build.pid = 0;
    });
}

// ---------------------------------------------------------------- the game

std::string make_session_log_name() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char name[64];
    std::strftime(name, sizeof(name), "session_%Y%m%d_%H%M%S.log", &local);
    return name;
}

// Tags each line of one of the game's output streams and appends it to the
// session log, flushed line by line so a hang or kill keeps everything.
struct SessionLog {
    std::mutex mutex;
    int fd = -1;
    void line(const char* tag, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fd < 0) return;
        const std::string out = std::string(tag) + text + "\n";
        if (write(fd, out.data(), out.size()) < 0) {}
        fdatasync(fd);
    }
};

void pump(int read_fd, const char* tag, SessionLog* log) {
    std::string pending;
    char buf[4096];
    ssize_t n;
    while ((n = read(read_fd, buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR)) {
        if (n < 0) continue;
        pending.append(buf, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            log->line(tag, trim(pending.substr(0, nl)));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) log->line(tag, pending);
    close(read_fd);
}

struct GameResult {
    bool started = false;
    bool crashed = false;
    // The unpacker guard caught its crash (unpacker_recovered.txt,
    // src/unpacker_guard.h).
    bool unpacker = false;
    int exit_code = 0;
    std::string rom;
    std::string log_path;
    std::string storage_error;
    std::vector<std::string> new_rewinds;
};

// Runs the game with the player release's settings and waits for it.
GameResult run_game(const std::string& rom) {
    GameResult result;
    result.rom = rom;
    const fs::path logs_dir = g_root / "logs";
    std::error_code ec;
    fs::create_directories(logs_dir, ec);
    prune_old_logs(logs_dir);
    result.log_path = (logs_dir / make_session_log_name()).string();
    const std::set<std::string> rewinds_before = rewind_dirs(logs_dir);
    // A crash file left by an earlier run must not end up in this run's report.
    fs::remove(g_root / "crash_memory.bin", ec);
    fs::remove(g_root / "crash_trail.csv", ec);
    // The unpacker catcher's pair (src/unpacker_catch.h): only this run's.
    fs::remove(g_root / "unpacker_catch.txt", ec);
    fs::remove(g_root / "unpacker_catch.bin", ec);
    // The unpacker guard's pair, the same way.
    fs::remove(g_root / "unpacker_recovered.txt", ec);
    fs::remove(g_root / "unpacker_recovered.bin", ec);
    fs::remove(g_root / "unpacker_journal.csv", ec);  // Unpacker audit
    const auto launch_time = fs::file_time_type::clock::now();

    // Variables both platforms set the same way (session_environment.h). This
    // is the player release's configuration: Enhanced Options and the
    // graphics card on; the console compositor's fallback picture made
    // widescreen slow on the Deck, so it is graphics card only.
    gsr::ChildEnvironmentPlan plan;
    gsr::CommonEnvironment common;
    common.controls_dir = (g_collection_root / "Controls").string();
    fs::create_directories(g_collection_root / "Controls");
    common.state_base = g_collection_play ? g_managed.state_base.string() : std::string();
    common.heal_cache = (g_root / "recomp_cache").string();
    // Release: point the game's self-heal at the bundled g++ (absent on dev).
    if (fs::exists(g_root / "builder" / "toolchain" / "bin" / "g++"))
        common.heal_toolchain = (g_root / "builder" / "toolchain").string();
    if (session_game_index == 1)  // The Lost Age imports Golden Sun saves.
        common.save_import = gsr_session::load_launcher_audio_settings(g_root).gs_save_import;
    const auto test_enabled = [](const char* name) {
        return gsr_collection::test_enabled(g_collection_settings, gsr_collection::test_index(name), session_game_index);
    };
    const bool gpu_field_check = g_collection_tests_override && test_enabled("TLA_GPU_FIELD_CHECK");
    common.gpu_field_only = !gpu_field_check;
    gsr::add_common_environment(plan, common);
    // Mutable-RAM native healing is enabled for both player and dev launchers;
    // a launcher test setting can still switch it off below.
    plan.put("GBARECOMP_SELFHEAL_RAM", "1");
#ifdef GSR_LINUX_DEV_LAUNCHER
    // The developer launcher's extras (make_release_linux.sh --dev):
    // per-frame timing CSVs beside the session log. Rewind stays
    // on, as for players, so F12 on the Deck saves the last 2 seconds.
    plan.put("GBARECOMP_FRAME_EVENTS",
             fs::path(result.log_path).replace_extension(".events.csv").string());
    plan.put("GBARECOMP_FRAME_PHASE",
             fs::path(result.log_path).replace_extension(".phase.csv").string());
#endif
    gsr::remove_variables(plan, gsr::bios_log_variables);
    std::string enabled_tests;
    std::string input_record_path;
    if (g_collection_tests_override) {
        gsr::add_test_environment(
            plan, [&](const char* name) -> std::optional<bool> { return test_enabled(name); },
            fs::path(result.log_path).replace_extension(".hostprof.txt").string());
        enabled_tests = plan.enabled_tests;
        if (gpu_field_check) enabled_tests += ",GSR_GPU_FIELD,GSR_GPU_FIELD_ONLY";
        const char* record = std::getenv("GBARECOMP_INPUT_RECORD");
        const char* replay = std::getenv("GBARECOMP_INPUT_REPLAY");
        const std::wstring explicit_record = record ? fs::path(record).wstring() : L"";
        const bool replay_requested = replay && *replay;
        // Only an explicit GBARECOMP_INPUT_RECORD records input.
        const auto policy = gsr::resolve_launcher_input_record_policy(
            false, explicit_record, replay_requested, L"");
        if (policy.replay_conflict) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, session_game().title,
                "Input recording and input replay cannot be used together.", nullptr);
            return result;
        }
        if (policy.enabled) {
            const fs::path path(policy.path);
            if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
            const int fd = ec ? -1 : open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd < 0) {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, session_game().title,
                    "The input recording file could not be created without overwriting an existing file.", nullptr);
                return result;
            }
            close(fd);
            input_record_path = path.string();
            plan.put("GBARECOMP_INPUT_RECORD", input_record_path);
        }
    }
    for (const std::string& name : plan.unset) unsetenv(name.c_str());
    for (const auto& [name, value] : plan.set) setenv(name.c_str(), value.c_str(), 1);

    SessionLog log;
    log.fd = open(result.log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_collection_tests_override) {
        log.line("[launcher] ", std::string("test_variables=") +
            (g_collection_settings.test_variables ? "ON" : "OFF") + " set=" +
            (enabled_tests.empty() ? "(none)" : enabled_tests) + " gpu_field_only=" +
            (gsr_collection::test_enabled(g_collection_settings, "TLA_GPU_FIELD_CHECK", session_game_index) ? "0" : "1"));
        log.line("[launcher] ", input_record_path.empty() ? "input_record=DISABLED" :
            "input_record=ENABLED path=\"" + input_record_path + "\"");
    } else {
#ifdef GSR_LINUX_DEV_LAUNCHER
    log.line("[launcher] ", "dev launcher: test_variables=ON set=GBARECOMP_SELFHEAL_RAM,GSR_FRAME_REWIND,GSR_UNPACKER_CATCH gpu_field_only=1");
#else
    log.line("[launcher] ", "test_variables=OFF set=GBARECOMP_SELFHEAL_RAM,GSR_FRAME_REWIND,GSR_UNPACKER_CATCH gpu_field_only=1");
#endif

    }

    const std::string game = gsr_session::engine(g_root, session_game_index).string();
    const std::string root_s = g_root.string();
    std::string rom_arg = rom;
    std::string save_arg = g_managed.save.string();
    std::vector<char*> argv = {const_cast<char*>(game.c_str()),
                               const_cast<char*>("--rom"), rom_arg.data(), nullptr};
    if (g_collection_play) {
        argv.pop_back();
        argv.push_back(const_cast<char*>("--save"));
        argv.push_back(save_arg.data());
        argv.push_back(nullptr);
    }
    int out_fd[2], err_fd[2];
    if (pipe(out_fd) != 0 || pipe(err_fd) != 0) {
        launcher_log(std::string("Start game: no pipe: ") + std::strerror(errno));
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        launcher_log(std::string("Start game: could not start: ") + std::strerror(errno));
        return result;
    }
    if (pid == 0) {
        dup2(out_fd[1], 1);
        dup2(err_fd[1], 2);
        close(out_fd[0]);
        close(out_fd[1]);
        close(err_fd[0]);
        close(err_fd[1]);
        if (chdir(root_s.c_str()) != 0) _exit(127);
        execv(game.c_str(), argv.data());
        _exit(127);
    }
    result.started = true;
    launcher_log("Start game: running " + game + ", session log " +
                 fs::path(result.log_path).filename().string() + ".");
    close(out_fd[1]);
    close(err_fd[1]);
    std::thread out_thread(pump, out_fd[0], "[OUT] ", &log);
    std::thread err_thread(pump, err_fd[0], "[ERR] ", &log);
    out_thread.join();
    err_thread.join();
    int status = 0;
    waitpid(pid, &status, 0);
    if (g_collection_play &&
        !gsr_collection::sync_legacy_states(g_managed, result.storage_error))
        launcher_log("Could not copy savestates: " + result.storage_error);
    if (log.fd >= 0) close(log.fd);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

    for (const std::string& dir : rewind_dirs(logs_dir))
        if (!rewinds_before.count(dir)) result.new_rewinds.push_back(dir);
    const fs::path crash_report = g_root / "crash_report.txt";
    result.crashed = fs::is_regular_file(crash_report, ec) &&
                     fs::last_write_time(crash_report, ec) >= launch_time;
    result.unpacker = fs::is_regular_file(g_root / "unpacker_recovered.txt", ec);
    launcher_log("Game ended: exit code " + std::to_string(result.exit_code) +
                 (result.exit_code == 127 ? " (GoldenSunRecomp could not be run)" : "") +
                 (result.crashed ? ", crashed (crash_report.txt written)" : "") +
                 (result.unpacker ? ", unpacker crash caught (unpacker_recovered.txt)" : "") +
                 ", rewind captures " + std::to_string(result.new_rewinds.size()) + ".");
    return result;
}

// ---------------------------------------------------------------- bug reports

std::string os_release_name() {
    std::istringstream in(read_text("/etc/os-release"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string v = line.substr(12);
            v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
            return v;
        }
    }
    return "?";
}

}  // namespace
namespace gsr_launcher {

// The text in a report with the player's home folder taken out of paths
// (/home/name/... becomes /home/<user>/...), so a report never shows who
// sent it.
std::string scrub_user_name(std::string text) {
    std::vector<std::pair<std::string, std::string>> swaps;
    if (const char* home = std::getenv("HOME"); home && std::strlen(home) > 1)
        swaps.emplace_back(home, "/home/<user>");
    if (const char* user = std::getenv("USER"); user && *user)
        swaps.emplace_back(std::string("/home/") + user, "/home/<user>");
    return scrub_paths(std::move(text), swaps);
}

}  // namespace gsr_launcher
namespace {

// Like the Windows session's make_bug_report: the session log, the settings
// files and the crash files in every archive, plus at most ONE F12 capture
// per archive, so each upload stays small. Returns the archives, or the
// folder when packing failed.
std::vector<fs::path> make_bug_report(const GameResult& game) {
    const fs::path logs_dir = g_root / "logs";
    const fs::path reports = logs_dir / "bug_reports";
    std::error_code ec;
    fs::create_directories(reports, ec);
    std::string name = fs::path(game.log_path).stem().string();
    if (name.rfind("session_", 0) == 0) name = name.substr(8);
    if (name.empty()) name = "latest";
    name = "bug_report_" + name;
    const fs::path staging = reports / name;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    const std::string stem = fs::path(game.log_path).stem().string();
    for (const auto& entry : fs::directory_iterator(logs_dir, ec))
        if (entry.path().filename().string().rfind(stem, 0) == 0)
            copy_into_report(entry.path(), staging);
    copy_into_report(logs_dir / "launcher.log", staging);
    // The game's save (the managed one in collection play, else beside the ROM:
    // runtime.cpp uses the ROM path with .sav), so the problem can be replayed.
    if (!game.rom.empty())
        copy_into_report(g_collection_play ? g_managed.save :
            fs::path(game.rom).replace_extension(".sav"), staging);
    copy_report_files(g_root, g_root, g_root, game.crashed, staging);
    write_text(staging / "report_info.txt",
               report_info_text("System: Linux, " + os_release_name(),
                                game.new_rewinds.size(), game.crashed,
                                game.unpacker, game.exit_code));

    std::vector<fs::path> archives;
    const std::size_t parts = std::max<std::size_t>(1, game.new_rewinds.size());
    for (std::size_t i = 0; i < parts; ++i) {
        const std::string suffix =
            parts > 1 ? "_part" + std::to_string(i + 1) + "of" + std::to_string(parts) : "";
        const fs::path archive = reports / (name + suffix + ".tar.gz");
        fs::remove(archive, ec);
        std::string cmd = "tar -czf " + shell_quote(archive.string()) + " -C " +
                          shell_quote(staging.string()) + " .";
        if (!game.new_rewinds.empty())
            cmd += " -C " + shell_quote(logs_dir.string()) + " " +
                   shell_quote(game.new_rewinds[i]);
        cmd += " >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0 || !fs::is_regular_file(archive, ec)) {
            for (const fs::path& made : archives) fs::remove(made, ec);
            fs::remove(archive, ec);
            return {staging};
        }
        archives.push_back(archive);
    }
    fs::remove_all(staging, ec);
    for (const std::string& dir : game.new_rewinds) fs::remove_all(logs_dir / dir, ec);
    return archives;
}

// Runs a program found on PATH and returns what it printed (stdout) and its
// exit code; -1 when it could not start, 127 when it is not installed.
int run_capture(const std::vector<std::string>& args, std::string* out) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) return -1;
    std::vector<std::string> owned = args;
    std::vector<char*> argv;
    for (std::string& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pipe_fd[1], 1);
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, 2);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipe_fd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipe_fd[0], buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR))
        if (n > 0 && out->size() < 4 * 1024 * 1024) out->append(buf, static_cast<std::size_t>(n));
    close(pipe_fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Update check (launcher_online.h): GitHub's newest release, through curl.
// Found on its own thread; the main loop shows the notice.
std::mutex g_update_mutex;
gsr_online::Release g_update;
std::atomic<bool> g_update_found{false};
// The newest release's notes, shown over the logo whether or not it is newer.
std::string g_release_notes;  // title line + plain text; under g_update_mutex
std::atomic<bool> g_release_notes_ready{false};
// Set however the update check ends; the automatic start waits for it.
std::atomic<bool> g_update_check_done{false};

void check_for_update() {
    if ((g_collection_play && g_collection_update_checked) ||
        !gsr_online::update_checks_enabled()) {
        g_update_check_done = true;
        return;
    }
    std::thread([] {
        // An update found is flagged before this is set.
        struct MarkDone {
            ~MarkDone() { g_update_check_done = true; }
        } mark_done;
        std::string reply;
        const int rc = run_capture({"curl", "-sS", "-f", "-L", "-m", "15", "-H",
                                    std::string("User-Agent: ") + gsr_online::kUserAgent,
                                    "-H", "Accept: application/vnd.github+json",
                                    std::string("https://") + gsr_online::kReleasesApiHost +
                                        gsr_online::kReleasesApiPath},
                                   &reply);
        if (rc != 0) {
            launcher_log(rc == 127 ? "Update check: curl is not installed."
                                   : "Update check: no answer from GitHub (curl " +
                                         std::to_string(rc) + ").");
            return;
        }
        gsr_online::Release release;
        std::string error;
        if (!gsr_online::parse_newest_release(reply, true, &release, &error)) {
            launcher_log("Update check: " + error);
            return;
        }
        {
            const std::string notes = gsr_online::notes_to_plain_text(release.notes);
            if (!notes.empty()) {
                {
                    std::lock_guard<std::mutex> lock(g_update_mutex);
                    g_release_notes = "What's new in " + release.tag + "\n\n" + notes;
                }
                g_release_notes_ready = true;
            }
        }
        if (!gsr_online::is_newer_release(release)) {
            launcher_log("Update check: " + release.tag + " is the newest.");
            return;
        }
        launcher_log("Update check: " + release.tag + " is available.");
        {
            std::lock_guard<std::mutex> lock(g_update_mutex);
            g_update = release;
        }
        g_update_found = true;
    }).detach();
}

// The Send report button: uploads every report archive with the player's
// description and, when they gave one, their Reddit username to the report
// service, through curl. Returns an empty string when all were sent,
// otherwise why not.
std::string send_reports(const std::vector<fs::path>& reports, const std::string& description,
                         const std::string& reddit) {
    for (std::size_t i = 0; i < reports.size(); ++i) {
        std::string part = description;
        if (reports.size() > 1)
            part += " [part " + std::to_string(i + 1) + " of " + std::to_string(reports.size()) + "]";
        std::vector<std::string> command = {
            "curl", "-sS", "-m", "300", "-X", "POST",
            "-H", "X-GSR-Client: GoldenSunLauncher",
            "-H", "X-GSR-Platform: linux",
            "-H", "X-GSR-Game: " + gsr_online::percent_encode(session_game().stem),
            "-H", std::string("X-GSR-Launcher: ") + gsr_online::kReleaseVersion + " " + __DATE__,
            "-H", "X-GSR-File: " + gsr_online::percent_encode(reports[i].filename().string(), 120),
            "-H", "X-GSR-Description: " + gsr_online::percent_encode(part),
            "-H", "Content-Type: application/octet-stream",
            "--data-binary", "@" + reports[i].string(),
            "-w", "\n%{http_code}",
            std::string("https://") + gsr_online::kReportHost + gsr_online::kReportPath};
        if (!reddit.empty()) command.insert(command.begin() + 6, {"-H", "X-GSR-Reddit: " + reddit});
        std::string answer;
        const int rc = run_capture(command, &answer);
        if (rc == 127) return "curl is not installed on this system.";
        if (rc != 0) return "No connection to the report service (curl " + std::to_string(rc) + ").";
        const std::size_t nl = answer.rfind('\n');
        const std::string code = nl == std::string::npos ? answer : answer.substr(nl + 1);
        if (code != "200") {
            std::string body = nl == std::string::npos ? std::string() : trim(answer.substr(0, nl));
            return body.empty() ? "The server said no (" + code + ")." : body;
        }
    }
    return {};
}

void open_with_desktop(const std::string& target) {
    const std::string cmd = "xdg-open " + shell_quote(target) + " >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {}
}

// ---------------------------------------------------------------- window

enum class Dialog { None, Message, TypePath, BugReport, Update, AskAutoStart,
                  AskStartOrNotes };

struct Ui {
    Dialog dialog = Dialog::None;
    std::string title, message;
    bool error = false;
    bool after_message_quit = false;
    char typed_path[1024] = {};
    std::vector<fs::path> reports;
    bool reports_packed = false;
    char description[512] = {};
    char reddit[25] = {};        // optional: the player's Reddit username
    std::thread sender;          // the Send report upload
    std::atomic<int> send_state{0};  // 0 not sent, 1 sending, 2 sent, 3 failed
    std::mutex send_mutex;
    std::string send_error;
    std::string pending_rom;   // picked; run or build when the frame ends
    std::string remembered_rom;  // non-empty: the button says Play and uses it
    // Start automatically: with the box ticked, a built game and a good
    // remembered ROM, the game starts as soon as the update check has
    // answered (at most kAutoStartMaxWaitMs). When an update is offered,
    // "Not now" starts the game and opening the download page does not. A
    // needed build never starts by itself. A message or the box unticked
    // stop it. The player is asked once, after the first build. Kept in
    // local/launcher-autostart.txt.
    bool auto_start = false;
    Uint32 auto_start_began = 0;  // SDL ticks; 0 = not waiting
    bool auto_start_paused = false;  // waiting on the update question
    bool quit = false;
} g_ui;

void show_message(const std::string& title, const std::string& message, bool error) {
    g_ui.dialog = Dialog::Message;
    g_ui.title = title;
    g_ui.message = message;
    g_ui.error = error;
    if (g_collection_play) g_ui.after_message_quit = true;
}

void load_fonts(ImGuiIO& io) {
    // A system font when there is one (DejaVu and Noto ship with almost
    // every desktop, the Steam Deck included); ImGui's own otherwise.
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf",
                             "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                             "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/google-noto/NotoSans-Regular.ttf"}) {
        std::error_code ec;
        if (fs::is_regular_file(path, ec) &&
            io.Fonts->AddFontFromFileTTF(path, 18.0f))
            return;
    }
    ImFontConfig config;
    config.SizePixels = 18.0f;
    io.Fonts->AddFontDefault(&config);
}

void style_ui() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 6.0f;
    s.FrameRounding = 6.0f;
    s.FramePadding = ImVec2(14, 8);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Button] = ImVec4(0.42f, 0.25f, 0.62f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.52f, 0.33f, 0.74f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.34f, 0.19f, 0.52f, 1.0f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.55f, 0.35f, 0.80f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.98f);
}

// Pick ROM: check it, then build the game code first if needed, else play.
// Once a ROM was accepted the button says Play and uses it without asking.
void pick_rom() {
    if (!g_ui.remembered_rom.empty()) {
        std::string error;
        if (validate_rom(g_ui.remembered_rom, &error)) {
            launcher_log("Play: using the remembered ROM " + g_ui.remembered_rom + ".");
            g_ui.pending_rom = g_ui.remembered_rom;
            return;
        }
        launcher_log("Remembered ROM " + g_ui.remembered_rom +
                     " can no longer be used: " + error);
        g_ui.remembered_rom.clear();
    }
    const std::string cached = trim(read_text(rom_cache_path()));
    const std::string start_dir = cached.empty() ? std::string()
                                                 : fs::path(cached).parent_path().string();
    bool no_dialog = false;
    const std::string picked = pick_rom_with_dialog(start_dir, &no_dialog);
    if (no_dialog) {
        std::snprintf(g_ui.typed_path, sizeof(g_ui.typed_path), "%s", cached.c_str());
        g_ui.dialog = Dialog::TypePath;
        return;
    }
    if (!picked.empty()) g_ui.pending_rom = picked;
    else {
        launcher_log("Pick ROM: no file chosen.");
        if (g_collection_play) g_ui.quit = true;
    }
}

void draw_build_progress(int done, int total, float width, double seconds) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 end(origin.x + width, origin.y + progress_height);
    const auto color = [](ProgressColor value) {
        return IM_COL32(value.r, value.g, value.b, value.a);
    };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, end, color(progress_deep_background), progress_corner_radius);
    const float energy_width = total > 0 ? width * progress_fraction(done, total) : width;
    if (energy_width > 0) {
        draw->PushClipRect(ImVec2(origin.x + 2, origin.y + 2),
                          ImVec2(origin.x + std::min(width - 2, energy_width), end.y - 2), true);
        draw->AddRectFilled(ImVec2(origin.x + 2, origin.y + 2),
                            ImVec2(origin.x + energy_width, end.y - 2),
                            color(progress_dark_purple), progress_corner_radius - 2);
        for (int strand = 0; strand < 3; ++strand) {
            std::vector<ImVec2> points;
            for (float x = 2; x <= energy_width; x += 4)
                points.emplace_back(origin.x + x, origin.y +
                    energy_y(x, width, progress_height, seconds, strand));
            if (points.size() > 1) {
                draw->AddPolyline(points.data(), static_cast<int>(points.size()),
                    IM_COL32(172, 118, 220, 25), 0, 8.0f);
                draw->AddPolyline(points.data(), static_cast<int>(points.size()),
                    color(progress_light_purple), 0, 1.5f);
            }
        }
        const float shimmer = static_cast<float>(std::fmod(seconds * 0.08, 1.0)) * width;
        draw->AddRectFilledMultiColor(ImVec2(origin.x + shimmer - 22, origin.y + 6),
            ImVec2(origin.x + shimmer + 22, end.y - 6),
            IM_COL32(210, 167, 244, 0), IM_COL32(210, 167, 244, 18),
            IM_COL32(210, 167, 244, 18), IM_COL32(210, 167, 244, 0));
        draw->PopClipRect();
    }
    draw->AddRect(origin, end, color(progress_gold), progress_corner_radius, 0, 1.5f);
    ImGui::Dummy(ImVec2(width, progress_height));
}

bool session_ready() {
#ifdef GSR_LINUX_DEV_LAUNCHER
    const auto game = gsr_session::engine(g_root, session_game_index);
    if (fs::is_regular_file(game) && fs::is_regular_file(game.parent_path() / session_game().library))
        return true;
#endif
    return game_code_ready(g_root);
}

void use_rom(const std::string& selected_rom) {
    std::string rom = selected_rom;
    std::string error;
    if (!validate_rom(rom, &error)) {
        launcher_log("Pick ROM: " + rom + " - refused: " + error);
        show_message("Not the right ROM", error, true);
        return;
    }
    launcher_log("Pick ROM: " + rom + " - accepted.");
    const PreparationState preparation = session_ready() ? PreparationState::Ready : preparation_state(g_root);
#ifdef GSR_LINUX_DEV_LAUNCHER
    if (preparation != PreparationState::Ready && !fs::is_directory(g_root / "builder")) {
        show_message("The game needs a build", "Run the collection build before playing this game.", true);
        g_ui.quit = true;
        return;
    }
#endif
    if (preparation != PreparationState::Ready) {
        g_ui.pending_rom.clear();
        g_ui.auto_start_began = 0;
        g_ui.auto_start_paused = false;
        const std::string message = preparation_prompt_text(preparation);
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes"},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"}
        };
        const SDL_MessageBoxData question = {SDL_MESSAGEBOX_INFORMATION, nullptr,
            session_game().title, message.c_str(), 2, buttons, nullptr};
        int answer = 0;
        if (SDL_ShowMessageBox(&question, &answer) != 0 || answer != 1) {
            launcher_log("Preparation: declined; no build started.");
            if (g_collection_play) g_ui.quit = true;
            return;
        }
        launcher_log("Preparation: accepted.");
    }
    if (g_collection_play) {
        if (!gsr_collection::import_selected_rom(g_collection_root, rom,
                                                session_game().stem, g_managed, error)) {
            show_message("The ROM or saves could not be copied", error, true);
            return;
        }
        rom = g_managed.rom.string();
        if (!validate_rom(rom, &error)) {
            show_message("The copied ROM can't be used", error, true);
            return;
        }
    }
    write_text(rom_cache_path(), rom + "\n");
    g_ui.remembered_rom = rom;
    g_ui.pending_rom.clear();
    if (preparation != PreparationState::Ready) {
        launcher_log("The game needs preparation: building it.");
        start_game_build(rom);
        return;
    }
    launcher_log("Starting the game.");
    g_ui.pending_rom = "@play:" + rom;
}

}  // namespace

int gsr_session_main(int game, int argc, char** argv) {
    session_game_index = game;
    std::string settings_error;
    int seamless_slot = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--direct3d") == 0)
            settings_error = "Direct3D is not supported on Linux.";
        else if (std::strcmp(argv[i], "--collection-tests") == 0) {
            if (g_collection_tests_override || i + 1 >= argc ||
                !gsr_collection::parse_tests(argv[++i], g_collection_settings))
                settings_error = "Invalid collection test settings.";
            g_collection_tests_override = true;
        }
        else if (std::strcmp(argv[i], "--collection-play") == 0)
            g_collection_play = true;
        else if (std::strcmp(argv[i], "--collection-update-checked") == 0)
            g_collection_update_checked = true;
        else if (std::strcmp(argv[i], "--collection-root") == 0 && i + 1 < argc)
            g_collection_root = fs::path(argv[++i]);
        else if (std::strcmp(argv[i], "--seamless-slot") == 0 && i + 1 < argc) {
            // Slot 0..2 of The Broken Seal's save, from the collection window.
            seamless_slot = gbarecomp::seamless::parse_slot(argv[++i]);
        }
    }
    if (g_collection_tests_override && !g_collection_play)
        settings_error = "Collection test settings require collection play.";
#ifndef GSR_LINUX_DEV_LAUNCHER
    if (g_collection_tests_override)
        settings_error = "Collection test settings require the developer launcher.";
#endif
    if (!settings_error.empty()) {
        std::fprintf(stderr, "%s\n", settings_error.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, session_game().title, settings_error.c_str(), nullptr);
        return 1;
    }
    if (!g_collection_play || !g_collection_root.is_absolute()) {
        std::fprintf(stderr, "Start this game from the collection launcher.\n");
        return 1;
    }
    g_root = gsr_session::game_root(g_collection_root, session_game_index);
    // "Skip the ROM check" (any_rom.h): this session, the builder and the game
    // all read it from here.
    setenv(gbarecomp::kAnyRomEnv,
           gsr_session::load_launcher_audio_settings(g_root).any_rom ? "1" : "0", 1);
    fs::current_path(g_root);
    const std::string crash_root = g_root.string();
    gbarecomp::crash_handler_install(crash_root.c_str());
    open_launcher_log();
    // The Lost Age's game inherits The Broken Seal's save (seamless.h).
    const fs::path gs_save = session_game_index == 1
        ? gsr_session::broken_seal_save(g_collection_root) : fs::path{};
    const bool have_gs_save = !gs_save.empty();
    if (have_gs_save) setenv(gbarecomp::seamless::kSaveEnv, gs_save.c_str(), 1);
    if (seamless_slot >= 0 && have_gs_save) {
        // Seamless mode: The Broken Seal just saved its Clear Data. The game
        // inherits the slot too; the collection window keeps this session's
        // window hidden, and the remembered ROM starts the game without
        // asking (the normal path below).
        launcher_log("Seamless: starting The Lost Age with The Broken Seal's save, slot " +
                     std::to_string(seamless_slot + 1) + ".");
        setenv(gbarecomp::seamless::kSlotEnv, std::to_string(seamless_slot + 1).c_str(), 1);
    }
    g_ui.remembered_rom = remembered_rom();
    g_ui.auto_start = trim(read_text(auto_start_path())) == "1";
    check_for_update();

    // The game controller too: in Steam Deck Game Mode the pad is the only
    // input the launcher gets.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL could not start: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_Window* window = SDL_CreateWindow(session_game().title, SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, 720, 640,
                                          SDL_WINDOW_ALLOW_HIGHDPI |
                                              (g_collection_play ? SDL_WINDOW_HIDDEN : 0));
    if (!window) {
        std::fprintf(stderr, "The launcher window could not be created: %s\n",
                     SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    load_fonts(io);
    style_ui();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    SDL_Texture* logo = nullptr;
    int logo_w = 0, logo_h = 0;
    {
        std::vector<unsigned char> rgba;
        if (decode_png(kLauncherLogoPng, sizeof(kLauncherLogoPng), &rgba, &logo_w, &logo_h)) {
            logo = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                                     SDL_TEXTUREACCESS_STATIC, logo_w, logo_h);
            if (logo) {
                SDL_UpdateTexture(logo, nullptr, rgba.data(), logo_w * 4);
                SDL_SetTextureBlendMode(logo, SDL_BLENDMODE_BLEND);
            }
        }
    }

    bool ready = session_ready();
    bool running = true;
    std::string release_notes;  // empty until the update check has them
    g_collection_activation_pending = g_collection_play;
    if (!g_collection_play && g_ui.auto_start && ready && !g_ui.remembered_rom.empty()) {
        launcher_log("Start automatically: waiting for the update check.");
        g_ui.auto_start_began = std::max<Uint32>(1, SDL_GetTicks());
    }
    while (running && !g_ui.quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }
        if (!running) break;

        if (g_collection_activation_pending && g_update_check_done &&
            !g_update_found && g_ui.dialog == Dialog::None) {
            g_collection_activation_pending = false;
            pick_rom();
            if (g_ui.quit) continue;
        }

        // A build that just finished: play, or say why not.
        bool build_running = false, build_finished = false, build_ok = false;
        std::string build_error, stage;
        int done = 0, total = 0;
        {
            std::lock_guard<std::mutex> lock(g_build.mutex);
            build_running = g_build.running;
            build_finished = g_build.finished;
            build_ok = g_build.succeeded;
            build_error = g_build.error;
            stage = g_build.stage;
            done = g_build.done;
            total = g_build.total;
            g_build.finished = false;
        }
        if (build_finished) {
            if (g_build_thread.joinable()) g_build_thread.join();
            if (build_ok) write_text(g_root / session_game().release_stamp, release_fingerprint(g_root));
            ready = build_ok && session_ready();
            launcher_log(ready      ? std::string("Build: finished, the game is ready.")
                         : build_ok ? std::string("Build: finished, but the game files do "
                                                  "not match this release.")
                                    : "Build: failed: " + build_error);
            if (ready) {
                if (g_collection_play) {
                    g_ui.pending_rom = "@play:" + g_build_rom;
                } else {
                    // The standalone launcher asks about future starts.
                    std::error_code exists_ec;
                    if (!fs::exists(auto_start_path(), exists_ec))
                        g_ui.dialog = Dialog::AskAutoStart;
                    else
                        g_ui.dialog = Dialog::AskStartOrNotes;
                }
            } else {
                show_message("The game could not be prepared",
                             build_error + "\n\nThe full log is builder/work/build-log.txt.",
                             true);
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        const ImVec2 size = io.DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("##launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (logo) {
            const float w = std::min(size.x - 60.0f, 620.0f);
            const float h = w * logo_h / std::max(1, logo_w);
            ImGui::SetCursorPos(ImVec2((size.x - w) * 0.5f, 40.0f));
            ImGui::Image(reinterpret_cast<ImTextureID>(logo), ImVec2(w, h));
        }

        const float bottom = size.y - 40.0f;
        if (g_release_notes_ready.exchange(false)) {
            std::lock_guard<std::mutex> lock(g_update_mutex);
            release_notes = g_release_notes;
        }
        // Over the logo, from y 40 down to just above the status text.
        if (!release_notes.empty()) {
            ImGui::SetCursorPos(ImVec2(40.0f, 40.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.02f, 0.08f, 0.88f));
            if (ImGui::BeginChild("##notes", ImVec2(size.x - 80.0f, bottom - 170.0f - 5.0f - 40.0f),
                                  true)) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(release_notes.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        std::string text, note;
        if (build_running) {
            const bool step_two = stage.find("Compiling") != std::string::npos ||
                                  stage.find("Linking") != std::string::npos;
            text = std::string(step_two ? "Step 2 of 2: " : "Step 1 of 2: ") + stage;
            if (total > 0) text += "  (" + std::to_string(done) + " of " + std::to_string(total) + ")";
            note = "Step 1 translates the game from your ROM, step 2 compiles it. "
                   "This can take several minutes, and the game starts by itself when it's done.";
        } else if (!ready) {
            text = "First start: pick your Golden Sun ROM. The game is prepared from it "
                   "once, which takes a few minutes.";
            note = "Step 1 translates the game from your ROM, step 2 compiles it. "
                   "After an update this happens again.";
        }
        ImGui::SetCursorPos(ImVec2(40.0f, bottom - 170.0f));
        ImGui::PushTextWrapPos(size.x - 40.0f);
        if (!text.empty()) ImGui::TextUnformatted(text.c_str());
        if (build_running) {
            ImGui::Dummy(ImVec2(0, 4));
            draw_build_progress(done, total, size.x - 80.0f, ImGui::GetTime());
        }
        if (!note.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.75f, 1.0f));
            ImGui::TextUnformatted(note.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();

        // Start automatically once the update check answered. An update
        // found is flagged before the check is marked done, so it is seen
        // first and pauses this for the question.
        if (g_ui.auto_start_began != 0) {
            if (g_update_found || g_ui.dialog == Dialog::Update) {
                g_ui.auto_start_began = 0;
                g_ui.auto_start_paused = true;
            } else if (g_ui.dialog != Dialog::None || build_running) {
                launcher_log("Start automatically: stopped (a message).");
                g_ui.auto_start_began = 0;
            } else if (g_update_check_done ||
                       SDL_GetTicks() - g_ui.auto_start_began >= kAutoStartMaxWaitMs) {
                launcher_log("Start automatically: starting the game.");
                g_ui.auto_start_began = 0;
                pick_rom();
            }
        }

        const ImVec2 button(200.0f, 48.0f);
        if (!g_collection_play && ready && !build_running) {
            const char* label = "Start the game automatically";
            const float box_w = ImGui::GetFrameHeight() +
                                ImGui::GetStyle().ItemInnerSpacing.x +
                                ImGui::CalcTextSize(label).x;
            ImGui::SetCursorPos(ImVec2((size.x - box_w) * 0.5f, bottom - button.y - 44.0f));
            if (ImGui::Checkbox(label, &g_ui.auto_start)) {
                write_text(auto_start_path(), g_ui.auto_start ? "1\n" : "0\n");
                launcher_log(g_ui.auto_start ? "Start automatically: on."
                                             : "Start automatically: off.");
                if (!g_ui.auto_start && g_ui.auto_start_began != 0) {
                    launcher_log("Start automatically: stopped (box unticked).");
                    g_ui.auto_start_began = 0;
                }
            }
        }
        ImGui::SetCursorPos(ImVec2(size.x * 0.5f - button.x - 10.0f, bottom - button.y));
        if (!g_collection_play) {
        ImGui::BeginDisabled(build_running || g_ui.dialog != Dialog::None);
        if (ImGui::Button(g_ui.remembered_rom.empty() ? "Pick ROM###rom" : "Play###rom",
                          button)) {
            g_ui.auto_start_began = 0;
            pick_rom();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 20.0f);
        }
        if (ImGui::Button(g_collection_play ? "Cancel" : "Quit", button)) running = false;
        ImGui::End();

        // Dialogs. The update notice waits until nothing else is open.
        if (g_update_found && g_ui.dialog == Dialog::None && !build_running) {
            g_update_found = false;
            g_ui.dialog = Dialog::Update;
        }
        if (g_collection_play && (build_running || g_ui.dialog != Dialog::None))
            SDL_ShowWindow(window);
        if (g_ui.dialog != Dialog::None && !ImGui::IsPopupOpen("##dialog"))
            ImGui::OpenPopup("##dialog");
        ImGui::SetNextWindowPos(ImVec2(size.x * 0.5f, size.y * 0.5f), ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(size.x - 60.0f, 600.0f), 0));
        if (ImGui::BeginPopupModal("##dialog", nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(0.0f);
            if (g_ui.dialog == Dialog::Message) {
                ImGui::TextUnformatted(g_ui.title.c_str());
                ImGui::Separator();
                ImGui::TextUnformatted(g_ui.message.c_str());
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                    g_ui.dialog = Dialog::None;
                    if (g_ui.after_message_quit) g_ui.quit = true;
                    ImGui::CloseCurrentPopup();
                }
            } else if (g_ui.dialog == Dialog::TypePath) {
                ImGui::TextUnformatted(
                    "No file chooser was found on this system (zenity or kdialog). "
                    "Type the full path of your Golden Sun ROM:");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##rom", g_ui.typed_path, sizeof(g_ui.typed_path));
                if (ImGui::Button("Use this ROM", ImVec2(160, 0))) {
                    g_ui.pending_rom = trim(g_ui.typed_path);
                    g_ui.dialog = Dialog::None;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    g_ui.dialog = Dialog::None;
                    if (g_collection_play) g_ui.quit = true;
                    ImGui::CloseCurrentPopup();
                }
            } else if (g_ui.dialog == Dialog::BugReport) {
                const int state = g_ui.send_state.load();
                ImGui::TextUnformatted(g_ui.title.c_str());
                ImGui::Separator();
                const bool can_send = g_ui.reports_packed && gsr_online::report_upload_enabled();
                if (can_send) {
                    ImGui::TextUnformatted("What happened, and where in the game? A few words "
                                           "help a lot:");
                    ImGui::BeginDisabled(state == 1 || state == 2);
                    ImGui::InputTextMultiline("##description", g_ui.description,
                                              sizeof(g_ui.description), ImVec2(-1, 110));
                    ImGui::TextWrapped("Your Reddit username, if you'd like the developer to "
                                       "reach out to you (optional; leave it blank if not):");
                    ImGui::SetNextItemWidth(240);
                    ImGui::InputTextWithHint("##reddit", "optional", g_ui.reddit,
                                             sizeof(g_ui.reddit));
                    ImGui::EndDisabled();
                }
                ImGui::TextUnformatted(g_ui.message.c_str());
                if (state == 1) ImGui::TextUnformatted("Sending...");
                if (state == 2) ImGui::TextUnformatted("Sent. Thank you!");
                if (state == 3) {
                    std::lock_guard<std::mutex> lock(g_ui.send_mutex);
                    ImGui::TextUnformatted(("Not sent: " + g_ui.send_error).c_str());
                }
                if (can_send && state != 2) {
                    ImGui::BeginDisabled(state == 1);
                    if (ImGui::Button(state == 3 ? "Try again" : "Send report", ImVec2(180, 0))) {
                        if (g_ui.sender.joinable()) g_ui.sender.join();
                        g_ui.send_state = 1;
                        launcher_log("Bug report: sending " + std::to_string(g_ui.reports.size()) +
                                     " file(s).");
                        g_ui.sender = std::thread([reports = g_ui.reports,
                                                   text = std::string(g_ui.description),
                                                   reddit = gsr_online::reddit_name(g_ui.reddit)] {
                            const std::string error = send_reports(reports, text, reddit);
                            {
                                std::lock_guard<std::mutex> lock(g_ui.send_mutex);
                                g_ui.send_error = error;
                            }
                            launcher_log(error.empty() ? std::string("Bug report: sent.")
                                                       : "Bug report: sending failed: " + error);
                            g_ui.send_state = error.empty() ? 2 : 3;
                        });
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                }
                ImGui::BeginDisabled(state == 1);
                if (ImGui::Button(state == 2 ? "Close" : "Don't send", ImVec2(140, 0))) {
                    g_ui.dialog = Dialog::None;
                    ImGui::CloseCurrentPopup();
                    running = false;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Open folder", ImVec2(140, 0)))
                    open_with_desktop(g_ui.reports.front().parent_path().string());
                if (state == 3 || !can_send) {
                    if (ImGui::Button("Open the report form instead", ImVec2(300, 0))) {
                        open_with_desktop(kBugReportUrl);
                        open_with_desktop(g_ui.reports.front().parent_path().string());
                    }
                }
            } else if (g_ui.dialog == Dialog::Update) {
                gsr_online::Release release;
                {
                    std::lock_guard<std::mutex> lock(g_update_mutex);
                    release = g_update;
                }
                ImGui::TextUnformatted(("A new version is out: " + release.tag).c_str());
                ImGui::Separator();
                const std::string text =
                    (release.title.empty() ? std::string() : release.title + "\n\n") +
                    "You have " + gsr_online::kReleaseVersion +
                    ". Unzip the new version over this folder to keep your settings and "
                    "saves. The game is then prepared from your ROM once more, which takes a "
                    "few minutes.";
                ImGui::TextUnformatted(text.c_str());
                if (ImGui::Button("Open the download page", ImVec2(240, 0))) {
                    launcher_log("Update: opened the page of " + release.tag + ".");
                    open_with_desktop(release.page.empty() ? gsr_online::kReleasesPage
                                                           : release.page);
                    if (g_ui.auto_start_paused)
                        launcher_log("Start automatically: stopped (going to update).");
                    g_ui.auto_start_paused = false;
                    g_ui.dialog = Dialog::None;
                    if (g_collection_play) g_ui.quit = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Not now", ImVec2(140, 0))) {
                    launcher_log("Update: not now.");
                    if (g_ui.auto_start_paused) {
                        launcher_log("Start automatically: starting the game.");
                        pick_rom();
                    }
                    g_ui.auto_start_paused = false;
                    g_ui.dialog = Dialog::None;
                    ImGui::CloseCurrentPopup();
                }
            } else if (g_ui.dialog == Dialog::AskAutoStart) {
                ImGui::TextUnformatted("The game is ready");
                ImGui::Separator();
                ImGui::TextUnformatted(
                    "Start it automatically from now on when you open the launcher? The "
                    "launcher still checks for updates first, and the \"Start the game "
                    "automatically\" box above the Play button changes this later.");
                const bool yes = ImGui::Button("Yes", ImVec2(120, 0));
                ImGui::SameLine();
                const bool no = ImGui::Button("No", ImVec2(120, 0));
                if (yes || no) {
                    g_ui.auto_start = yes;
                    write_text(auto_start_path(), yes ? "1\n" : "0\n");
                    launcher_log(yes ? "Start automatically: on (asked after the build)."
                                     : "Start automatically: off (asked after the build).");
                    g_ui.dialog = Dialog::AskStartOrNotes;
                    ImGui::CloseCurrentPopup();
                }
            } else if (g_ui.dialog == Dialog::AskStartOrNotes) {
                ImGui::TextUnformatted("The game is ready");
                ImGui::Separator();
                ImGui::TextUnformatted(
                    "The game is ready.\n\nStart it now, or read what's new in this version "
                    "first? The Play button starts it whenever you are ready.");
                const bool start = ImGui::Button("Start now", ImVec2(120, 0));
                ImGui::SameLine();
                const bool notes = ImGui::Button("What's new", ImVec2(120, 0));
                if (start || notes) {
                    if (start) {
                        launcher_log("Build: finished; the player chose to start now.");
                        g_ui.pending_rom = "@play:" + g_build_rom;
                    } else {
                        launcher_log("Build: finished; the player chose to read what's new first.");
                    }
                    g_ui.dialog = Dialog::None;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
        }

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0x1b, 0x14, 0x26, 0xff);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        // Work picked this frame, done outside ImGui's frame.
        if (!g_ui.quit && running && !g_ui.pending_rom.empty() &&
            g_ui.dialog == Dialog::None && !build_running) {
            const std::string pending = g_ui.pending_rom;
            if (pending.rfind("@play:", 0) == 0) {
                g_ui.pending_rom.clear();
                // Golden Sun's flash save is 64 KiB; the engine refuses a larger
                // .sav (runtime.cpp "save file too large"), so say why.
                constexpr std::uintmax_t kGameSaveBytes = 0x10000;
                const fs::path save = g_collection_play ? g_managed.save :
                    fs::path(pending.substr(6)).replace_extension(".sav");
                std::error_code save_ec;
                const std::uintmax_t save_size = fs::file_size(save, save_ec);
                if (!save_ec && save_size > kGameSaveBytes) {
                    launcher_log("Start game: save file " + save.string() + " is " +
                                 std::to_string(save_size) +
                                 " bytes, larger than 65536: not started.");
                    show_message("This save file can't be used",
                                 "The save file was made by an emulator "
                                 "or another program, and Golden Sun Recompiled can't "
                                 "read it.\n\n" + save.string() +
                                 "\n\nMove it to another folder or delete it, then start "
                                 "the game again. The game will make a new save there.",
                                 true);
                    continue;
                }
                SDL_HideWindow(window);
                const GameResult game = run_game(pending.substr(6));
                if (game.started && game.storage_error.empty() &&
                    game.exit_code == gbarecomp::soft_reset::kExitCode) {
                    // Restart this session's game directly. Returning through
                    // the collection selector ended the Linux play session.
                    // After a handover this is TLA, without another import.
                    unsetenv(gbarecomp::seamless::kSlotEnv);
                    g_ui.pending_rom = pending;
                    launcher_log("Soft Reset: restarting the current game at its title screen.");
                    continue;
                }
                // Seamless mode: The Broken Seal wrote its Clear Data and asks
                // for The Lost Age. A hand-over, not a failure: exit with the
                // same code (no bug report offer); the collection window goes on.
                const int seamless_done = gbarecomp::seamless::slot_of_exit_code(game.exit_code);
                if (session_game_index == 0 && game.started && game.storage_error.empty() &&
                    seamless_done >= 0) {
                    launcher_log("Seamless: The Broken Seal saved Clear Data to slot " +
                                 std::to_string(seamless_done + 1) + "; starting The Lost Age.");
                    g_session_exit_code = game.exit_code;
                    running = false;
                    continue;
                }
                if (!g_collection_play) SDL_ShowWindow(window);
                if (!game.storage_error.empty())
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                        "Savestates could not be copied", game.storage_error.c_str(), window);
                if (!game.started) {
                    show_message("The game could not start",
                                 "GoldenSunRecomp could not be started.", true);
                } else if (!game.new_rewinds.empty() || game.crashed || game.unpacker) {
                    g_ui.reports = make_bug_report(game);
                    if (g_ui.reports.empty()) {
                        running = false;
                        continue;
                    }
                    const bool packed = g_ui.reports.front().extension() == ".gz";
                    g_ui.reports_packed = packed;
                    g_ui.send_state = 0;
                    g_ui.description[0] = '\0';
                    g_ui.reddit[0] = '\0';
                    g_ui.title =
                        game.unpacker && game.crashed
                            ? "The game's data unpacker hit a crash and could not recover.\n"
                              "A bug report was saved; sending it helps a lot in finding\n"
                              "the proper fix."
                        : game.unpacker
                            ? "The game's data unpacker hit a crash. It was caught and the\n"
                              "game kept running, and a bug report was saved. Sending it\n"
                              "helps a lot in finding the proper fix."
                        : game.crashed
                            ? "The game closed unexpectedly. A bug report was saved."
                            : "Your bug report was saved.";
                    std::error_code size_ec;
                    std::uintmax_t bytes = 0;
                    for (const fs::path& report : g_ui.reports)
                        bytes += fs::file_size(report, size_ec);
                    if (packed && !gsr_online::report_upload_enabled()) {
                        g_ui.message = "The report is in " +
                                       g_ui.reports.front().parent_path().string() +
                                       ". Open the report form, write a few words about what "
                                       "happened and add the file. Nothing is sent "
                                       "automatically.";
                    } else if (packed) {
                        g_ui.message =
                            "Send report uploads " +
                            (g_ui.reports.size() > 1
                                 ? std::to_string(g_ui.reports.size()) + " files"
                                 : std::string("the file")) +
                            " (" + std::to_string(std::max<std::uintmax_t>(1, bytes >> 20)) +
                            " MB) to the developer: the game's log and screen captures, your "
                            "save file, your settings and your Linux version, plus the Reddit "
                            "username above only if you typed one. Your user name is taken out. "
                            "Nothing is sent unless you press Send report.";
                    } else {
                        g_ui.message = "It could not be packed: " +
                                       g_ui.reports.front().string() +
                                       "\nPlease pack this folder (and any gpu_rewind folders "
                                       "next to it) and add it to the report form.";
                    }
                    g_ui.dialog = Dialog::BugReport;
                } else {
                    running = false;
                }
            } else {
                use_rom(pending);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_build.mutex);
        if (g_build.pid > 0) kill(g_build.pid, SIGTERM);
    }
    if (g_build_thread.joinable()) g_build_thread.join();
    if (g_ui.sender.joinable()) g_ui.sender.join();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (logo) SDL_DestroyTexture(logo);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    gbarecomp::crash_handler_mark_clean_exit();
    return g_session_exit_code;
}
