#include "game_options.h"

#include "file_replace.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace gbarecomp {
namespace {

std::mutex g_mutex;
std::string g_path;
// Transparent comparison: a lookup by const char* builds no std::string.
std::map<std::string, int, std::less<>> g_values;

// The file is replaced whole (file_replace.h), so a failed write keeps the
// previous settings. Values stay in memory and go out with the next change.
void save_locked() {
    if (g_path.empty()) return;
    std::string text;
    for (const auto& [key, value] : g_values)
        text += key + "=" + std::to_string(value) + "\n";
    std::string err;
    if (!gba::replace_file(g_path, text.data(), text.size(), &err))
        std::fprintf(stderr, "[options] %s\n", err.c_str());
}

}  // namespace

void game_options_load(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
    g_values.clear();
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        const std::string text(line);
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos || equals == 0) continue;
        g_values[text.substr(0, equals)] = std::atoi(text.c_str() + equals + 1);
    }
    std::fclose(f);
}

int game_option_get(const char* key, int fallback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_values.find(key);
    return it == g_values.end() ? fallback : it->second;
}

void game_option_set(const char* key, int value) {
    const GameOptionValue change{key, value};
    game_options_update({&change, 1});
}

void game_option_reset_prefix(const char* prefix) {
    game_options_update({}, {&prefix, 1});
}

void game_options_update(std::span<const GameOptionValue> values,
                         std::span<const char* const> reset_prefixes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    bool changed = false;
    for (const char* prefix : reset_prefixes) {
        const std::string start(prefix);
        if (start.empty()) continue;
        auto it = g_values.lower_bound(start);
        while (it != g_values.end() && it->first.compare(0, start.size(), start) == 0) {
            it = g_values.erase(it);
            changed = true;
        }
    }
    for (const auto& value : values) {
        const auto it = g_values.find(value.key);
        if (it != g_values.end() && it->second == value.value) continue;
        g_values[value.key] = value.value;
        changed = true;
    }
    if (changed) save_locked();
}

}  // namespace gbarecomp
