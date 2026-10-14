#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Shared collection storage. External imports preserve originals and existing
// progress. Legacy progress beside managed ROMs is verified and moved to Saves.
namespace gsr_collection {
namespace fs = std::filesystem;

struct ManagedFiles {
    fs::path root, rom, save, state_base;
};

namespace storage_detail {
inline std::string lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
inline std::string text(const fs::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}
inline bool path_present(const fs::path& path) {
    return fs::exists(fs::symlink_status(path));
}
inline bool same_bytes(const fs::path& a, const fs::path& b) {
    if (!fs::is_regular_file(a) || !fs::is_regular_file(b)) return false;
    if (fs::equivalent(a, b)) return true;
    if (fs::file_size(a) != fs::file_size(b)) return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    if (!left || !right) throw std::runtime_error("Cannot read files for comparison: " + text(a) + " / " + text(b));
    std::array<char, 65536> l{}, r{};
    do {
        left.read(l.data(), l.size()); right.read(r.data(), r.size());
        if (left.bad() || right.bad()) throw std::runtime_error("Cannot read files for comparison: " + text(a) + " / " + text(b));
        if (left.gcount() != right.gcount() || !std::equal(l.begin(), l.begin() + left.gcount(), r.begin())) return false;
    } while (left.gcount());
    return true;
}
inline fs::path unused_path(const fs::path& base) {
    if (!path_present(base)) return base;
    for (unsigned long long i = 1; ; ++i) {
        fs::path candidate = base.parent_path() / (base.stem().native() + fs::path(" (" + std::to_string(i) + ")").native() + base.extension().native());
        if (!path_present(candidate)) return candidate;
    }
}
// Stage the entire copy before changing the destination. On Windows rename
// cannot replace an existing file, so retain a recoverable backup until commit.
inline void copy_preserving_time(const fs::path& source, const fs::path& destination, bool replace = false) {
    if (path_present(destination) &&
        (!fs::is_regular_file(destination) || fs::is_symlink(fs::symlink_status(destination))))
        throw std::runtime_error("Expected a regular managed file: " + text(destination));
    if (path_present(destination) && same_bytes(source, destination)) return;
    if (path_present(destination) && !replace) throw std::runtime_error("File already exists: " + text(destination));
    fs::create_directories(destination.parent_path());
    const auto temporary = unused_path(fs::path(destination.native() + fs::path(".collection.tmp").native()));
    fs::path backup;
    try {
        fs::copy_file(source, temporary, fs::copy_options::none);
        fs::last_write_time(temporary, fs::last_write_time(source));
        if (!same_bytes(source, temporary)) throw std::runtime_error("Copy verification failed: " + text(temporary));
        if (path_present(destination)) {
            if (!replace) throw std::runtime_error("File already exists: " + text(destination));
            backup = unused_path(fs::path(destination.native() + fs::path(".collection.backup").native()));
            fs::rename(destination, backup);
        }
        try { fs::rename(temporary, destination); }
        catch (...) {
            if (!backup.empty()) {
                std::error_code ec;
                fs::rename(backup, destination, ec);
                if (ec) throw std::runtime_error("Cannot restore " + text(destination) + "; previous file preserved at " + text(backup));
            }
            throw;
        }
        if (!backup.empty()) fs::remove(backup);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}
inline bool native_slot(const std::string& suffix) {
    return suffix.size() > 6 && suffix.compare(0, 6, ".state") == 0 &&
        std::all_of(suffix.begin() + 6, suffix.end(), [](char c) { return c >= '0' && c <= '9'; });
}
inline bool progress_suffix(const std::string& suffix) {
    return suffix == ".sav" || suffix == ".state" || suffix == ".sgm" || native_slot(suffix) ||
        (suffix.size() == 4 && suffix.compare(0, 3, ".ss") == 0 && suffix[3] >= '0' && suffix[3] <= '9');
}
inline std::string associated_suffix(const fs::path& file, const fs::path& rom) {
    const auto name = lower(text(file.filename()));
    // Longest prefix first: handle both Game.state0 and Game.gba.state0.
    for (const auto& prefix : {lower(text(rom.filename())), lower(text(rom.stem()))}) {
        if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
            const auto suffix = name.substr(prefix.size());
            if (progress_suffix(suffix)) return suffix;
        }
    }
    return {};
}
inline std::vector<fs::path> files_in(const fs::path& directory) {
    std::vector<fs::path> files;
    if (!path_present(directory)) return files;
    if (!fs::is_directory(directory)) throw std::runtime_error("Expected folder: " + text(directory));
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.is_regular_file()) files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}
inline fs::path preserve_conflict(const fs::path& source, const ManagedFiles& files) {
    const auto directory = files.root / "Saves" / "Imported" / files.state_base.filename();
    fs::create_directories(directory);
    const auto filename = lower(text(source.filename()));
    const auto prefix = lower(text(source.stem())) + " (";
    const auto extension = lower(text(source.extension()));
    for (const auto& previous : files_in(directory)) {
        const auto stem = lower(text(previous.stem()));
        const bool numbered = lower(text(previous.extension())) == extension && stem.size() > prefix.size() + 1 &&
            stem.compare(0, prefix.size(), prefix) == 0 && stem.back() == ')' &&
            std::all_of(stem.begin() + prefix.size(), stem.end() - 1, [](char c) { return c >= '0' && c <= '9'; });
        if ((lower(text(previous.filename())) == filename || numbered) && same_bytes(source, previous)) return previous;
    }
    const auto saved = unused_path(directory / source.filename());
    copy_preserving_time(source, saved);
    if (!same_bytes(source, saved)) throw std::runtime_error("Import verification failed: " + text(saved));
    return saved;
}
inline std::vector<fs::path> associated_directories(const fs::path& source) {
    std::vector<fs::path> result{source.parent_path()};
    std::vector<fs::path> parents{source.parent_path()};
    const auto folder = lower(text(source.parent_path().filename()));
    if (folder == "roms" || folder == "rom") parents.push_back(source.parent_path().parent_path());
    for (const auto& parent : parents) {
        bool found = false;
        for (const auto& entry : fs::directory_iterator(parent)) {
            const auto name = lower(text(entry.path().filename()));
            if ((name == "saves" || name == "states" || name == "savestates") && entry.is_directory()) {
                result.push_back(entry.path()); found = true;
            }
        }
        if (found) break;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
} // namespace storage_detail

inline ManagedFiles managed_paths(const fs::path& root, const std::string& game_stem) {
    if (root.empty()) throw std::runtime_error("Cannot locate the collection launcher folder.");
    if (game_stem != "The Broken Seal" && game_stem != "The Lost Age")
        throw std::runtime_error("Unknown collection game: " + game_stem);
    const auto absolute = fs::absolute(root).lexically_normal();
    return {absolute, absolute / "ROMs" / (game_stem + ".gba"), absolute / "Saves" / (game_stem + ".sav"), absolute / "Saves" / game_stem};
}

inline fs::path infer_collection_root(const fs::path& child_root) {
    using namespace storage_detail;
    if (child_root.empty()) throw std::runtime_error("Cannot locate the collection launcher folder from an empty game path.");
    auto child = fs::absolute(child_root).lexically_normal();
    if (fs::is_regular_file(child)) child = child.parent_path();
    const auto name = lower(text(child.filename()));
    if (name == "the broken seal" || name == "the lost age") {
        const auto parent = child.parent_path();
        if (lower(text(parent.filename())) == "games") return parent.parent_path();
        return parent;
    }
    for (auto candidate = child; !candidate.empty(); candidate = candidate.parent_path()) {
        if (fs::is_directory(candidate / "The Broken Seal") && fs::is_directory(candidate / "The Lost Age") &&
            (fs::is_regular_file(candidate / "GoldenSunLauncher.exe") || fs::is_regular_file(candidate / "GoldenSunCollection") ||
             fs::is_regular_file(candidate / "GoldenSunLauncher"))) return candidate;
        if (candidate == candidate.parent_path()) break;
    }
    throw std::runtime_error("Cannot locate the collection launcher folder from " + text(child_root));
}

inline bool sync_legacy_states(const ManagedFiles& files, std::string& error) {
    using namespace storage_detail;
    error.clear();
    try {
        const auto expected = managed_paths(files.root, text(files.state_base.filename()));
        if (files.rom.lexically_normal() != expected.rom || files.state_base.lexically_normal() != expected.state_base)
            throw std::runtime_error("Invalid managed ROMs or Saves path.");
        const auto rom_directory = files.root / "ROMs";
        if (!path_present(rom_directory)) return true;
        if (!fs::is_directory(rom_directory) || fs::is_symlink(fs::symlink_status(rom_directory)))
            throw std::runtime_error("Expected a regular ROMs folder: " + text(rom_directory));
        const auto resolved_roms = fs::canonical(rom_directory);
        for (const auto& source : files_in(files.rom.parent_path())) {
            const auto suffix = associated_suffix(source, files.rom);
            if (suffix.empty() || fs::is_symlink(fs::symlink_status(source))) continue;
            if (fs::canonical(source).parent_path() != resolved_roms) continue;
            const auto source_time = fs::last_write_time(source);
            const auto target = fs::path(files.state_base.native() + fs::path(suffix).native());
            fs::path verified_copy;
            if (path_present(target)) {
                if (!fs::is_regular_file(target) || fs::is_symlink(fs::symlink_status(target)))
                    throw std::runtime_error("Expected a regular managed save: " + text(target));
                if (same_bytes(source, target)) {
                    if (source_time > fs::last_write_time(target)) fs::last_write_time(target, source_time);
                    verified_copy = target;
                } else if (source_time > fs::last_write_time(target)) {
                    // Preserve the previous canonical progress before adopting
                    // the newer legacy file, then verify before removing it.
                    preserve_conflict(target, files);
                    copy_preserving_time(source, target, true);
                    verified_copy = target;
                } else {
                    verified_copy = preserve_conflict(source, files);
                }
            } else {
                copy_preserving_time(source, target);
                verified_copy = target;
            }
            if (!same_bytes(source, verified_copy) || fs::last_write_time(source) != source_time)
                throw std::runtime_error("Legacy progress changed during migration; original retained: " + text(source));
            // Only managed, regular, associated files may be removed. Recheck
            // scope and type immediately before this final migration step.
            const auto status = fs::symlink_status(source);
            if (!fs::is_regular_file(status) || fs::is_symlink(status) || fs::canonical(source).parent_path() != resolved_roms)
                throw std::runtime_error("Legacy progress changed during migration; original retained: " + text(source));
            if (!fs::remove(source)) throw std::runtime_error("Cannot remove migrated legacy file: " + text(source));
        }
        return true;
    } catch (const std::exception& e) { error = "Cannot move legacy progress to Saves: " + std::string(e.what()); return false; }
}

// Caller validates the selected ROM's game identity before importing it.
inline bool import_selected_rom(const fs::path& root, const fs::path& selected,
                                const std::string& game_stem, ManagedFiles& out, std::string& error) {
    using namespace storage_detail;
    error.clear();
    try {
        const auto source = fs::absolute(selected).lexically_normal();
        out = managed_paths(root, game_stem);
        if (lower(text(source.extension())) != ".gba" || !fs::is_regular_file(source))
            throw std::runtime_error("Select an existing .gba ROM: " + text(source));
        if (path_present(out.rom) && (!fs::is_regular_file(out.rom) || fs::is_symlink(fs::symlink_status(out.rom)) || !same_bytes(source, out.rom)))
            throw std::runtime_error("A different ROM already exists at " + text(out.rom) + ". Move it aside before selecting this ROM.");
        if (!path_present(out.rom)) copy_preserving_time(source, out.rom);
        fs::create_directories(out.root / "Saves");
        // Move any old working progress out of managed ROMs; all engines now
        // save directly in Saves and nothing is copied back beside the ROM.
        if (!sync_legacy_states(out, error)) return false;
        for (const auto& directory : associated_directories(source)) {
            for (const auto& progress : files_in(directory)) {
                const auto suffix = associated_suffix(progress, source);
                if (suffix.empty()) continue;
                const auto target = fs::path(out.state_base.native() + fs::path(suffix).native());
                if (!path_present(target)) copy_preserving_time(progress, target);
                else if (!same_bytes(progress, target)) preserve_conflict(progress, out);
            }
        }
        return true;
    } catch (const std::exception& e) { error = "Cannot import game files: " + std::string(e.what()); return false; }
}
} // namespace gsr_collection
