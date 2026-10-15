#pragma once

#include <filesystem>
#include <stdexcept>

#include "launcher_game.h"

namespace gsr_session {
namespace fs = std::filesystem;

inline fs::path game_root(const fs::path& collection, int game) {
    if (!collection.is_absolute() || game < 0 || game > 1)
        throw std::invalid_argument("Invalid collection session.");
    const char* name = gsr_launcher::kGames[game].stem;
    std::error_code error;
    return fs::is_directory(collection / "games", error)
        ? collection / "games" / name : collection / name;
}

inline fs::path developer_engine(const fs::path& root, int game) {
    const char* name = gsr_launcher::kGames[game].executable;
    const char* directory = game == 0 ? "gs011_opt" :
#ifdef _WIN32
        "tla";
#else
        "tla-linux";
#endif
    return root / "build" / directory / name;
}

inline fs::path engine(const fs::path& root, int game) {
    const auto developer = developer_engine(root, game);
    std::error_code error;
    return fs::is_regular_file(developer, error) ? developer : root / developer.filename();
}
} // namespace gsr_session

// The game session (session_windows.cpp / session_linux.cpp); game 0 is The
// Broken Seal, 1 The Lost Age.
int gsr_session_main(int game, int argc, char** argv);
