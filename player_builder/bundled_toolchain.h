// Where a Windows release keeps its compiler. Used by the builder (its
// default --toolchain) and by the launcher (the game's self-heal compiler),
// so both resolve the same folder.
//
// One copy in games/mingw64 serves both games. Releases up to 0.6 kept one
// in each game's builder/mingw64; that stays the fallback, so an install
// unpacked over an older release keeps working. GCC finds its Windows
// headers at bin/../../mingw64/include, so the folder must be called mingw64.
#pragma once

#include <filesystem>
#include <system_error>

inline std::filesystem::path bundled_toolchain(const std::filesystem::path& game_root) {
    std::error_code ec;
    const std::filesystem::path shared = game_root.parent_path() / "mingw64";
    if (std::filesystem::exists(shared / "bin" / "g++.exe", ec)) return shared;
    return game_root / "builder" / "mingw64";
}
