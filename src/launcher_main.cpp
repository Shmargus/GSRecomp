// Golden Sun Recompiled click-to-play launcher.
//
// This launcher never copies or embeds the user's ROM. It selects the ROM,
// verifies the exact USA/Europe image, and starts the already-built runner.

#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <winhttp.h>

#include "crash_handler.h"
#include "launcher_audio_policy.h"
#include "launcher_replay_policy.h"
#include "launcher_session_id.h"
#include "launcher_test_policy.h"
#include "launcher_widescreen_diagnostics_policy.h"
#include "launcher_logo.h"  // generated from assets/launcher_logo.png
#include "launcher_online.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// GSR_RELEASE_LAUNCHER (CMake option of the same name, used by
// make_release.bat): a player build with only Pick ROM and Quit. No
// diagnostics, no settings file, and the game is looked for next to the
// launcher. The play settings are fixed to the ones the game ships with.
#ifdef GSR_RELEASE_LAUNCHER
constexpr bool kReleaseLauncher = true;
#else
constexpr bool kReleaseLauncher = false;
#endif

namespace {

constexpr char kExpectedRomSha1[] =
    "5c4695205413df7db52b9a184815a07783999971";

// ── Release: first-run game build ─────────────────────────────────────
// A release holds no game code. The first time the player picks their ROM
// the launcher runs builder\gsr_builder.exe, which translates the ROM and
// compiles GoldenSunGame.dll beside the game (tools/gsr_builder). Its
// progress lines (@stage / @progress / @done / @error) drive the text and
// bar drawn over the splash. GoldenSunGame.build.txt records what the DLL
// was built from; a different ROM, builder or engine means building again.
constexpr UINT kBuildUpdateMessage = WM_APP + 1;
constexpr UINT kBuildFinishedMessage = WM_APP + 2;
// Updates (launcher_online.h): the start-up check found a newer release.
constexpr UINT kUpdateFoundMessage = WM_APP + 3;
// ...and read the newest release's notes (shown whether or not it is newer).
constexpr UINT kReleaseNotesMessage = WM_APP + 4;
constexpr char kBuilderVersion[] = "2";  // tools/gsr_builder kBuilderVersion

struct BuildStatus {
    std::mutex mutex;
    bool running = false;
    bool ready = false;          // GoldenSunGame.dll matches this release
    std::wstring stage;
    int done = 0;
    int total = 0;
    std::wstring error;
    bool succeeded = false;
};
BuildStatus g_build;
std::thread g_build_thread;
HANDLE g_build_job = nullptr;
std::wstring g_build_rom;

// Diagnostic variables are deliberately opt-in. The checkbox is reset to
// this value on every launcher start, and the selected values are applied to
// the child environment only.
const auto k_launcher_test_defaults = gsr::launcher_test_defaults();
bool g_test_variables = k_launcher_test_defaults.master;
// Child selections are remembered while the launcher is open. They are
// intentionally not persisted. Self-healing remains active for normal
// launches; the other children start unchecked and require the master
// checkbox.
bool g_test_selfheal_ram = k_launcher_test_defaults.self_heal_ram;
bool g_test_cost_probe = k_launcher_test_defaults.cost_probe;
bool g_test_host_prof = k_launcher_test_defaults.host_prof;
bool g_test_present_cadence = k_launcher_test_defaults.present_cadence;
bool g_test_ram_churn_probe = k_launcher_test_defaults.ram_churn_probe;
bool g_test_map_record = k_launcher_test_defaults.map_record;
bool g_test_obj_record = k_launcher_test_defaults.obj_record;
bool g_test_function_tracer = k_launcher_test_defaults.function_tracer;
bool g_test_text_record = k_launcher_test_defaults.text_record;
bool g_test_headroom_probe = k_launcher_test_defaults.headroom_probe;
bool g_test_vram_trace = k_launcher_test_defaults.vram_map_trace;
bool g_test_effect_trace = k_launcher_test_defaults.effect_trace;
bool g_test_frame_rewind = k_launcher_test_defaults.frame_rewind;
bool g_test_battle_bg1_record = k_launcher_test_defaults.battle_bg1_record;
// Saves the rewind ring by itself on a blink, a sprite the handheld shows
// but we draw nowhere, or a refused frame (GSR_AUTO_CAPTURE; the runner
// keeps the ring on for it). Off each session, like the other probes.
bool g_test_auto_capture = false;
bool g_test_mod_field_test = k_launcher_test_defaults.mod_field_test;
bool g_test_room_buffer = k_launcher_test_defaults.room_buffer;
bool g_test_swi_log = k_launcher_test_defaults.swi_log;

struct LauncherAudioSettings {
    bool native_mp2k = false;
    bool turbo_decoupled = false;
    bool copy_session_id_to_clipboard = false;
    bool enhanced_options = false;
    bool gpu_field = false;
    // Draw ONLY with the graphics card: no fallback to the console
    // compositor, which is also then not run at all. Meaningless with
    // gpu_field off, and forced off below in that case.
    bool gpu_field_only = false;
};

// Root-launcher settings live outside config/local.json: that file contains
// user machine paths and must not be rewritten by the launcher.
LauncherAudioSettings g_audio_settings{};
bool g_strict_static_route = false;

// What a release launch plays with: Enhanced Options (expanded view
// rendering and spell effects) and the graphics-card field renderer ALONE.
// The console compositor fallback is off: on the Steam Deck it drew a
// full 360x240 picture per frame that was almost never shown and made
// widescreen slow (2026-10-04: one refused frame in ~590,000 across the
// developer sessions). Everything experimental or diagnostic stays off.
LauncherAudioSettings release_launch_settings() {
    LauncherAudioSettings settings{};
    settings.enhanced_options = true;
    settings.gpu_field = true;
    settings.gpu_field_only = true;
    return settings;
}

bool parse_bool_setting(const std::string& value) {
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "on";
}

std::string trim_setting(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

LauncherAudioSettings load_launcher_audio_settings(const fs::path& root) {
    LauncherAudioSettings settings;
    std::ifstream file(root / L"local" / L"launcher-settings.ini");
    if (!file) return settings;

    bool in_audio = false;
    bool in_launcher = false;
    std::string line;
    while (std::getline(file, line)) {
        line = trim_setting(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            in_audio = line == "[Audio]";
            in_launcher = line == "[Launcher]";
            continue;
        }
        if (!in_audio && !in_launcher) continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim_setting(line.substr(0, equals));
        const bool value = parse_bool_setting(
            trim_setting(line.substr(equals + 1)));
        if (in_audio && key == "NativeMp2kAudio") settings.native_mp2k = value;
        else if (in_audio && key == "TurboAudioDecoupled") {
            settings.turbo_decoupled = value;
        } else if (in_launcher && key == "CopySessionIdToClipboard") {
            settings.copy_session_id_to_clipboard = value;
        } else if (in_launcher && key == "EnhancedOptions") {
            settings.enhanced_options = value;
        } else if (in_launcher && key == "GpuFieldRenderer") {
            settings.gpu_field = value;
        } else if (in_launcher && key == "GpuFieldOnly") {
            settings.gpu_field_only = value;
        }
    }
    if (!settings.native_mp2k) settings.turbo_decoupled = false;
    if (!settings.gpu_field) settings.gpu_field_only = false;
    return settings;
}

void save_launcher_audio_settings(const fs::path& root,
                                  const LauncherAudioSettings& settings) {
    const fs::path path = root / L"local" / L"launcher-settings.ini";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return;

    std::vector<std::string> lines;
    if (std::ifstream input(path); input) {
        std::string line;
        while (std::getline(input, line)) lines.push_back(line);
    }

    std::vector<std::string> kept;
    bool in_managed_section = false;
    for (const std::string& line : lines) {
        const std::string trimmed = trim_setting(line);
        if (!trimmed.empty() && trimmed.front() == '[' &&
            trimmed.back() == ']') {
            in_managed_section = trimmed == "[Audio]" ||
                                 trimmed == "[Launcher]";
            if (in_managed_section) continue;
        }
        if (!in_managed_section) kept.push_back(line);
    }
    while (!kept.empty() && trim_setting(kept.back()).empty()) kept.pop_back();

    std::ofstream output(path, std::ios::trunc);
    if (!output) return;
    for (const std::string& line : kept) output << line << '\n';
    if (!kept.empty()) output << '\n';
    output << "[Audio]\n"
           << "NativeMp2kAudio=" << (settings.native_mp2k ? "true" : "false")
           << "\n"
           << "TurboAudioDecoupled="
           << (settings.native_mp2k && settings.turbo_decoupled ? "true" : "false")
           << "\n"
           << "[Launcher]\n"
           << "CopySessionIdToClipboard="
           << (settings.copy_session_id_to_clipboard ? "true" : "false")
           << "\n"
           << "EnhancedOptions="
           << (settings.enhanced_options ? "true" : "false")
           << "\n"
           << "GpuFieldRenderer="
           << (settings.gpu_field ? "true" : "false")
           << "\n"
           << "GpuFieldOnly="
           << (settings.gpu_field_only ? "true" : "false")
           << "\n";
}

bool inherited_environment_truthy(const wchar_t* name) {
    wchar_t value[8] = {};
    const DWORD length = GetEnvironmentVariableW(name, value,
                                                   static_cast<DWORD>(std::size(value)));
    return length != 0 && !(length == 1 && value[0] == L'0');
}

std::wstring inherited_environment_value(const wchar_t* name) {
    std::vector<wchar_t> buffer(256);
    for (;;) {
        const DWORD length = GetEnvironmentVariableW(
            name, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
}

std::wstring module_dir() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
        if (n == 0) return fs::current_path().wstring();
        if (n < buffer.size() - 1) {
            return fs::path(std::wstring(buffer.data(), n)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      text.data(), static_cast<int>(text.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                      static_cast<int>(text.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

// ── Launcher log ───────────────────────────────────────────────────────
// logs\launcher.log: what the launcher itself did, one timestamped line per
// step (ROM picked and checked, game build started and how it ended, game
// started and how it exited), so a "I picked my ROM and nothing happened"
// report always has a file to look at. Started fresh past 256 KB.
fs::path g_launcher_log_path;

void launcher_log(std::wstring line) {
    if (g_launcher_log_path.empty()) return;
    for (wchar_t& c : line)
        if (c == L'\r' || c == L'\n') c = L' ';
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u  ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::ofstream file(g_launcher_log_path, std::ios::binary | std::ios::app);
    if (file) file << stamp << wide_to_utf8(line) << "\r\n";
}

void open_launcher_log(const fs::path& root) {
    const fs::path dir = root / L"logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    g_launcher_log_path = dir / L"launcher.log";
    const auto size = fs::file_size(g_launcher_log_path, ec);
    if (!ec && size > 256 * 1024) fs::remove(g_launcher_log_path, ec);
    launcher_log(L"---- Launcher " + utf8_to_wide(gsr_online::kReleaseVersion) +
                 L" started (built " __DATE__ " " __TIME__ ") in " + root.wstring());
}

std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

std::wstring read_cached_path(const fs::path& path) {
    const std::string line = read_text(path);
    const std::size_t end = line.find_first_of("\r\n");
    return utf8_to_wide(line.substr(0, end));
}

void write_cached_path(const fs::path& path, const std::wstring& value) {
    // A fresh release folder has no local/ yet; without it the path was
    // silently not kept and the next start asked for the ROM again.
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (file) file << wide_to_utf8(value) << '\n';
}

bool sha1_file(const fs::path& path, std::string* out) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;
    bool ok = false;

    do {
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
        if (file == INVALID_HANDLE_VALUE) break;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM,
                                        nullptr, 0) < 0) break;

        DWORD object_len = 0;
        DWORD result_len = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&object_len),
                              sizeof(object_len), &result_len, 0) < 0) break;
        DWORD hash_len = 0;
        if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                              reinterpret_cast<PUCHAR>(&hash_len),
                              sizeof(hash_len), &result_len, 0) < 0) break;
        object.resize(object_len);
        digest.resize(hash_len);
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_len,
                             nullptr, 0, 0) < 0) break;

        std::vector<UCHAR> buffer(1024 * 1024);
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                          &read, nullptr)) break;
            if (read == 0) {
                if (BCryptFinishHash(hash, digest.data(), hash_len, 0) < 0)
                    break;
                static constexpr char hex[] = "0123456789abcdef";
                out->clear();
                for (UCHAR byte : digest) {
                    out->push_back(hex[byte >> 4]);
                    out->push_back(hex[byte & 15]);
                }
                ok = true;
                break;
            }
            if (BCryptHashData(hash, buffer.data(), read, 0) < 0) break;
        }
    } while (false);

    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

std::wstring pick_file(const wchar_t* title, const wchar_t* filter,
                       const std::wstring& initial_directory = {},
                       HWND owner = nullptr) {
    wchar_t buffer[32768] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrTitle = title;
    dialog.lpstrInitialDir = initial_directory.empty()
        ? nullptr : initial_directory.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return {};
    return buffer;
}

bool validate_rom(const std::wstring& path, std::wstring* error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        *error = L"That file could not be opened.";
        return false;
    }
    std::string actual;
    if (!sha1_file(path, &actual)) {
        *error = L"The ROM could not be read.";
        return false;
    }
    if (actual != kExpectedRomSha1) {
        *error = L"That is not the required Golden Sun USA/Europe ROM.\n\n"
                 L"Expected SHA-1:\n";
        *error += utf8_to_wide(kExpectedRomSha1);
        *error += L"\n\nFound SHA-1:\n";
        *error += utf8_to_wide(actual);
        return false;
    }
    return true;
}

// `owner` is the launcher window: the file dialog and the "not the right
// ROM" message stay in front of it. Without an owner the message could open
// behind the launcher, and the launcher then looked like it did nothing.
std::wstring choose_rom(const fs::path& root, HWND owner) {
    const fs::path cache = root / L"local" / L"launcher-rom.txt";
    const std::wstring cached = read_cached_path(cache);
    std::wstring initial_directory;
    if (!cached.empty()) initial_directory = fs::path(cached).parent_path();
    for (;;) {
        const std::wstring candidate = pick_file(
            L"Select the Golden Sun USA/Europe ROM",
            L"Golden Sun ROM (*.gba)\0*.gba\0All files (*.*)\0*.*\0\0",
            initial_directory, owner);
        if (candidate.empty()) {
            launcher_log(L"Pick ROM: no file chosen.");
            return {};
        }

        std::wstring error;
        if (validate_rom(candidate, &error)) {
            launcher_log(L"Pick ROM: " + candidate + L" - accepted.");
            write_cached_path(cache, candidate);
            return candidate;
        }
        launcher_log(L"Pick ROM: " + candidate + L" - refused: " + error);
        MessageBoxW(owner, error.c_str(), L"Golden Sun Recompiled",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
}

// The ROM picked last time, when it is still there and still the right ROM;
// empty otherwise. The launcher then plays it without asking again.
std::wstring remembered_rom(const fs::path& root) {
    const std::wstring cached = read_cached_path(root / L"local" / L"launcher-rom.txt");
    if (cached.empty()) return {};
    std::wstring error;
    if (validate_rom(cached, &error)) return cached;
    launcher_log(L"Remembered ROM " + cached + L" can no longer be used: " + error);
    return {};
}

// ── Session log capture ────────────────────────────────────────────────
// The game (build/gs011/GoldenSunRecomp.exe) is a console-subsystem exe.
// Launched from this WIN32-subsystem launcher (which has no console of its
// own) without redirected handles, Windows auto-allocates it a fresh console
// window that is destroyed the instant the child exits — so a crash, a
// freeze the user kills from Task Manager, or even a clean quit all lose
// the scrollback. This captures the child's stdout/stderr into a
// timestamped file under logs/ instead, durably: each line is flushed to
// disk (FILE_FLAG_WRITE_THROUGH + an explicit FlushFileBuffers) the moment
// it is read from the pipe, so a kill mid-freeze still leaves everything
// produced up to that point on disk.
//
// stdout and stderr are interleaved into ONE file, each line tagged
// "[OUT] "/"[ERR] ", rather than written to two separate files: a single
// chronological, greppable log is easier for the user to hand over than two
// files they'd have to interleave by eye, and the tag keeps the streams
// distinguishable.
constexpr int kKeepLogCount = 20;  // recent sessions kept; older ones pruned

std::wstring make_session_log_path(const fs::path& logs_dir) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, std::size(name), L"session_%04u%02u%02u_%02u%02u%02u.log",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return (logs_dir / name).wstring();
}

// Clipboard support is best-effort. The launcher's clipboard preference must
// never turn a valid game launch into an error if another application owns
// the clipboard or the allocation/API call fails.
bool copy_session_id_to_clipboard(HWND owner, const std::wstring& log_path) {
    const std::wstring session_id = gsr::session_log_id_from_path(log_path);
    if (session_id.empty()) return false;

    const SIZE_T bytes = (session_id.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return false;
    auto* text = static_cast<wchar_t*>(GlobalLock(memory));
    if (!text) {
        GlobalFree(memory);
        return false;
    }
    std::copy(session_id.begin(), session_id.end(), text);
    text[session_id.size()] = L'\0';
    GlobalUnlock(memory);

    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return false;
    }

    bool copied = false;
    if (EmptyClipboard()) {
        if (SetClipboardData(CF_UNICODETEXT, memory)) {
            // Ownership transfers to the clipboard on success.
            memory = nullptr;
            copied = true;
        }
    }
    CloseClipboard();
    if (memory) GlobalFree(memory);
    return copied;
}

// Keeps the newest (kKeepLogCount - 1) existing logs so this session's new
// file brings the total back up to kKeepLogCount. session_YYYYMMDD_HHMMSS
// sorts lexicographically in chronological order, so no parsing is needed.
void prune_old_logs(const fs::path& logs_dir) {
    std::error_code ec;
    std::vector<fs::path> logs;
    for (const auto& entry : fs::directory_iterator(logs_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"session_", 0) == 0 &&
            name.size() > 4 && name.compare(name.size() - 4, 4, L".log") == 0) {
            logs.push_back(entry.path());
        }
    }
    if (logs.size() < static_cast<std::size_t>(kKeepLogCount)) return;
    std::sort(logs.begin(), logs.end());
    const std::size_t remove_count =
        logs.size() - (static_cast<std::size_t>(kKeepLogCount) - 1);
    for (std::size_t i = 0; i < remove_count; ++i) {
        fs::path events = logs[i];
        fs::path phase = logs[i];
        events.replace_extension(L".events.csv");
        phase.replace_extension(L".phase.csv");
        const fs::path misses = events.wstring() + L".misses.csv";
        const fs::path recursion = events.wstring() + L".recursion.csv";
        const fs::path ram_churn = events.wstring() + L".ram-churn.csv";
        const fs::path input = logs[i].wstring().substr(
            0, logs[i].wstring().size() - 4) + L".input";
        fs::remove(logs[i], ec);
        fs::remove(events, ec);
        fs::remove(misses, ec);
        fs::remove(recursion, ec);
        fs::remove(ram_churn, ec);
        fs::remove(phase, ec);
        fs::remove(input, ec);
        for (unsigned suffix = 1; suffix < 1000; ++suffix) {
            const fs::path suffixed = logs[i].wstring().substr(
                0, logs[i].wstring().size() - 4) + L"_" +
                std::to_wstring(suffix) + L".input";
            fs::remove(suffixed, ec);
        }
    }
}

bool create_empty_file_exclusive(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

// Thread-safe sink for the two pipe-reader threads below. One HANDLE, one
// mutex — writes from either stream serialize through write_line so tagged
// lines never interleave mid-line in the file.
class SessionLogWriter {
public:
    explicit SessionLogWriter(HANDLE file) : file_(file) {}

    void write_line(const char* tag, const char* data, std::size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        DWORD written = 0;
        WriteFile(file_, tag, 6, &written, nullptr);
        if (len > 0) {
            WriteFile(file_, data, static_cast<DWORD>(len), &written, nullptr);
        }
        WriteFile(file_, "\r\n", 2, &written, nullptr);
        // Belt-and-suspenders alongside FILE_FLAG_WRITE_THROUGH on the file
        // handle: guarantees this line is durable before the reader thread
        // goes back to blocking on the next ReadFile, which is exactly the
        // window a Task-Manager kill can land in.
        FlushFileBuffers(file_);
    }

private:
    HANDLE file_;
    std::mutex mutex_;
};

// Drains one child pipe handle on its own thread until EOF (the child
// closing its end, which happens when it exits), tagging and forwarding
// complete lines to `writer` as they arrive. A dedicated thread per pipe
// (rather than one polling loop over both, or overlapped I/O) is the
// simplest correct fix for the classic "child blocks writing to a full pipe
// buffer nobody is draining" deadlock: ReadFile here returns as soon as ANY
// bytes are available, so each thread is either blocked *waiting for data*
// (never blocking the child) or immediately draining what arrived, and it
// never waits on anything the child itself is blocked on — no circular
// wait, so no deadlock is reachable by construction.
void pump_pipe_to_log(HANDLE pipe_read, const char* tag,
                      SessionLogWriter* writer) {
    std::string pending;
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(pipe_read, buffer, sizeof(buffer), &read,
                                 nullptr);
        if (!ok || read == 0) break;  // child exited (EOF) or pipe error
        pending.append(buffer, read);
        std::size_t start = 0;
        for (;;) {
            const std::size_t newline = pending.find('\n', start);
            if (newline == std::string::npos) break;
            std::size_t line_len = newline - start;
            if (line_len > 0 && pending[start + line_len - 1] == '\r') {
                --line_len;  // tolerate the child's own CRLF too
            }
            writer->write_line(tag, pending.data() + start, line_len);
            start = newline + 1;
        }
        pending.erase(0, start);
    }
    if (!pending.empty()) {
        // A final message with no trailing newline (e.g. an abort() print
        // right before the process dies) must not be dropped.
        writer->write_line(tag, pending.data(), pending.size());
    }
    CloseHandle(pipe_read);
}

// CreateProcess takes a complete environment block when one is supplied. Use
// that instead of SetEnvironmentVariableW so launcher settings do not leak
// into the launcher process (or into a later child if the UI is reused).
class ChildEnvironment {
public:
    ChildEnvironment() {
        LPWCH source = GetEnvironmentStringsW();
        if (!source) return;
        valid_ = true;
        for (const wchar_t* entry = source; *entry != L'\0';
             entry += std::wcslen(entry) + 1) {
            entries_.emplace_back(entry);
        }
        FreeEnvironmentStringsW(source);
    }

    bool valid() const { return valid_; }

    bool contains(const std::wstring& name) const {
        return find(name) != entries_.end();
    }

    void set(const std::wstring& name, const std::wstring& value) {
        remove(name);
        entries_.push_back(name + L"=" + value);
    }

    void unset(const std::wstring& name) { remove(name); }

    std::vector<wchar_t> block() const {
        std::vector<std::wstring> sorted = entries_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::wstring& left, const std::wstring& right) {
                      return _wcsicmp(left.c_str(), right.c_str()) < 0;
                  });

        std::vector<wchar_t> result;
        for (const std::wstring& entry : sorted) {
            result.insert(result.end(), entry.begin(), entry.end());
            result.push_back(L'\0');
        }
        // The environment block is terminated by an additional NUL.
        if (result.empty()) result.push_back(L'\0');
        result.push_back(L'\0');
        return result;
    }

private:
    using EntryList = std::vector<std::wstring>;

    EntryList::iterator find(const std::wstring& name) {
        return std::find_if(entries_.begin(), entries_.end(),
                            [&name](const std::wstring& entry) {
                                const std::size_t equals = entry.find(L'=');
                                // Entries beginning with '=' are Windows'...
                                // per-drive current-directory variables.
                                return equals != std::wstring::npos &&
                                       equals != 0 &&
                                       equals == name.size() &&
                                       _wcsnicmp(entry.c_str(), name.c_str(),
                                                 equals) == 0;
                            });
    }

    EntryList::const_iterator find(const std::wstring& name) const {
        return std::find_if(entries_.begin(), entries_.end(),
                            [&name](const std::wstring& entry) {
                                const std::size_t equals = entry.find(L'=');
                                return equals != std::wstring::npos &&
                                       equals != 0 &&
                                       equals == name.size() &&
                                       _wcsnicmp(entry.c_str(), name.c_str(),
                                                 equals) == 0;
                            });
    }

    void remove(const std::wstring& name) {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&name](const std::wstring& entry) {
                                          const std::size_t equals =
                                              entry.find(L'=');
                                          return equals != std::wstring::npos &&
                                                 equals != 0 &&
                                                 equals == name.size() &&
                                                 _wcsnicmp(entry.c_str(),
                                                           name.c_str(),
                                                           equals) == 0;
                                      }),
                       entries_.end());
    }

    bool valid_ = false;
    EntryList entries_;
};

// ---- Bug reports (player launcher) -------------------------------------------
// Players press F12 in the game: the runner writes the last 2 seconds of
// frames (logs/gpu_rewind_NNNN, GSR_FRAME_REWIND, always on in a release)
// and shows "BUG REPORT SAVED". When the game closes, the launcher packs
// every capture from that session, or the crash files if it crashed, with
// the session log, the settings files and a short system note into one zip
// in logs/bug_reports, and points the player at it.
std::set<std::wstring> rewind_dirs(const fs::path& logs_dir) {
    std::set<std::wstring> dirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(logs_dir, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (entry.is_directory(ec) && name.rfind(L"gpu_rewind_", 0) == 0)
            dirs.insert(name);
    }
    return dirs;
}

std::string registry_text(const wchar_t* value) {
    wchar_t buffer[256] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", value,
                     RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
        return "?";
    return wide_to_utf8(buffer);
}

// Runs tar.exe (in Windows since 10 version 1803) to write the zip.
bool run_tar(const std::wstring& arguments) {
    wchar_t system_dir[MAX_PATH] = {};
    GetSystemDirectoryW(system_dir, MAX_PATH);
    const std::wstring tar = std::wstring(system_dir) + L"\\tar.exe";
    std::wstring command = L"\"" + tar + L"\" " + arguments;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tar.c_str(), mutable_command.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process))
        return false;
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0;
}

// ── Online: HTTPS through WinHTTP ──────────────────────────────────────
// Used for checking GitHub for a newer release and, when the player presses
// Send report, uploading a bug report.

struct HttpTarget {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

bool crack_url(const std::wstring& url, HttpTarget* target) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[4096] = {}, extra[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    target->host.assign(host, parts.dwHostNameLength);
    target->path.assign(path, parts.dwUrlPathLength);
    target->path.append(extra, parts.dwExtraInfoLength);
    target->port = parts.nPort;
    return true;
}

using HttpProgress = std::function<void(std::uint64_t done, std::uint64_t total)>;

// One HTTPS request. `upload` is sent as the body when given; the answer goes
// to `download` when given, otherwise into *text. Redirects (GitHub's
// downloads use them) are followed. Returns the HTTP status, or 0 when there
// was no answer at all (*error then says why).
int http_request(const wchar_t* verb, const HttpTarget& target,
                 const std::vector<std::wstring>& headers, const fs::path* upload,
                 const fs::path* download, std::string* text,
                 const HttpProgress& progress, std::wstring* error) {
    struct Handle {
        HINTERNET h = nullptr;
        ~Handle() { if (h) WinHttpCloseHandle(h); }
    } session, connection, request;
    auto fail = [&](const wchar_t* what) {
        const DWORD code = GetLastError();
        *error = std::wstring(what) + L" (error " + std::to_wstring(code) + L")";
        if (code == ERROR_WINHTTP_NAME_NOT_RESOLVED || code == ERROR_WINHTTP_CANNOT_CONNECT)
            *error = L"No internet connection.";
        else if (code == ERROR_WINHTTP_TIMEOUT)
            *error = L"The connection timed out.";
        return 0;
    };
    session.h = WinHttpOpen(L"GoldenSunLauncher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) return fail(L"Could not start an internet connection");
    WinHttpSetTimeouts(session.h, 15000, 15000, 60000, 60000);
    connection.h = WinHttpConnect(session.h, target.host.c_str(), target.port, 0);
    if (!connection.h) return fail(L"Could not connect");
    request.h = WinHttpOpenRequest(connection.h, verb, target.path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
    if (!request.h) return fail(L"Could not start the request");
    for (const std::wstring& header : headers)
        WinHttpAddRequestHeaders(request.h, header.c_str(), static_cast<DWORD>(-1L),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    std::ifstream body;
    std::uint64_t body_size = 0;
    if (upload) {
        std::error_code ec;
        body_size = fs::file_size(*upload, ec);
        body.open(*upload, std::ios::binary);
        if (ec || !body) {
            *error = L"The report file could not be read.";
            return 0;
        }
    }
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(body_size), 0))
        return fail(L"Could not reach the server");
    if (upload) {
        std::vector<char> chunk(64 * 1024);
        std::uint64_t sent = 0;
        while (sent < body_size) {
            body.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const DWORD n = static_cast<DWORD>(body.gcount());
            if (n == 0) break;
            DWORD written = 0;
            if (!WinHttpWriteData(request.h, chunk.data(), n, &written))
                return fail(L"Sending stopped");
            sent += written;
            if (progress) progress(sent, body_size);
        }
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) return fail(L"No answer from the server");

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    wchar_t length_text[32] = {};
    DWORD length_size = sizeof(length_text);
    std::uint64_t total = 0;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, length_text, &length_size,
                            WINHTTP_NO_HEADER_INDEX))
        total = std::wcstoull(length_text, nullptr, 10);

    std::ofstream out;
    if (download && status == 200) {
        out.open(*download, std::ios::binary | std::ios::trunc);
        if (!out) {
            *error = L"The download could not be saved.";
            return 0;
        }
    }
    std::vector<char> buffer(256 * 1024);
    std::uint64_t received = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return fail(L"The download stopped");
        if (available == 0) break;
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(),
                             std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &got))
            return fail(L"The download stopped");
        if (got == 0) break;
        received += got;
        if (out.is_open()) {
            out.write(buffer.data(), got);
            if (!out) {
                *error = L"The download could not be saved (is the disk full?).";
                return 0;
            }
            if (progress) progress(received, total);
        } else if (text && text->size() < 4 * 1024 * 1024) {
            text->append(buffer.data(), got);
        }
    }
    return static_cast<int>(status);
}

// ── Bug reports ────────────────────────────────────────────────────────
// The launcher's Send report button uploads the zips to the project's report
// service (tools/report_service). Every zip holds the small logs plus at most
// ONE F12 capture (about 7 MB zipped), so each upload stays small; a session
// with three captures makes three zips.

// The text in a report with the player's Windows user name taken out of
// paths (C:\Users\Name\... becomes C:\Users\<user>\...), so a report never
// shows who sent it.
std::string scrub_user_name(std::string text) {
    auto lower = [](std::string s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    };
    // The account name and the profile folder's name can differ (a Microsoft
    // account's folder is often cut short), so both are taken out.
    std::vector<std::string> names;
    wchar_t user[256] = {};
    if (GetEnvironmentVariableW(L"USERNAME", user, 256) && user[0])
        names.push_back(wide_to_utf8(user));
    wchar_t profile[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) && profile[0]) {
        const std::string folder = wide_to_utf8(fs::path(profile).filename().wstring());
        if (!folder.empty() && lower(folder) != (names.empty() ? "" : lower(names[0])))
            names.push_back(folder);
    }
    for (const std::string& name : names) {
        for (const char* sep : {"\\", "/", "\\\\"}) {
            const std::string needle = lower(std::string(sep) + "users" + sep + name);
            const std::string replacement = std::string(sep) + "Users" + sep + "<user>";
            std::string folded = lower(text);
            std::size_t at = 0;
            while ((at = folded.find(needle, at)) != std::string::npos) {
                const std::size_t after = at + needle.size();
                // Only the whole folder name: "Jim" must not match "Jimmy".
                if (after < text.size() && text[after] != '\\' && text[after] != '/' &&
                    text[after] != '"' && text[after] != '\'' && text[after] != ' ' &&
                    text[after] != '\r' && text[after] != '\n') {
                    at = after;
                    continue;
                }
                text.replace(at, needle.size(), replacement);
                folded.replace(at, needle.size(), lower(replacement));
                at += replacement.size();
            }
        }
    }
    return text;
}

// Copies a log or settings file into the report, user name taken out. A
// very long session log keeps its first 256 KB and its last 3 MB: the start
// says how the game was set up, the end is where the problem is.
void copy_into_report(const fs::path& from, const fs::path& staging) {
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) return;
    std::wstring ext = from.extension().wstring();
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    const bool text = ext == L".log" || ext == L".txt" || ext == L".ini" ||
                      ext == L".csv";
    if (!text) {
        fs::copy_file(from, staging / from.filename(),
                      fs::copy_options::overwrite_existing, ec);
        return;
    }
    std::string content = read_text(from);
    constexpr std::size_t kHead = 256 * 1024, kTail = 3 * 1024 * 1024;
    if (content.size() > kHead + kTail) {
        content = content.substr(0, kHead) +
                  "\n\n[... the middle of this log was left out of the bug report ...]\n\n" +
                  content.substr(content.size() - kTail);
    }
    std::ofstream out(staging / from.filename(), std::ios::binary | std::ios::trunc);
    out << scrub_user_name(content);
}

// Returns the zips (one per F12 capture, or one when there was none), or
// the folder when zipping failed; empty when there was nothing to report.
std::vector<fs::path> make_bug_report(const fs::path& root, const fs::path& game_dir,
                                      const std::wstring& rom,
                                      const std::wstring& log_path,
                                      const std::vector<std::wstring>& rewinds,
                                      bool crashed, DWORD exit_code) {
    const fs::path logs_dir = root / L"logs";
    const fs::path reports = logs_dir / L"bug_reports";
    std::error_code ec;
    fs::create_directories(reports, ec);
    std::wstring name = fs::path(log_path).stem().wstring();
    if (name.rfind(L"session_", 0) == 0) name = name.substr(8);
    if (name.empty()) name = L"latest";
    name = L"bug_report_" + name;
    const fs::path staging = reports / name;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) return {};

    // The session log and anything written beside it under the same name.
    if (!log_path.empty()) {
        const fs::path log(log_path);
        const std::wstring stem = log.stem().wstring();
        for (const auto& entry : fs::directory_iterator(logs_dir, ec)) {
            if (entry.path().filename().wstring().rfind(stem, 0) == 0)
                copy_into_report(entry.path(), staging);
        }
    }
    copy_into_report(logs_dir / L"launcher.log", staging);
    for (const wchar_t* file : {L"config.ini", L"game_options.ini",
                                L"keybinds.ini", L"GoldenSunGame.build.txt",
                                L"run_state.txt"})
        copy_into_report(game_dir / file, staging);
    // The game's save, kept beside the ROM (runtime.cpp: the ROM path with
    // .sav), so the problem can be played again from the same place.
    if (!rom.empty())
        copy_into_report(fs::path(rom).replace_extension(L".sav"), staging);
    if (crashed) {
        copy_into_report(game_dir / L"crash_report.txt", staging);
        copy_into_report(game_dir / L"crash_dump.dmp", staging);
        copy_into_report(game_dir / L"crash_memory.bin", staging);
        copy_into_report(game_dir / L"crash_trail.csv", staging);
    }
    {
        std::ofstream info(staging / L"report_info.txt", std::ios::binary);
        info << "Golden Sun Recompiled bug report\n"
             << "Launcher built: " << __DATE__ << " " << __TIME__ << "\n"
             << "Windows: " << registry_text(L"ProductName") << " "
             << registry_text(L"DisplayVersion") << " (build "
             << registry_text(L"CurrentBuild") << ")\n"
             << "F12 captures: " << rewinds.size() << "\n"
             << "Game crashed: " << (crashed ? "yes" : "no") << "\n"
             << "Game exit code: " << exit_code << "\n";
    }

    std::vector<fs::path> zips;
    const std::size_t parts = std::max<std::size_t>(1, rewinds.size());
    for (std::size_t i = 0; i < parts; ++i) {
        const std::wstring suffix =
            parts > 1 ? L"_part" + std::to_wstring(i + 1) + L"of" + std::to_wstring(parts)
                      : L"";
        const fs::path zip = reports / (name + suffix + L".zip");
        fs::remove(zip, ec);
        // Each file by name, not ".": entries stored as "./name" make
        // Windows' own zip viewer show the zip as empty.
        std::wstring arguments = L"-a -c -f \"" + zip.wstring() + L"\" -C \"" +
                                 staging.wstring() + L"\"";
        for (const auto& entry : fs::directory_iterator(staging, ec))
            arguments += L" \"" + entry.path().filename().wstring() + L"\"";
        if (!rewinds.empty())
            arguments += L" -C \"" + logs_dir.wstring() + L"\" \"" + rewinds[i] + L"\"";
        if (!run_tar(arguments) || !fs::is_regular_file(zip, ec)) {
            for (const fs::path& made : zips) fs::remove(made, ec);
            fs::remove(zip, ec);
            return {staging};
        }
        zips.push_back(zip);
    }
    fs::remove_all(staging, ec);
    // The captures are in the zips now; they are large, so drop the loose
    // copies.
    for (const std::wstring& dir : rewinds) fs::remove_all(logs_dir / dir, ec);
    return zips;
}

// Where players can still upload a report by hand: the Google Form, offered
// when sending fails. The project page links the same form.
constexpr const wchar_t* kBugReportFormUrl =
    L"https://docs.google.com/forms/d/e/"
    L"1FAIpQLScTmnmH6_BXYsYVfIKtjDsN-gLx59mv4pVjYx4ITbH2YGktBw/viewform";

// ── The "Send report" window ───────────────────────────────────────────
// Shown after the game closed with a crash or F12 captures. The player can
// write what happened and press Send, which uploads the report zips to the
// project's report service; nothing is sent otherwise.
constexpr UINT kReportProgressMessage = WM_APP + 20;  // wParam: percent
constexpr UINT kReportDoneMessage = WM_APP + 21;      // wParam: 1 sent
constexpr int kReportEdit = 3001;
constexpr int kReportSend = 3002;
constexpr int kReportClose = 3003;
constexpr int kReportFolder = 3004;

struct ReportWindow {
    std::vector<fs::path> reports;
    bool crashed = false;
    bool zipped = false;
    HWND edit = nullptr, status = nullptr, send = nullptr, close = nullptr;
    HFONT font = nullptr;
    std::thread worker;
    std::wstring error;
    bool sent = false;
};
ReportWindow* g_report_window = nullptr;

void report_set_status(const std::wstring& text) {
    if (g_report_window && g_report_window->status)
        SetWindowTextW(g_report_window->status, text.c_str());
}

// Uploads every report zip with the player's description; runs on its own
// thread and posts kReportProgressMessage / kReportDoneMessage to `window`.
void send_reports(HWND window, std::vector<fs::path> reports, std::string description,
                  std::wstring* error) {
    HttpTarget target;
    target.host = utf8_to_wide(gsr_online::kReportHost);
    target.path = utf8_to_wide(gsr_online::kReportPath);
    std::uint64_t total = 0, before = 0;
    std::error_code ec;
    for (const fs::path& report : reports) total += fs::file_size(report, ec);
    bool ok = true;
    for (std::size_t i = 0; i < reports.size() && ok; ++i) {
        std::string part = description;
        if (reports.size() > 1)
            part += " [part " + std::to_string(i + 1) + " of " + std::to_string(reports.size()) + "]";
        const std::vector<std::wstring> headers = {
            L"X-GSR-Client: GoldenSunLauncher",
            L"X-GSR-Platform: windows",
            L"X-GSR-Launcher: " + utf8_to_wide(gsr_online::kReleaseVersion) + L" " +
                utf8_to_wide(__DATE__),
            L"X-GSR-File: " + utf8_to_wide(gsr_online::percent_encode(
                                  wide_to_utf8(reports[i].filename().wstring()), 120)),
            L"X-GSR-Description: " + utf8_to_wide(gsr_online::percent_encode(part)),
            L"Content-Type: application/octet-stream",
        };
        std::string answer;
        const int status = http_request(
            L"POST", target, headers, &reports[i], nullptr, &answer,
            [&](std::uint64_t done, std::uint64_t) {
                const int percent = total ? static_cast<int>((before + done) * 100 / total) : 0;
                PostMessageW(window, kReportProgressMessage, static_cast<WPARAM>(percent), 0);
            },
            error);
        before += fs::file_size(reports[i], ec);
        if (status != 200) {
            ok = false;
            if (status != 0) {
                while (!answer.empty() && (answer.back() == '\n' || answer.back() == '\r'))
                    answer.pop_back();
                *error = utf8_to_wide(answer.empty() ? "The server said no (" +
                                                           std::to_string(status) + ")."
                                                     : answer);
            }
        }
    }
    PostMessageW(window, kReportDoneMessage, ok ? 1 : 0, 0);
}

LRESULT CALLBACK report_window_proc(HWND window, UINT message, WPARAM w_param,
                                    LPARAM l_param) {
    ReportWindow* state = g_report_window;
    switch (message) {
    case WM_COMMAND: {
        if (!state) break;
        const int id = LOWORD(w_param);
        if (id == kReportFolder) {
            const std::wstring select = L"/select,\"" + state->reports.front().wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr,
                          SW_SHOWNORMAL);
            return 0;
        }
        if (id == kReportClose) {
            DestroyWindow(window);
            return 0;
        }
        if (id == kReportSend && !state->sent) {
            const int length = GetWindowTextLengthW(state->edit);
            std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
            GetWindowTextW(state->edit, text.data(), length + 1);
            text.resize(static_cast<std::size_t>(length));
            EnableWindow(state->send, FALSE);
            EnableWindow(state->close, FALSE);
            SendMessageW(state->edit, EM_SETREADONLY, TRUE, 0);
            report_set_status(L"Sending...");
            launcher_log(L"Bug report: sending " + std::to_wstring(state->reports.size()) +
                         L" file(s).");
            if (state->worker.joinable()) state->worker.join();
            state->error.clear();
            state->worker = std::thread(send_reports, window, state->reports,
                                        wide_to_utf8(text), &state->error);
            return 0;
        }
        break;
    }
    case kReportProgressMessage:
        report_set_status(L"Sending... " + std::to_wstring(static_cast<int>(w_param)) + L"%");
        return 0;
    case kReportDoneMessage:
        if (!state) return 0;
        if (state->worker.joinable()) state->worker.join();
        EnableWindow(state->close, TRUE);
        if (w_param == 1) {
            state->sent = true;
            launcher_log(L"Bug report: sent.");
            report_set_status(L"Sent. Thank you!");
            SetWindowTextW(state->close, L"Close");
            return 0;
        }
        launcher_log(L"Bug report: sending failed: " + state->error);
        report_set_status(L"Not sent: " + state->error);
        EnableWindow(state->send, TRUE);
        SetWindowTextW(state->send, L"Try again");
        SendMessageW(state->edit, EM_SETREADONLY, FALSE, 0);
        if (MessageBoxW(window,
                        (L"The report could not be sent:\n" + state->error +
                         L"\n\nYou can try again, or upload the zip yourself on the bug "
                         L"report form. Open the form and the folder now?")
                            .c_str(),
                        L"Golden Sun Recompiled", MB_YESNO | MB_ICONWARNING) == IDYES) {
            ShellExecuteW(nullptr, L"open", kBugReportFormUrl, nullptr, nullptr, SW_SHOWNORMAL);
            const std::wstring select = L"/select,\"" + state->reports.front().wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr,
                          SW_SHOWNORMAL);
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w_param);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_CLOSE:
        if (state && state->worker.joinable()) return 0;  // wait for the upload
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void offer_bug_report(const std::vector<fs::path>& reports, bool crashed) {
    if (reports.empty()) return;
    ReportWindow state;
    state.reports = reports;
    state.crashed = crashed;
    state.zipped = reports.front().extension() == L".zip";
    if (!state.zipped) {
        // Zipping failed: nothing to upload, only the folder to point at.
        const std::wstring text =
            L"A bug report was saved, but it could not be zipped:\n\n" +
            reports.front().wstring() +
            L"\n\nPress OK to open the bug report form and this folder; zip the folder "
            L"(and any gpu_rewind folders next to it) and add it to the form.";
        if (MessageBoxW(nullptr, text.c_str(), L"Golden Sun Recompiled",
                        MB_OKCANCEL | MB_SETFOREGROUND | MB_ICONWARNING) == IDOK) {
            ShellExecuteW(nullptr, L"open", kBugReportFormUrl, nullptr, nullptr, SW_SHOWNORMAL);
            ShellExecuteW(nullptr, L"open", L"explorer.exe", reports.front().c_str(), nullptr,
                          SW_SHOWNORMAL);
        }
        return;
    }
    if (!gsr_online::report_upload_enabled()) {
        // A launcher built without the report service (a fork, a developer
        // build): the zips and the form, as before the Send button.
        std::wstring text = crashed
            ? L"The game closed unexpectedly. A bug report was saved:\n\n"
            : L"Your bug report was saved:\n\n";
        for (const fs::path& report : reports) text += report.filename().wstring() + L"\n";
        text += L"in " + reports.front().parent_path().wstring() +
                L"\n\nPress OK to open the bug report form and this folder, write a few "
                L"words about what happened and add the zip. Cancel keeps the files "
                L"without sending anything.";
        if (MessageBoxW(nullptr, text.c_str(), L"Golden Sun Recompiled",
                        MB_OKCANCEL | MB_SETFOREGROUND |
                            (crashed ? MB_ICONWARNING : MB_ICONINFORMATION)) == IDOK) {
            ShellExecuteW(nullptr, L"open", kBugReportFormUrl, nullptr, nullptr, SW_SHOWNORMAL);
            const std::wstring select = L"/select,\"" + reports.front().wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr,
                          SW_SHOWNORMAL);
        }
        return;
    }

    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };

    const wchar_t class_name[] = L"GoldenSunRecompiledReportWindow";
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = report_window_proc;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&window_class);

    const int width = px(520), height = px(400);
    RECT frame{0, 0, width, height};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&frame, style, FALSE, WS_EX_APPWINDOW);
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_TOPMOST, class_name, L"Golden Sun Recompiled: bug report",
        style, (GetSystemMetrics(SM_CXSCREEN) - (frame.right - frame.left)) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - (frame.bottom - frame.top)) / 2,
        frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!window) return;
    g_report_window = &state;
    state.font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    auto add = [&](const wchar_t* cls, const std::wstring& text, DWORD extra, int id, int x,
                   int y, int w, int h) {
        HWND child = CreateWindowExW(
            cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text.c_str(),
            WS_CHILD | WS_VISIBLE | extra, px(x), px(y), px(w), px(h), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
            nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
        return child;
    };

    std::wstring heading = crashed ? L"The game closed unexpectedly. A bug report was saved."
                                   : L"Your bug report was saved.";
    heading += L"\nWhat happened, and where in the game? A few words help a lot:";
    add(L"STATIC", heading, 0, 0, 20, 16, 480, 44);
    state.edit = add(L"EDIT", L"",
                     ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
                     kReportEdit, 20, 66, 480, 110);
    SendMessageW(state.edit, EM_SETLIMITTEXT, 500, 0);
    std::wstring files;
    std::uint64_t bytes = 0;
    std::error_code ec;
    for (const fs::path& report : reports) bytes += fs::file_size(report, ec);
    files = L"Send report uploads " +
            std::wstring(reports.size() > 1 ? std::to_wstring(reports.size()) + L" zips"
                                            : std::wstring(L"the zip")) +
            L" (" + std::to_wstring(std::max<std::uint64_t>(1, bytes / (1024 * 1024))) +
            L" MB) to the developer: the game's log and screen captures, your save "
            L"file, your settings and your Windows version. Your Windows user name is taken out. Nothing is "
            L"sent unless you press Send report.";
    add(L"STATIC", files, 0, 0, 20, 186, 480, 84);
    state.status = add(L"STATIC", L"", 0, 0, 20, 276, 480, 40);
    state.send = add(L"BUTTON", L"Send report", BS_DEFPUSHBUTTON | WS_TABSTOP, kReportSend, 20,
                     330, 150, 40);
    state.close = add(L"BUTTON", L"Don't send", WS_TABSTOP, kReportClose, 180, 330, 130, 40);
    add(L"BUTTON", L"Open folder", WS_TABSTOP, kReportFolder, 370, 330, 130, 40);

    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(state.edit);
    // The launcher window was closed when the game started, so its WM_QUIT
    // is pending. Windows hands it out whenever the queue runs empty, which
    // would end this loop and leave the window on screen with nobody
    // handling its buttons (the 0.2 "Send does nothing" bug). So the loop
    // runs until this window is gone, and a WM_QUIT seen meanwhile is posted
    // again afterwards for the launcher's own loop.
    bool launcher_quit = false;
    int launcher_exit_code = 0;
    MSG message{};
    while (IsWindow(window)) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got == -1) break;
        if (got == 0) {
            launcher_quit = true;
            launcher_exit_code = static_cast<int>(message.wParam);
            continue;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.worker.joinable()) state.worker.join();
    g_report_window = nullptr;
    if (state.font) DeleteObject(state.font);
    UnregisterClassW(class_name, GetModuleHandleW(nullptr));
    if (launcher_quit) PostQuitMessage(launcher_exit_code);
}

int run_game(const fs::path& root, const std::wstring& rom, HWND window) {
    // Golden Sun's flash save is 64 KiB; the engine refuses a larger .sav
    // (runtime.cpp "save file too large"), so say why instead of exiting.
    constexpr std::uintmax_t kGameSaveBytes = 0x10000;
    {
        const fs::path save = fs::path(rom).replace_extension(L".sav");
        std::error_code save_ec;
        const std::uintmax_t save_size = fs::file_size(save, save_ec);
        if (!save_ec && save_size > kGameSaveBytes) {
            launcher_log(L"Start game: save file " + save.wstring() + L" is " +
                         std::to_wstring(save_size) +
                         L" bytes, larger than 65536: not started.");
            const std::wstring text =
                L"This save file can't be used\n\n"
                L"The save file next to your ROM was made by an emulator or "
                L"another program, and Golden Sun Recompiled can't read it.\n\n" +
                save.wstring() +
                L"\n\nMove it to another folder or delete it, then start the "
                L"game again. The game will make a new save there.";
            MessageBoxW(window, text.c_str(), L"Golden Sun Recompiled",
                        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return 1;
        }
    }
    // Save before spawning so a launch cannot lose a changed checkbox.
    if (!kReleaseLauncher) save_launcher_audio_settings(root, g_audio_settings);
    // The game runs without the BIOS (Jimmy, 2026-09-24): no BIOS path is
    // needed or passed and no Nintendo code executes.

    // Prefer gs011_opt: it is configured WITH SDL2 (the host window), so the
    // game actually presents a frame. build/gs011_rel is the same tree at
    // Release -O3 but was configured without SDL2 (verified 2026-08-15:
    // SDL2_INCLUDE_DIR/SDL2_LIBRARY both NOT-FOUND in its CMakeCache), so
    // host_window stubs out and it exits after presenting nothing. build/gs011
    // was configured Debug with no -O flag at all (verified 2026-08-14: zero
    // -O matches in its build.ninja against 685 -g), which measured ~3.7x
    // slower headless — 9.2s vs 2.5s over 600 frames. Fall back to gs011_rel,
    // then gs011, so a checkout without the current build still runs.
    fs::path game = root / L"build" / L"gs011_opt" / L"GoldenSunRecomp.exe";
    // A release folder holds the game right beside the launcher.
    if (kReleaseLauncher) game = root / L"GoldenSunRecomp.exe";
    if (!fs::is_regular_file(game)) {
        game = root / L"build" / L"gs011_rel" / L"GoldenSunRecomp.exe";
    }
    if (!fs::is_regular_file(game)) {
        game = root / L"build" / L"gs011" / L"GoldenSunRecomp.exe";
    }
    if (!fs::is_regular_file(game)) {
        launcher_log(L"Start game: GoldenSunRecomp.exe not found.");
        MessageBoxW(window,
                    L"GoldenSunRecomp.exe was not found. Run the build first.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }

    fs::create_directories(root / L"recomp_cache");

    const fs::path logs_dir = root / L"logs";
    std::error_code dir_ec;
    fs::create_directories(logs_dir, dir_ec);
    HANDLE log_file = INVALID_HANDLE_VALUE;
    std::wstring log_path;
    if (!dir_ec) {
        prune_old_logs(logs_dir);
        log_path = make_session_log_path(logs_dir);
        // FILE_FLAG_WRITE_THROUGH: every WriteFile below is committed to
        // disk before it returns, not left sitting in the OS cache — the
        // crash/freeze durability requirement, satisfied at the handle
        // level rather than relying solely on the FlushFileBuffers calls.
        log_file = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                               nullptr);
    }
    // A log that can't be opened (full disk, permissions) must not block
    // play: fall back to the pre-existing unredirected behavior below.
    const bool logging = (log_file != INVALID_HANDLE_VALUE);

    // Discoverability without a console: stray console windows stealing
    // focus are exactly what this feature must not introduce, so the
    // session path is never printed anywhere — it is written into a fixed,
    // overwritten-every-launch file instead. logs/latest.txt is a stable
    // path anyone (the user, a bug report, a second tool) can read to find
    // the current/most recent session log without parsing timestamps.
    if (logging) {
        std::ofstream latest(logs_dir / L"latest.txt", std::ios::binary | std::ios::trunc);
        if (latest) latest << wide_to_utf8(log_path) << '\n';

        if (g_audio_settings.copy_session_id_to_clipboard) {
            // Failure is intentionally silent; logging and launch continue.
            copy_session_id_to_clipboard(window, log_path);
        }
    }

    ChildEnvironment child_environment;
    if (!child_environment.valid()) {
        if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        launcher_log(L"Start game: the game environment could not be prepared.");
        MessageBoxW(window, L"The game environment could not be prepared.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }

    // Golden Sun's Camelot intro overflows the Windows host stack with the
    // present-in-place path. Use the stable frame-boundary unwind path here.
    child_environment.set(L"GBARECOMP_PRESENT_IN_PLACE", L"0");
    // Native MP2K and decoupled Turbo are experimental and opt-in. Strict
    // static acceptance always wins over the UI and keeps both canonical.
    const auto audio_policy = gsr::resolve_launcher_audio_policy(
        g_audio_settings.native_mp2k,
        g_audio_settings.turbo_decoupled,
        g_strict_static_route,
        false /* MuteDuringTurbo is runtime-owned and has precedence there. */);
    const bool native_mp2k = audio_policy.native_mp2k;
    const bool turbo_decoupled = audio_policy.turbo_decoupled;
    child_environment.set(L"GBARECOMP_AUDIO_NATIVE",
                          native_mp2k ? L"1" : L"0");
    child_environment.set(L"GBARECOMP_TURBO_AUDIO",
                          turbo_decoupled ? L"decoupled" : L"0");
    // Keep the canonical GBA left/right Direct Sound buses instead of the
    // legacy faithful mono host route. This does not enable native MP2K.
    child_environment.set(L"GBARECOMP_AUDIO_STEREO", L"1");
    child_environment.set(L"GBARECOMP_HEAL_CACHE",
                          (root / L"recomp_cache").wstring());
    // Release: point the game's self-heal at the bundled g++ (absent on dev).
    if (std::filesystem::exists(root / L"builder" / L"mingw64" / L"bin" /
                                L"g++.exe")) {
        child_environment.set(L"GBARECOMP_HEAL_TOOLCHAIN",
                              (root / L"builder" / L"mingw64").wstring());
    }

    // A function-tracer launch must carry its exact user input sequence so
    // the resulting trace can be replayed. Replay and recording are mutually
    // exclusive in the runner; reject an explicit conflict before spawning
    // rather than allowing a partial, misleading session.
    const std::wstring explicit_input_record =
        inherited_environment_value(L"GBARECOMP_INPUT_RECORD");
    const std::wstring input_replay =
        inherited_environment_value(L"GBARECOMP_INPUT_REPLAY");
    std::wstring automatic_input_path;
    if (logging && input_replay.empty() && explicit_input_record.empty() &&
        (g_test_function_tracer || g_test_text_record)) {
        automatic_input_path = gsr::choose_unique_input_record_path(
            log_path, [](const std::wstring& candidate) {
                return fs::exists(fs::path(candidate));
            });
    }
    const auto input_record_policy =
        gsr::resolve_launcher_input_record_policy(
        (g_test_function_tracer || g_test_text_record), explicit_input_record,
            !input_replay.empty(), automatic_input_path);
    if (input_record_policy.replay_conflict) {
        if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        MessageBoxW(window,
                    L"GBARECOMP_INPUT_RECORD cannot be used together with "
                    L"GBARECOMP_INPUT_REPLAY.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }
    if ((g_test_function_tracer || g_test_text_record) &&
        input_replay.empty() &&
        explicit_input_record.empty() && !input_record_policy.enabled) {
        if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        MessageBoxW(window,
                    L"Function tracer could not create its input recording "
                    L"path.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (input_record_policy.enabled) {
        const fs::path input_path(input_record_policy.path);
        std::error_code input_ec;
        if (!input_path.parent_path().empty())
            fs::create_directories(input_path.parent_path(), input_ec);
        if (input_ec || fs::exists(input_path) ||
            !create_empty_file_exclusive(input_path)) {
            if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
            MessageBoxW(window,
                        L"The input recording path could not be created "
                        L"without overwriting an existing file.",
                        L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
            return 1;
        }
        child_environment.set(L"GBARECOMP_INPUT_RECORD",
                              input_record_policy.path);
    }
    if (logging) {
        const std::string state =
            std::string("[launcher] input_record=") +
            (input_record_policy.enabled ? "ENABLED path=\"" +
                 wide_to_utf8(input_record_policy.path) + "\"\n"
                                         : "DISABLED\n");
        DWORD written = 0;
        WriteFile(log_file, state.data(), static_cast<DWORD>(state.size()),
                  &written, nullptr);
        FlushFileBuffers(log_file);
    }

    // These are diagnostics, never part of the faithful default. Explicitly
    // remove inherited values when the master checkbox is off so a
    // shell-launched value cannot silently turn this into a crutch run;
    // self-healing is the one intentional exception.
    struct TestEnvironmentVariable {
        const wchar_t* name;
        bool enabled;
    };
    const TestEnvironmentVariable test_environment_variables[] = {
        {L"GBARECOMP_SELFHEAL_RAM", g_test_selfheal_ram},
        {L"GBARECOMP_COST_PROBE", g_test_cost_probe},
        {L"GBARECOMP_HOST_PROF", g_test_host_prof},
        {L"GBARECOMP_PRESENT_CADENCE", g_test_present_cadence},
        {L"GSR_RAM_CHURN_PROBE", g_test_ram_churn_probe},
        // Rides along with the sprite recorder: it adds the bounded
        // OAM-shadow write printout. The runner's placement observer is armed
        // independently for recording and Enhanced Options.
        {L"GSR_OAM_SHADOW_TRACE", g_test_obj_record},
        {L"GSR_MAP_RECORD", g_test_map_record},
        {L"GSR_OBJ_RECORD", g_test_obj_record},
        {L"GSR_TEXT_RECORD", g_test_text_record},
        {L"GBARECOMP_HEADROOM_PROBE", g_test_headroom_probe},
        {L"GBARECOMP_FN_TRACER",
         g_test_function_tracer || g_test_text_record},
        {L"GBARECOMP_VRAM_MAP_TRACE", g_test_vram_trace},
        {L"GSR_EFFECT_TRACE", g_test_effect_trace},
        {L"GSR_FRAME_REWIND", g_test_frame_rewind},
        {L"GSR_AUTO_CAPTURE", g_test_auto_capture},
        {L"GSR_BATTLE_BG1_RECORD", g_test_battle_bg1_record},
        {L"GSR_ROOM_BUFFER", g_test_room_buffer},
        // First piece of the mod loader: swaps one item icon and one Psynergy
        // animation in the loaded ROM data (src/mod_loader.cpp).
        {L"GSR_MOD_FIELD_TEST", g_test_mod_field_test},
    };
    // Record what actually reached the child. Three capture sessions in a row
    // came back missing a diagnostic with no way to tell whether the checkbox
    // never set the variable or the game never acted on it; this line settles
    // that from the session log alone.
    std::string enabled_list;
    for (const TestEnvironmentVariable& variable : test_environment_variables) {
        const bool self_heal =
            std::wcscmp(variable.name, L"GBARECOMP_SELFHEAL_RAM") == 0;
        if (gsr::launcher_test_variable_enabled(g_test_variables, self_heal,
                                                variable.enabled)) {
            if (!enabled_list.empty()) enabled_list += ",";
            enabled_list += wide_to_utf8(variable.name);
            // Everything here is a plain on/off flag except the host
            // profiler, which names the file it writes its samples to. Put it
            // beside this session's log so the two are matched by name, the
            // way the recorder directories already are.
            if (std::wcscmp(variable.name, L"GBARECOMP_HOST_PROF") == 0 &&
                !log_path.empty()) {
                std::wstring prof_path = log_path;
                const std::size_t dot = prof_path.rfind(L".log");
                if (dot != std::wstring::npos) prof_path.resize(dot);
                prof_path += L".hostprof.txt";
                child_environment.set(variable.name, prof_path);
            } else {
                child_environment.set(variable.name, L"1");
            }
        } else {
            child_environment.unset(variable.name);
        }
    }
    // Players: F12 always saves the last 2 seconds for a bug report.
    if (kReleaseLauncher) {
        child_environment.set(L"GSR_FRAME_REWIND", L"1");
        if (!enabled_list.empty()) enabled_list += ",";
        enabled_list += "GSR_FRAME_REWIND";
    }
    if (logging) {
        const std::string line =
            std::string("[launcher] test_variables=") +
            (g_test_variables ? "ON" : "OFF") + " set=" +
            (enabled_list.empty() ? "(none)" : enabled_list) + "\n";
        DWORD written = 0;
        WriteFile(log_file, line.data(), static_cast<DWORD>(line.size()),
                  &written, nullptr);
        FlushFileBuffers(log_file);
    }

    // Enhanced Options combines the existing expanded-sprite/shadow safety
    // net with room-buffer rendering and expanded spell effects. The self-check stays an
    // independent Test variables control.
    child_environment.set(L"GBARECOMP_EXPERIMENTAL_FIXES",
                          g_audio_settings.enhanced_options ? L"1" : L"0");
    // Rendering is owned by Enhanced Options; clear any inherited value when
    // it is off so a normal launch cannot silently enable it.
    child_environment.unset(L"GSR_ROOM_BUFFER_RENDER");
    child_environment.unset(L"GSR_HOST_EFFECTS");
    if (g_audio_settings.enhanced_options) {
        child_environment.set(L"GSR_ROOM_BUFFER_RENDER", L"1");
        child_environment.set(L"GSR_HOST_EFFECTS", L"1");
    }

    // Drawing the field with the graphics card is a persistent player
    // setting like Enhanced Options, not a session diagnostic. It replaces the
    // per-pixel console compositor for field frames only and falls back to
    // it for any frame it cannot reproduce, so a launch with the box
    // unticked behaves exactly as before.
    child_environment.set(L"GSR_GPU_FIELD",
                          g_audio_settings.gpu_field ? L"1" : L"0");

    // "Draw ONLY with the graphics card" turns off the console compositor
    // completely: it is not run, and a frame the card refuses is painted a
    // flat colour instead of quietly falling back. That is the point -- with
    // the fallback in place a refused frame is invisible, and the refusals
    // are exactly what still needs finding. Only meaningful with the box
    // above ticked, which load_launcher_settings already enforces.
    child_environment.set(L"GSR_GPU_FIELD_ONLY",
                          (g_audio_settings.gpu_field &&
                           g_audio_settings.gpu_field_only) ? L"1" : L"0");

    // The BIOS SWI log is a session-only diagnostic. Remove inherited values
    // first, then add the launcher-owned path only when its checkbox is
    // enabled.
    child_environment.unset(L"GBARECOMP_SWI_LOG");
    child_environment.unset(L"GBARECOMP_BIOS_PC_LOG");
    child_environment.unset(L"GBARECOMP_BIOS_READ_LOG");
    const bool swi_log_enabled = gsr::launcher_test_variable_enabled(
        g_test_variables, false, g_test_swi_log);
    if (swi_log_enabled) {
        const fs::path bios_inventory_dir =
            root / L"logs" / L"bios_inventory";
        std::error_code bios_inventory_ec;
        fs::create_directories(bios_inventory_dir, bios_inventory_ec);
        if (!bios_inventory_ec) {
            child_environment.set(
                L"GBARECOMP_SWI_LOG",
                (bios_inventory_dir / L"swi.csv").wstring());
        }
    }

    if (logging && !kReleaseLauncher) {
        // Respect an explicit developer override inherited by the launcher.
        fs::path events_path = log_path;
        fs::path phase_path = log_path;
        events_path.replace_extension(L".events.csv");
        phase_path.replace_extension(L".phase.csv");
        if (!child_environment.contains(L"GBARECOMP_FRAME_EVENTS"))
            child_environment.set(L"GBARECOMP_FRAME_EVENTS",
                                  events_path.wstring());
        if (!child_environment.contains(L"GBARECOMP_FRAME_PHASE"))
            child_environment.set(L"GBARECOMP_FRAME_PHASE",
                                  phase_path.wstring());
    }

    std::wstring command = L"\"" + game.wstring() + L"\"";
    command += L" --rom \"" + rom + L"\"";
    // Developer-only replay seam. The normal launcher command is unchanged
    // when these inherited variables are absent. Input replay is already
    // inherited by ChildEnvironment; these controls only bridge the state
    // path and an optional windowed frame budget to the runner CLI.
    gsr::append_developer_replay_arguments(
        command, inherited_environment_value(L"GBARECOMP_LOAD_STATE"),
        inherited_environment_value(L"GBARECOMP_REPLAY_FRAMES"));
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    HANDLE out_read = nullptr, out_write = nullptr;
    HANDLE err_read = nullptr, err_write = nullptr;
    BOOL inherit_handles = FALSE;
    if (logging) {
        SECURITY_ATTRIBUTES pipe_sa{};
        pipe_sa.nLength = sizeof(pipe_sa);
        pipe_sa.bInheritHandle = TRUE;
        if (CreatePipe(&out_read, &out_write, &pipe_sa, 0) &&
            CreatePipe(&err_read, &err_write, &pipe_sa, 0)) {
            // Only the write ends should be inherited by the child; the
            // parent's read ends must stay private or the child's own copy
            // (inherited from CreateProcessW's snapshot) would keep the
            // pipe open even after the real write end closes, so the
            // reader thread would never see EOF.
            SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdOutput = out_write;
            startup.hStdError = err_write;
            startup.hStdInput = nullptr;
            inherit_handles = TRUE;
        } else {
            if (out_read) CloseHandle(out_read);
            if (out_write) CloseHandle(out_write);
            if (err_read) CloseHandle(err_read);
            if (err_write) CloseHandle(err_write);
            out_read = out_write = err_read = err_write = nullptr;
        }
    }

    const std::set<std::wstring> rewinds_before = rewind_dirs(logs_dir);
    // A crash file left by an earlier run must not end up in this run's report.
    {
        std::error_code stale_ec;
        fs::remove(game.parent_path() / L"crash_memory.bin", stale_ec);
        fs::remove(game.parent_path() / L"crash_trail.csv", stale_ec);
    }
    const auto launch_time = fs::file_time_type::clock::now();

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> environment_block = child_environment.block();
    // The game is a console-subsystem exe, so Windows would give it a console
    // window. A player release hides it; its output still reaches the
    // session log through the pipes above.
    const DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT |
        (kReleaseLauncher ? CREATE_NO_WINDOW : 0);
    if (!CreateProcessW(game.c_str(), mutable_command.data(), nullptr, nullptr,
                        inherit_handles, creation_flags,
                        environment_block.data(), root.c_str(), &startup,
                        &process)) {
        const DWORD start_error = GetLastError();
        if (out_read) CloseHandle(out_read);
        if (out_write) CloseHandle(out_write);
        if (err_read) CloseHandle(err_read);
        if (err_write) CloseHandle(err_write);
        if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        launcher_log(L"Start game: Windows could not start " + game.wstring() +
                     L" (error " + std::to_wstring(start_error) + L").");
        MessageBoxW(window, L"Windows could not start the recompiled game.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }
    CloseHandle(process.hThread);
    launcher_log(L"Start game: running " + game.wstring() + L", session log " +
                 (log_path.empty() ? std::wstring(L"(none)")
                                   : fs::path(log_path).filename().wstring()) + L".");

    // The splash window has nothing left to do once the game has actually
    // started; destroying it now (rather than after the wait below) avoids
    // Windows flagging it "Not Responding" for the whole play session,
    // since capturing the log means this call now blocks until the game
    // exits instead of returning immediately.
    if (window) DestroyWindow(window);

    if (out_write && err_write) {
        // The parent's copies of the write ends MUST close before the
        // reader threads are started: as long as ANY write-end handle is
        // open (ours or the child's inherited copy), ReadFile on the read
        // end blocks instead of returning EOF, even after the child exits.
        CloseHandle(out_write);
        CloseHandle(err_write);
        SessionLogWriter writer(log_file);
        std::thread out_thread(pump_pipe_to_log, out_read, "[OUT] ", &writer);
        std::thread err_thread(pump_pipe_to_log, err_read, "[ERR] ", &writer);
        // Joining blocks until both pipes hit EOF, which only happens once
        // the child has exited (cleanly, crashed, or been killed) and
        // released its inherited handles — so this doubles as the wait for
        // the child, with everything it ever wrote already durably logged
        // by the time these return.
        out_thread.join();
        err_thread.join();
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    CloseHandle(process.hProcess);

    if (kReleaseLauncher) {
        std::vector<std::wstring> new_rewinds;
        for (const std::wstring& dir : rewind_dirs(logs_dir))
            if (!rewinds_before.count(dir)) new_rewinds.push_back(dir);
        std::error_code time_ec;
        const fs::path crash_report = game.parent_path() / L"crash_report.txt";
        const bool crashed = fs::is_regular_file(crash_report, time_ec) &&
            fs::last_write_time(crash_report, time_ec) >= launch_time;
        launcher_log(L"Game ended: exit code " + std::to_wstring(exit_code) +
                     (crashed ? L", crashed (crash_report.txt written)" : L"") +
                     L", rewind captures " + std::to_wstring(new_rewinds.size()) + L".");
        if (!new_rewinds.empty() || crashed) {
            offer_bug_report(make_bug_report(root, game.parent_path(), rom,
                                             log_path, new_rewinds, crashed,
                                             exit_code),
                             crashed);
        }
    }
    return 0;
}

constexpr int kPickRomButton = 1001;
constexpr int kQuitButton = 1002;
constexpr int kTestVariablesButton = 1003;
constexpr int kSelfHealRamButton = 1004;
constexpr int kCostProbeButton = 1005;
// Next free id; nothing already in use is renumbered.
constexpr int kHostProfButton = 1040;
constexpr int kPresentCadenceButton = 1006;
constexpr int kRamChurnProbeButton = 1009;
constexpr int kNativeMp2kButton = 1010;
constexpr int kTurboAudioButton = 1011;
constexpr int kAudioHelpText = 1012;
constexpr int kCopySessionIdButton = 1013;
constexpr int kMapRecordButton = 1019;
constexpr int kFunctionTracerButton = 1020;
constexpr int kVramMapTraceButton = 1021;
constexpr int kRoomBufferButton = 1022;
constexpr int kObjRecordButton = 1024;
constexpr int kSwiLogButton = 1025;
constexpr int kEnhancedOptionsButton = 1027;
constexpr int kTextRecordButton = 1028;
constexpr int kHeadroomProbeButton = 1029;
// Next free id; nothing already in use is renumbered.
constexpr int kGpuFieldButton = 1041;
constexpr int kGpuFieldOnlyButton = 1042;
constexpr int kEffectTraceButton = 1043;
constexpr int kFrameRewindButton = 1044;
constexpr int kBattleBg1RecordButton = 1045;
constexpr int kModFieldTestButton = 1050;
constexpr int kAutoCaptureButton = 1051;
constexpr int kAutoStartButton = 1052;

fs::path g_launcher_root;

// ── Start automatically (player launcher) ──────────────────────────────
// With the box ticked, a launcher whose game is built and whose remembered
// ROM is still good starts the game by itself as soon as the update check
// has answered (at most kAutoStartMaxWaitMs). When an update is offered,
// "No" starts the game and "Yes" does not. A needed build never starts by
// itself (begin_auto_start requires a built game). Unticking the box while
// it waits stops it. The player is asked once, after the first build. Kept
// in local/launcher-autostart.txt.
constexpr UINT_PTR kAutoStartTimer = 2;
constexpr UINT kAutoStartPollMs = 100;
constexpr ULONGLONG kAutoStartMaxWaitMs = 10000;  // for the update check
bool g_auto_start = false;
ULONGLONG g_auto_start_began = 0;      // GetTickCount64 when waiting began
std::atomic<bool> g_update_check_done{false};

fs::path auto_start_path(const fs::path& root) {
    return root / L"local" / L"launcher-autostart.txt";
}

// False until the player has answered the question or used the box.
bool auto_start_chosen(const fs::path& root) {
    std::error_code ec;
    return fs::exists(auto_start_path(root), ec);
}

bool load_auto_start(const fs::path& root) {
    return read_text(auto_start_path(root)).rfind("1", 0) == 0;
}

void save_auto_start(const fs::path& root, bool on) {
    std::error_code ec;
    fs::create_directories(auto_start_path(root).parent_path(), ec);
    std::ofstream file(auto_start_path(root), std::ios::binary | std::ios::trunc);
    if (file) file << (on ? "1" : "0") << '\n';
}

// The box is shown only when the game is built and no build is running:
// otherwise the build text sits where it would be.
void update_auto_start_box(HWND window) {
    HWND box = GetDlgItem(window, kAutoStartButton);
    if (!box) return;
    bool show = false;
    {
        std::lock_guard<std::mutex> lock(g_build.mutex);
        show = g_build.ready && !g_build.running;
    }
    ShowWindow(box, show ? SW_SHOW : SW_HIDE);
}

void stop_auto_start(HWND window, const wchar_t* why) {
    if (!KillTimer(window, kAutoStartTimer)) return;
    launcher_log(std::wstring(L"Start automatically: stopped (") + why + L").");
}

// Presses Play the way a click does.
void auto_start_now(HWND window) {
    launcher_log(L"Start automatically: starting the game.");
    PostMessageW(window, WM_COMMAND, MAKEWPARAM(kPickRomButton, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, kPickRomButton)));
}

void begin_auto_start(HWND window) {
    if (!kReleaseLauncher || !g_auto_start || !g_build.ready) return;
    if (remembered_rom(g_launcher_root).empty()) {
        launcher_log(L"Start automatically: no remembered ROM, waiting for Play.");
        return;
    }
    g_auto_start_began = GetTickCount64();
    launcher_log(L"Start automatically: waiting for the update check.");
    SetTimer(window, kAutoStartTimer, kAutoStartPollMs, nullptr);
}

// Relabels the buttons of the question below while it opens.
HHOOK g_ask_hook = nullptr;
LRESULT CALLBACK ask_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HCBT_ACTIVATE) {
        HWND dialog = reinterpret_cast<HWND>(wparam);
        SetDlgItemTextW(dialog, IDYES, L"Start now");
        SetDlgItemTextW(dialog, IDNO, L"What's new");
        UnhookWindowsHookEx(g_ask_hook);
        g_ask_hook = nullptr;
        return 0;
    }
    return CallNextHookEx(g_ask_hook, code, wparam, lparam);
}

// After a build: IDYES = start the game now, IDNO = stay in the launcher.
int ask_start_or_notes(HWND window) {
    g_ask_hook = SetWindowsHookExW(WH_CBT, ask_hook_proc, nullptr, GetCurrentThreadId());
    const int answer = MessageBoxW(
        window,
        L"The game is ready.\n\nStart it now, or read what's new in this version first? "
        L"The Play button starts it whenever you are ready.",
        L"Golden Sun Recompiled", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND);
    if (g_ask_hook) {
        UnhookWindowsHookEx(g_ask_hook);
        g_ask_hook = nullptr;
    }
    return answer;
}
std::unique_ptr<Gdiplus::Image> g_splash_image;
HFONT g_button_font = nullptr;
HFONT g_body_font = nullptr;
// Backdrop behind the checkbox groups, recomputed each time layout_buttons
// runs. Kept as client-relative window coordinates so paint_splash can draw
// it without recomputing the layout itself.
RECT g_panel_rect{};
// Release launcher "What's new" box: painted by draw_release_notes, not a
// control, so the logo shows through it. UI thread only.
RECT g_notes_rect{};
std::wstring g_notes_text;
float g_notes_scroll = 0.0f;
float g_notes_max_scroll = 0.0f;
float g_notes_line_height = 18.0f;
float g_notes_text_height = 0.0f;
int g_notes_measured_width = -1;

// Every control below is positioned by a single running cursor rather than
// by hand-tuned offsets between controls, so adding, removing, or hiding a
// checkbox only ever changes how far the cursor advances -- two controls
// can no longer end up sharing the same row by accident.
void layout_buttons(HWND window) {
    RECT client{};
    GetClientRect(window, &client);

    // ---- Bottom action buttons (Pick ROM / Quit) --------------------------
    constexpr int button_width = 190;
    constexpr int button_height = 48;
    constexpr int button_gap = 18;
    constexpr int total_button_width = button_width * 2 + button_gap;
    const int button_x = std::max<int>(
        0, (client.right - total_button_width) / 2);
    const int button_y = std::max<int>(0, client.bottom - button_height - 28);
    HWND pick = GetDlgItem(window, kPickRomButton);
    HWND quit = GetDlgItem(window, kQuitButton);
    if (pick) MoveWindow(pick, button_x, button_y, button_width, button_height, TRUE);
    if (quit) MoveWindow(quit, button_x + button_width + button_gap, button_y,
                         button_width, button_height, TRUE);
    if (kReleaseLauncher) {
        // Only the two buttons and the Start automatically box exist: no
        // settings panel behind anything. The box sits in the build text's
        // line, which is empty whenever the box is shown.
        if (HWND box = GetDlgItem(window, kAutoStartButton)) {
            constexpr int box_width = 330;
            MoveWindow(box, std::max<int>(0, (client.right - box_width) / 2),
                       std::max<int>(0, button_y - 44), box_width, 30, TRUE);
            update_auto_start_box(window);
        }
        // The release notes sit over the logo: the window's side margin
        // wide, from y 28 down to the bottom band (160 px).
        g_notes_rect = {40, 28, 40 + std::max<int>(0, client.right - 80),
                        28 + std::max<int>(0, client.bottom - 160 - 8 - 28)};
        g_notes_measured_width = -1;
        g_panel_rect = {};
        InvalidateRect(window, nullptr, FALSE);
        return;
    }

    // ---- Checkbox panel -----------------------------------------------
    constexpr int kPanelPaddingX = 24;
    constexpr int kPanelPaddingY = 20;
    constexpr int kRowHeight = 26;
    constexpr int kRowGap = 6;
    constexpr int kGroupGap = 18;
    constexpr int kIndent = 28;
    constexpr int kContentWidth = 480;
    constexpr int kPanelBottomMargin = 20;

    const int panel_width = kContentWidth + kPanelPaddingX * 2;
    const int panel_x = std::max<int>(0, (client.right - panel_width) / 2);
    const int content_x = panel_x + kPanelPaddingX;

    HWND native_mp2k = GetDlgItem(window, kNativeMp2kButton);
    HWND turbo_audio = GetDlgItem(window, kTurboAudioButton);
    HWND audio_help = GetDlgItem(window, kAudioHelpText);
    HWND copy_session_id = GetDlgItem(window, kCopySessionIdButton);
    HWND enhanced_options = GetDlgItem(window, kEnhancedOptionsButton);
    HWND gpu_field = GetDlgItem(window, kGpuFieldButton);
    HWND gpu_field_only = GetDlgItem(window, kGpuFieldOnlyButton);
    HWND test_variables = GetDlgItem(window, kTestVariablesButton);

    // The help text wraps to as many lines as its content needs at the
    // panel's content width, instead of a fixed height that can clip it
    // mid-sentence.
    int help_height = kRowHeight;
    if (audio_help && g_body_font) {
        HDC dc = GetDC(window);
        HFONT old_font = static_cast<HFONT>(SelectObject(dc, g_body_font));
        wchar_t text[512] = {};
        GetWindowTextW(audio_help, text, static_cast<int>(std::size(text)));
        RECT calc{0, 0, kContentWidth, 0};
        DrawTextW(dc, text, -1, &calc, DT_LEFT | DT_WORDBREAK | DT_CALCRECT);
        SelectObject(dc, old_font);
        ReleaseDC(window, dc);
        help_height = std::max<int>(kRowHeight, calc.bottom - calc.top);
    }

    int cursor = kPanelPaddingY;  // offset from the panel's top

    // Audio group.
    const int help_y = cursor;
    cursor += help_height + kRowGap;
    const int native_mp2k_y = cursor;
    cursor += kRowHeight + kRowGap;
    const int turbo_audio_y = cursor;
    cursor += kRowHeight + kRowGap;
    const int copy_session_id_y = cursor;
    cursor += kRowHeight + kGroupGap;

    // Enhanced Options is a normal launcher setting, separate from the
    // session-only diagnostic controls below.
    const int enhanced_options_y = cursor;
    cursor += kRowHeight + kRowGap;
    const int gpu_field_y = cursor;
    cursor += kRowHeight + kRowGap;
    const int gpu_field_only_y = cursor;
    cursor += kRowHeight + kGroupGap;

    // Test variables group. The children stack below the master checkbox,
    // full content width, so a long label (e.g. "Log off-screen sprite
    // positions") never gets clipped; adding another entry to `children[]`
    // just adds another row and the panel grows to match.
    const int test_variables_y = cursor;
    cursor += kRowHeight;
    const int children_top = cursor + kRowGap;

    // Row order for the diagnostic sub-toggles. Rows are packed by how many
    // controls EXIST, not by position in this array. Indexing by array
    // position instead left a blank row for every uncreated id, which pushed
    // the last checkbox off the bottom of the panel and under the action
    // buttons, where it could not be clicked at all. That is how the CPU
    // headroom capture came back empty from three sessions in a row.
    // Grouped by what they are FOR, and laid out in two columns below, so the
    // list reads as short groups instead of one long column. The order here
    // is the left column top to bottom, then the right.
    const int child_ids[] = {
        // left: capture something to a file
        kMapRecordButton,      kObjRecordButton,      kTextRecordButton,
        kSwiLogButton,
        // left, continued: traces
        kFunctionTracerButton, kVramMapTraceButton,   kEffectTraceButton,
        kBattleBg1RecordButton,
        // right: rendering
        kRoomBufferButton,     kFrameRewindButton,     kAutoCaptureButton,
        kModFieldTestButton,
        // right, continued: performance
        kHeadroomProbeButton,  kCostProbeButton,      kHostProfButton,
        kPresentCadenceButton, kRamChurnProbeButton,
        // right, last: the one safety toggle, kept apart from the probes
        kSelfHealRamButton,
    };
    const int child_count = static_cast<int>(std::size(child_ids));
    int visible_children = 0;
    for (int i = 0; i < child_count; ++i) {
        if (GetDlgItem(window, child_ids[i])) ++visible_children;
    }

    // Two columns. The sub-toggles in one column stood so tall they pushed
    // the panel past everything else; in two it is half that and the groups
    // above stay together, because the fill is column-major.
    constexpr int kChildColumns = 2;
    constexpr int kColumnGap = 12;
    const int child_rows =
        (visible_children + kChildColumns - 1) / kChildColumns;
    const int child_width =
        (kContentWidth - kIndent - kColumnGap * (kChildColumns - 1)) /
        kChildColumns;
    if (g_test_variables && child_rows > 0) {
        cursor = children_top + child_rows * kRowHeight +
                 (child_rows - 1) * kRowGap;
    }
    cursor += kPanelPaddingY;

    const int panel_height = cursor;
    const int panel_top = std::max<int>(
        0, button_y - kPanelBottomMargin - panel_height);
    g_panel_rect = {panel_x, panel_top, panel_x + panel_width,
                    panel_top + panel_height};

    if (audio_help) {
        MoveWindow(audio_help, content_x, panel_top + help_y, kContentWidth,
                   help_height, TRUE);
    }
    if (native_mp2k) {
        MoveWindow(native_mp2k, content_x, panel_top + native_mp2k_y,
                   kContentWidth, kRowHeight, TRUE);
        EnableWindow(native_mp2k, !g_strict_static_route);
    }
    if (turbo_audio) {
        // Sub-toggle of "Native MP2K audio": indented under its master and
        // greyed out (not hidden) while that master is unchecked, same as
        // before.
        MoveWindow(turbo_audio, content_x + kIndent,
                   panel_top + turbo_audio_y, kContentWidth - kIndent,
                   kRowHeight, TRUE);
        EnableWindow(turbo_audio, g_audio_settings.native_mp2k &&
                                       !g_strict_static_route);
    }
    if (copy_session_id) {
        MoveWindow(copy_session_id, content_x, panel_top + copy_session_id_y,
                   kContentWidth, kRowHeight, TRUE);
    }
    if (enhanced_options) {
        MoveWindow(enhanced_options, content_x,
                   panel_top + enhanced_options_y, kContentWidth, kRowHeight,
                   TRUE);
    }
    if (gpu_field) {
        MoveWindow(gpu_field, content_x, panel_top + gpu_field_y,
                   kContentWidth, kRowHeight, TRUE);
    }
    if (gpu_field_only) {
        // Sub-toggle of "Draw the field with the graphics card": indented
        // under its master and greyed out (not hidden) while that master is
        // unchecked, same as the Turbo sub-toggle above.
        MoveWindow(gpu_field_only, content_x + kIndent,
                   panel_top + gpu_field_only_y,
                   kContentWidth - kIndent, kRowHeight, TRUE);
        EnableWindow(gpu_field_only, g_audio_settings.gpu_field);
    }
    if (test_variables) {
        MoveWindow(test_variables, content_x, panel_top + test_variables_y,
                   kContentWidth, kRowHeight, TRUE);
    }

    int placed = 0;
    for (int i = 0; i < child_count; ++i) {
        HWND child = GetDlgItem(window, child_ids[i]);
        if (!child) continue;
        // Sub-toggles of "Test variables": indented under the master, hidden
        // while it is unchecked. Filled COLUMN-major -- down the left, then
        // down the right -- so the groups in child_ids stay together instead
        // of being interleaved across the two columns. `placed` advances only
        // for controls that exist, so an uncreated id leaves no gap.
        const int column = child_rows > 0 ? placed / child_rows : 0;
        const int row = child_rows > 0 ? placed % child_rows : 0;
        ++placed;
        const int child_y =
            panel_top + children_top + row * (kRowHeight + kRowGap);
        const int child_x =
            content_x + kIndent + column * (child_width + kColumnGap);
        MoveWindow(child, child_x, child_y, child_width, kRowHeight, TRUE);
        ShowWindow(child, g_test_variables ? SW_SHOW : SW_HIDE);
    }

    // The panel's extent just changed; repaint the backdrop under it.
    InvalidateRect(window, nullptr, FALSE);
}

// The backdrop: a dark indigo gradient, a soft purple glow and the logo
// (assets/launcher_logo.png, built in), fitted into the space above the
// settings panel, or above the buttons in the player launcher. One bitmap,
// redrawn only when the window or the panel changes.
struct Backdrop {
    int width = 0, height = 0, logo_bottom = 0;
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
};
Backdrop g_backdrop;

Gdiplus::Bitmap* launcher_backdrop(int width, int height, int logo_bottom) {
    Backdrop& b = g_backdrop;
    if (b.bitmap && b.width == width && b.height == height &&
        b.logo_bottom == logo_bottom)
        return b.bitmap.get();
    b.bitmap = std::make_unique<Gdiplus::Bitmap>(width, height,
                                                 PixelFormat32bppPARGB);
    b.width = width;
    b.height = height;
    b.logo_bottom = logo_bottom;
    Gdiplus::Graphics g(b.bitmap.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    Gdiplus::LinearGradientBrush sky(Gdiplus::Point(0, 0),
                                     Gdiplus::Point(0, height),
                                     Gdiplus::Color(255, 32, 21, 56),
                                     Gdiplus::Color(255, 8, 7, 14));
    g.FillRectangle(&sky, 0, 0, width, height);
    if (g_splash_image && g_splash_image->GetLastStatus() == Gdiplus::Ok) {
        const int top = 28;
        const int room_h = logo_bottom - top;
        const int room_w = width - 80;
        if (room_h > 60 && room_w > 60) {
            const float iw = static_cast<float>(g_splash_image->GetWidth());
            const float ih = static_cast<float>(g_splash_image->GetHeight());
            const float scale = std::min({room_w / iw, room_h / ih, 1.0f});
            const int dw = static_cast<int>(iw * scale);
            const int dh = static_cast<int>(ih * scale);
            const int dx = (width - dw) / 2;
            const int dy = top + (room_h - dh) / 2;
            Gdiplus::GraphicsPath glow;
            glow.AddEllipse(dx - dw * 0.18f, dy - dh * 0.25f, dw * 1.36f,
                            dh * 1.5f);
            Gdiplus::PathGradientBrush glow_brush(&glow);
            glow_brush.SetCenterColor(Gdiplus::Color(120, 130, 70, 210));
            Gdiplus::Color edge(0, 130, 70, 210);
            int edges = 1;
            glow_brush.SetSurroundColors(&edge, &edges);
            g.FillPath(&glow_brush, &glow);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(g_splash_image.get(), Gdiplus::Rect(dx, dy, dw, dh));
        }
    }
    return b.bitmap.get();
}

void rounded_rect_path(Gdiplus::GraphicsPath& path, float x, float y, float w,
                       float h, float r) {
    r = std::min({r, w / 2.0f, h / 2.0f});
    if (r <= 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    const float d = r * 2.0f;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// The area the progress animation repaints (WM_TIMER), set by the painter.
RECT g_progress_rect{};
constexpr UINT_PTR kProgressTimer = 1;

// The first-start build's progress: a glowing purple bar with a light
// sweeping along the filled part. Before the builder reports a total it is
// a glow sliding back and forth.
void paint_progress_bar(Gdiplus::Graphics& g, int width, int height, int done,
                        int total) {
    const float bar_w = static_cast<float>(std::min(width - 140, 480));
    const float bar_h = 16.0f;
    const float bar_x = (width - bar_w) / 2.0f;
    const float bar_y = static_cast<float>(height - 94);
    g_progress_rect = {static_cast<LONG>(bar_x) - 28, static_cast<LONG>(bar_y) - 28,
                       static_cast<LONG>(bar_x + bar_w) + 70,
                       static_cast<LONG>(bar_y + bar_h) + 28};
    const double t = static_cast<double>(GetTickCount64() % 1000000u) / 1000.0;
    const bool known = total > 0;
    const float frac = known
        ? std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0f, 1.0f)
        : 0.0f;
    float fill_w = known ? std::max(bar_h, bar_w * frac) : bar_w * 0.3f;
    float fill_x = bar_x;
    if (!known) {
        const double phase = 0.5 - 0.5 * std::cos(t * 1.6);
        fill_x = bar_x + static_cast<float>(phase) * (bar_w - fill_w);
    }
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

    // Glow: soft purple layers around the filled part, gently pulsing.
    const float pulse = 0.72f + 0.28f * static_cast<float>(std::sin(t * 3.0));
    for (int i = 8; i >= 1; --i) {
        const float grow = i * 2.4f;
        Gdiplus::GraphicsPath layer;
        rounded_rect_path(layer, fill_x - grow, bar_y - grow, fill_w + grow * 2,
                          bar_h + grow * 2, bar_h / 2 + grow);
        Gdiplus::SolidBrush brush(Gdiplus::Color(
            static_cast<BYTE>(pulse * 13.0f), 176, 96, 255));
        g.FillPath(&brush, &layer);
    }

    // Track.
    Gdiplus::GraphicsPath track;
    rounded_rect_path(track, bar_x, bar_y, bar_w, bar_h, bar_h / 2);
    Gdiplus::SolidBrush track_fill(Gdiplus::Color(235, 22, 13, 40));
    g.FillPath(&track_fill, &track);

    // Fill: violet to lilac, glossy top, a light sweeping along it.
    Gdiplus::GraphicsPath fill;
    rounded_rect_path(fill, fill_x, bar_y, fill_w, bar_h, bar_h / 2);
    Gdiplus::LinearGradientBrush fill_brush(
        Gdiplus::PointF(fill_x - 1, 0), Gdiplus::PointF(fill_x + fill_w + 1, 0),
        Gdiplus::Color(255, 106, 46, 214), Gdiplus::Color(255, 214, 156, 255));
    g.FillPath(&fill_brush, &fill);
    g.SetClip(&fill, Gdiplus::CombineModeIntersect);
    Gdiplus::SolidBrush gloss(Gdiplus::Color(55, 255, 255, 255));
    g.FillRectangle(&gloss, fill_x, bar_y, fill_w, bar_h * 0.45f);
    if (known) {
        const float band = 90.0f;
        // The light crosses the whole track at a steady pace and shows only
        // over the filled part, so the fill growing does not restart it (its
        // position used to be taken modulo the filled width).
        const float travel = bar_w + band * 2;
        const float sx = bar_x - band +
            static_cast<float>(std::fmod(t * 280.0, static_cast<double>(travel)));
        Gdiplus::LinearGradientBrush shine(
            Gdiplus::PointF(sx - 1, 0), Gdiplus::PointF(sx + band + 1, 0),
            Gdiplus::Color(0, 255, 255, 255), Gdiplus::Color(0, 255, 255, 255));
        shine.SetBlendTriangularShape(0.5f, 1.0f);
        Gdiplus::Color shine_colours[] = {Gdiplus::Color(0, 255, 255, 255),
                                          Gdiplus::Color(150, 255, 240, 255),
                                          Gdiplus::Color(0, 255, 255, 255)};
        Gdiplus::REAL shine_stops[] = {0.0f, 0.5f, 1.0f};
        shine.SetInterpolationColors(shine_colours, shine_stops, 3);
        g.FillRectangle(&shine, sx, bar_y, band, bar_h);
    }
    g.ResetClip();

    Gdiplus::Pen rim(Gdiplus::Color(170, 168, 120, 245), 1.0f);
    g.DrawPath(&rim, &track);

    if (known) {
        const std::wstring percent =
            std::to_wstring(static_cast<int>(frac * 100.0f + 0.5f)) + L"%";
        Gdiplus::Font font(L"Segoe UI", 10.0f, Gdiplus::FontStyleBold,
                           Gdiplus::UnitPoint);
        Gdiplus::SolidBrush ink(Gdiplus::Color(255, 226, 206, 255));
        g.DrawString(percent.c_str(), -1, &font,
                     Gdiplus::PointF(bar_x + bar_w + 12.0f, bar_y - 4.0f), &ink);
    }
}

// Paints the "What's new" box: a translucent dark rectangle over the logo with
// word-wrapped, scrollable text. Drawn on the caller's Graphics so it works
// for the double-buffered window paint and for child-control backdrops alike.
void draw_release_notes(Gdiplus::Graphics& graphics, HDC dc) {
    if (g_notes_text.empty() || g_notes_rect.right <= g_notes_rect.left ||
        g_notes_rect.bottom <= g_notes_rect.top)
        return;
    const int left = g_notes_rect.left, top = g_notes_rect.top;
    const int box_w = g_notes_rect.right - g_notes_rect.left;
    const int box_h = g_notes_rect.bottom - g_notes_rect.top;
    Gdiplus::SolidBrush fill(Gdiplus::Color(165, 10, 8, 18));
    graphics.FillRectangle(&fill, left, top, box_w, box_h);
    Gdiplus::Pen border(Gdiplus::Color(90, 255, 220, 150), 1.0f);
    graphics.DrawRectangle(&border, left, top, box_w - 1, box_h - 1);

    constexpr int kPad = 10;
    const Gdiplus::REAL inner_w = static_cast<Gdiplus::REAL>(box_w - kPad * 2);
    const Gdiplus::REAL inner_h = static_cast<Gdiplus::REAL>(box_h - kPad * 2);
    if (inner_w <= 8.0f || inner_h <= 8.0f) return;

    HGDIOBJ previous = g_body_font ? SelectObject(dc, g_body_font) : nullptr;
    Gdiplus::Font font(dc);
    if (previous) SelectObject(dc, previous);
    if (font.GetLastStatus() != Gdiplus::Ok) return;

    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    Gdiplus::StringFormat format;
    g_notes_line_height = std::max(1.0f, font.GetHeight(&graphics));
    if (g_notes_measured_width != static_cast<int>(inner_w)) {
        Gdiplus::RectF bounds;
        graphics.MeasureString(g_notes_text.c_str(), -1, &font,
                               Gdiplus::RectF(0, 0, inner_w, 100000.0f),
                               &format, &bounds);
        g_notes_text_height = bounds.Height;
        g_notes_measured_width = static_cast<int>(inner_w);
    }
    g_notes_max_scroll = std::max(0.0f, g_notes_text_height - inner_h);
    g_notes_scroll = std::min(std::max(g_notes_scroll, 0.0f), g_notes_max_scroll);

    const Gdiplus::RectF inner(static_cast<Gdiplus::REAL>(left + kPad),
                               static_cast<Gdiplus::REAL>(top + kPad), inner_w,
                               inner_h);
    const Gdiplus::GraphicsState saved = graphics.Save();
    graphics.SetClip(inner, Gdiplus::CombineModeIntersect);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 240, 232, 210));
    graphics.DrawString(g_notes_text.c_str(), -1, &font,
                        Gdiplus::RectF(inner.X, inner.Y - g_notes_scroll,
                                       inner_w, g_notes_text_height + 40.0f),
                        &format, &ink);
    graphics.Restore(saved);

    if (g_notes_max_scroll > 0.0f) {
        const Gdiplus::REAL track = inner_h;
        Gdiplus::REAL thumb = std::max(24.0f, track * inner_h / g_notes_text_height);
        thumb = std::min(thumb, track);
        const Gdiplus::REAL y = inner.Y + (track - thumb) *
                                (g_notes_scroll / g_notes_max_scroll);
        const Gdiplus::REAL x = static_cast<Gdiplus::REAL>(left + box_w - 4 - 5);
        Gdiplus::GraphicsPath path;
        path.AddArc(x, y, 4.0f, 4.0f, 180.0f, 180.0f);
        path.AddArc(x, y + thumb - 4.0f, 4.0f, 4.0f, 0.0f, 180.0f);
        path.CloseFigure();
        Gdiplus::SolidBrush thumb_brush(Gdiplus::Color(120, 255, 240, 210));
        graphics.FillPath(&thumb_brush, &path);
    }
}

// origin_x/origin_y let this same routine paint into a child control's DC:
// (0, 0) in that DC is (origin_x, origin_y) in the launcher window's own
// client coordinates, so passing the child's client-relative position here
// draws exactly the artwork/panel slice that sits behind that child --
// see checkbox_erase_background below.
void paint_splash(HWND window, HDC dc, int origin_x = 0, int origin_y = 0) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.TranslateTransform(static_cast<Gdiplus::REAL>(-origin_x),
                                static_cast<Gdiplus::REAL>(-origin_y));
    graphics.Clear(Gdiplus::Color(255, 13, 11, 20));

    if (width > 0 && height > 0) {
        const bool has_panel = g_panel_rect.bottom > g_panel_rect.top;
        const int logo_bottom = has_panel ? g_panel_rect.top - 12 : height - 172;
        if (Gdiplus::Bitmap* backdrop =
                launcher_backdrop(width, height, logo_bottom)) {
            graphics.SetInterpolationMode(
                Gdiplus::InterpolationModeNearestNeighbor);
            graphics.DrawImage(backdrop, Gdiplus::Rect(0, 0, width, height), 0,
                               0, width, height, Gdiplus::UnitPixel);
        }
    }

    // One coherent backdrop behind every checkbox group, instead of a
    // separate translucent strip per row: same alpha throughout, so the
    // groups read as one panel over the artwork rather than scattered
    // patches at inconsistent widths.
    if (g_panel_rect.right > g_panel_rect.left &&
        g_panel_rect.bottom > g_panel_rect.top) {
        const int panel_width = g_panel_rect.right - g_panel_rect.left;
        const int panel_height = g_panel_rect.bottom - g_panel_rect.top;
        Gdiplus::SolidBrush panel_fill(Gdiplus::Color(150, 12, 10, 8));
        graphics.FillRectangle(&panel_fill, g_panel_rect.left, g_panel_rect.top,
                               panel_width, panel_height);
        Gdiplus::Pen panel_border(Gdiplus::Color(90, 255, 220, 150), 1.0f);
        graphics.DrawRectangle(&panel_border, g_panel_rect.left,
                               g_panel_rect.top, panel_width - 1,
                               panel_height - 1);
    }

    if (kReleaseLauncher) draw_release_notes(graphics, dc);

    // The player launcher's band is taller: it holds the build note, the
    // step line and the bar above the buttons.
    const int band = kReleaseLauncher ? 160 : 132;
    Gdiplus::SolidBrush bottom_overlay(Gdiplus::Color(145, 0, 0, 0));
    graphics.FillRectangle(&bottom_overlay, 0, std::max(0, height - band),
                           width, band);

    if (kReleaseLauncher) {
        std::wstring text, note;
        int done = 0, total = 0;
        bool running = false;
        {
            std::lock_guard<std::mutex> lock(g_build.mutex);
            running = g_build.running;
            done = g_build.done;
            total = g_build.total;
            if (running) {
                // The builder's stages (tools/gsr_builder): checking the ROM
                // and translating are step 1, compiling and linking step 2.
                const std::wstring& stage = g_build.stage;
                const bool step_two =
                    stage.find(L"Compiling") != std::wstring::npos ||
                    stage.find(L"Linking") != std::wstring::npos;
                text = std::wstring(step_two ? L"Step 2 of 2: " : L"Step 1 of 2: ") +
                       stage;
                if (total > 0)
                    text += L"  (" + std::to_wstring(done) + L" of " +
                            std::to_wstring(total) + L")";
                note = L"Step 1 translates the game from your ROM, step 2 "
                       L"compiles it. This can take several minutes, and the "
                       L"game starts by itself when it's done.";
            } else if (!g_build.ready) {
                text = L"First start: pick your Golden Sun ROM. The game is "
                       L"prepared from it once, which takes a few minutes.";
                note = L"Step 1 translates the game from your ROM, step 2 "
                       L"compiles it. After an update this happens again.";
            }
        }
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        if (!note.empty()) {
            Gdiplus::Font small(L"Segoe UI", 9.0f, Gdiplus::FontStyleRegular,
                                Gdiplus::UnitPoint);
            Gdiplus::SolidBrush muted(Gdiplus::Color(255, 190, 176, 222));
            const Gdiplus::RectF box(40.0f, static_cast<Gdiplus::REAL>(height - 154),
                                     static_cast<Gdiplus::REAL>(width - 80), 34.0f);
            graphics.DrawString(note.c_str(), -1, &small, box, &format, &muted);
        }
        if (!text.empty()) {
            Gdiplus::Font font(L"Segoe UI", 10.5f, Gdiplus::FontStyleRegular,
                               Gdiplus::UnitPoint);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255, 240, 232, 210));
            const Gdiplus::RectF box(24.0f, static_cast<Gdiplus::REAL>(height - 120),
                                     static_cast<Gdiplus::REAL>(width - 48), 22.0f);
            graphics.DrawString(text.c_str(), -1, &font, box, &format, &ink);
        }
        if (running) paint_progress_bar(graphics, width, height, done, total);
    }
}

// Standard (non-owner-drawn) BS_AUTOCHECKBOX controls always erase their own
// background solid before drawing the checkbox glyph and label, with no
// message that lets the parent supply a custom (or transparent) fill for
// them -- that opaque system-colored box behind every checkbox is the
// per-row "strip" this launcher is being tidied up to avoid. Every checkbox
// below is therefore BS_OWNERDRAW instead, drawn by draw_checkbox_item, the
// same way the Pick ROM/Quit buttons already are; this table is how its
// WM_DRAWITEM handler finds which flag each checkbox's glyph reflects and
// toggles on click, keyed by control ID so growing the checkbox list is
// just one more case.
bool* checkbox_state_for_id(int id) {
    switch (id) {
    case kTestVariablesButton: return &g_test_variables;
    case kSelfHealRamButton: return &g_test_selfheal_ram;
    case kCostProbeButton: return &g_test_cost_probe;
    case kHostProfButton: return &g_test_host_prof;
    case kPresentCadenceButton: return &g_test_present_cadence;
    case kRamChurnProbeButton: return &g_test_ram_churn_probe;
    case kMapRecordButton: return &g_test_map_record;
    case kObjRecordButton: return &g_test_obj_record;
    case kFunctionTracerButton: return &g_test_function_tracer;
    case kVramMapTraceButton: return &g_test_vram_trace;
    case kEffectTraceButton: return &g_test_effect_trace;
    case kFrameRewindButton: return &g_test_frame_rewind;
    case kAutoCaptureButton: return &g_test_auto_capture;
    case kBattleBg1RecordButton: return &g_test_battle_bg1_record;
    case kModFieldTestButton: return &g_test_mod_field_test;
    case kRoomBufferButton: return &g_test_room_buffer;
    case kSwiLogButton: return &g_test_swi_log;
    case kTextRecordButton: return &g_test_text_record;
    case kHeadroomProbeButton: return &g_test_headroom_probe;
    case kEnhancedOptionsButton: return &g_audio_settings.enhanced_options;
    case kGpuFieldButton: return &g_audio_settings.gpu_field;
    case kGpuFieldOnlyButton: return &g_audio_settings.gpu_field_only;
    case kNativeMp2kButton: return &g_audio_settings.native_mp2k;
    case kTurboAudioButton: return &g_audio_settings.turbo_decoupled;
    case kCopySessionIdButton:
        return &g_audio_settings.copy_session_id_to_clipboard;
    case kAutoStartButton: return &g_auto_start;
    default: return nullptr;
    }
}

// Draws one owner-drawn checkbox: the exact artwork/panel slice behind it
// (via paint_splash's origin offset, so it reads as sitting on the panel
// rather than on its own opaque box), the check glyph, and its label.
void draw_checkbox_item(const DRAWITEMSTRUCT& item, bool checked) {
    HWND parent = GetParent(item.hwndItem);
    POINT origin{0, 0};
    RECT window_rect{};
    if (parent && GetWindowRect(item.hwndItem, &window_rect)) {
        origin = {window_rect.left, window_rect.top};
        ScreenToClient(parent, &origin);
    }
    paint_splash(parent, item.hDC, origin.x, origin.y);

    const bool enabled = !(item.itemState & ODS_DISABLED);
    constexpr int kBoxSize = 16;
    RECT box_rect = item.rcItem;
    box_rect.top += (item.rcItem.bottom - item.rcItem.top - kBoxSize) / 2;
    box_rect.bottom = box_rect.top + kBoxSize;
    box_rect.right = box_rect.left + kBoxSize;
    UINT frame_state = DFCS_BUTTONCHECK;
    if (checked) frame_state |= DFCS_CHECKED;
    if (!enabled) frame_state |= DFCS_INACTIVE;
    if (item.itemState & ODS_SELECTED) frame_state |= DFCS_PUSHED;
    DrawFrameControl(item.hDC, &box_rect, DFC_BUTTON, frame_state);

    wchar_t label[256] = {};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    RECT text_rect = item.rcItem;
    text_rect.left = box_rect.right + 8;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC,
                enabled ? RGB(240, 232, 210) : RGB(140, 132, 116));
    HFONT old_font = static_cast<HFONT>(SelectObject(item.hDC, g_button_font));
    DrawTextW(item.hDC, label, -1, &text_rect,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item.hDC, old_font);

    if (item.itemState & ODS_FOCUS) DrawFocusRect(item.hDC, &item.rcItem);
}

// ---- Pick ROM / Quit buttons ---------------------------------------------
// Owner-drawn, flat: slightly rounded, a solid gold Pick ROM and an outlined
// Quit, with hover and pressed states. The corners show the artwork behind
// them rather than a square box.
int g_hot_button = 0;           // control id under the mouse, 0 = none
WNDPROC g_button_base_proc = nullptr;

LRESULT CALLBACK action_button_proc(HWND button, UINT message, WPARAM w_param,
                                    LPARAM l_param) {
    const int id = GetDlgCtrlID(button);
    if (message == WM_MOUSEMOVE && g_hot_button != id) {
        g_hot_button = id;
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, button, 0};
        TrackMouseEvent(&track);
        InvalidateRect(button, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE && g_hot_button == id) {
        g_hot_button = 0;
        InvalidateRect(button, nullptr, FALSE);
    } else if (message == WM_ERASEBKGND) {
        return 1;  // WM_DRAWITEM paints every pixel
    }
    return CallWindowProcW(g_button_base_proc, button, message, w_param,
                           l_param);
}

void subclass_action_button(HWND button) {
    if (!button) return;
    const auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        button, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&action_button_proc)));
    if (!g_button_base_proc) g_button_base_proc = previous;
}

Gdiplus::Color shade(const Gdiplus::Color& c, float f) {
    auto ch = [f](BYTE v) {
        return static_cast<BYTE>(std::clamp(v * f, 0.0f, 255.0f));
    };
    return Gdiplus::Color(c.GetA(), ch(c.GetR()), ch(c.GetG()), ch(c.GetB()));
}

void draw_action_button(const DRAWITEMSTRUCT& item) {
    const int w = item.rcItem.right - item.rcItem.left;
    const int h = item.rcItem.bottom - item.rcItem.top;
    if (w <= 0 || h <= 0) return;

    // Draw off-screen, then copy once: no flicker on hover.
    HDC dc = CreateCompatibleDC(item.hDC);
    HBITMAP bitmap = CreateCompatibleBitmap(item.hDC, w, h);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);

    HWND parent = GetParent(item.hwndItem);
    POINT origin{0, 0};
    RECT window_rect{};
    if (parent && GetWindowRect(item.hwndItem, &window_rect)) {
        origin = {window_rect.left, window_rect.top};
        ScreenToClient(parent, &origin);
    }
    paint_splash(parent, dc, origin.x, origin.y);

    const bool quit = item.CtlID == kQuitButton;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hot = g_hot_button == static_cast<int>(item.CtlID);
    const bool focused = (item.itemState & ODS_FOCUS) != 0;

    // Flat and quiet: Pick ROM is a solid muted gold, Quit an outline on a
    // faint dark wash. Hover lightens, pressing darkens; nothing else.
    const float lift = pressed ? 0.88f : (hot ? 1.10f : 1.0f);
    const Gdiplus::Color fill = quit
        ? Gdiplus::Color(pressed ? 150 : (hot ? 120 : 90), 10, 12, 20)
        : shade(Gdiplus::Color(255, 214, 172, 82), lift);
    const Gdiplus::Color edge = quit
        ? Gdiplus::Color(hot || focused ? 230 : 150, 240, 232, 214)
        : shade(Gdiplus::Color(255, 214, 172, 82), lift);
    const Gdiplus::Color text = quit ? Gdiplus::Color(255, 240, 234, 214)
                                     : Gdiplus::Color(255, 40, 28, 10);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    const float inset = 1.5f;
    const float bx = inset;
    const float by = inset;
    const float bw = static_cast<float>(w) - inset * 2.0f;
    const float bh = static_cast<float>(h) - inset * 2.0f;
    const float radius = std::min(6.0f, bh / 2.0f);

    Gdiplus::GraphicsPath body;
    rounded_rect_path(body, bx, by, bw, bh, radius);
    Gdiplus::SolidBrush fill_brush(fill);
    g.FillPath(&fill_brush, &body);
    Gdiplus::Pen border(edge, 1.0f);
    g.DrawPath(&border, &body);
    // Keyboard focus on the gold button: a thin inner line, not a dotted box.
    if (focused && !quit) {
        Gdiplus::GraphicsPath ring;
        rounded_rect_path(ring, bx + 3.0f, by + 3.0f, bw - 6.0f, bh - 6.0f,
                          std::max(1.0f, radius - 2.0f));
        Gdiplus::Pen ring_pen(Gdiplus::Color(140, 40, 28, 10), 1.0f);
        g.DrawPath(&ring_pen, &ring);
    }

    wchar_t label[64] = {};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    Gdiplus::Font font(dc, g_button_font);
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush text_brush(text);
    g.DrawString(label, -1, &font, Gdiplus::RectF(bx, by, bw, bh), &format,
                 &text_brush);

    BitBlt(item.hDC, item.rcItem.left, item.rcItem.top, w, h, dc, 0, 0,
           SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// What the game code in a release is built from, one "path sha1" line per
// file: the engine, the builder, the translator and everything in
// builder\data and builder\engine. Written beside GoldenSunGame.dll after a
// build (GoldenSunGame.release.txt); any difference, such as a new release
// unzipped over an old one, means building again. The engine check below
// already caught a new engine; this also catches a release that changes only
// the builder, the translator or its data. The toolchain is left out: it is
// large, and it only changes together with the builder.
std::string release_fingerprint(const fs::path& root) {
    std::vector<fs::path> files = {root / L"GoldenSunRecomp.exe",
                                   root / L"builder" / L"gsr_builder.exe",
                                   root / L"builder" / L"gba_recompile.exe"};
    std::error_code ec;
    for (const wchar_t* dir : {L"data", L"engine"}) {
        std::vector<fs::path> found;
        for (const auto& entry :
             fs::recursive_directory_iterator(root / L"builder" / dir, ec)) {
            if (entry.is_regular_file(ec)) found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        files.insert(files.end(), found.begin(), found.end());
    }
    std::string text;
    for (const fs::path& file : files) {
        std::string digest;
        if (!sha1_file(file, &digest)) digest = "missing";
        text += fs::relative(file, root, ec).generic_string() + " " + digest + "\n";
    }
    return text;
}

// Whether GoldenSunGame.dll beside the game was built from the supported
// ROM by this release's builder against this release's engine.
bool game_code_ready(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_regular_file(root / L"GoldenSunGame.dll", ec)) return false;
    {
        std::ifstream recorded(root / L"GoldenSunGame.release.txt",
                               std::ios::binary);
        std::ostringstream text;
        text << recorded.rdbuf();
        if (!recorded || text.str() != release_fingerprint(root)) return false;
    }
    std::ifstream in(root / L"GoldenSunGame.build.txt");
    std::string line, rom, builder, engine;
    while (std::getline(in, line)) {
        if (line.rfind("rom_sha1=", 0) == 0) rom = line.substr(9);
        else if (line.rfind("builder=", 0) == 0) builder = line.substr(8);
        else if (line.rfind("engine_sha1=", 0) == 0) engine = line.substr(12);
    }
    std::string engine_now;
    if (!sha1_file(root / L"GoldenSunRecomp.exe", &engine_now)) return false;
    return rom == kExpectedRomSha1 && builder == kBuilderVersion &&
           engine == engine_now;
}

// Run builder\gsr_builder.exe for `rom` in the background; progress arrives
// as kBuildUpdateMessage, the end as kBuildFinishedMessage.
void start_game_build(HWND window, const std::wstring& rom) {
    const fs::path builder = g_launcher_root / L"builder" / L"gsr_builder.exe";
    std::error_code ec;
    if (!fs::is_regular_file(builder, ec)) {
        launcher_log(L"Build: builder\\gsr_builder.exe is missing.");
        MessageBoxW(window,
                    L"Part of Golden Sun Recompiled is missing (builder\\gsr_builder.exe).\n\n"
                    L"Please unzip the whole download again.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        launcher_log(L"Build: no pipe for the builder (error " +
                     std::to_wstring(GetLastError()) + L").");
        MessageBoxW(window, L"The game builder could not start.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + builder.wstring() + L"\" --rom \"" + rom + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');
    const BOOL started = CreateProcessW(
        nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
        builder.parent_path().c_str(), &si, &pi);
    const DWORD start_error = started ? 0 : GetLastError();
    CloseHandle(write_end);
    if (!started) {
        CloseHandle(read_end);
        // Error 225 or 5 here usually means antivirus blocked or removed it.
        launcher_log(L"Build: Windows could not start the builder (error " +
                     std::to_wstring(start_error) + L").");
        MessageBoxW(window, L"The game builder could not start.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return;
    }
    // Closing the launcher mid-build stops the builder and its compilers.
    if (!g_build_job) {
        g_build_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (g_build_job)
            SetInformationJobObject(g_build_job, JobObjectExtendedLimitInformation,
                                    &info, sizeof(info));
    }
    if (g_build_job) AssignProcessToJobObject(g_build_job, pi.hProcess);
    launcher_log(L"Build: started (log: builder\\work\\build-log.txt).");
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(g_build.mutex);
        g_build.running = true;
        g_build.stage = L"Starting";
        g_build.done = g_build.total = 0;
        g_build.error.clear();
        g_build.succeeded = false;
    }
    g_build_rom = rom;
    EnableWindow(GetDlgItem(window, 1001 /* kPickRomButton */), FALSE);
    update_auto_start_box(window);
    InvalidateRect(window, nullptr, FALSE);

    if (g_build_thread.joinable()) g_build_thread.join();
    g_build_thread = std::thread([window, read_end, process = pi.hProcess] {
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        bool done = false;
        std::string error;
        auto handle = [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(g_build.mutex);
            if (line.rfind("@stage ", 0) == 0) {
                g_build.stage = utf8_to_wide(line.substr(7));
                g_build.done = g_build.total = 0;
            } else if (line.rfind("@progress ", 0) == 0) {
                std::sscanf(line.c_str() + 10, "%d %d", &g_build.done, &g_build.total);
            } else if (line == "@done") {
                done = true;
            } else if (line.rfind("@error ", 0) == 0) {
                error = line.substr(7);
            }
        };
        while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0) {
            pending.append(buf, n);
            std::size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pending.erase(0, nl + 1);
                handle(line);
                PostMessageW(window, kBuildUpdateMessage, 0, 0);
            }
        }
        CloseHandle(read_end);
        WaitForSingleObject(process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        {
            std::lock_guard<std::mutex> lock(g_build.mutex);
            g_build.running = false;
            g_build.succeeded = done && code == 0;
            g_build.error = utf8_to_wide(
                error.empty() && !g_build.succeeded
                    ? std::string("The build stopped unexpectedly.") : error);
        }
        PostMessageW(window, kBuildFinishedMessage, 0, 0);
    });
}

// ── Updates ────────────────────────────────────────────────────────────
// A release launcher asks GitHub for the newest release when it starts
// (launcher_online.h). When it differs from this launcher's version, the
// player is told and can open its GitHub page to download it; the launcher
// itself downloads and installs nothing.
std::mutex g_update_mutex;
gsr_online::Release g_update;
std::wstring g_release_notes;  // title line + plain text; under g_update_mutex

void check_for_update(HWND window) {
    if (!kReleaseLauncher || !gsr_online::update_checks_enabled()) {
        g_update_check_done = true;
        return;
    }
    std::thread([window] {
        // Done however the check ends (the auto start waits for it). An
        // update found is posted before this is set.
        struct MarkDone {
            ~MarkDone() { g_update_check_done = true; }
        } mark_done;
        HttpTarget target;
        target.host = utf8_to_wide(gsr_online::kReleasesApiHost);
        target.path = utf8_to_wide(gsr_online::kReleasesApiPath);
        std::string reply;
        std::wstring error;
        const int status = http_request(L"GET", target, {L"Accept: application/vnd.github+json"},
                                        nullptr, nullptr, &reply, nullptr, &error);
        if (status != 200) {
            launcher_log(L"Update check: " +
                         (status ? L"GitHub answered " + std::to_wstring(status) : error));
            return;
        }
        gsr_online::Release release;
        std::string parse_error;
        if (!gsr_online::parse_newest_release(reply, false, &release, &parse_error)) {
            launcher_log(L"Update check: " + utf8_to_wide(parse_error));
            return;
        }
        {
            const std::string notes = gsr_online::notes_to_plain_text(release.notes);
            if (!notes.empty()) {
                std::wstring text = L"What's new in " + utf8_to_wide(release.tag) +
                                    L"\n\n" + utf8_to_wide(notes);
                std::wstring crlf;
                for (wchar_t c : text) {
                    if (c == L'\n') crlf += L'\r';
                    crlf += c;
                }
                {
                    std::lock_guard<std::mutex> lock(g_update_mutex);
                    g_release_notes = crlf;
                }
                PostMessageW(window, kReleaseNotesMessage, 0, 0);
            }
        }
        if (!gsr_online::is_newer_release(release)) {
            launcher_log(L"Update check: " + utf8_to_wide(release.tag) + L" is the newest.");
            return;
        }
        launcher_log(L"Update check: " + utf8_to_wide(release.tag) + L" is available.");
        {
            std::lock_guard<std::mutex> lock(g_update_mutex);
            g_update = release;
        }
        PostMessageW(window, kUpdateFoundMessage, 0, 0);
    }).detach();
}

LRESULT CALLBACK launcher_window_proc(HWND window, UINT message,
                                      WPARAM w_param, LPARAM l_param) {
    switch (message) {
    case WM_CREATE: {
        // Optional acceptance diagnostics are session-only and always start
        // unchecked; this prevents a prior trace run from making normal play
        // noisy on the next launcher invocation.
        g_test_variables = k_launcher_test_defaults.master;
        g_test_selfheal_ram = k_launcher_test_defaults.self_heal_ram;
        g_test_cost_probe = k_launcher_test_defaults.cost_probe;
        g_test_host_prof = k_launcher_test_defaults.host_prof;
        g_test_present_cadence = k_launcher_test_defaults.present_cadence;
        g_test_ram_churn_probe = k_launcher_test_defaults.ram_churn_probe;
        g_test_map_record = k_launcher_test_defaults.map_record;
        g_test_function_tracer = k_launcher_test_defaults.function_tracer;
        g_test_text_record = k_launcher_test_defaults.text_record;
        g_test_headroom_probe = k_launcher_test_defaults.headroom_probe;
        g_test_vram_trace = k_launcher_test_defaults.vram_map_trace;
        g_test_room_buffer = k_launcher_test_defaults.room_buffer;
        g_test_swi_log = k_launcher_test_defaults.swi_log;
        g_test_mod_field_test = k_launcher_test_defaults.mod_field_test;
        // "Play" once a ROM has been picked and is still where it was.
        CreateWindowExW(0, L"BUTTON",
                        remembered_rom(g_launcher_root).empty() ? L"Pick ROM" : L"Play",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kPickRomButton),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Quit",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kQuitButton),
                        GetModuleHandleW(nullptr), nullptr);
        subclass_action_button(GetDlgItem(window, kPickRomButton));
        subclass_action_button(GetDlgItem(window, kQuitButton));
        if (kReleaseLauncher) {
            CreateWindowExW(0, L"BUTTON", L"Start the game automatically",
                            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                            0, 0, 0, 0, window,
                            reinterpret_cast<HMENU>(kAutoStartButton),
                            GetModuleHandleW(nullptr), nullptr);
            // The release notes are painted by paint_splash once the update
            // check has them (see draw_release_notes).
            layout_buttons(window);
            return 0;
        }
        // Every checkbox below is BS_OWNERDRAW (drawn by draw_checkbox_item,
        // state read from checkbox_state_for_id) rather than BS_AUTOCHECKBOX
        // so it can sit on the panel's real background instead of painting
        // its own opaque box; their initial checked state is simply whatever
        // the backing global already holds; no BM_SETCHECK needed.
        // Test-variable children default to their fresh-session policy; the
        // persistent Enhanced Options setting is loaded before WM_CREATE.
        CreateWindowExW(0, L"BUTTON", L"Test variables",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kTestVariablesButton),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Enhanced Options",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kEnhancedOptionsButton),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Draw the field with the graphics card",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kGpuFieldButton),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON",
                        L"...and only with it (refused frames go magenta)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kGpuFieldOnlyButton),
                        GetModuleHandleW(nullptr), nullptr);
        struct TestChildControl {
            int id;
            const wchar_t* label;
        };
        const TestChildControl children[] = {
            {kSelfHealRamButton, L"Self-heal RAM"},
            {kFunctionTracerButton, L"Function tracer"},
            {kTextRecordButton, L"Record text progression"},
            {kHeadroomProbeButton, L"Record CPU headroom"},
            {kMapRecordButton, L"Record map + scene data"},
            {kObjRecordButton, L"Record sprite placement"},
            {kVramMapTraceButton, L"VRAM map write trace"},
            {kEffectTraceButton, L"Spell effect canvas trace"},
            // Which code writes BG1 in battle, per frame, with the 2x state:
            // finds the updates battle speed 2x loses.
            {kBattleBg1RecordButton, L"Record battle BG1 writes"},
            // Adds the test Psynergy Earth Surge (src/mod_loader.cpp): Isaac
            // knows it from level 1. It plays the casting flourish, then
            // his jump attack with a white trail and a big earth explosion
            // with a screen shake (src/earth_surge.cpp); the explosion needs
            // the host spell effects on.
            // Also logs [move-probe] lines for battle moves (src/move_probe.cpp):
            // the frame each blow lands and where its sparks are on screen.
            {kModFieldTestButton, L"Mod test: Earth Surge"},
            {kRoomBufferButton, L"Room buffer self-check"},
            // F12 saves the frames shown just before it, not only the
            // current one: a one-frame flicker is gone before F12 lands.
            {kFrameRewindButton, L"Rewind F12 (keep last 2 s)"},
            // Saves those 2 s by itself when a glitch check fires; the
            // reason goes to auto_capture.txt in the capture folder.
            {kAutoCaptureButton, L"Auto-capture glitches"},
            {kSwiLogButton, L"Record BIOS SWI calls"},
            // Restored 2026-09-13: the battle/effect slowdown cannot be
            // attributed without it, and handing over a raw environment
            // variable is not how this project ships a debug option.
            {kCostProbeButton, L"Cost probe"},
            {kHostProfButton, L"Host profiler"},
            // Restored 2026-09-13: the host profiler shows the emulation
            // thread parked in a kernel wait for a quarter of its samples,
            // and this is the probe that times the present itself.
            {kPresentCadenceButton, L"Present cadence"},
            // Instrument for RAM-resident code stalls. The multi-second ones
            // were Defender holding each new heal DLL on first load
            // (FACTS.md, 2026-09-23; docs/features/STATIC_RAM_CODE.md).
            {kRamChurnProbeButton, L"RAM churn probe"},
        };
        for (const TestChildControl& child : children) {
            CreateWindowExW(0, L"BUTTON", child.label,
                            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                            0, 0, 0, 0, window,
                            reinterpret_cast<HMENU>(child.id),
                            GetModuleHandleW(nullptr), nullptr);
        }

        CreateWindowExW(0, L"BUTTON", L"Native MP2K audio (Experimental)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kNativeMp2kButton),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Normal-speed Turbo audio (Experimental)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kTurboAudioButton),
                        GetModuleHandleW(nullptr), nullptr);
        HWND audio_help = CreateWindowExW(
            0, L"STATIC",
            L"Default audio coupled. Launcher toggle: MP2K music-only Turbo; "
            L"PSG/FIFO omitted. 2x-4x only; 1x/>4x/uncapped or "
            L"reverb/unsupported falls back to canonical coupled audio. "
            L"MuteDuringTurbo wins.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(kAudioHelpText),
            GetModuleHandleW(nullptr), nullptr);
        // A smaller body font for the paragraph-length help text: at the
        // checkbox font's size the text needs far more lines to stay
        // readable without clipping.
        if (audio_help) {
            SendMessageW(audio_help, WM_SETFONT,
                         reinterpret_cast<WPARAM>(g_body_font), TRUE);
        }
        CreateWindowExW(0, L"BUTTON", L"Copy session ID to clipboard",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kCopySessionIdButton),
                        GetModuleHandleW(nullptr), nullptr);
        layout_buttons(window);
        return 0;
    }
    case WM_SIZE:
        layout_buttons(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        // Drawn off screen and copied in, so the progress animation's
        // repaints do not flicker.
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, client.right,
                                                         client.bottom)
                                : nullptr;
        if (memory && bitmap) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            paint_splash(window, memory);
            BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top,
                   paint.rcPaint.right - paint.rcPaint.left,
                   paint.rcPaint.bottom - paint.rcPaint.top, memory,
                   paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
            SelectObject(memory, old);
        } else {
            paint_splash(window, dc);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_TIMER:
        if (w_param == kAutoStartTimer) {
            // An update found is posted before the check is marked done, and
            // posted messages are handled before WM_TIMER, so the update
            // question always comes first and stops this timer.
            if (!g_update_check_done &&
                GetTickCount64() - g_auto_start_began < kAutoStartMaxWaitMs)
                return 0;
            KillTimer(window, kAutoStartTimer);
            auto_start_now(window);
            return 0;
        }
        if (w_param == kProgressTimer) {
            bool running = false;
            {
                std::lock_guard<std::mutex> lock(g_build.mutex);
                running = g_build.running;
            }
            if (running && g_progress_rect.right > g_progress_rect.left)
                InvalidateRect(window, &g_progress_rect, FALSE);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        // The help text is a plain (non-owner-drawn) STATIC label. Left
        // unhandled it erases its own rect to an opaque system color before
        // drawing its text, the same flat-box problem the checkboxes have.
        // Painting is already done by the time this fires (WM_PAINT above
        // covers the whole client area, including under this control), so
        // returning NULL_BRUSH here just tells it to skip that erase and
        // draw text straight over what is already there.
        HDC dc = reinterpret_cast<HDC>(w_param);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 232, 210));
        return reinterpret_cast<INT_PTR>(GetStockObject(NULL_BRUSH));
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
        if (!item || !g_button_font) return FALSE;
        if (bool* state = checkbox_state_for_id(item->CtlID)) {
            bool checked = *state;
            // Strict static acceptance always wins over the UI: these two
            // must read as unchecked (as well as disabled) while it is
            // active, same as before.
            if (item->CtlID == kNativeMp2kButton ||
                item->CtlID == kTurboAudioButton) {
                checked = checked && !g_strict_static_route;
            }
            draw_checkbox_item(*item, checked);
            return TRUE;
        }
        draw_action_button(*item);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(w_param) != BN_CLICKED) return 0;
        if (LOWORD(w_param) == kQuitButton) {
            DestroyWindow(window);
            return 0;
        }
        // Owner-drawn checkboxes have no OS-maintained check state (that is
        // only automatic for BS_AUTOCHECKBOX), so a click just flips the
        // flag this ID maps to and repaints it -- same table WM_DRAWITEM
        // reads from, so a new checkbox only ever needs the one entry there.
        if (bool* state = checkbox_state_for_id(LOWORD(w_param))) {
            *state = !*state;
            switch (LOWORD(w_param)) {
            case kNativeMp2kButton:
                if (!g_audio_settings.native_mp2k) {
                    g_audio_settings.turbo_decoupled = false;
                }
                save_launcher_audio_settings(g_launcher_root, g_audio_settings);
                layout_buttons(window);
                break;
            case kTurboAudioButton:
                if (!g_audio_settings.native_mp2k) {
                    g_audio_settings.turbo_decoupled = false;
                }
                save_launcher_audio_settings(g_launcher_root, g_audio_settings);
                break;
            case kGpuFieldButton:
                if (!g_audio_settings.gpu_field) {
                    g_audio_settings.gpu_field_only = false;
                }
                save_launcher_audio_settings(g_launcher_root, g_audio_settings);
                layout_buttons(window);
                break;
            case kGpuFieldOnlyButton:
                if (!g_audio_settings.gpu_field) {
                    g_audio_settings.gpu_field_only = false;
                }
                save_launcher_audio_settings(g_launcher_root, g_audio_settings);
                break;
            case kCopySessionIdButton:
                save_launcher_audio_settings(g_launcher_root, g_audio_settings);
                break;
            case kAutoStartButton:
                save_auto_start(g_launcher_root, g_auto_start);
                launcher_log(g_auto_start ? L"Start automatically: on."
                                          : L"Start automatically: off.");
                if (!g_auto_start) stop_auto_start(window, L"box unticked");
                break;
            case kEnhancedOptionsButton:
                save_launcher_audio_settings(g_launcher_root, g_audio_settings);
                break;
            case kTestVariablesButton:
                layout_buttons(window);
                break;
            default:
                break;
            }
            HWND clicked = GetDlgItem(window, LOWORD(w_param));
            if (clicked) InvalidateRect(clicked, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(w_param) == kPickRomButton) {
            KillTimer(window, kAutoStartTimer);
            std::wstring rom = remembered_rom(g_launcher_root);
            if (!rom.empty()) {
                launcher_log(L"Play: using the remembered ROM " + rom + L".");
            } else {
                SetWindowTextW(GetDlgItem(window, kPickRomButton), L"Pick ROM");
                rom = choose_rom(g_launcher_root, window);
                if (rom.empty()) return 0;
                SetWindowTextW(GetDlgItem(window, kPickRomButton), L"Play");
            }
            // A release builds the game code from the ROM first, once.
            if (kReleaseLauncher && !game_code_ready(g_launcher_root)) {
                launcher_log(L"The game is not built yet (or is out of date): building it.");
                start_game_build(window, rom);
                return 0;
            }
            launcher_log(L"Starting the game.");
            if (run_game(g_launcher_root, rom, window) == 0) {
                DestroyWindow(window);  // no-op if run_game already destroyed it
            }
            return 0;
        }
        break;
    case kBuildUpdateMessage: {
        // Only the status line and the bar change.
        RECT client{};
        GetClientRect(window, &client);
        RECT status{0, client.bottom - 164, client.right, client.bottom};
        InvalidateRect(window, &status, FALSE);
        return 0;
    }
    case kReleaseNotesMessage: {
        std::wstring notes;
        {
            std::lock_guard<std::mutex> lock(g_update_mutex);
            notes = g_release_notes;
        }
        g_notes_text = notes;
        g_notes_scroll = 0;
        g_notes_measured_width = -1;
        if (g_notes_rect.right > g_notes_rect.left)
            InvalidateRect(window, &g_notes_rect, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (!kReleaseLauncher || g_notes_text.empty() ||
            g_notes_rect.right <= g_notes_rect.left)
            break;
        POINT cursor{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
        ScreenToClient(window, &cursor);
        if (!PtInRect(&g_notes_rect, cursor)) break;
        const int notches = GET_WHEEL_DELTA_WPARAM(w_param);
        const float step = g_notes_line_height * 3.0f;
        float next = g_notes_scroll -
                     step * static_cast<float>(notches) / WHEEL_DELTA;
        next = std::min(next, g_notes_max_scroll);
        next = std::max(next, 0.0f);
        if (next != g_notes_scroll) {
            g_notes_scroll = next;
            InvalidateRect(window, &g_notes_rect, FALSE);
        }
        return 0;
    }
    case kUpdateFoundMessage: {
        gsr_online::Release release;
        {
            std::lock_guard<std::mutex> lock(g_update_mutex);
            release = g_update;
        }
        {
            std::lock_guard<std::mutex> lock(g_build.mutex);
            if (g_build.running) return 0;  // asked again on the next start
        }
        // The automatic start must not fire behind the question: the message
        // box's own loop would still deliver its timer.
        const bool was_counting = KillTimer(window, kAutoStartTimer) != 0;
        std::wstring text = L"A new version of Golden Sun Recompiled is out: " +
                            utf8_to_wide(release.tag);
        if (!release.title.empty()) text += L"\n" + utf8_to_wide(release.title);
        text += L"\n\nYou have " + utf8_to_wide(gsr_online::kReleaseVersion) +
                L". Open the download page now?\n\nUnzip the new version over this "
                L"folder to keep your settings and saves. The game is then prepared from "
                L"your ROM once more, which takes a few minutes.";
        if (MessageBoxW(window, text.c_str(), L"Update available",
                        MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            launcher_log(L"Update: opened the page of " + utf8_to_wide(release.tag) + L".");
            const std::wstring url =
                utf8_to_wide(release.page.empty() ? gsr_online::kReleasesPage : release.page);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (was_counting)
                launcher_log(L"Start automatically: stopped (going to update).");
        } else {
            launcher_log(L"Update: not now.");
            if (was_counting) auto_start_now(window);
        }
        return 0;
    }
    case kBuildFinishedMessage: {
        if (g_build_thread.joinable()) g_build_thread.join();
        bool ok = false;
        std::wstring error;
        {
            std::lock_guard<std::mutex> lock(g_build.mutex);
            ok = g_build.succeeded;
            error = g_build.error;
        }
        EnableWindow(GetDlgItem(window, kPickRomButton), TRUE);
        if (ok) {
            std::ofstream stamp(g_launcher_root / L"GoldenSunGame.release.txt",
                                std::ios::binary | std::ios::trunc);
            stamp << release_fingerprint(g_launcher_root);
        }
        const bool ready = ok && game_code_ready(g_launcher_root);
        {
            std::lock_guard<std::mutex> lock(g_build.mutex);
            g_build.ready = ready;
        }
        update_auto_start_box(window);
        launcher_log(ready ? std::wstring(L"Build: finished, the game is ready.")
                     : ok  ? std::wstring(L"Build: finished, but the game files do not "
                                          L"match this release.")
                           : L"Build: failed: " + error);
        InvalidateRect(window, nullptr, FALSE);
        if (!ready) {
            std::wstring msg = L"The game could not be prepared.\n\n" + error +
                L"\n\nThe full log is builder\\work\\build-log.txt.";
            MessageBoxW(window, msg.c_str(), L"Golden Sun Recompiled",
                        MB_OK | MB_ICONERROR);
            return 0;
        }
        if (!auto_start_chosen(g_launcher_root)) {
            g_auto_start =
                MessageBoxW(window,
                            L"The game is ready.\n\nStart it automatically from now on "
                            L"when you open the launcher? The launcher still checks for "
                            L"updates first, and the \"Start the game automatically\" box "
                            L"above the Play button changes this later.",
                            L"Golden Sun Recompiled",
                            MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
            save_auto_start(g_launcher_root, g_auto_start);
            launcher_log(g_auto_start ? L"Start automatically: on (asked after the build)."
                                      : L"Start automatically: off (asked after the build).");
            InvalidateRect(GetDlgItem(window, kAutoStartButton), nullptr, FALSE);
        }
        KillTimer(window, kAutoStartTimer);
        if (ask_start_or_notes(window) != IDYES) {
            launcher_log(L"Build: finished; the player chose to read what's new first.");
            update_auto_start_box(window);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        launcher_log(L"Build: finished; the player chose to start now.");
        if (run_game(g_launcher_root, g_build_rom, window) == 0)
            DestroyWindow(window);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kProgressTimer);
        KillTimer(window, kAutoStartTimer);
        // Quit mid-build: closing the job stops the builder and its
        // compilers, which ends the reader thread.
        if (g_build_job) {
            CloseHandle(g_build_job);
            g_build_job = nullptr;
        }
        if (g_build_thread.joinable()) g_build_thread.join();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

int show_launcher(const fs::path& root) {
    Gdiplus::GdiplusStartupInput startup_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &startup_input, nullptr) !=
        Gdiplus::Ok) {
        MessageBoxW(nullptr, L"The splash screen could not start.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_launcher_root = root;
    open_launcher_log(root);
    if (kReleaseLauncher) g_build.ready = game_code_ready(root);
    if (kReleaseLauncher) g_auto_start = load_auto_start(root);
    g_audio_settings = kReleaseLauncher ? release_launch_settings()
                                        : load_launcher_audio_settings(root);
    g_strict_static_route = inherited_environment_truthy(
        L"GBARECOMP_STRICT_STATIC");
    // The logo is built into the launcher (launcher_logo.h). GDI+ wants the
    // stream kept for an image's life, so it is copied into a plain bitmap.
    if (IStream* stream = SHCreateMemStream(kLauncherLogoPng,
                                            sizeof(kLauncherLogoPng))) {
        std::unique_ptr<Gdiplus::Bitmap> decoded(
            Gdiplus::Bitmap::FromStream(stream));
        if (decoded && decoded->GetLastStatus() == Gdiplus::Ok) {
            auto copy = std::make_unique<Gdiplus::Bitmap>(
                static_cast<INT>(decoded->GetWidth()),
                static_cast<INT>(decoded->GetHeight()), PixelFormat32bppPARGB);
            {
                Gdiplus::Graphics g(copy.get());
                g.DrawImage(decoded.get(), 0, 0,
                            static_cast<INT>(decoded->GetWidth()),
                            static_cast<INT>(decoded->GetHeight()));
            }
            g_splash_image = std::move(copy);
        }
        decoded.reset();
        stream->Release();
    }
    g_button_font = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_body_font = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    const wchar_t class_name[] = L"GoldenSunRecompiledLauncherWindow";
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = launcher_window_proc;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    RegisterClassW(&window_class);

    // Tall enough that the panel still clears the logo even in the worst
    // case (Test variables checked, all diagnostic children shown in one
    // column).
    //
    // That worst case is arithmetic, not taste. From layout_buttons the panel
    // needs
    //   20 padding + help + 6 + 32 + 32 + 44 + 44 + 26 + 6
    //     + (rows * 26 + (rows - 1) * 6) + 20 padding
    // and the window needs that plus the 20px panel margin, the 48px buttons
    // and their 28px bottom inset. At the ten rows actually created that is
    // 640 + help, leaving room for the help text to wrap to about nine lines
    // before anything collides. Each further visible row costs 32.
    // The release launcher has no panel, so it only needs the artwork (which
    // is square) and the buttons.
    RECT desired{0, 0, 720, kReleaseLauncher ? 720 : 880};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                 WS_MINIMIZEBOX, FALSE, WS_EX_APPWINDOW);
    // The player launcher's window leaves its two buttons out of its own
    // painting (WS_CLIPCHILDREN): each build update repaints the window, and
    // painting over the buttons before they redrew made them flicker. Not
    // in the developer launcher, whose transparent help text relies on the
    // window painting behind it.
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, L"Golden Sun Recompiled",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
            (kReleaseLauncher ? WS_CLIPCHILDREN : 0),
        CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left,
        desired.bottom - desired.top, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!window) {
        if (g_button_font) DeleteObject(g_button_font);
        g_button_font = nullptr;
        if (g_body_font) DeleteObject(g_body_font);
        g_body_font = nullptr;
        g_splash_image.reset();
        g_backdrop.bitmap.reset();
        Gdiplus::GdiplusShutdown(gdiplus_token);
        MessageBoxW(nullptr, L"The launcher window could not be created.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }

    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(window, nullptr,
                 (screen_width - (desired.right - desired.left)) / 2,
                 (screen_height - (desired.bottom - desired.top)) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
    if (kReleaseLauncher) SetTimer(window, kProgressTimer, 33, nullptr);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    check_for_update(window);
    begin_auto_start(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_button_font) DeleteObject(g_button_font);
    g_button_font = nullptr;
    if (g_body_font) DeleteObject(g_body_font);
    g_body_font = nullptr;
    g_splash_image.reset();
    g_backdrop.bitmap.reset();
    UnregisterClassW(class_name, GetModuleHandleW(nullptr));
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return static_cast<int>(message.wParam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Must be the FIRST thing in WinMain, before any other subsystem.
    // nullptr = default to the directory this executable lives in.
    gbarecomp::crash_handler_install(nullptr);

    const fs::path root = module_dir();
    const bool developer_auto_launch = !kReleaseLauncher &&
        inherited_environment_truthy(L"GBARECOMP_AUTO_LAUNCH");
    if (developer_auto_launch) {
        // This path is deliberately opt-in and uses the same cached ROM plus
        // exact SHA-1 validation as the normal Pick ROM button. It exists so
        // scripted replay can still enter through GoldenSunLauncher.exe.
        const fs::path cached_path = root / L"local" / L"launcher-rom.txt";
        const std::wstring cached_rom = read_cached_path(cached_path);
        std::wstring error;
        if (!cached_rom.empty() && validate_rom(cached_rom, &error)) {
            g_audio_settings = load_launcher_audio_settings(root);
            g_strict_static_route = inherited_environment_truthy(
                L"GBARECOMP_STRICT_STATIC");
            SetProcessDPIAware();
            const int result = run_game(root, cached_rom, nullptr);
            gbarecomp::crash_handler_mark_clean_exit();
            return result;
        }
    }
    SetProcessDPIAware();
    const int result = show_launcher(root);
    gbarecomp::crash_handler_mark_clean_exit();
    return result;
}
