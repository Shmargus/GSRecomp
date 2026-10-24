#pragma once
#include "launcher_common.h"
#include "collection_preferences.h"
#include <algorithm>
#include <atomic>
#include <map>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace gsr_collection {
namespace fs = std::filesystem;

// These paths are development inputs, never player ROMs, progress or settings.
// The build writer reads the same rules, so additions/deletions also invalidate
// a record. Contents, rather than timestamps, identify the build.
inline bool ignored_build_path(const fs::path& path) {
    for (const auto& part : path) {
        const auto name = part.generic_string();
        if ((!name.empty() && name.front() == '.') ||
            name.find(".bak") != std::string::npos || name == "__pycache__") return true;
    }
    return false;
}

inline std::string build_path_text(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline bool developer_build_ready(const fs::path& game_root, int choice,
                                  const fs::path& engine, std::stop_token stop = {}) {
    try {
        const auto record = gsr_launcher::read_text(engine.parent_path() / "collection-build.txt");
        if (record.rfind("developer_build=1\n", 0) != 0) return false;
        const auto root = fs::canonical(game_root.parent_path());
        std::map<std::string, std::string> files;
        std::vector<std::pair<std::string, fs::path>> inputs;
        auto add = [&](const fs::path& file) {
            if (stop.stop_requested()) throw std::runtime_error("Build check cancelled.");
            const auto relative = fs::canonical(file).lexically_relative(root);
            if (relative.empty() || relative.is_absolute()) throw std::runtime_error("Invalid build input.");
            for (const auto& part : relative)
                if (part == "..") throw std::runtime_error("Build input outside collection.");
            const auto name = build_path_text(relative);
            if (name.find_first_of("\t\r\n") != std::string::npos) throw std::runtime_error("Invalid build path.");
            inputs.emplace_back(name, file);
        };
        const auto spec = gsr_launcher::read_text(root / "scripts" / "developer_build_inputs.txt");
        if (spec.empty()) return false;
        std::istringstream lines(spec);
        std::string line;
        while (std::getline(lines, line)) {
            if (stop.stop_requested()) return false;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t\r\n") == std::string::npos || line.front() == '#') continue;
            const auto tab = line.find('\t');
            if (tab == std::string::npos) return false;
            const auto game = line.substr(0, tab);
            if (game != "Both" && game != "BrokenSeal" && game != "LostAge") return false;
            if (game != "Both" && game != (choice ? "LostAge" : "BrokenSeal")) continue;
            auto name = line.substr(tab + 1);
            if (const auto corpus = name.find("{tla_corpus}"); corpus != std::string::npos)
#ifdef _WIN32
                name.replace(corpus, 12, "tla");
#else
                name.replace(corpus, 12, "tla_linux");
#endif
            if (name.empty() || name.find_first_of("\\:\t\r\n") != std::string::npos) return false;
            const fs::path relative(std::u8string(name.begin(), name.end()));
            if (relative.empty() || relative.is_absolute()) return false;
            for (const auto& part : relative) if (part == "." || part == "..") return false;
            const auto input = root / relative;
            if (fs::is_symlink(input)) return false;
            if (fs::is_directory(input)) {
                for (auto at = fs::recursive_directory_iterator(input);
                     at != fs::recursive_directory_iterator(); ++at) {
                    if (stop.stop_requested()) return false;
                    if (ignored_build_path(at->path().lexically_relative(input))) {
                        if (at->is_directory()) at.disable_recursion_pending();
                        continue;
                    }
                    if (at->is_symlink()) return false;
                    if (at->is_regular_file()) add(at->path());
                    else if (!at->is_directory()) return false;
                }
            } else if (fs::is_regular_file(input)) add(input);
            else return false;
        }
        // The program, the game library and (Windows) the engine plug-in,
        // as scripts/developer_build_stamp.py records them.
        const auto& identity = gsr_launcher::kGames[choice];
        std::vector<fs::path> outputs = {engine, engine.parent_path() / identity.library};
        if (std::string_view(identity.engine) != identity.executable)
            outputs.push_back(engine.parent_path() / identity.engine);
        for (const auto& output : outputs)
            if (fs::is_symlink(output) || !fs::is_regular_file(output) ||
                fs::file_size(output) == 0) return false;
        for (const auto& output : outputs) add(output);
        add(engine.parent_path() / "CMakeCache.txt");
#ifdef _WIN32
        // The Lost Age's "Pre-built RAM code" setting is a build setting: the
        // tree must have been built with the current choice. A tree from
        // before the setting existed has no key and was built with it.
        if (choice == 1) {
            const auto cache = gsr_launcher::read_text(engine.parent_path() / "CMakeCache.txt");
            const bool built_with = cache.find("TLA_PREBUILT_RAM_CODE:BOOL=OFF") == std::string::npos;
            if (built_with != gsr_session::load_launcher_audio_settings(game_root).prebuilt_ram_code) return false;
        }
#endif
        // Hashing one file at a time took ~10 s for both games; spread it over
        // every core (measured ~0.3 s warm).
        std::vector<std::string> digests(inputs.size());
        std::atomic<std::size_t> next{0};
        std::atomic<bool> failed{false};
        {
            std::vector<std::jthread> pool(std::max(1u, std::thread::hardware_concurrency()));
            for (auto& thread : pool) thread = std::jthread([&] {
                for (std::size_t at; !failed && (at = next++) < inputs.size();)
                    if (stop.stop_requested() || !gsr_launcher::sha1_file(inputs[at].second, &digests[at]))
                        failed = true;
            });
        }
        if (failed) return false;
        for (std::size_t at = 0; at < inputs.size(); ++at) files[inputs[at].first] = std::move(digests[at]);
        std::string current = "developer_build=1\n";
        for (const auto& [path, hash] : files) current += hash + "\t" + path + "\n";
        return current == record;
    } catch (const std::exception&) { return false; }
}
} // namespace gsr_collection
