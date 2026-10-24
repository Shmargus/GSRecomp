#pragma once
#include <string>
#include <vector>

namespace gbarecomp {
// Register game-owned actions before creating the host window. The standard
// Hotkeys page supplies keyboard/modifier and controller capture/persistence.
// Names and labels must outlive the host; callbacks only run on the host thread.
struct GameHotkey {
    const char* name;
    const char* label;
    std::string default_key;
    void (*pressed)() = nullptr;
    void (*held)(bool) = nullptr;
};
inline std::vector<GameHotkey>& game_hotkeys() {
    static std::vector<GameHotkey> actions;
    return actions;
}
} // namespace gbarecomp
