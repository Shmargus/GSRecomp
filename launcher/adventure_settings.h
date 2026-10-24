#pragma once
#include "collection_view_settings.h"
#include "imgui.h"

namespace gsr_collection {
inline void adventure_settings(const std::filesystem::path& directory, bool save, std::string& error) {
    const auto path = directory / "game_options.ini";
    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    bool enabled = false, record = false, guide_mode = true, collectibles = false, completionist = true;
    while (std::getline(input, line)) {
        lines.push_back(line);
        const auto token = view_setting_token(line);
        if (token == "adventureguide=1") enabled = true;
        if (token == "adventureguidemode=0") guide_mode = false;
        if (token == "adventureguidemode=1") guide_mode = true;
        if (token == "adventurerecord=1") record = true;
        if (token == "adventurecollectibles=1") collectibles = true;
        if (token == "adventurecompletionist=0") completionist = false;
        if (token == "adventurecompletionist=1") completionist = true;
    }
    input.close();
    ImGui::Separator();
    bool changed = ImGui::Checkbox("Quest Tracker and Guide (TBS)", &enabled);
    changed |= ImGui::Checkbox("Area collectibles window (TBS)", &collectibles);
    int overlay_mode = guide_mode ? 1 : 0;
    if (ImGui::Combo("Overlay mode", &overlay_mode, "Quest Tracker\0Guide\0")) {
        guide_mode = overlay_mode == 1; changed = true;
    }
    if (enabled && guide_mode) {
        int walkthrough_mode = completionist ? 1 : 0;
        if (ImGui::Combo("Walkthrough", &walkthrough_mode, "Regular walkthrough\0" "100% walkthrough\0")) {
            completionist = walkthrough_mode == 1; changed = true;
        }
    }
    changed |= ImGui::Checkbox("Record adventure progress for verification", &record);
    ImGui::TextWrapped("Quest Tracker shows your current mapped objective and Djinn count. Regular walkthrough follows the story; 100% also guides optional Djinn, treasure and areas. Area collectibles lists nearby pickups and Djinn with collection marks, independently of the guide. F1 > Hotkeys configures keyboard and controller shortcuts for guide on/off, guide show/hide and collectibles show/hide. F1 > Game has the quest log and overlay layout. Full route tracking is still in progress.");
    if (!changed || !save) return;
    const auto set = [&](const std::string& key, bool value) {
        bool found = false;
        for (auto& kept : lines) {
            const auto token = view_setting_token(kept);
            if (token.rfind(view_setting_token(key) + "=", 0) != 0) continue;
            kept = key + "=" + (value ? "1" : "0"); found = true;
        }
        if (!found) lines.push_back(key + "=" + (value ? "1" : "0"));
    };
    set("AdventureGuide", enabled); set("AdventureGuideMode", guide_mode);
    set("AdventureCompletionist", completionist);
    set("AdventureRecord", record); set("AdventureCollectibles", collectibles);
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { error = "Could not save adventure settings."; return; }
    std::ofstream output(path, std::ios::trunc);
    for (const auto& kept : lines) output << kept << '\n';
    output.close();
    if (!output) error = "Could not save adventure settings.";
}
} // namespace gsr_collection
