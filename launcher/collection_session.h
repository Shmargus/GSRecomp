#pragma once

#include <filesystem>
#include <stdexcept>

namespace gsr_session {
namespace fs = std::filesystem;

inline fs::path game_root(const fs::path& collection, int game) {
    if (!collection.is_absolute() || game < 0 || game > 1)
        throw std::invalid_argument("Invalid collection session.");
    const char* name = game == 0 ? "The Broken Seal" : "The Lost Age";
    std::error_code error;
    return fs::is_directory(collection / "games", error)
        ? collection / "games" / name : collection / name;
}

inline fs::path developer_engine(const fs::path& root, int game) {
#ifdef _WIN32
    const char* name = game == 0 ? "GoldenSunRecomp.exe" : "TLARecomp.exe";
#else
    const char* name = game == 0 ? "GoldenSunRecomp" : "TLARecomp";
#endif
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

int gsr_broken_seal_session_main(int argc, char** argv);
int gsr_lost_age_session_main(int argc, char** argv);
