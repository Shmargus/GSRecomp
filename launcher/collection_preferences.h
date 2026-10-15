#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace gsr_session {
namespace fs = std::filesystem;
struct LauncherAudioSettings {
    bool gs_save_import = true;
    bool copy_session_id_to_clipboard = false;
    bool enhanced_options = false;
    bool gpu_field = false;
    // Draw ONLY with the graphics card: no fallback to the console
    // compositor, which is also then not run at all. Meaningless with
    // gpu_field off, and forced off below in that case.
    bool gpu_field_only = false;
    // Skip the ROM check: take a ROM that is not the supported one
    // (engine any_rom.h). Off by default.
    bool any_rom = false;
};

inline bool parse_bool_setting(const std::string& value) {
    return value == "1" || value == "true" || value == "TRUE" ||
           value == "yes" || value == "on";
}

inline std::string trim_setting(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline LauncherAudioSettings load_launcher_audio_settings(const fs::path& root) {
    LauncherAudioSettings settings;
    std::ifstream file(root / "local" / "launcher-settings.ini");
    if (!file) return settings;

    bool in_audio = false;
    bool in_launcher = false;
    std::string line;
    while (std::getline(file, line)) {
        line = trim_setting(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            in_audio = line == "[Audio]";
            in_launcher = line == "[Launcher]";
            continue;
        }
        if (!in_audio && !in_launcher) continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim_setting(line.substr(0, equals));
        const bool value = parse_bool_setting(
            trim_setting(line.substr(equals + 1)));
        if (in_launcher && key == "CopySessionIdToClipboard") {
            settings.copy_session_id_to_clipboard = value;
        } else if (in_launcher && key == "EnhancedOptions") {
            settings.enhanced_options = value;
        } else if (in_launcher && key == "GpuFieldRenderer") {
            settings.gpu_field = value;
        } else if (in_launcher && key == "GpuFieldOnly") {
            settings.gpu_field_only = value;
        } else if (in_launcher && key == "GoldenSunSaveImport") {
            settings.gs_save_import = value;
        } else if (in_launcher && key == "AnyRom") {
            settings.any_rom = value;
        }
    }
    if (!settings.gpu_field) settings.gpu_field_only = false;
    return settings;
}

inline bool save_launcher_audio_settings(const fs::path& root,
                                  const LauncherAudioSettings& settings) {
    const fs::path path = root / "local" / "launcher-settings.ini";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::vector<std::string> lines;
    if (std::ifstream input(path); input) {
        std::string line;
        while (std::getline(input, line)) lines.push_back(line);
    }

    std::vector<std::string> kept;
    std::string section;
    for (const std::string& line : lines) {
        const std::string trimmed = trim_setting(line);
        if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']')
            section = trimmed;
        const auto equals = trimmed.find('=');
        const std::string key = trim_setting(trimmed.substr(0, equals));
        if (section == "[Audio]" &&
            (key == "NativeMp2kAudio" || key == "TurboAudioDecoupled")) continue;
        if (section == "[Launcher]" &&
            (key == "CopySessionIdToClipboard" || key == "EnhancedOptions" ||
             key == "GpuFieldRenderer" || key == "GpuFieldOnly" ||
             key == "GoldenSunSaveImport" || key == "AnyRom")) continue;
        kept.push_back(line);
    }
    while (!kept.empty() && trim_setting(kept.back()).empty()) kept.pop_back();

    std::vector<std::string> managed = {
        "CopySessionIdToClipboard=" + std::string(settings.copy_session_id_to_clipboard ? "true" : "false"),
        "EnhancedOptions=" + std::string(settings.enhanced_options ? "true" : "false"),
        "GpuFieldRenderer=" + std::string(settings.gpu_field ? "true" : "false"),
        "GpuFieldOnly=" + std::string(settings.gpu_field_only ? "true" : "false"),
        "GoldenSunSaveImport=" + std::string(settings.gs_save_import ? "true" : "false"),
        "AnyRom=" + std::string(settings.any_rom ? "true" : "false")};
    std::size_t insertion = kept.size();
    bool found_launcher = false;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        const std::string trimmed = trim_setting(kept[i]);
        if (trimmed == "[Launcher]") {
            found_launcher = true;
            insertion = i + 1;
        } else if (found_launcher && !trimmed.empty() && trimmed.front() == '[') {
            break;
        } else if (found_launcher) insertion = i + 1;
    }
    if (!found_launcher) {
        if (!kept.empty()) kept.push_back("");
        kept.push_back("[Launcher]");
        insertion = kept.size();
    }
    kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(insertion),
                managed.begin(), managed.end());
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    for (const std::string& line : kept) output << line << '\n';
    return bool(output);
}

} // namespace gsr_session
