#pragma once

#include "../engine/src/runtime/view_config.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace gsr_collection {

struct ViewSettings {
    int mode = 0;
    int aspect = 0;
    bool sharp_pixels = true;
};

inline std::string view_setting_token(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// The launcher and F1 menu share the engine's existing config.ini.
inline ViewSettings load_view_settings(const std::filesystem::path& directory) {
    ViewSettings result;
    std::ifstream input(directory / "config.ini");
    std::string section, line;
    bool has_mode = false;
    while (std::getline(input, line)) {
        const auto text = view_setting_token(line);
        if (text.empty() || text[0] == ';' || text[0] == '#') continue;
        if (text.front() == '[' && text.back() == ']') {
            section = text.substr(1, text.size() - 2);
            continue;
        }
        const auto equal = text.find('=');
        if (equal == std::string::npos) continue;
        const auto key = view_setting_token(text.substr(0, equal));
        const auto value = view_setting_token(text.substr(equal + 1));
        if (section == "enhancements" && key == "viewmode") {
            if (value.size() == 1 && value[0] >= '0' && value[0] < '0' + gbarecomp::kViewModeCount) {
                result.mode = value[0] - '0';
                has_mode = true;
            }
        } else if (section == "enhancements" && key == "widescreen" && !has_mode) {
            result.mode = value == "true" || value == "1" ? 1 : 0;
        } else if (section == "picture" && key == "aspectratio") {
            for (int i = 0; i < gbarecomp::kViewAspectCount; ++i)
                if (value == gbarecomp::kViewAspectKeys[i]) result.aspect = i;
        } else if (section == "video" && key == "integerscale") {
            result.sharp_pixels = value == "true" || value == "1";
        }
    }
    if (result.mode != 0 && result.aspect == gbarecomp::ViewAspectMinimalSharpPixels) result.sharp_pixels = true;
    return result;
}

inline void set_view_setting(std::vector<std::string>& lines,
                             const std::string& section,
                             const std::string& key, const std::string& value) {
    bool selected = false;
    std::size_t insertion = lines.size();
    bool found_section = false, found_key = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto text = view_setting_token(lines[i]);
        if (!text.empty() && text.front() == '[' && text.back() == ']') {
            if (selected) insertion = i;
            selected = text.substr(1, text.size() - 2) == section;
            found_section |= selected;
            if (selected) insertion = i + 1;
        } else if (selected) {
            insertion = i + 1;
            const auto equal = text.find('=');
            if (equal != std::string::npos && view_setting_token(text.substr(0, equal)) == key) {
                lines[i] = key + "=" + value;
                found_key = true;
            }
        }
    }
    if (found_key) return;
    if (!found_section) {
        if (!lines.empty()) lines.emplace_back();
        lines.push_back("[" + section + "]");
        insertion = lines.size();
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertion), key + "=" + value);
}

inline bool save_view_settings(const std::filesystem::path& directory,
                               const ViewSettings& settings) {
    if (settings.mode < 0 || settings.mode >= gbarecomp::kViewModeCount ||
        settings.aspect < 0 || settings.aspect >= gbarecomp::kViewAspectCount) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    std::vector<std::string> lines;
    std::ifstream input(directory / "config.ini");
    std::string line;
    while (std::getline(input, line)) lines.push_back(line);
    set_view_setting(lines, "enhancements", "viewmode", std::to_string(settings.mode));
    set_view_setting(lines, "enhancements", "widescreen", settings.mode ? "true" : "false");
    set_view_setting(lines, "picture", "aspectratio", gbarecomp::kViewAspectKeys[settings.aspect]);
    set_view_setting(lines, "video", "integerscale",
        settings.sharp_pixels || (settings.mode != 0 && settings.aspect == gbarecomp::ViewAspectMinimalSharpPixels) ? "true" : "false");
    input.close();
    std::ofstream output(directory / "config.ini", std::ios::trunc);
    for (const auto& kept : lines) output << kept << '\n';
    return bool(output);
}

} // namespace gsr_collection
