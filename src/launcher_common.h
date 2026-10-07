// launcher_common.h: the file and report helpers the Windows launcher
// (launcher_main.cpp) and the Linux launcher (launcher_linux.cpp) share. Each
// launcher keeps its own window code; what has no platform in it lives here
// once, so the two cannot drift apart.
//
// Two things are platform code and are defined by each launcher, inside
// namespace gsr_launcher: sha1_file (BCrypt on Windows, gba::sha1 on Linux)
// and scrub_user_name (it knows where the account name lives).
#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace gsr_launcher {

namespace fs = std::filesystem;

constexpr char kExpectedRomSha1[] = "5c4695205413df7db52b9a184815a07783999971";
constexpr char kBuilderVersion[] = "2";  // tools/gsr_builder kBuilderVersion
constexpr int kKeepLogCount = 20;  // recent sessions kept; older ones pruned

#ifdef _WIN32
constexpr char kGameExe[] = "GoldenSunRecomp.exe";
constexpr char kGameLibrary[] = "GoldenSunGame.dll";
constexpr char kBuilderExe[] = "gsr_builder.exe";
constexpr char kRecompilerExe[] = "gba_recompile.exe";
#else
constexpr char kGameExe[] = "GoldenSunRecomp";
constexpr char kGameLibrary[] = "libGoldenSunGame.so";
constexpr char kBuilderExe[] = "gsr_builder";
constexpr char kRecompilerExe[] = "gba_recompile";
#endif

// Defined by each launcher.
bool sha1_file(const fs::path& path, std::string* out);
std::string scrub_user_name(std::string text);

// The whole file, or "" when it cannot be opened.
inline std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

// Whether `path` is the supported ROM. The error text is for the player.
inline bool validate_rom(const fs::path& path, std::string* error) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        *error = "That file could not be opened.";
        return false;
    }
    std::string actual;
    if (!sha1_file(path, &actual)) {
        *error = "The ROM could not be read.";
        return false;
    }
    if (actual != kExpectedRomSha1) {
        *error = std::string("That is not the required Golden Sun USA/Europe ROM.\n\n"
                             "Expected SHA-1:\n") + kExpectedRomSha1 +
                 "\n\nFound SHA-1:\n" + actual;
        return false;
    }
    return true;
}

// Replaces each needle with its replacement, matching case-insensitively,
// and only the whole folder name: it must be followed by the end of the text
// or one of \ / " ' space CR LF ("Jim" must not match "Jimmy").
inline std::string scrub_paths(
    std::string text,
    const std::vector<std::pair<std::string, std::string>>& swaps) {
    auto lower = [](std::string s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    };
    for (const auto& [raw_needle, replacement] : swaps) {
        const std::string needle = lower(raw_needle);
        std::string folded = lower(text);
        std::size_t at = 0;
        while ((at = folded.find(needle, at)) != std::string::npos) {
            const std::size_t after = at + needle.size();
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
    return text;
}

// Whether a file name starts with `prefix`, compared in the platform's own
// characters: string() would throw on Windows for a name it cannot convert.
inline bool name_starts_with(const fs::path& path, const char* prefix) {
    return path.filename().native().rfind(fs::path(prefix).native(), 0) == 0;
}

// Keeps the newest (kKeepLogCount - 1) existing logs so this session's new
// file brings the total back up to kKeepLogCount. session_YYYYMMDD_HHMMSS
// sorts lexicographically in chronological order, so no parsing is needed.
inline void prune_old_logs(const fs::path& logs_dir) {
    std::error_code ec;
    std::vector<fs::path> logs;
    for (const auto& entry : fs::directory_iterator(logs_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const fs::path& path = entry.path();
        if (name_starts_with(path, "session_") &&
            path.extension() == ".log")
            logs.push_back(path);
    }
    if (logs.size() < static_cast<std::size_t>(kKeepLogCount)) return;
    std::sort(logs.begin(), logs.end());
    const std::size_t remove_count =
        logs.size() - (static_cast<std::size_t>(kKeepLogCount) - 1);
    for (std::size_t i = 0; i < remove_count; ++i) {
        const fs::path& log = logs[i];
        fs::path events = log;
        events.replace_extension(".events.csv");
        // The files that belong to one session: the log itself, the frame
        // timing CSVs and their probes, and the recorded input.
        fs::remove(log, ec);
        fs::remove(events, ec);
        fs::remove(fs::path(events).concat(".misses.csv"), ec);
        fs::remove(fs::path(events).concat(".recursion.csv"), ec);
        fs::remove(fs::path(events).concat(".ram-churn.csv"), ec);
        fs::remove(fs::path(log).replace_extension(".phase.csv"), ec);
        fs::remove(fs::path(log).replace_extension(".input"), ec);
        for (unsigned suffix = 1; suffix < 1000; ++suffix) {
            fs::remove(fs::path(log).replace_extension("").concat(
                           "_" + std::to_string(suffix) + ".input"), ec);
        }
    }
}

// The F12 capture folders (logs/gpu_rewind_*) in `logs_dir`, by name.
inline std::set<fs::path::string_type> rewind_dirs(const fs::path& logs_dir) {
    std::set<fs::path::string_type> dirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(logs_dir, ec)) {
        if (entry.is_directory(ec) &&
            name_starts_with(entry.path(), "gpu_rewind_"))
            dirs.insert(entry.path().filename().native());
    }
    return dirs;
}

// Copies a log or settings file into the report, user name taken out. A
// very long session log keeps its first 256 KB and its last 3 MB: the start
// says how the game was set up, the end is where the problem is.
inline void copy_into_report(const fs::path& from, const fs::path& staging) {
    std::error_code ec;
    if (!fs::is_regular_file(from, ec)) return;
    std::string ext = from.extension().string();
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const bool text = ext == ".log" || ext == ".txt" || ext == ".ini" ||
                      ext == ".csv";
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

// What the game code in a release is built from, one "path sha1" line per
// file: the engine, the builder, the translator and everything in
// builder/data and builder/engine. Written beside the game library after a
// build (GoldenSunGame.release.txt); any difference, such as a new release
// unzipped over an old one, means building again. The engine check in
// game_code_ready already caught a new engine; this also catches a release
// that changes only the builder, the translator or its data. The toolchain is
// left out: it is large, and it only changes together with the builder.
inline std::string release_fingerprint(const fs::path& root) {
    std::vector<fs::path> files = {root / kGameExe,
                                   root / "builder" / kBuilderExe,
                                   root / "builder" / kRecompilerExe};
    std::error_code ec;
    for (const char* dir : {"data", "engine"}) {
        std::vector<fs::path> found;
        for (const auto& entry :
             fs::recursive_directory_iterator(root / "builder" / dir, ec)) {
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

// Whether the game library beside the game was built from the supported ROM
// by this release's builder against this release's engine.
inline bool game_code_ready(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_regular_file(root / kGameLibrary, ec)) return false;
    if (read_text(root / "GoldenSunGame.release.txt") != release_fingerprint(root))
        return false;
    std::istringstream in(read_text(root / "GoldenSunGame.build.txt"));
    std::string line, rom, builder, engine;
    while (std::getline(in, line)) {
        if (line.rfind("rom_sha1=", 0) == 0) rom = line.substr(9);
        else if (line.rfind("builder=", 0) == 0) builder = line.substr(8);
        else if (line.rfind("engine_sha1=", 0) == 0) engine = line.substr(12);
    }
    std::string engine_now;
    if (!sha1_file(root / kGameExe, &engine_now)) return false;
    return rom == kExpectedRomSha1 && builder == kBuilderVersion &&
           engine == engine_now;
}

}  // namespace gsr_launcher
