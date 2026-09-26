// Golden Sun Recompiled click-to-play launcher.
//
// This launcher never copies or embeds the user's ROM. It selects the ROM,
// verifies the exact USA/Europe image, and starts the already-built runner.

#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <gdiplus.h>

#include "crash_handler.h"
#include "launcher_audio_policy.h"
#include "launcher_replay_policy.h"
#include "launcher_session_id.h"
#include "launcher_test_policy.h"
#include "launcher_widescreen_diagnostics_policy.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char kExpectedRomSha1[] =
    "5c4695205413df7db52b9a184815a07783999971";

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
bool g_test_with_bios = k_launcher_test_defaults.with_bios;
bool g_test_battle_bg1_record = k_launcher_test_defaults.battle_bg1_record;
bool g_test_room_buffer = k_launcher_test_defaults.room_buffer;
bool g_test_swi_log = k_launcher_test_defaults.swi_log;
bool g_test_bios_pc_log = k_launcher_test_defaults.bios_pc_log;

struct LauncherAudioSettings {
    bool native_mp2k = false;
    bool turbo_decoupled = false;
    bool copy_session_id_to_clipboard = false;
    bool enhanced_options = false;
    bool instant_text = false;
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
        } else if (in_launcher && key == "InstantText") {
            settings.instant_text = value;
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
           << "InstantText="
           << (settings.instant_text ? "true" : "false")
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

std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

std::wstring read_json_string(const fs::path& path, const char* key) {
    const std::string text = read_text(path);
    const std::string marker = std::string("\"") + key + "\"";
    const std::size_t key_pos = text.find(marker);
    if (key_pos == std::string::npos) return {};
    const std::size_t colon = text.find(':', key_pos + marker.size());
    if (colon == std::string::npos) return {};
    const std::size_t quote = text.find('"', colon + 1);
    if (quote == std::string::npos) return {};

    std::string value;
    for (std::size_t i = quote + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') break;
        if (c != '\\' || i + 1 >= text.size()) {
            value.push_back(c);
            continue;
        }
        const char escaped = text[++i];
        switch (escaped) {
        case '\\': value.push_back('\\'); break;
        case '"': value.push_back('"'); break;
        case '/': value.push_back('/'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(escaped); break;
        }
    }
    return utf8_to_wide(value);
}

std::wstring read_cached_path(const fs::path& path) {
    const std::string line = read_text(path);
    const std::size_t end = line.find_first_of("\r\n");
    return utf8_to_wide(line.substr(0, end));
}

void write_cached_path(const fs::path& path, const std::wstring& value) {
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
                       const std::wstring& initial_directory = {}) {
    wchar_t buffer[32768] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
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

std::wstring choose_rom(const fs::path& root) {
    const fs::path cache = root / L"local" / L"launcher-rom.txt";
    const std::wstring cached = read_cached_path(cache);
    std::wstring initial_directory;
    if (!cached.empty()) initial_directory = fs::path(cached).parent_path();
    for (;;) {
        const std::wstring candidate = pick_file(
            L"Select the Golden Sun USA/Europe ROM",
            L"Golden Sun ROM (*.gba)\0*.gba\0All files (*.*)\0*.*\0\0",
            initial_directory);
        if (candidate.empty()) return {};

        std::wstring error;
        if (validate_rom(candidate, &error)) {
            write_cached_path(cache, candidate);
            return candidate;
        }
        MessageBoxW(nullptr, error.c_str(), L"Golden Sun Recompiled",
                    MB_OK | MB_ICONERROR);
    }
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

int run_game(const fs::path& root, const std::wstring& rom,
             const std::wstring& bios, HWND window) {
    // Save before spawning so a launch cannot lose a changed checkbox.
    save_launcher_audio_settings(root, g_audio_settings);
    // The game runs without the BIOS (Jimmy, 2026-09-24): no BIOS path is
    // needed or passed and no Nintendo code executes. The test checkbox
    // passes the configured BIOS for a comparison run, which only a build
    // configured with GBARECOMP_LINK_BIOS=ON can perform.
    const bool no_bios = !gsr::launcher_test_variable_enabled(
        g_test_variables, false, g_test_with_bios);

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
    if (!fs::is_regular_file(game)) {
        game = root / L"build" / L"gs011_rel" / L"GoldenSunRecomp.exe";
    }
    if (!fs::is_regular_file(game)) {
        game = root / L"build" / L"gs011" / L"GoldenSunRecomp.exe";
    }
    if (!fs::is_regular_file(game)) {
        MessageBoxW(nullptr,
                    L"GoldenSunRecomp.exe was not found. Run the build first.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!no_bios && (bios.empty() || !fs::is_regular_file(bios))) {
        MessageBoxW(nullptr,
                    L"The configured GBA BIOS was not found.\n\nEdit "
                    L"config\\local.json before a comparison run, or "
                    L"untick \"Run with real BIOS\".",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
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
        MessageBoxW(nullptr, L"The game environment could not be prepared.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
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
        {L"GSR_BATTLE_BG1_RECORD", g_test_battle_bg1_record},
        {L"GSR_ROOM_BUFFER", g_test_room_buffer},
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

    // Instant text is a normal launcher setting like Enhanced Options, not a
    // session diagnostic: it changes how the game plays, so it persists and is
    // not gated behind "Test variables". The runner no longer reads it: the
    // in-game Message speed menu's fourth choice, Instant, replaces it. Kept
    // until that choice is confirmed in play, then removed.
    child_environment.set(L"GSR_INSTANT_TEXT",
                          g_audio_settings.instant_text ? L"1" : L"0");

    // Drawing the field with the graphics card is a persistent player
    // setting like the two above, not a session diagnostic. It replaces the
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

    // BIOS inventory logs are session-only diagnostics. Remove inherited
    // values first, then add the launcher-owned paths only when their
    // corresponding checkboxes are enabled.
    child_environment.unset(L"GBARECOMP_SWI_LOG");
    child_environment.unset(L"GBARECOMP_BIOS_PC_LOG");
    child_environment.unset(L"GBARECOMP_BIOS_READ_LOG");
    const bool swi_log_enabled = gsr::launcher_test_variable_enabled(
        g_test_variables, false, g_test_swi_log);
    const bool bios_pc_log_enabled = gsr::launcher_test_variable_enabled(
        g_test_variables, false, g_test_bios_pc_log);
    if (swi_log_enabled || bios_pc_log_enabled) {
        const fs::path bios_inventory_dir =
            root / L"logs" / L"bios_inventory";
        std::error_code bios_inventory_ec;
        fs::create_directories(bios_inventory_dir, bios_inventory_ec);
        if (!bios_inventory_ec) {
            if (swi_log_enabled) {
                child_environment.set(
                    L"GBARECOMP_SWI_LOG",
                    (bios_inventory_dir / L"swi.csv").wstring());
            }
            if (bios_pc_log_enabled) {
                child_environment.set(
                    L"GBARECOMP_BIOS_PC_LOG",
                    (bios_inventory_dir / L"bios_pc.csv").wstring());
                // Protected BIOS-region reads ride along: same inventory.
                child_environment.set(
                    L"GBARECOMP_BIOS_READ_LOG",
                    (bios_inventory_dir / L"bios_read.csv").wstring());
            }
        }
    }

    if (logging) {
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
    if (!no_bios) command += L" --bios \"" + bios + L"\"";
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

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> environment_block = child_environment.block();
    if (!CreateProcessW(game.c_str(), mutable_command.data(), nullptr, nullptr,
                        inherit_handles, CREATE_UNICODE_ENVIRONMENT,
                        environment_block.data(), root.c_str(), &startup,
                        &process)) {
        if (out_read) CloseHandle(out_read);
        if (out_write) CloseHandle(out_write);
        if (err_read) CloseHandle(err_read);
        if (err_write) CloseHandle(err_write);
        if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
        MessageBoxW(nullptr, L"Windows could not start the recompiled game.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(process.hThread);

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
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    CloseHandle(process.hProcess);
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
constexpr int kBiosPcLogButton = 1026;
constexpr int kEnhancedOptionsButton = 1027;
constexpr int kTextRecordButton = 1028;
constexpr int kHeadroomProbeButton = 1029;
constexpr int kInstantTextButton = 1030;
// Next free id; nothing already in use is renumbered.
constexpr int kGpuFieldButton = 1041;
constexpr int kGpuFieldOnlyButton = 1042;
constexpr int kEffectTraceButton = 1043;
constexpr int kFrameRewindButton = 1044;
constexpr int kBattleBg1RecordButton = 1045;
constexpr int kWithBiosButton = 1046;

fs::path g_launcher_root;
std::wstring g_launcher_bios;
std::unique_ptr<Gdiplus::Image> g_splash_image;
HFONT g_button_font = nullptr;
HFONT g_body_font = nullptr;
// Backdrop behind the checkbox groups, recomputed each time layout_buttons
// runs. Kept as client-relative window coordinates so paint_splash can draw
// it without recomputing the layout itself.
RECT g_panel_rect{};

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
    HWND instant_text = GetDlgItem(window, kInstantTextButton);
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
    const int instant_text_y = cursor;
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

    // Row order for the diagnostic sub-toggles. Several ids here are
    // deliberately not created (see the commented-out entries in children[]
    // under WM_CREATE), so this list is longer than the visible list -- and
    // rows are therefore packed by how many controls EXIST, not by position
    // in this array. Indexing by array position instead left a blank row for
    // every uncreated id: ten checkboxes spread over seventeen row slots,
    // which pushed the last one off the bottom of the panel and under the
    // action buttons, where it could not be clicked at all. That is how the
    // CPU headroom capture came back empty from three sessions in a row.
    // Grouped by what they are FOR, and laid out in two columns below, so the
    // list reads as four short groups instead of one column of sixteen. The
    // order here is the left column top to bottom, then the right.
    const int child_ids[] = {
        // left: capture something to a file
        kMapRecordButton,      kObjRecordButton,      kTextRecordButton,
        kSwiLogButton,         kBiosPcLogButton,
        // left, continued: traces
        kFunctionTracerButton, kVramMapTraceButton,   kEffectTraceButton,
        kBattleBg1RecordButton,
        // right: rendering
        kRoomBufferButton,     kFrameRewindButton,
        // right, continued: performance
        kHeadroomProbeButton,  kCostProbeButton,      kHostProfButton,
        kPresentCadenceButton, kRamChurnProbeButton,
        // right, last: the one safety toggle, kept apart from the probes,
        // and the BIOS comparison run
        kSelfHealRamButton,    kWithBiosButton,
    };
    const int child_count = static_cast<int>(std::size(child_ids));
    int visible_children = 0;
    for (int i = 0; i < child_count; ++i) {
        if (GetDlgItem(window, child_ids[i])) ++visible_children;
    }

    // Two columns. Sixteen sub-toggles in one column stood 512 pixels tall and
    // pushed the panel past everything else; in two it is half that and the
    // groups above stay together, because the fill is column-major.
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
    if (instant_text) {
        MoveWindow(instant_text, content_x, panel_top + instant_text_y,
                   kContentWidth, kRowHeight, TRUE);
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
    graphics.Clear(Gdiplus::Color(255, 13, 17, 15));

    if (g_splash_image && g_splash_image->GetLastStatus() == Gdiplus::Ok &&
        width > 0 && height > 0) {
        const auto image_width = static_cast<float>(g_splash_image->GetWidth());
        const auto image_height = static_cast<float>(g_splash_image->GetHeight());
        const float scale = std::max(width / image_width, height / image_height);
        const int draw_width = static_cast<int>(image_width * scale);
        const int draw_height = static_cast<int>(image_height * scale);
        const int draw_x = (width - draw_width) / 2;
        const int draw_y = (height - draw_height) / 2;

        Gdiplus::ColorMatrix dim = {
            {{0.52f, 0.00f, 0.00f, 0.00f, 0.00f},
             {0.00f, 0.52f, 0.00f, 0.00f, 0.00f},
             {0.00f, 0.00f, 0.52f, 0.00f, 0.00f},
             {0.00f, 0.00f, 0.00f, 1.00f, 0.00f},
             {0.00f, 0.00f, 0.00f, 0.00f, 1.00f}}};
        Gdiplus::ImageAttributes attributes;
        attributes.SetColorMatrix(&dim);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(g_splash_image.get(),
                           Gdiplus::Rect(draw_x, draw_y, draw_width, draw_height),
                           0, 0, g_splash_image->GetWidth(),
                           g_splash_image->GetHeight(), Gdiplus::UnitPixel,
                           &attributes);
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

    Gdiplus::SolidBrush bottom_overlay(Gdiplus::Color(145, 0, 0, 0));
    graphics.FillRectangle(&bottom_overlay, 0, std::max(0, height - 132),
                           width, 132);
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
    case kWithBiosButton: return &g_test_with_bios;
    case kBattleBg1RecordButton: return &g_test_battle_bg1_record;
    case kRoomBufferButton: return &g_test_room_buffer;
    case kSwiLogButton: return &g_test_swi_log;
    case kBiosPcLogButton: return &g_test_bios_pc_log;
    case kTextRecordButton: return &g_test_text_record;
    case kHeadroomProbeButton: return &g_test_headroom_probe;
    case kEnhancedOptionsButton: return &g_audio_settings.enhanced_options;
    case kInstantTextButton: return &g_audio_settings.instant_text;
    case kGpuFieldButton: return &g_audio_settings.gpu_field;
    case kGpuFieldOnlyButton: return &g_audio_settings.gpu_field_only;
    case kNativeMp2kButton: return &g_audio_settings.native_mp2k;
    case kTurboAudioButton: return &g_audio_settings.turbo_decoupled;
    case kCopySessionIdButton:
        return &g_audio_settings.copy_session_id_to_clipboard;
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
        g_test_bios_pc_log = k_launcher_test_defaults.bios_pc_log;
        CreateWindowExW(0, L"BUTTON", L"Pick ROM",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kPickRomButton),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"Quit",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kQuitButton),
                        GetModuleHandleW(nullptr), nullptr);
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
        CreateWindowExW(0, L"BUTTON", L"Instant text (unused: now in-game)",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                        0, 0, 0, 0, window,
                        reinterpret_cast<HMENU>(kInstantTextButton),
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
            {kRoomBufferButton, L"Room buffer self-check"},
            // F12 saves the frames shown just before it, not only the
            // current one: a one-frame flicker is gone before F12 lands.
            {kFrameRewindButton, L"Rewind F12 (keep last 2 s)"},
            {kSwiLogButton, L"Record BIOS SWI calls"},
            {kBiosPcLogButton, L"Record BIOS PC inventory"},
            // Normal launches run no BIOS code. This passes the BIOS for
            // a comparison run in a GBARECOMP_LINK_BIOS=ON build.
            {kWithBiosButton, L"Run with real BIOS (comparison build)"},
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
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        paint_splash(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
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
        const bool quit = item->CtlID == kQuitButton;
        const COLORREF fill = quit ? RGB(92, 42, 37) : RGB(180, 127, 35);
        const COLORREF border = quit ? RGB(202, 120, 95) : RGB(255, 220, 112);
        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(item->hDC, &item->rcItem, brush);
        DeleteObject(brush);
        HBRUSH border_brush = CreateSolidBrush(border);
        FrameRect(item->hDC, &item->rcItem, border_brush);
        DeleteObject(border_brush);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, RGB(255, 246, 215));
        HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, g_button_font));
        RECT text_rect = item->rcItem;
        if (item->itemState & ODS_SELECTED) OffsetRect(&text_rect, 1, 1);
        DrawTextW(item->hDC, quit ? L"Quit" : L"Pick ROM", -1, &text_rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, old_font);
        if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &item->rcItem);
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
            const std::wstring rom = choose_rom(g_launcher_root);
            if (!rom.empty() &&
                run_game(g_launcher_root, rom, g_launcher_bios, window) == 0) {
                DestroyWindow(window);  // no-op if run_game already destroyed it
            }
            return 0;
        }
        break;
    case WM_CLOSE:
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

int show_launcher(const fs::path& root, const std::wstring& bios) {
    Gdiplus::GdiplusStartupInput startup_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &startup_input, nullptr) !=
        Gdiplus::Ok) {
        MessageBoxW(nullptr, L"The splash screen could not start.",
                    L"Golden Sun Recompiled", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_launcher_root = root;
    g_launcher_bios = bios;
    g_audio_settings = load_launcher_audio_settings(root);
    g_strict_static_route = inherited_environment_truthy(
        L"GBARECOMP_STRICT_STATIC");
    g_splash_image = std::make_unique<Gdiplus::Image>(
        (root / L"gssplash.jpg").c_str());
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
    RECT desired{0, 0, 720, 880};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                 WS_MINIMIZEBOX, FALSE, WS_EX_APPWINDOW);
    HWND window = CreateWindowExW(
        WS_EX_APPWINDOW, class_name, L"Golden Sun Recompiled",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, desired.right - desired.left,
        desired.bottom - desired.top, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!window) {
        if (g_button_font) DeleteObject(g_button_font);
        g_button_font = nullptr;
        if (g_body_font) DeleteObject(g_body_font);
        g_body_font = nullptr;
        g_splash_image.reset();
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
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

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
    // May be empty: a no-BIOS launch needs none, and run_game reports a
    // missing BIOS only when one is actually required.
    const std::wstring bios = read_json_string(root / L"config" / L"local.json",
                                               "bios");
    const bool developer_auto_launch =
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
            const int result = run_game(root, cached_rom, bios, nullptr);
            gbarecomp::crash_handler_mark_clean_exit();
            return result;
        }
    }
    SetProcessDPIAware();
    const int result = show_launcher(root, bios);
    gbarecomp::crash_handler_mark_clean_exit();
    return result;
}
