#pragma once

#include <cctype>
#include <cstdlib>

namespace gbarecomp {

constexpr int normalize_player_walk_run_speed_multiplier(int value) {
    return value >= 1 && value <= 3 ? value : 1;
}

inline bool player_speed_ascii_equal(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) return false;
    while (*lhs && *rhs) {
        const unsigned char a = static_cast<unsigned char>(*lhs++);
        const unsigned char b = static_cast<unsigned char>(*rhs++);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return *lhs == '\0' && *rhs == '\0';
}

inline int parse_player_walk_run_speed_multiplier(const char* value) {
    if (!value || value[0] == '\0' || value[1] != '\0') return 0;
    return value[0] >= '1' && value[0] <= '3' ? value[0] - '0' : 0;
}

inline bool parse_legacy_player_walk_run_2x(const char* value) {
    return player_speed_ascii_equal(value, "true") ||
           player_speed_ascii_equal(value, "on") ||
           player_speed_ascii_equal(value, "yes") ||
           player_speed_ascii_equal(value, "1");
}

constexpr int resolve_player_walk_run_speed_multiplier(
    int persisted_multiplier, bool legacy_present, bool legacy_2x) {
    return persisted_multiplier >= 1 && persisted_multiplier <= 3
        ? persisted_multiplier
        : (legacy_present && legacy_2x ? 2 : 1);
}

constexpr const char* player_walk_run_speed_persisted_value(int multiplier) {
    return multiplier == 2 ? "2" : multiplier == 3 ? "3" : "1";
}

}  // namespace gbarecomp
