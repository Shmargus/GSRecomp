// Internal Windows game session, embedded in the collection launcher.
//
// Select and verify the exact USA/Europe ROM before preparing or starting it.
// Collection entry keeps the user's ROM and saves in the collection folder.

#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <winhttp.h>

#include "crash_handler.h"
#include "launcher_replay_policy.h"
#include "launcher_session_id.h"
#include "launcher_graphics_retry.h"
#include "launcher_test_policy.h"
#include "session_environment.h"
#include "launcher_widescreen_diagnostics_policy.h"
#include "launcher_logo.h"  // generated from assets/launcher_logo.png
#include "launcher_common.h"
#include "launcher_build_progress.h"
#include "collection_storage.h"
#include "collection_settings.h"
#include "launcher_online.h"
#include "collection_session.h"
#include "collection_preferences.h"
#include "../engine/src/runtime/handover.h"
#include "../engine/src/runtime/seamless.h"
#include "../engine/src/runtime/soft_reset.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace gsr_launcher;

// GSR_RELEASE_LAUNCHER (CMake option of the same name, used by
// make_release.bat): a player build with only a Cancel button. No
// diagnostics, no settings file, and the game is looked for next to the
// launcher. The play settings are fixed to the ones the game ships with.
#ifdef GSR_RELEASE_LAUNCHER
constexpr bool kReleaseLauncher = true;
#else
constexpr bool kReleaseLauncher = false;
#endif

namespace {

// One activation from the collection selector; never a saved preference.
bool g_collection_direct3d = false;
bool g_collection_tests_supplied = false;
gsr_collection::GameSettings g_collection_tests;
fs::path g_collection_root;
bool g_collection_update_checked = false;
bool g_collection_activation_pending = false;

// ── Release: first-run game build ─────────────────────────────────────
// A release holds no game code. The first time the player picks their ROM
// the launcher runs builder\gsr_builder.exe, which translates the ROM and
// compiles the game library (GoldenSunGame.dll, TLAGame.dll) beside the game
// (player_builder). Its progress lines (@stage / @progress / @done / @error)
// drive the text and bar drawn over the splash. The build stamp
// (GoldenSunGame.build.txt, TLAGame.build.txt) records what the DLL was built
// from; a different ROM, builder or engine means building again.
constexpr UINT kBuildUpdateMessage = WM_APP + 1;
constexpr UINT kBuildFinishedMessage = WM_APP + 2;
// Updates (launcher_online.h): the start-up check found a newer release.
constexpr UINT kUpdateFoundMessage = WM_APP + 3;
// ...and read the newest release's notes (shown whether or not it is newer).
constexpr UINT kReleaseNotesMessage = WM_APP + 4;

struct BuildStatus {
    std::mutex mutex;
    bool running = false;
    bool ready = false;          // the game library matches this release
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

// Diagnostic variables are deliberately opt-in. run_game copies the test
// options chosen in the collection launcher (collection_settings.h) into
// these; without collection test settings only self-healing is on.
const auto k_launcher_test_defaults = gsr::launcher_test_defaults();
bool g_test_variables = k_launcher_test_defaults.master;
bool g_test_selfheal_ram = k_launcher_test_defaults.self_heal_ram;
bool g_test_cost_probe = k_launcher_test_defaults.cost_probe;
bool g_test_host_prof = k_launcher_test_defaults.host_prof;
bool g_test_present_cadence = k_launcher_test_defaults.present_cadence;
bool g_test_map_record = k_launcher_test_defaults.map_record;
bool g_test_obj_record = k_launcher_test_defaults.obj_record;
bool g_test_effect_trace = k_launcher_test_defaults.effect_trace;
bool g_test_unpacker_force_fault = k_launcher_test_defaults.unpacker_force_fault;
bool g_test_unpacker_audit = k_launcher_test_defaults.unpacker_audit;

using gsr_session::LauncherAudioSettings;
using gsr_session::load_launcher_audio_settings;
using gsr_session::save_launcher_audio_settings;

// Root-launcher settings live outside config/local.json: that file contains
// user machine paths and must not be rewritten by the launcher.
LauncherAudioSettings g_audio_settings{};
bool g_strict_static_route = false;
// Set by run_game for a Seamless handover or Soft Reset; the
// session process then exits with it (show_launcher / gsr_session_main).
int g_session_exit_code = 0;
// The Lost Age started by Seamless mode: run_game sends it to The Broken
// Seal's program, still open with its window, instead of starting
// TLARecomp.exe (handover.h).
bool g_handover_next = false;

// What a release launch plays with: Enhanced Options (expanded view
// rendering and spell effects) and the graphics-card field renderer ALONE.
// The console compositor fallback is off: on the Steam Deck it drew a
// full 360x240 picture per frame that was almost never shown and made
// widescreen slow (2026-10-04: one refused frame in ~590,000 across the
// developer sessions). Everything experimental or diagnostic stays off.
LauncherAudioSettings release_launch_settings(const fs::path& root) {
    LauncherAudioSettings settings{};
    settings.enhanced_options = true;
    settings.gpu_field = true;
    settings.gpu_field_only = true;
    const LauncherAudioSettings saved = load_launcher_audio_settings(root);
    settings.gs_save_import = saved.gs_save_import;
    settings.any_rom = saved.any_rom;
    return settings;
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

}  // namespace
namespace {

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

// `owner` is the launcher window: the file dialog and the "not the right
// ROM" message stay in front of it. Without an owner the message could open
// behind the launcher, and the launcher then looked like it did nothing.
std::wstring choose_rom(const fs::path& root, HWND owner) {
    const fs::path cache = root / L"local" / L"launcher-rom.txt";
    const std::wstring cached = read_cached_path(cache);
    std::wstring initial_directory;
    if (!cached.empty()) initial_directory = fs::path(cached).parent_path();
    const std::wstring candidate = pick_file(
        session_game().rom_prompt_w,
        L"Golden Sun ROM (*.gba)\0*.gba\0All files (*.*)\0*.*\0\0",
        initial_directory, owner);
    if (candidate.empty()) {
        launcher_log(L"Pick ROM: no file chosen.");
        return {};
    }

    std::string error_utf8;
    if (validate_rom(candidate, &error_utf8)) {
        launcher_log(L"Pick ROM: " + candidate + L" - accepted.");
        return candidate;
    }
    const std::wstring error = utf8_to_wide(error_utf8);
    launcher_log(L"Pick ROM: " + candidate + L" - refused: " + error);
    MessageBoxW(owner, error.c_str(), session_game().title_w,
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return {};
}

// The ROM picked last time, when it is still there and still the right ROM;
// empty otherwise. The launcher then plays it without asking again.
std::wstring remembered_rom(const fs::path& root) {
    const std::wstring cached = read_cached_path(root / L"local" / L"launcher-rom.txt");
    if (cached.empty()) return {};
    std::string error;
    if (validate_rom(cached, &error)) return cached;
    launcher_log(L"Remembered ROM " + cached + L" can no longer be used: " +
                 utf8_to_wide(error));
    return {};
}

bool prepare_collection_rom(const fs::path& root, std::wstring& rom, HWND owner,
                            gsr_collection::ManagedFiles* result = nullptr) {
    std::string error;
    gsr_collection::ManagedFiles files;
    try {
        if (g_collection_root.empty())
            g_collection_root = gsr_collection::infer_collection_root(root);
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    if (!error.empty() || !validate_rom(rom, &error) ||
        !gsr_collection::import_selected_rom(g_collection_root, fs::path(rom),
                                             session_game().stem, files, error) ||
        !validate_rom(files.rom, &error)) {
        const std::wstring message = L"Your ROM and saves could not be prepared.\n\n" +
                                     utf8_to_wide(error);
        launcher_log(message);
        MessageBoxW(owner, message.c_str(), session_game().title_w,
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return false;
    }
    rom = files.rom.wstring();
    write_cached_path(root / L"local" / L"launcher-rom.txt", rom);
    if (result) *result = files;
    return true;
}

fs::path active_save_path(const fs::path& root) {
    return gsr_collection::managed_paths(g_collection_root.empty()
        ? gsr_collection::infer_collection_root(root) : g_collection_root,
        session_game().stem).save;
}

// ── Session log capture ────────────────────────────────────────────────
// The game program is a console-subsystem exe.
// Started from this launcher (which has no console) without redirected
// handles, Windows gives it a fresh console window that is destroyed the
// instant the child exits, so a crash, a freeze killed from Task Manager, or
// even a clean quit loses the scrollback. This captures the child's
// stdout/stderr into a timestamped file under logs/ instead: each line is
// flushed to disk (FILE_FLAG_WRITE_THROUGH + FlushFileBuffers) as it is read
// from the pipe, so a kill mid-freeze keeps everything written so far.
//
// stdout and stderr go into ONE chronological file, each line tagged
// "[OUT] "/"[ERR] ", which is easier to hand over than two files.
std::wstring make_session_log_path(const fs::path& logs_dir) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf(name, std::size(name), L"session_%04u%02u%02u_%02u%02u%02u.log",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fs::path path = logs_dir / name;
    std::error_code ec;
    for (unsigned suffix = 1; fs::exists(path, ec); ++suffix)
        path = logs_dir / (fs::path(name).stem().wstring() + L"_" +
                           std::to_wstring(suffix) + L".log");
    return path.wstring();
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

bool create_empty_file_exclusive(const fs::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file);
    return true;
}

// Thread-safe sink for the two pipe-reader threads below: writes from either
// stream go through write_line, so tagged lines never interleave mid-line.
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
        // Alongside FILE_FLAG_WRITE_THROUGH: this line is on disk before the
        // reader goes back to blocking on ReadFile, where a kill can land.
        FlushFileBuffers(file_);
    }

private:
    HANDLE file_;
    std::mutex mutex_;
};

// Drains one child pipe on its own thread until EOF (the child closing its
// end, which happens when it exits), tagging and forwarding complete lines to
// `writer`. One thread per pipe avoids the "child blocks writing to a full
// pipe nobody drains" deadlock: ReadFile returns as soon as any bytes are
// available, and the thread never waits on anything the child is blocked on.
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
// service (The Broken Seal/tools/report_service). Every zip holds the small logs plus at most
// ONE F12 capture (about 7 MB zipped), so each upload stays small; a session
// with three captures makes three zips.

}  // namespace
namespace gsr_launcher {

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
    std::vector<std::pair<std::string, std::string>> swaps;
    for (const std::string& name : names) {
        for (const char* sep : {"\\", "/", "\\\\"}) {
            swaps.emplace_back(std::string(sep) + "users" + sep + name,
                               std::string(sep) + "Users" + sep + "<user>");
        }
    }
    return scrub_paths(std::move(text), swaps);
}

}  // namespace gsr_launcher
namespace {

// Returns the zips (one per F12 capture, or one when there was none), or
// the folder when zipping failed; empty when there was nothing to report.
std::vector<fs::path> make_bug_report(const fs::path& root, const fs::path& game_dir,
                                      const fs::path& crash_dir,
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
    // Include the actual active save so the problem can be replayed.
    if (!rom.empty())
        copy_into_report(active_save_path(root), staging);
    copy_report_files(game_dir, crash_dir, root, crashed, staging);
    const bool unpacker_caught =
        fs::is_regular_file(root / L"unpacker_recovered.txt", ec);
    {
        std::ofstream info(staging / L"report_info.txt", std::ios::binary);
        info << report_info_text(
            "Windows: " + registry_text(L"ProductName") + " " +
                registry_text(L"DisplayVersion") + " (build " +
                registry_text(L"CurrentBuild") + ")",
            rewinds.size(), crashed, unpacker_caught, exit_code);
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

// Seamless mode: The Broken Seal's report from before the hand-over waits in
// this file (its zips, one per line) and is offered with The Lost Age's when
// that session ends, so the hand-over is not interrupted.
fs::path seamless_reports_file(int game) {
    const fs::path root = gsr_session::game_root(g_collection_root, game);
    return root.empty() ? fs::path() : root / L"logs" / L"bug_reports" / L"seamless_pending.txt";
}

void keep_seamless_reports(const std::vector<fs::path>& zips) {
    const fs::path file = seamless_reports_file(0);
    if (file.empty()) return;
    std::ofstream out(file, std::ios::binary | std::ios::app);
    for (const fs::path& zip : zips)
        if (zip.extension() == L".zip") out << wide_to_utf8(zip.wstring()) << "\n";
}

std::vector<fs::path> take_seamless_reports() {
    std::vector<fs::path> zips;
    const fs::path file = seamless_reports_file(0);
    if (file.empty()) return zips;
    std::ifstream in(file, std::ios::binary);
    std::error_code ec;
    for (std::string line; std::getline(in, line);) {
        const fs::path zip(utf8_to_wide(line));
        if (!line.empty() && fs::is_regular_file(zip, ec)) zips.push_back(zip);
    }
    in.close();
    fs::remove(file, ec);
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
constexpr int kReportReddit = 3005;

struct ReportWindow {
    std::vector<fs::path> reports;
    bool crashed = false;
    bool zipped = false;
    HWND edit = nullptr, reddit = nullptr, status = nullptr, send = nullptr, close = nullptr;
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

// The game a report zip belongs to: the one whose logs/bug_reports holds it.
// The Lost Age's report window can carry The Broken Seal's zips from before
// a Seamless hand-over (take_seamless_reports).
const char* report_game(const fs::path& zip) {
    for (int game = 0; game < 2; ++game) {
        const fs::path root = gsr_session::game_root(g_collection_root, game);
        if (!root.empty() && zip.parent_path().lexically_normal() ==
                                 (root / L"logs" / L"bug_reports").lexically_normal())
            return kGames[game].stem;
    }
    return session_game().stem;
}

// Uploads every report zip with the player's description and, when they
// gave one, their Reddit username; runs on its own thread and posts
// kReportProgressMessage / kReportDoneMessage to `window`.
void send_reports(HWND window, std::vector<fs::path> reports, std::string description,
                  std::string reddit, std::wstring* error) {
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
        std::vector<std::wstring> headers = {
            L"X-GSR-Client: GoldenSunLauncher",
            L"X-GSR-Platform: windows",
            L"X-GSR-Game: " + utf8_to_wide(gsr_online::percent_encode(report_game(reports[i]))),
            L"X-GSR-Launcher: " + utf8_to_wide(gsr_online::kReleaseVersion) + L" " +
                utf8_to_wide(__DATE__),
            L"X-GSR-File: " + utf8_to_wide(gsr_online::percent_encode(
                                  wide_to_utf8(reports[i].filename().wstring()), 120)),
            L"X-GSR-Description: " + utf8_to_wide(gsr_online::percent_encode(part)),
            L"Content-Type: application/octet-stream",
        };
        if (!reddit.empty()) headers.push_back(L"X-GSR-Reddit: " + utf8_to_wide(reddit));
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
            wchar_t reddit[32] = {};
            GetWindowTextW(state->reddit, reddit, 32);
            EnableWindow(state->send, FALSE);
            EnableWindow(state->close, FALSE);
            SendMessageW(state->edit, EM_SETREADONLY, TRUE, 0);
            SendMessageW(state->reddit, EM_SETREADONLY, TRUE, 0);
            report_set_status(L"Sending...");
            launcher_log(L"Bug report: sending " + std::to_wstring(state->reports.size()) +
                         L" file(s).");
            if (state->worker.joinable()) state->worker.join();
            state->error.clear();
            state->worker = std::thread(send_reports, window, state->reports,
                                        wide_to_utf8(text),
                                        gsr_online::reddit_name(wide_to_utf8(reddit)),
                                        &state->error);
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
                        session_game().title_w, MB_YESNO | MB_ICONWARNING) == IDYES) {
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

// The report window's opening sentence. `unpacker` is the unpacker guard's
// unpacker_recovered.txt (src/unpacker_guard.h).
std::wstring report_heading(bool crashed, bool unpacker) {
    if (unpacker && crashed)
        return L"The game's data unpacker hit a crash and could not recover. A bug "
               L"report was saved; sending it helps a lot in finding the proper fix.";
    if (unpacker)
        return L"The game's data unpacker hit a crash. It was caught and the game kept "
               L"running, and a bug report was saved. Sending it helps a lot in "
               L"finding the proper fix.";
    return crashed ? L"The game closed unexpectedly. A bug report was saved."
                   : L"Your bug report was saved.";
}

void offer_bug_report(const std::vector<fs::path>& reports, bool crashed, bool unpacker) {
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
        if (MessageBoxW(nullptr, text.c_str(), session_game().title_w,
                        MB_OKCANCEL | MB_SETFOREGROUND | MB_ICONWARNING) == IDOK) {
            ShellExecuteW(nullptr, L"open", kBugReportFormUrl, nullptr, nullptr, SW_SHOWNORMAL);
            ShellExecuteW(nullptr, L"open", L"explorer.exe", reports.front().c_str(), nullptr,
                          SW_SHOWNORMAL);
        }
        return;
    }
    if (!gsr_online::report_upload_enabled()) {
        // A launcher built without the report service (a fork, a developer
        // build): the zips and the form.
        std::wstring text = report_heading(crashed, unpacker) + L"\n\n";
        for (const fs::path& report : reports) text += report.filename().wstring() + L"\n";
        text += L"in " + reports.front().parent_path().wstring() +
                L"\n\nPress OK to open the bug report form and this folder, write a few "
                L"words about what happened and add the zip. Cancel keeps the files "
                L"without sending anything.";
        if (MessageBoxW(nullptr, text.c_str(), session_game().title_w,
                        MB_OKCANCEL | MB_SETFOREGROUND |
                            (crashed || unpacker ? MB_ICONWARNING : MB_ICONINFORMATION)) == IDOK) {
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

    // The unpacker's heading runs to three lines; everything below moves down.
    const int dy = unpacker ? 40 : 0;
    const int width = px(520), height = px(474 + dy);
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

    std::wstring heading = report_heading(crashed, unpacker);
    heading += L"\nWhat happened, and where in the game? A few words help a lot:";
    add(L"STATIC", heading, 0, 0, 20, 16, 480, 44 + dy);
    state.edit = add(L"EDIT", L"",
                     ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
                     kReportEdit, 20, 66 + dy, 480, 110);
    SendMessageW(state.edit, EM_SETLIMITTEXT, 500, 0);
    add(L"STATIC", L"Your Reddit username, if you'd like the developer to reach out to you "
                   L"(optional; leave it blank if not):", 0, 0, 20, 184 + dy, 480, 40);
    state.reddit = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kReportReddit, 20, 226 + dy,
                       240, 28);
    SendMessageW(state.reddit, EM_SETLIMITTEXT, 24, 0);
    std::wstring files;
    std::uint64_t bytes = 0;
    std::error_code ec;
    for (const fs::path& report : reports) bytes += fs::file_size(report, ec);
    files = L"Send report uploads " +
            std::wstring(reports.size() > 1 ? std::to_wstring(reports.size()) + L" zips"
                                            : std::wstring(L"the zip")) +
            L" (" + std::to_wstring(std::max<std::uint64_t>(1, bytes / (1024 * 1024))) +
            L" MB) to the developer: the game's log and screen captures, your save "
            L"file, your settings and your Windows version, plus the Reddit username above "
            L"only if you typed one. Your Windows user name is taken out. Nothing is "
            L"sent unless you press Send report.";
    add(L"STATIC", files, 0, 0, 20, 266 + dy, 480, 100);
    state.status = add(L"STATIC", L"", 0, 0, 20, 370 + dy, 480, 30);
    state.send = add(L"BUTTON", L"Send report", BS_DEFPUSHBUTTON | WS_TABSTOP, kReportSend, 20,
                     404 + dy, 150, 40);
    state.close = add(L"BUTTON", L"Don't send", WS_TABSTOP, kReportClose, 180, 404 + dy, 130,
                      40);
    add(L"BUTTON", L"Open folder", WS_TABSTOP, kReportFolder, 370, 404 + dy, 130, 40);

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

int run_game(const fs::path& root, const std::wstring& selected_rom, HWND window,
             bool direct3d_compat = false) {
    direct3d_compat = direct3d_compat || g_collection_direct3d;
    const bool collection_gpu_check = g_collection_tests_supplied &&
        gsr_collection::test_enabled(g_collection_tests, "TLA_GPU_FIELD_CHECK", session_game_index);
    if (g_collection_tests_supplied) {
        g_test_variables = g_collection_tests.test_variables;
        const auto enabled = [](const char* environment) {
            return gsr_collection::test_enabled(g_collection_tests, environment, session_game_index);
        };
        // Self-healing keeps its selected state even when the master switch is off.
        g_test_selfheal_ram = g_collection_tests.tests[gsr_collection::test_index("GBARECOMP_SELFHEAL_RAM")];
        g_test_obj_record = enabled("GSR_OBJ_RECORD");
        g_test_map_record = enabled("GSR_MAP_RECORD");
        g_test_effect_trace = enabled("GSR_EFFECT_TRACE");
        g_test_unpacker_force_fault = enabled("GSR_UNPACKER_FORCE_FAULT");
        g_test_unpacker_audit = enabled("GSR_UNPACKER_AUDIT");
        g_test_cost_probe = enabled("GBARECOMP_COST_PROBE");
        g_test_host_prof = enabled("GBARECOMP_HOST_PROF");
        g_test_present_cadence = enabled("GBARECOMP_PRESENT_CADENCE");
    }
    std::wstring rom = selected_rom;
    gsr_collection::ManagedFiles files;
    if (!prepare_collection_rom(root, rom, window, &files)) return 1;
    HANDLE log_file = INVALID_HANDLE_VALUE;
    // Close the session log if it is open, say why, and give up the start.
    auto fail = [&](const std::wstring& text, UINT icon = MB_ICONERROR) {
        if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        MessageBoxW(window, text.c_str(), session_game().title_w,
                    MB_OK | icon | MB_SETFOREGROUND);
        return 1;
    };
    // Golden Sun's flash save is 64 KiB; the engine refuses a larger .sav
    // (runtime.cpp "save file too large"), so say why instead of exiting.
    constexpr std::uintmax_t kGameSaveBytes = 0x10000;
    {
        const fs::path save = active_save_path(root);
        std::error_code save_ec;
        const std::uintmax_t save_size = fs::file_size(save, save_ec);
        if (!save_ec && save_size > kGameSaveBytes) {
            launcher_log(L"Start game: save file " + save.wstring() + L" is " +
                         std::to_wstring(save_size) +
                         L" bytes, larger than 65536: not started.");
            const std::wstring text =
                L"This save file can't be used\n\n"
                L"This save file was made by an emulator or "
                L"another program, and Golden Sun Recompiled can't read it.\n\n" +
                save.wstring() +
                L"\n\nMove it to another folder or delete it, then start the "
                L"game again. The game will make a new save there.";
            return fail(text, MB_ICONWARNING);
        }
    }
    // Save the settings before spawning so a launch cannot lose a change.
    if (!kReleaseLauncher) save_launcher_audio_settings(root, g_audio_settings);
    // The game runs without the BIOS, so no BIOS path is passed.

    const fs::path game = gsr_session::engine(root, session_game_index);
    if (!fs::is_regular_file(game)) {
        const std::wstring exe = session_game().executable_w;
        launcher_log(L"Start game: " + exe + L" not found.");
        return fail(exe + L" was not found. Run the build first.");
    }

    fs::create_directories(root / L"recomp_cache");

    const fs::path logs_dir = root / L"logs";
    std::error_code dir_ec;
    fs::create_directories(logs_dir, dir_ec);
    std::wstring log_path;
    if (!dir_ec) {
        prune_old_logs(logs_dir);
        log_path = make_session_log_path(logs_dir);
        // FILE_FLAG_WRITE_THROUGH: every WriteFile is committed to disk
        // before it returns, not left in the OS cache (see the session log
        // capture note above).
        log_file = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                               nullptr);
    }
    // A log that can't be opened (full disk, permissions) must not block
    // play: the game then runs with its output unredirected.
    const bool logging = (log_file != INVALID_HANDLE_VALUE);

    // The session log path is not printed anywhere (no console); logs/latest.txt,
    // rewritten on every launch, names the most recent session log.
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
        launcher_log(L"Start game: the game environment could not be prepared.");
        return fail(L"The game environment could not be prepared.");
    }

    // The variables both platforms set the same way (session_environment.h);
    // what differs on Windows is passed in. Direct3D compatibility, the frame
    // timing files and the input recording follow below.
    gsr::ChildEnvironmentPlan plan;
    gsr::CommonEnvironment common;
    common.controls_dir = wide_to_utf8((g_collection_root / L"Controls").wstring());
    fs::create_directories(g_collection_root / "Controls");
    common.state_base = wide_to_utf8(files.state_base.wstring());
    common.heal_cache = wide_to_utf8((root / L"recomp_cache").wstring());
    // Release: point the game's self-heal at the bundled g++ (absent on dev).
    if (std::filesystem::exists(root / L"builder" / L"mingw64" / L"bin" /
                                L"g++.exe")) {
        common.heal_toolchain = wide_to_utf8((root / L"builder" / L"mingw64").wstring());
    }
    if (session_game_index == 1)  // The Lost Age imports Golden Sun saves.
        common.save_import = g_audio_settings.gs_save_import;
    // Enhanced Options: experimental fixes, room-buffer rendering and host
    // spell effects (session_environment.h).
    common.enhanced_options = g_audio_settings.enhanced_options;
    // Drawing the field with the graphics card is a persistent player
    // setting like Enhanced Options. It falls back to the console compositor
    // for any frame it cannot reproduce. The GPU field check forces the card
    // on and the fallback on.
    common.gpu_field = collection_gpu_check || g_audio_settings.gpu_field;
    // "Draw ONLY with the graphics card" turns the console compositor off:
    // a frame the card refuses is painted a flat colour instead of quietly
    // falling back, because the refusals are exactly what still needs finding.
    common.gpu_field_only = !collection_gpu_check && g_audio_settings.gpu_field &&
                            g_audio_settings.gpu_field_only;
    gsr::add_common_environment(plan, common);

    // Replay and recording are mutually exclusive in the runner; reject an
    // explicit conflict before spawning rather than allowing a partial,
    // misleading session. Only an explicit GBARECOMP_INPUT_RECORD inherited
    // from the environment records input.
    const std::wstring explicit_input_record =
        inherited_environment_value(L"GBARECOMP_INPUT_RECORD");
    const std::wstring input_replay =
        inherited_environment_value(L"GBARECOMP_INPUT_REPLAY");
    const auto input_record_policy =
        gsr::resolve_launcher_input_record_policy(
            false, explicit_input_record, !input_replay.empty(), L"");
    if (input_record_policy.replay_conflict) {
        return fail(L"GBARECOMP_INPUT_RECORD cannot be used together with "
                    L"GBARECOMP_INPUT_REPLAY.");
    }
    if (input_record_policy.enabled) {
        const fs::path input_path(input_record_policy.path);
        std::error_code input_ec;
        if (!input_path.parent_path().empty())
            fs::create_directories(input_path.parent_path(), input_ec);
        if (input_ec || fs::exists(input_path) ||
            !create_empty_file_exclusive(input_path)) {
            return fail(L"The input recording path could not be created "
                        L"without overwriting an existing file.");
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

    // The test options are diagnostics, never part of the faithful default.
    // An option that is off is removed from the child so a shell-launched
    // value cannot silently turn this into a crutch run; self-healing is the
    // one option that is on by default. The BIOS logs and the test variables
    // whose launcher box was retired are always removed.
    struct TestOptionValue {
        const char* name;
        bool enabled;
    };
    const TestOptionValue test_option_values[] = {
        {"GBARECOMP_SELFHEAL_RAM", g_test_selfheal_ram},
        {"GSR_OBJ_RECORD", g_test_obj_record},
        {"GSR_MAP_RECORD", g_test_map_record},
        {"GSR_EFFECT_TRACE", g_test_effect_trace},
        {"GSR_UNPACKER_FORCE_FAULT", g_test_unpacker_force_fault},
        {"GSR_UNPACKER_AUDIT", g_test_unpacker_audit},
        {"GBARECOMP_COST_PROBE", g_test_cost_probe},
        {"GBARECOMP_HOST_PROF", g_test_host_prof},
        {"GBARECOMP_PRESENT_CADENCE", g_test_present_cadence},
    };
    const auto test_option_state = [&](const char* name) -> std::optional<bool> {
        for (const TestOptionValue& value : test_option_values)
            if (std::strcmp(value.name, name) == 0) return value.enabled;
        // Every other option only a launch with collection test settings
        // decides (GPU field check, Console sprite memory).
        if (g_collection_tests_supplied)
            return gsr_collection::test_enabled(g_collection_tests, name, session_game_index);
        return std::nullopt;
    };
    // The host profiler names the file it writes its samples to; put it beside
    // this session's log so the two are matched by name.
    std::string host_prof_file;
    if (!log_path.empty()) {
        std::wstring prof_path = log_path;
        const std::size_t dot = prof_path.rfind(L".log");
        if (dot != std::wstring::npos) prof_path.resize(dot);
        host_prof_file = wide_to_utf8(prof_path + L".hostprof.txt");
    }
    gsr::add_test_environment(plan, test_option_state, host_prof_file);
    if (collection_gpu_check) plan.enabled_tests += ",GSR_GPU_FIELD=1,GSR_GPU_FIELD_ONLY=0";
    gsr::remove_variables(plan, gsr::bios_log_variables);
    gsr::remove_variables(plan, gsr::retired_test_variables);
    for (const std::string& name : plan.unset)
        child_environment.unset(utf8_to_wide(name));
    for (const auto& [name, value] : plan.set)
        child_environment.set(utf8_to_wide(name), utf8_to_wide(value));
    // Record what actually reached the child, so the session log shows
    // whether a diagnostic was set even if the game did not act on it.
    if (logging) {
        std::string enabled_list = plan.enabled_tests;
        if (!enabled_list.empty()) enabled_list += ",";
        enabled_list += "GSR_FRAME_REWIND,GSR_SCREEN_FILTERS,GSR_UNPACKER_CATCH";
        const std::string line =
            std::string("[launcher] test_variables=") +
            (g_test_variables ? "ON" : "OFF") + " set=" + enabled_list + "\n";
        DWORD written = 0;
        WriteFile(log_file, line.data(), static_cast<DWORD>(line.size()),
                  &written, nullptr);
        FlushFileBuffers(log_file);
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

    // Apply Direct3D compatibility last so normal settings cannot override it.
    child_environment.unset(L"GSR_DIRECT3D_COMPAT");
    if (direct3d_compat) {
        child_environment.set(L"GSR_DIRECT3D_COMPAT", L"1");
        child_environment.set(L"SDL_RENDER_DRIVER", L"direct3d11");
        child_environment.set(L"GSR_GPU_FIELD", L"0");
        child_environment.set(L"GSR_GPU_FIELD_ONLY", L"0");
        child_environment.set(L"GSR_SCREEN_FILTERS", L"0");
        child_environment.unset(L"TLA_GPU_FIELD_CHECK");
        launcher_log(L"Direct3D compatibility: GSR_DIRECT3D_COMPAT=1, SDL_RENDER_DRIVER=direct3d11, "
                     L"GSR_GPU_FIELD=0, GSR_GPU_FIELD_ONLY=0, GSR_SCREEN_FILTERS=0.");
        if (logging) {
            const std::string line =
                "[launcher] graphics=direct3d11 compatibility=ON GPU_FIELD=0 GPU_FIELD_ONLY=0 SCREEN_FILTERS=0 TLA_GPU_FIELD_CHECK=OFF\r\n";
            DWORD written = 0;
            WriteFile(log_file, line.data(), static_cast<DWORD>(line.size()),
                      &written, nullptr);
            FlushFileBuffers(log_file);
        }
    }

    std::wstring command = L"\"" + game.wstring() + L"\"";
    command += L" --rom \"" + rom + L"\"";
    command += L" --save \"" + files.save.wstring() + L"\"";
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
            // Only the write ends are inherited by the child. If it also
            // inherited the read ends, its copies would keep the pipes open
            // after the real write ends close, and the reader threads would
            // never see EOF.
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
    // After a Seamless hand-over The Lost Age runs in The Broken Seal's
    // program, and its crash handler writes beside that program.
    const fs::path host_dir = g_handover_next
        ? gsr_session::engine(gsr_session::game_root(g_collection_root, 0), 0).parent_path()
        : game.parent_path();
    // A crash file left by an earlier run must not end up in this run's report.
    {
        std::error_code stale_ec;
        for (const fs::path& dir : {game.parent_path(), host_dir}) {
            fs::remove(dir / L"crash_memory.bin", stale_ec);
            fs::remove(dir / L"crash_trail.csv", stale_ec);
        }
        // The unpacker catcher's pair (src/unpacker_catch.h), written in the
        // game's working folder: at most one pair ever exists, from this run.
        fs::remove(root / L"unpacker_catch.txt", stale_ec);
        fs::remove(root / L"unpacker_catch.bin", stale_ec);
        // The unpacker guard's pair (src/unpacker_guard.h), the same way.
        fs::remove(root / L"unpacker_recovered.txt", stale_ec);
        fs::remove(root / L"unpacker_recovered.bin", stale_ec);
        fs::remove(root / L"unpacker_journal.csv", stale_ec);  // Unpacker audit
    }
    const auto launch_time = fs::file_time_type::clock::now();

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> environment_block = child_environment.block();
    // The game is a console-subsystem exe, so Windows would give it a console
    // window. A player release hides it; its output still reaches the
    // session log through the pipes above.
    const DWORD creation_flags = CREATE_UNICODE_ENVIRONMENT |
        (kReleaseLauncher ? CREATE_NO_WINDOW : 0);
    bool handed_over = false;
    if (g_handover_next) {
        gbarecomp::handover::Launch launch;
        launch.plugin = (game.parent_path() / session_game().engine).wstring();
        launch.command_line = command;
        launch.directory = root.wstring();
        launch.environment.assign(environment_block.begin(), environment_block.end());
        handed_over = gbarecomp::handover::run(launch, out_write, err_write,
                                               &process.hProcess, &process.dwProcessId);
        launcher_log(handed_over
            ? L"Seamless: The Lost Age runs in The Broken Seal's window."
            : L"Seamless: no window to hand over to; starting The Lost Age on its own.");
    }
    if (!handed_over &&
        !CreateProcessW(game.c_str(), mutable_command.data(), nullptr, nullptr,
                        inherit_handles, creation_flags,
                        environment_block.data(), root.c_str(), &startup,
                        &process)) {
        const DWORD start_error = GetLastError();
        if (out_read) CloseHandle(out_read);
        if (out_write) CloseHandle(out_write);
        if (err_read) CloseHandle(err_read);
        if (err_write) CloseHandle(err_write);
        launcher_log(L"Start game: Windows could not start " + game.wstring() +
                     L" (error " + std::to_wstring(start_error) + L").");
        return fail(L"Windows could not start the recompiled game.");
    }
    if (process.hThread) CloseHandle(process.hThread);
    launcher_log(L"Start game: running " + game.wstring() + L", session log " +
                 (log_path.empty() ? std::wstring(L"(none)")
                                   : fs::path(log_path).filename().wstring()) + L".");

    // The splash window has nothing left to do once the game has actually
    // started; destroying it now (rather than after the wait below) avoids
    // Windows flagging it "Not Responding" for the whole play session,
    // because this call blocks until the game exits.
    if (window) DestroyWindow(window);
    window = nullptr;

    if (out_write && err_write) {
        // Our copies of the write ends must close before the readers start:
        // while any write-end handle is open, ReadFile blocks instead of
        // returning EOF.
        CloseHandle(out_write);
        CloseHandle(err_write);
        SessionLogWriter writer(log_file);
        std::thread out_thread(pump_pipe_to_log, out_read, "[OUT] ", &writer);
        std::thread err_thread(pump_pipe_to_log, err_read, "[ERR] ", &writer);
        // Both pipes reach EOF only once the child has exited and released
        // its handles, so joining doubles as the wait for the child, with
        // everything it wrote already logged.
        out_thread.join();
        err_thread.join();
    }
    // Its output ended but the program still runs: The Broken Seal handed
    // over and its program keeps the window for The Lost Age (handover.h).
    DWORD exit_code = 0;
    bool program_waits = false;
    if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        std::int32_t code = 0;
        program_waits = gbarecomp::handover::query(process.dwProcessId, &code);
        if (program_waits) exit_code = static_cast<DWORD>(code);
    }
    if (!program_waits) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &exit_code);
    }
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    log_file=INVALID_HANDLE_VALUE;
    CloseHandle(process.hProcess);

    std::string sync_error;
    const bool saves_synced = gsr_collection::sync_legacy_states(files, sync_error);
    if (!saves_synced) {
        const std::wstring message = L"Your savestates could not be copied into Saves.\n\n" +
                                     utf8_to_wide(sync_error);
        launcher_log(message);
        MessageBoxW(nullptr, message.c_str(), session_game().title_w,
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }

    {
        std::vector<std::wstring> new_rewinds;
        for (const std::wstring& dir : rewind_dirs(logs_dir))
            if (!rewinds_before.count(dir)) new_rewinds.push_back(dir);
        std::error_code time_ec;
        const fs::path crash_dir = handed_over ? host_dir : game.parent_path();
        const fs::path crash_report = crash_dir / L"crash_report.txt";
        const bool crashed = fs::is_regular_file(crash_report, time_ec) &&
            fs::last_write_time(crash_report, time_ec) >= launch_time && !time_ec;
        const bool unpacker = fs::is_regular_file(root / L"unpacker_recovered.txt", time_ec);
        launcher_log(L"Game ended: exit code " + std::to_wstring(exit_code) +
                     (crashed ? L", crashed (crash_report.txt written)" : L"") +
                     (unpacker ? L", unpacker crash caught (unpacker_recovered.txt)" : L"") +
                     L", rewind captures " + std::to_wstring(new_rewinds.size()) + L".");
        // Seamless mode: The Broken Seal wrote its Clear Data and asks for The
        // Lost Age. This is a hand-over, not a failure; the session exits with
        // the same code and the collection window starts the next game.
        const int seamless_slot =
            gbarecomp::seamless::slot_of_exit_code(static_cast<int>(exit_code));
        if (session_game_index == 0 && saves_synced && seamless_slot >= 0) {
            launcher_log(L"Seamless: The Broken Seal saved Clear Data to slot " +
                         std::to_wstring(seamless_slot + 1) + L"; starting The Lost Age.");
            if (kReleaseLauncher && (!new_rewinds.empty() || crashed || unpacker)) {
                const auto reports = make_bug_report(root, game.parent_path(), crash_dir, rom,
                                                     log_path, new_rewinds, crashed, exit_code);
                keep_seamless_reports(reports);
                launcher_log(L"Seamless: bug report kept for the end of The Lost Age (" +
                             std::to_wstring(reports.size()) + L" file(s)).");
            }
            g_session_exit_code = static_cast<int>(exit_code);
            return 0;
        }
        if (program_waits) gbarecomp::handover::quit();
        if (saves_synced && exit_code == gbarecomp::soft_reset::kExitCode)
            g_session_exit_code = static_cast<int>(exit_code);
        if (!direct3d_compat && exit_code != 0 &&
            exit_code != gbarecomp::soft_reset::kExitCode &&
            gsr::should_offer_direct3d_retry(direct3d_compat, exit_code, crashed,
                                           read_text(log_path), read_text(crash_report))) {
            const int answer = MessageBoxW(nullptr,
                L"OpenGL graphics could not start.\n\n"
                L"Would you like to try Direct3D instead?\n\n"
                L"Some visual enhancements and screen filters will be unavailable for this run.",
                session_game().title_w, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_SETFOREGROUND);
            if (answer == IDYES) {
                const auto reports = make_bug_report(root, game.parent_path(), crash_dir, rom,
                                                     log_path, new_rewinds, crashed, exit_code);
                for (const auto& report : reports)
                    launcher_log(L"OpenGL startup crash archived: " + report.wstring());
                if (reports.empty()) {
                    launcher_log(L"OpenGL startup crash archive could not be created; preserving loose evidence before retry.");
                    const std::wstring stem = fs::path(log_path).stem().wstring();
                    if (crashed) for (const wchar_t* file : {L"crash_report.txt", L"crash_dump.dmp",
                                                L"crash_memory.bin", L"crash_trail.csv"}) {
                        std::error_code copy_ec;
                        const fs::path preserved = logs_dir / (stem + L"." + file);
                        fs::copy_file(crash_dir / file, preserved,
                                      fs::copy_options::none, copy_ec);
                        if (!copy_ec) launcher_log(L"OpenGL crash evidence: " + preserved.wstring());
                    }
                }
                return run_game(root, rom, nullptr, true);
            }
        }
        if (kReleaseLauncher) {
            std::vector<fs::path> reports;
            if (!new_rewinds.empty() || crashed || unpacker)
                reports = make_bug_report(root, game.parent_path(), crash_dir, rom, log_path,
                                          new_rewinds, crashed, exit_code);
            // A folder (zipping failed) is offered alone; the kept zips then
            // wait for the next session.
            if (session_game_index == 1 &&
                (reports.empty() || reports.front().extension() == L".zip")) {
                const std::vector<fs::path> kept = take_seamless_reports();
                reports.insert(reports.end(), kept.begin(), kept.end());
            }
            offer_bug_report(reports, crashed, unpacker);
        }
    }
    return saves_synced ? 0 : 1;
}

// kPickRomButton has no control in the session window: the start is posted to
// the window as a command once the update check has answered.
constexpr int kPickRomButton = 1001;
constexpr int kQuitButton = 1002;

fs::path g_launcher_root;

// Set once the start-up update check has answered (or was skipped).
std::atomic<bool> g_update_check_done{false};

std::unique_ptr<Gdiplus::Image> g_splash_image;
HFONT g_button_font = nullptr;
HFONT g_body_font = nullptr;
// Panel behind the artwork's lower half, recomputed each time layout_buttons
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

// Positions the Cancel button and the panel or release-notes box behind the
// artwork.
void layout_buttons(HWND window) {
    RECT client{};
    GetClientRect(window, &client);

    constexpr int button_width = 190;
    constexpr int button_height = 48;
    constexpr int button_gap = 18;
    constexpr int total_button_width = button_width * 2 + button_gap;
    const int button_x = std::max<int>(
        0, (client.right - total_button_width) / 2);
    const int button_y = std::max<int>(0, client.bottom - button_height - 28);
    if (HWND quit = GetDlgItem(window, kQuitButton))
        MoveWindow(quit, button_x + button_width + button_gap, button_y,
                   button_width, button_height, TRUE);
    if (kReleaseLauncher) {
        // The release notes sit over the logo: the window's side margin
        // wide, from y 28 down to the bottom band (160 px).
        g_notes_rect = {40, 28, 40 + std::max<int>(0, client.right - 80),
                        28 + std::max<int>(0, client.bottom - 160 - 8 - 28)};
        g_notes_measured_width = -1;
        g_panel_rect = {};
        InvalidateRect(window, nullptr, FALSE);
        return;
    }

    // The developer window draws an empty dark panel; the logo is fitted
    // above it.
    constexpr int kPanelWidth = 528;
    constexpr int kPanelHeight = 250;
    constexpr int kPanelBottomMargin = 20;
    const int panel_x = std::max<int>(0, (client.right - kPanelWidth) / 2);
    const int panel_top = std::max<int>(
        0, button_y - kPanelBottomMargin - kPanelHeight);
    g_panel_rect = {panel_x, panel_top, panel_x + kPanelWidth,
                    panel_top + kPanelHeight};
    InvalidateRect(window, nullptr, FALSE);
}

// The backdrop: a dark indigo gradient, a soft purple glow and the logo
// (assets/launcher_logo.png, built in), fitted into the space above the
// dark panel, or above the buttons in the player launcher. One bitmap,
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
    const float bar_h = progress_height;
    const float bar_x = (width - bar_w) / 2.0f;
    const float bar_y = static_cast<float>(height - 100);
    g_progress_rect = {static_cast<LONG>(bar_x) - 8, static_cast<LONG>(bar_y) - 8,
                       static_cast<LONG>(bar_x + bar_w) + 70,
                       static_cast<LONG>(bar_y + bar_h) + 8};
    const double t = static_cast<double>(GetTickCount64() % 1000000u) / 1000.0;
    const bool known = total > 0;
    const float frac = progress_fraction(done, total);
    const auto color = [](ProgressColor c) { return Gdiplus::Color(c.a,c.r,c.g,c.b); };
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    Gdiplus::GraphicsPath track;
    rounded_rect_path(track, bar_x, bar_y, bar_w, bar_h, progress_corner_radius);
    Gdiplus::SolidBrush background(color(progress_deep_background));
    g.FillPath(&background, &track);
    const auto state = g.Save();
    g.SetClip(&track, Gdiplus::CombineModeIntersect);
    // Unknown work shows energy across the track; it claims no completion.
    const float fill_w = known ? bar_w * frac : bar_w;
    Gdiplus::SolidBrush purple(color(progress_dark_purple));
    g.FillRectangle(&purple, bar_x, bar_y, fill_w, bar_h);
    g.SetClip(Gdiplus::RectF(bar_x, bar_y, fill_w, bar_h), Gdiplus::CombineModeIntersect);
    for (int strand = 0; strand < 3; ++strand) {
        Gdiplus::GraphicsPath wave;
        for (int x = 0; x < static_cast<int>(bar_w); x += 4) {
            const float next = std::min(bar_w, static_cast<float>(x + 4));
            wave.AddLine(bar_x + x, bar_y + energy_y(static_cast<float>(x),bar_w,bar_h,t,strand),
                         bar_x + next, bar_y + energy_y(next,bar_w,bar_h,t,strand));
        }
        Gdiplus::Pen energy(color(progress_light_purple), 2.0f + strand * 0.4f);
        g.DrawPath(&energy, &wave);
    }
    Gdiplus::SolidBrush gloss(Gdiplus::Color(20, 235, 215, 255));
    g.FillRectangle(&gloss, bar_x, bar_y, fill_w, bar_h * 0.3f);
    g.Restore(state);
    Gdiplus::Pen rim(color(progress_gold), 1.3f);
    g.DrawPath(&rim, &track);
    if (known) {
        const std::wstring percent = std::to_wstring(static_cast<int>(frac * 100.0f + 0.5f)) + L"%";
        Gdiplus::Font font(L"Segoe UI", 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush ink(color(progress_gold));
        g.DrawString(percent.c_str(), -1, &font,
                     Gdiplus::PointF(bar_x + bar_w + 12.0f, bar_y + 3.0f), &ink);
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
// draws exactly the artwork/panel slice that sits behind that child (see
// draw_action_button).
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
                // The builder's stages (player_builder): checking the ROM
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

// ---- Cancel button -------------------------------------------------------
// Owner-drawn, flat: slightly rounded, with hover and pressed states. The
// session window's only control is the outlined Cancel button (kQuitButton);
// the solid gold style is for kPickRomButton, which has no control here. The
// corners show the artwork behind them rather than a square box.
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

    // Flat and quiet: Pick ROM would be a solid muted gold, Cancel an outline on a
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
                    session_game().title_w, MB_OK | MB_ICONERROR);
        return;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        launcher_log(L"Build: no pipe for the builder (error " +
                     std::to_wstring(GetLastError()) + L").");
        MessageBoxW(window, L"The game builder could not start.",
                    session_game().title_w, MB_OK | MB_ICONERROR);
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
                    session_game().title_w, MB_OK | MB_ICONERROR);
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
    if (g_collection_update_checked ||
        !kReleaseLauncher || !gsr_online::update_checks_enabled()) {
        g_update_check_done = true;
        return;
    }
    std::thread([window] {
        // Done however the check ends (the session start waits for it). An
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
        // No Pick ROM or settings controls here: only a Cancel button.
        CreateWindowExW(0, L"BUTTON", L"Cancel",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kQuitButton),
                        GetModuleHandleW(nullptr), nullptr);
        subclass_action_button(GetDlgItem(window, kQuitButton));
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
        if (w_param == kProgressTimer) {
            if (g_collection_activation_pending && g_update_check_done) {
                g_collection_activation_pending = false;
                PostMessageW(window, WM_COMMAND, kPickRomButton, 0);
            }
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
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(l_param);
        if (!item || !g_button_font) return FALSE;
        draw_action_button(*item);
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD(w_param) != BN_CLICKED) return 0;
        if (LOWORD(w_param) == kQuitButton) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(w_param) == kPickRomButton) {
            std::wstring rom = remembered_rom(g_launcher_root);
            if (!rom.empty()) {
                launcher_log(L"Play: using the remembered ROM " + rom + L".");
            } else {
                rom = choose_rom(g_launcher_root, window);
                if (rom.empty()) {
                    DestroyWindow(window);
                    return 0;
                }
            }
            if (!prepare_collection_rom(g_launcher_root, rom, window)) {
                DestroyWindow(window);
                return 0;
            }
            // A release builds the game code from the ROM first, once.
            if (kReleaseLauncher && !game_code_ready(g_launcher_root)) {
                const std::wstring prompt = utf8_to_wide(preparation_prompt_text(
                    preparation_state(g_launcher_root)));
                if (MessageBoxW(window, prompt.c_str(), session_game().title_w,
                        MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) {
                    DestroyWindow(window);
                    return 0;
                }
                launcher_log(L"The game is not built yet (or is out of date): building it.");
                ShowWindow(window, SW_SHOW);
                UpdateWindow(window);
                start_game_build(window, rom);
                bool running = false;
                {
                    std::lock_guard<std::mutex> lock(g_build.mutex);
                    running = g_build.running;
                }
                // Start failures already displayed their error. A completed
                // worker has queued kBuildFinishedMessage, so keep it alive.
                if (!running && !g_build_thread.joinable()) DestroyWindow(window);
                return 0;
            }
            launcher_log(L"Starting the game.");
            ShowWindow(window, SW_HIDE);
            run_game(g_launcher_root, rom, window);
            DestroyWindow(window);  // no-op if run_game already destroyed it
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
        // The start must not fire behind the question: the message box's own
        // loop would still deliver the timer.
        const bool collection_was_pending = g_collection_activation_pending;
        g_collection_activation_pending = false;
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
            g_collection_activation_pending = false;
            DestroyWindow(window);
            return 0;
        }
        launcher_log(L"Update: not now.");
        g_collection_activation_pending = collection_was_pending;
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
        if (ok) {
            std::ofstream stamp(g_launcher_root / session_game().release_stamp_w,
                                std::ios::binary | std::ios::trunc);
            stamp << release_fingerprint(g_launcher_root);
        }
        const bool ready = ok && game_code_ready(g_launcher_root);
        {
            std::lock_guard<std::mutex> lock(g_build.mutex);
            g_build.ready = ready;
        }
        launcher_log(ready ? std::wstring(L"Build: finished, the game is ready.")
                     : ok  ? std::wstring(L"Build: finished, but the game files do not "
                                          L"match this release.")
                           : L"Build: failed: " + error);
        InvalidateRect(window, nullptr, FALSE);
        if (!ready) {
            std::wstring msg = L"The game could not be prepared.\n\n" + error +
                L"\n\nThe full log is builder\\work\\build-log.txt.";
            MessageBoxW(window, msg.c_str(), session_game().title_w,
                        MB_OK | MB_ICONERROR);
            DestroyWindow(window);
            return 0;
        }
        ShowWindow(window, SW_HIDE);
        run_game(g_launcher_root, g_build_rom, window);
        DestroyWindow(window);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kProgressTimer);
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
                    session_game().title_w, MB_OK | MB_ICONERROR);
        return 1;
    }

    g_launcher_root = root;
    open_launcher_log(root);
    if (kReleaseLauncher) g_build.ready = game_code_ready(root);
    g_audio_settings = kReleaseLauncher ? release_launch_settings(root)
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

    // The developer window is tall enough for the logo above the 250 px panel
    // (layout_buttons) and the Cancel button. The release launcher has no
    // panel, so it only needs the artwork (which is square) and the button.
    RECT desired{0, 0, 720, kReleaseLauncher ? 720 : 880};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                 WS_MINIMIZEBOX, FALSE, WS_EX_APPWINDOW);
    // The player launcher's window leaves its button out of its own painting
    // (WS_CLIPCHILDREN): each build update repaints the window, and painting
    // over the button before it redrew made it flicker.
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, session_game().title_w,
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
                    session_game().title_w, MB_OK | MB_ICONERROR);
        return 1;
    }

    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(window, nullptr,
                 (screen_width - (desired.right - desired.left)) / 2,
                 (screen_height - (desired.bottom - desired.top)) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
    SetTimer(window, kProgressTimer, 33, nullptr);
    check_for_update(window);
    g_collection_activation_pending = true;

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
    return g_session_exit_code ? g_session_exit_code : static_cast<int>(message.wParam);
}

}  // namespace

int gsr_session_main(int game, int, char**) {
    session_game_index = game;
    // The only way in: the collection launcher starts this session.
    bool collection_play = false;
    int seamless_slot = -1;
    int argument_count = 0;
    if (wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count)) {
        for (int i = 1; i < argument_count; ++i) {
            if (std::wcscmp(arguments[i], L"--collection-play") == 0)
                collection_play = true;
            else if (std::wcscmp(arguments[i], L"--collection-update-checked") == 0)
                g_collection_update_checked = true;
            else if (std::wcscmp(arguments[i], L"--direct3d") == 0)
                g_collection_direct3d = true;
            else if (std::wcscmp(arguments[i], L"--seamless-slot") == 0) {
                // Slot 0..2 of The Broken Seal's save, from the collection window.
                if (++i < argument_count)
                    seamless_slot = gbarecomp::seamless::parse_slot(arguments[i]);
            }
            else if (std::wcscmp(arguments[i], L"--collection-tests") == 0) {
                if (g_collection_tests_supplied || ++i >= argument_count ||
                    !gsr_collection::parse_tests(wide_to_utf8(arguments[i]),
                                                 g_collection_tests)) {
                    MessageBoxW(nullptr, (L"Collection test settings must contain exactly " + std::to_wstring(gsr_collection::test_options.size() + 1) + L" binary digits.").c_str(),
                                session_game().title_w, MB_OK | MB_ICONERROR);
                    LocalFree(arguments);
                    return 1;
                }
                g_collection_tests_supplied = true;
            }
            else if (std::wcscmp(arguments[i], L"--collection-root") == 0) {
                if (++i >= argument_count || !fs::path(arguments[i]).is_absolute()) {
                    MessageBoxW(nullptr, L"The collection folder must be an absolute path.",
                                session_game().title_w, MB_OK | MB_ICONERROR);
                    LocalFree(arguments);
                    return 1;
                }
                g_collection_root = fs::path(arguments[i]).lexically_normal();
            }
        }
        LocalFree(arguments);
    }
    if (g_collection_tests_supplied && (kReleaseLauncher || !collection_play)) {
        MessageBoxW(nullptr, L"Collection test settings require a developer collection launch.",
                    session_game().title_w, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!collection_play || g_collection_root.empty()) {
        MessageBoxW(nullptr, L"Start this game from the collection launcher.",
                    session_game().title_w, MB_OK | MB_ICONERROR);
        return 1;
    }
    const fs::path root = gsr_session::game_root(g_collection_root, session_game_index);
    // "Skip the ROM check" (any_rom.h): this session, the builder and the game
    // all read it from here.
    _putenv_s(gbarecomp::kAnyRomEnv, load_launcher_audio_settings(root).any_rom ? "1" : "0");
    // The Lost Age's game inherits The Broken Seal's save (seamless.h).
    const fs::path gs_save = session_game_index == 1
        ? gsr_session::broken_seal_save(g_collection_root) : fs::path{};
    if (!gs_save.empty())
        SetEnvironmentVariableW(utf8_to_wide(gbarecomp::seamless::kSaveEnv).c_str(),
                                gs_save.c_str());
    fs::current_path(root);
    const std::string crash_root = root.string();
    gbarecomp::crash_handler_install(crash_root.c_str());
    const bool developer_auto_launch = !kReleaseLauncher &&
        inherited_environment_truthy(L"GBARECOMP_AUTO_LAUNCH");
    if (developer_auto_launch) {
        // This path is deliberately opt-in and uses the same cached ROM plus
        // exact SHA-1 validation as the normal Pick ROM button. It exists so
        // scripted replay can still enter through GoldenSunLauncher.exe.
        const fs::path cached_path = root / L"local" / L"launcher-rom.txt";
        const std::wstring cached_rom = read_cached_path(cached_path);
        std::string error;
        if (!cached_rom.empty() && validate_rom(cached_rom, &error)) {
            g_audio_settings = load_launcher_audio_settings(root);
            g_strict_static_route = inherited_environment_truthy(
                L"GBARECOMP_STRICT_STATIC");
            SetProcessDPIAware();
            const int result = run_game(root, cached_rom, nullptr);
            gbarecomp::crash_handler_mark_clean_exit();
            return g_session_exit_code ? g_session_exit_code : result;
        }
    }
    if (seamless_slot >= 0 && session_game_index == 1) {
        // Seamless mode: The Broken Seal just saved its Clear Data. Start The
        // Lost Age with the cached, validated ROM, without the launcher window.
        // The game inherits the save (set above) and the slot from this process.
        const fs::path cached_path = root / L"local" / L"launcher-rom.txt";
        const std::wstring cached_rom = read_cached_path(cached_path);
        std::string error;
        if (!cached_rom.empty() && validate_rom(cached_rom, &error) && !gs_save.empty()) {
            g_launcher_root = root;
            open_launcher_log(root);
            launcher_log(L"Seamless: starting The Lost Age with The Broken Seal's save, slot " +
                         std::to_wstring(seamless_slot + 1) + L".");
            SetEnvironmentVariableW(utf8_to_wide(gbarecomp::seamless::kSlotEnv).c_str(),
                                    std::to_wstring(seamless_slot + 1).c_str());
            g_handover_next = true;
            g_audio_settings = kReleaseLauncher ? release_launch_settings(root)
                                                : load_launcher_audio_settings(root);
            g_strict_static_route = inherited_environment_truthy(L"GBARECOMP_STRICT_STATIC");
            SetProcessDPIAware();
            const int result = run_game(root, cached_rom, nullptr);
            gbarecomp::handover::quit();  // run_game stopped before handing over
            gbarecomp::crash_handler_mark_clean_exit();
            return g_session_exit_code ? g_session_exit_code : result;
        }
        gbarecomp::handover::quit();  // release the waiting Broken Seal program
    }
    SetProcessDPIAware();
    const int result = show_launcher(root);
    gbarecomp::crash_handler_mark_clean_exit();
    return result;
}
