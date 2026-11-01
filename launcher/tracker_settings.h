#pragma once
#include "collection_view_settings.h"
#include "imgui.h"

namespace gsr_collection {
// The trackers: the same DjinnTracker, DjinnTrackerShowAll and
// TreasureTracker lines in game_options.ini that F1 > Game switches. Read
// when Settings opens (load_tracker_settings); a click rewrites these lines
// and keeps every other line of the file.
struct TrackerSettings { bool djinn = false, djinn_show_all = false, treasure = false; };

struct TrackerOption { const char* key; bool TrackerSettings::*value; const char* label; const char* caption; };
inline constexpr TrackerOption kTrackerOptions[] = {
    {"DjinnTracker", &TrackerSettings::djinn, "Djinn Tracker",
     "A window with every Djinni, counted per element. Djinn you have not found are question marks; "
     "point at one to see where it is, click an element to fold it. With OpenGL it is its own window "
     "you can drag anywhere; with Direct3D it stays inside the game. Also in game under F1 > Game."},
    {"DjinnTrackerShowAll", &TrackerSettings::djinn_show_all, "Djinn Tracker: show all Djinn",
     "Djinn you have not found yet keep their name and picture, dimmed, in their element instead of "
     "hiding as question marks."},
    {"TreasureTracker", &TrackerSettings::treasure, "Treasure Tracker",
     "A window listing every treasure per location, ticked once taken; the place you are in opens by "
     "itself. In the window: chests only, and what each one holds. Also in game under F1 > Game."}};

// The option a line sets, or nullptr.
inline const TrackerOption* tracker_option(const std::string& line) {
    const auto token = view_setting_token(line);
    for (const auto& option : kTrackerOptions)
        if (token.rfind(view_setting_token(option.key) + "=", 0) == 0) return &option;
    return nullptr;
}

inline TrackerSettings load_tracker_settings(const std::filesystem::path& directory) {
    TrackerSettings settings;
    std::ifstream input(directory / "game_options.ini");
    std::string line;
    while (std::getline(input, line))
        if (const auto* option = tracker_option(line))
            settings.*option->value = view_setting_token(line).back() == '1';
    return settings;
}

inline void tracker_settings(TrackerSettings& settings, const std::filesystem::path& directory, bool save,
                             std::string& error) {
    ImGui::Separator();
    bool changed = false;
    for (const auto& option : kTrackerOptions) {
        changed |= ImGui::Checkbox(option.label, &(settings.*option.value));
        ImGui::TextWrapped("%s", option.caption);
    }
    if (!changed || !save) return;
    const auto path = directory / "game_options.ini";
    std::vector<std::string> lines;
    {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line))
            if (!tracker_option(line)) lines.push_back(line);
    }
    for (const auto& option : kTrackerOptions)
        lines.push_back(std::string(option.key) + "=" + (settings.*option.value ? "1" : "0"));
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    std::ofstream output(path, std::ios::trunc);
    for (const auto& kept : lines) output << kept << '\n';
    output.close();
    if (ec || !output) error = "Could not save the tracker settings.";
}
} // namespace gsr_collection
