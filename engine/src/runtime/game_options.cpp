#include "game_options.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace gbarecomp {
namespace {

std::mutex g_mutex;
std::string g_path;
std::map<std::string, int> g_values;

void save_locked() {
    std::FILE* f = std::fopen(g_path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "[options] could not write %s\n", g_path.c_str());
        return;
    }
    for (const auto& [key, value] : g_values)
        std::fprintf(f, "%s=%d\n", key.c_str(), value);
    std::fclose(f);
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
    std::lock_guard<std::mutex> lock(g_mutex);
    g_values[key] = value;
    save_locked();
}

}  // namespace gbarecomp
