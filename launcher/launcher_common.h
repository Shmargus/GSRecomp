// Shared file/report helpers for the collection's Windows and Linux sessions.
// Both games use these helpers; platform-specific session windows stay separate.
//
// Two things are defined elsewhere, inside namespace gsr_launcher: sha1_file
// (collection_main.cpp) and scrub_user_name (each platform's session, which
// knows where the account name lives).
#pragma once
#include "launcher_game.h"
#include "collection_storage.h"
#include "../engine/src/armv4t/any_rom.h"
#include "../engine/src/gba/file_replace.h"
#include "../player_builder/build_record.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gsr_launcher {

namespace fs = std::filesystem;

constexpr int kKeepLogCount = 20;  // recent sessions kept; older ones pruned

#ifdef _WIN32
constexpr char kBuilderExe[] = "gsr_builder.exe";
constexpr char kRecompilerExe[] = "gba_recompile.exe";
#else
constexpr char kBuilderExe[] = "gsr_builder";
constexpr char kRecompilerExe[] = "gba_recompile";
#endif

// Defined outside this header (see the top).
bool sha1_file(const fs::path& path, std::string* out);
std::string scrub_user_name(std::string text);

// The whole file, or "" when it cannot be opened.
inline std::string read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    return {std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
}

// Whether `path` is a supported ROM (with `language` >= 0, only that one of
// game.rom_sha1s), or with the launcher's "Skip the ROM check" on, any
// readable file. The error text is for the player.
inline bool validate_rom(const fs::path& path, std::string* error,
                         const GameIdentity& game = session_game(),
                         bool any_rom = gbarecomp::any_rom_allowed(),
                         int language = -1) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        *error = "That file could not be opened.";
        return false;
    }
    if (any_rom) {
        if (std::ifstream file(path, std::ios::binary); file) return true;
        *error = "The ROM could not be read.";
        return false;
    }
    std::string actual;
    if (!sha1_file(path, &actual)) {
        *error = "The ROM could not be read.";
        return false;
    }
    if (!accepts_rom(game, actual)) {
        *error = std::string("That is not a supported ") + game.title +
                 " ROM.\n\nExpected SHA-1:";
        for (const char* accepted : game.rom_sha1s)
            if (accepted) *error += std::string("\n") + accepted;
        *error += "\n\nFound SHA-1:\n" + actual;
        return false;
    }
    if (language >= 0 && actual != game.rom_sha1s[language]) {
        int found = 0;
        while (actual != game.rom_sha1s[found]) ++found;  // accepts_rom checked above
        *error = std::string("That is the ") + game.rom_names[found] + " ROM, but the flag under " +
                 game.title + " is set to " + game.rom_names[language] +
                 ".\n\nClick the flag to switch, or choose the " + game.rom_names[language] + " ROM.";
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

// Both games' F12 capture folders, by name. Each session scans its own logs.
inline std::set<fs::path::string_type> rewind_dirs(const fs::path& logs_dir) {
    std::set<fs::path::string_type> dirs;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(logs_dir, ec)) {
        if (entry.is_directory(ec) &&
            (name_starts_with(entry.path(), "gpu_rewind_") ||
             name_starts_with(entry.path(), "tla_rewind_")))
            dirs.insert(entry.path().filename().native());
    }
    return dirs;
}

// Copies a log or settings file into the report, user name taken out. A
// very long session log keeps its first 256 KB and its last 3 MB: the start
// says how the game was set up, the end is where the problem is.
inline bool copy_into_report(const fs::path& from, const fs::path& staging,
                             bool optional = true) {
    std::error_code ec;
    const auto status = fs::status(from, ec);
    if (status.type() == fs::file_type::not_found)
        return optional && (!ec || ec == std::errc::no_such_file_or_directory);
    if (ec || !fs::is_regular_file(status)) return false;
    std::string ext = from.extension().string();
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const bool text = ext == ".log" || ext == ".txt" || ext == ".ini" ||
                      ext == ".csv";
    if (!text) {
        return fs::copy_file(from, staging / from.filename(),
                             fs::copy_options::overwrite_existing, ec);
    }
    std::ifstream in(from, std::ios::binary | std::ios::ate);
    const auto size = in.tellg();
    if (size < 0 || !in.seekg(0)) return false;
    std::string content(static_cast<std::size_t>(size), '\0');
    if (!in.read(content.data(), static_cast<std::streamsize>(content.size())))
        return false;
    constexpr std::size_t kHead = 256 * 1024, kTail = 3 * 1024 * 1024;
    if (content.size() > kHead + kTail) {
        content = content.substr(0, kHead) +
                  "\n\n[... the middle of this log was left out of the bug report ...]\n\n" +
                  content.substr(content.size() - kTail);
    }
    std::ofstream out(staging / from.filename(), std::ios::binary | std::ios::trunc);
    out << scrub_user_name(content);
    out.close();
    return static_cast<bool>(out);
}

// report_info.txt: `system` names the operating system and its version.
inline std::string report_info_text(const std::string& system, std::size_t captures,
                                    bool crashed, bool unpacker_caught,
                                    unsigned long exit_code) {
    return std::string("Golden Sun Recompiled bug report\n") +
           "Game: " + session_game().stem + "\n" +
           "Launcher built: " + __DATE__ + " " + __TIME__ + "\n" +
           system + "\n" +
           "F12 captures: " + std::to_string(captures) + "\n" +
           "Game crashed: " + (crashed ? "yes" : "no") + "\n" +
           "Unpacker crash caught: " + (unpacker_caught ? "yes" : "no") + "\n" +
           "Game exit code: " + std::to_string(exit_code) + "\n";
}

struct ReportSources {
    fs::path root, game_dir, crash_dir, session_log, save;
    bool crashed = false;
};

struct StagedReport {
    fs::path folder;
    std::string errors;
};

inline StagedReport stage_bug_report(const ReportSources& sources, std::string info) {
    const auto logs = sources.root / "logs";
    const auto reports = logs / "bug_reports";
    const auto stem = sources.session_log.stem().native();
    const auto prefix = fs::path("session_").native();
    const auto suffix = stem.starts_with(prefix) ? stem.substr(prefix.size()) : stem;
    fs::path name("bug_report_");
    name += suffix.empty() ? fs::path("latest").native() : suffix;
    StagedReport staged{reports / name, {}};
    std::error_code ec;
    fs::create_directories(reports, ec);
    if (!ec) {
        for (unsigned retry = 1; !fs::create_directory(staged.folder, ec) && !ec; ++retry)
            staged.folder = reports / (fs::path(name).concat("_retry" + std::to_string(retry)));
    }
    if (ec) return {logs, "Could not create the report folder: " + ec.message() + "\n"};

    const auto copy = [&](const fs::path& file, bool optional = true) {
        if (!copy_into_report(file, staged.folder, optional))
            staged.errors += "Could not include " + gsr_collection::storage_detail::text(file.filename()) + "\n";
    };
    if (!sources.session_log.empty()) {
        copy(sources.session_log, false);
        for (fs::directory_iterator it(logs, ec), end; it != end && !ec; it.increment(ec)) {
            // Files written beside the log under its name; folders are not logs.
            const auto& path = it->path();
            std::error_code type_ec;
            if (path.filename() != sources.session_log.filename() &&
                path.filename().native().starts_with(stem) &&
                it->is_regular_file(type_ec))
                copy(path, false);
        }
        if (ec) staged.errors += "Could not list all session logs\n";
    }
    copy(logs / "launcher.log");
    if (!sources.save.empty()) copy(sources.save);
    for (const char* file : {"config.ini", "game_options.ini", "keybinds.ini", "run_state.txt"})
        copy(sources.game_dir / file);
    copy(sources.game_dir / session_game().build_stamp);
    // Crash files belong to the program that was running during a hand-over.
    if (sources.crashed) {
        copy(sources.crash_dir / "crash_report.txt", false);
        for (const char* file : {"crash_dump.dmp", "crash_memory.bin",
                                 "crash_trail.csv", "crash_context.txt"})
            copy(sources.crash_dir / file);
    }
    for (const char* file : {"unpacker_catch.txt", "unpacker_catch.bin",
                             "unpacker_recovered.txt", "unpacker_recovered.bin",
                             "unpacker_journal.csv"})
        copy(sources.root / file);

    if (!staged.errors.empty()) info += "\nReport incomplete\n" + staged.errors;
    std::ofstream summary(staged.folder / "report_info.txt", std::ios::binary);
    summary << info;
    summary.close();
    if (!summary) staged.errors += "Could not write report_info.txt\n";
    return staged;
}

// What the game code in a release is built from: its release version, then
// one "path sha1" line per file: the engine, the builder, the translator and everything in
// builder/data and builder/engine. Written beside the game library after a
// build (GoldenSunGame.release.txt); any difference, such as a new release
// unzipped over an old one, means building again. The engine check in
// game_code_ready already caught a new engine; this also catches a release
// that changes only the builder, the translator or its data. The toolchain is
// left out: it is large, and it only changes together with the builder.
inline std::string release_fingerprint(const fs::path& root,
                                       const GameIdentity& game = session_game()) {
    std::vector<fs::path> files = {root / game.executable};
    if (std::string_view(game.engine) != game.executable) files.push_back(root / game.engine);
    files.insert(files.end(), {root / "builder" / kBuilderExe,
                               root / "builder" / kRecompilerExe});
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
#ifdef GSR_RELEASE_VERSION
    std::string text = "release_version=" GSR_RELEASE_VERSION "\n";
#else
    std::string text = "release_version=dev\n";
#endif
    for (const fs::path& file : files) {
        std::string digest;
        if (!sha1_file(file, &digest)) digest = "missing";
        text += fs::relative(file, root, ec).generic_string() + " " + digest + "\n";
    }
    return text;
}

// The ROM the game code must be built from: the copy this collection plays
// for the chosen language (The Broken Seal's flag) when it is that one (or
// with the ROM check off, any), so switching language asks for a rebuild.
// Otherwise that language's ROM, or with the ROM check off, empty.
inline std::string required_rom_sha1(const fs::path& root, const GameIdentity& game,
                                     bool any_rom) {
    std::string digest;
    int language = 0;
    try {
        const auto collection = gsr_collection::infer_collection_root(root);
        language = gsr_collection::rom_language(collection,&game == &kGames[1]);
        const auto rom = gsr_collection::managed_paths(collection, game.stem).rom;
        if (sha1_file(rom, &digest) && (any_rom || digest == game.rom_sha1s[language]))
            return digest;
    } catch (const std::exception&) {}
    return any_rom ? std::string{} : game.rom_sha1s[language];
}

inline bool player_build_inputs_match(const fs::path& root, const GameIdentity& game,
                                      const gsr_player::BuildRecord& record,
                                      const std::string& rom_sha1) {
    std::string engine_now;
    return !rom_sha1.empty() && record.rom_sha1 == rom_sha1 &&
           record.builder == gsr_player::kBuilderVersion &&
           sha1_file(root / game.engine, &engine_now) && record.engine_sha1 == engine_now;
}

inline bool player_library_exists(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && fs::file_size(path, ec) > 0 && !ec;
}

// Call only after the builder has exited successfully and reported completion.
// A launcher-only update can still be paired with a version-2 builder whose
// freshly written record predates the library checksum.
inline bool finish_player_build_record(const fs::path& root, const GameIdentity& game,
                                       const fs::path& rom, std::string* error) {
    const auto path = root / game.build_stamp;
    auto record = gsr_player::read_build_record(read_text(path));
    const auto library = root / game.library;
    std::string rom_sha1, library_sha1;
    if (!player_library_exists(library) || !sha1_file(rom, &rom_sha1) ||
        !player_build_inputs_match(root, game, record, rom_sha1) ||
        !sha1_file(library, &library_sha1)) {
        *error = "The new game library or its build record could not be verified.";
        return false;
    }
    if (record.library_sha1) {
        if (*record.library_sha1 == library_sha1) return true;
        *error = "The new game library does not match its build record.";
        return false;
    }
    record.library_sha1 = std::move(library_sha1);
    const auto text = gsr_player::write_build_record(record);
    return gba::replace_file(path, text.data(), text.size(), error);
}

// Whether the installed library matches the build from this release and ROM.
inline bool game_code_ready(const fs::path& root, const GameIdentity& game = session_game(),
                            bool any_rom = gbarecomp::any_rom_allowed()) {
    const auto library = root / game.library;
    if (!player_library_exists(library)) return false;
    const auto record = gsr_player::read_build_record(read_text(root / game.build_stamp));
    if (!record.library_sha1 || record.library_sha1->empty()) return false;
    if (read_text(root / game.release_stamp) != release_fingerprint(root, game))
        return false;
    std::string library_now;
    return player_build_inputs_match(root, game, record, required_rom_sha1(root, game, any_rom)) &&
           sha1_file(library, &library_now) &&
           *record.library_sha1 == library_now;
}

enum class PreparationState { Ready, FirstBuild, Rebuild };
inline PreparationState preparation_state(const fs::path& root, const GameIdentity& game = session_game()) {
    if (game_code_ready(root, game)) return PreparationState::Ready;
    std::error_code ec;
    return fs::exists(root / game.build_stamp, ec) || fs::exists(root / game.release_stamp, ec)
        ? PreparationState::Rebuild : PreparationState::FirstBuild;
}
inline std::string preparation_prompt_text(PreparationState state) {
    if (state == PreparationState::Ready) return {};
    return std::string(state == PreparationState::FirstBuild
        ? "Build this game for your first play?"
        : "Build this game for this version?") +
        "\n\nThis may take a few minutes. Your saves and progress will be kept.";
}

}  // namespace gsr_launcher
