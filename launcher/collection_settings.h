#pragma once
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace gsr_collection {
// Both games list every option in this order; `games` (bit 0 The Broken
// Seal, bit 1 The Lost Age) greys out an option the game cannot use yet.
// Frame rewind, screen filters and the unpacker catcher are always on.
struct TestOption { const char* label; const char* environment; unsigned games; bool default_selected; };
inline constexpr std::array<TestOption, 13> test_options{{
    {"Self-heal RAM", "GBARECOMP_SELFHEAL_RAM", 3, true},
    {"Sprite recorder", "GSR_OBJ_RECORD", 1, false},
    {"Map recorder", "GSR_MAP_RECORD", 1, false},
    {"Effect trace", "GSR_EFFECT_TRACE", 3, false},
    {"Cost probe", "GBARECOMP_COST_PROBE", 3, false},
    {"Host profiler", "GBARECOMP_HOST_PROF", 3, false},
    {"Present cadence", "GBARECOMP_PRESENT_CADENCE", 3, false},
    {"GPU field check", "TLA_GPU_FIELD_CHECK", 2, false},
    // The data unpacker crash (unpacker_guard.h): break one unpack on
    // purpose, or check every unpack against a sandbox run.
    {"Force unpacker fault", "GSR_UNPACKER_FORCE_FAULT", 3, false},
    {"Unpacker audit", "GSR_UNPACKER_AUDIT", 3, false},
    // The Lost Age's second sprite page for field people in the wider view
    // (src/tla_sprite_memory.cpp); this keeps the console's 32 KB only.
    {"Console sprite memory", "TLA_CONSOLE_SPRITE_MEMORY", 2, false},
    {"Sprite memory trace", "TLA_SPRITE_MEMORY_TRACE", 2, false},
    {"Local music", "GSR_INTRO_MUSIC", 1, false},
}};
constexpr std::size_t test_index(std::string_view environment) {
    for (std::size_t i = 0; i < test_options.size(); ++i)
        if (environment == test_options[i].environment) return i;
    return test_options.size();
}
constexpr std::array<bool, test_options.size()> defaults() {
    std::array<bool, test_options.size()> values{};
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = test_options[i].default_selected;
    return values;
}
struct GameSettings {
    bool direct3d = false;
    bool test_variables = false;
    std::array<bool, test_options.size()> tests = defaults();
};
using CollectionSettings = std::array<GameSettings, 2>;
inline std::string serialize_tests(const GameSettings& settings) {
    std::string value(1, settings.test_variables ? '1' : '0');
    for (bool selected : settings.tests) value += selected ? '1' : '0';
    return value;
}
inline bool parse_tests(std::string_view value, GameSettings& settings) {
    if (value.size() != test_options.size() + 1 || value.find_first_not_of("01") != std::string_view::npos) return false;
    settings.test_variables = value[0] == '1';
    for (std::size_t i = 0; i < settings.tests.size(); ++i) settings.tests[i] = value[i + 1] == '1';
    return true;
}
inline bool test_enabled(const GameSettings& settings, std::size_t index, int game) {
    return game >= 0 && game < 2 && index < test_options.size() &&
        (test_options[index].games & (1u << game)) && settings.tests[index] && (settings.test_variables || index == 0);
}
inline bool test_enabled(const GameSettings& settings, std::string_view environment, int game) {
    return test_enabled(settings, test_index(environment), game);
}
inline CollectionSettings load_settings(const std::filesystem::path& root) {
    CollectionSettings settings{};
    std::ifstream file(root / "local" / "collection-launcher.ini");
    std::string line;
    while (std::getline(file, line)) {
        if (line == "broken_seal_direct3d=1") settings[0].direct3d = true;
        if (line == "lost_age_direct3d=1") settings[1].direct3d = true;
    }
    return settings;
}
inline bool save_settings(const std::filesystem::path& root, const CollectionSettings& settings) {
    std::error_code error;
    std::filesystem::create_directories(root / "local", error);
    if (error) return false;
    std::ofstream file(root / "local" / "collection-launcher.ini", std::ios::trunc);
    file << "broken_seal_direct3d=" << settings[0].direct3d << '\n'
         << "lost_age_direct3d=" << settings[1].direct3d << '\n';
    return bool(file);
}
} // namespace gsr_collection
