// save_extension.h — companion file name for a game's battery extension
// (gba_save.h configure_extension). The one rule, shared with the launcher.

#pragma once

#include <filesystem>
#include <string>

namespace gba {

// "Saves/Game.sav" -> "Saves/Game.sav4-9"; no extension just appends it.
inline std::filesystem::path save_extension_path(std::filesystem::path save_path) {
    save_path.replace_extension(".sav4-9");
    return save_path;
}
inline std::string save_extension_path(const std::string& save_path) {
    return save_extension_path(std::filesystem::path(save_path)).string();
}
inline std::string save_extension_path(const char* save_path) {
    return save_extension_path(std::string(save_path));
}

}  // namespace gba
