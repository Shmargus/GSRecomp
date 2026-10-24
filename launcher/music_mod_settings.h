#pragma once
// The Music mod switch: recordings in the collection's Music folder play in
// place of a game's songs (engine host_local_music; MUSIC_MODS.md). Stored as
// MusicMod in each game's game_options.ini, the key F1 > Audio also writes,
// so the launcher and the running game share one setting.
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "../engine/src/runtime/game_options.h"
#include "../engine/src/runtime/host_window.h"

namespace gsr_collection {

// A folder holding at least one FLAC or WAV file.
inline bool music_folder_has_recordings(const std::filesystem::path& folder) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error) || entry_error) continue;
        std::string extension = it->path().extension().string();
        for (char& ch : extension) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (extension == ".flac" || extension == ".wav") return true;
    }
    return false;
}

// The folders a game plays from, first choice first: The Lost Age plays its
// own album and The Broken Seal's for the songs it carried over.
inline std::vector<std::filesystem::path> music_folders(const std::filesystem::path& collection, int game) {
    const auto music = collection / "Music";
    if (game == 1) return {music / "TLA", music / "TBS"};
    return {music / "TBS"};
}

inline bool music_mod_available(const std::filesystem::path& collection, int game) {
    for (const auto& folder : music_folders(collection, game))
        if (music_folder_has_recordings(folder)) return true;
    return false;
}

// music_folders() as the game's GBARECOMP_MUSIC_DIR (host_window.h).
inline std::string music_folder_list(const std::filesystem::path& collection, int game) {
    std::string list;
    for (const auto& folder : music_folders(collection, game)) {
        if (!list.empty()) list += gbarecomp::kLocalMusicFolderSeparator;
        const auto text = folder.generic_u8string();
        list.append(text.begin(), text.end());
    }
    return list;
}

// The player's choice, or nothing if they were never asked.
inline std::optional<bool> music_mod_choice(const std::filesystem::path& options_file) {
    std::ifstream input(options_file);
    const std::string key = std::string(gbarecomp::kOptionMusicMod) + "=";
    std::string line;
    std::optional<bool> choice;
    while (std::getline(input, line))
        if (line.rfind(key, 0) == 0) choice = line.compare(key.size(), std::string::npos, "0") != 0;
    return choice;
}

// Rewrites the one key and keeps every other line of the game's file.
inline bool set_music_mod(const std::filesystem::path& options_file, bool on) {
    const std::string key = std::string(gbarecomp::kOptionMusicMod) + "=";
    std::vector<std::string> lines;
    {
        std::ifstream input(options_file);
        std::string line;
        while (std::getline(input, line))
            if (line.rfind(key, 0) != 0) lines.push_back(line);
    }
    lines.push_back(key + (on ? "1" : "0"));
    std::error_code ec;
    std::filesystem::create_directories(options_file.parent_path(), ec);
    std::ofstream output(options_file, std::ios::trunc);
    for (const auto& kept : lines) output << kept << '\n';
    return static_cast<bool>(output);
}

}  // namespace gsr_collection
