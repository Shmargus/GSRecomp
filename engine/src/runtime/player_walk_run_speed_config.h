#pragma once

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace gbarecomp {

// Walk/run speed values are in halves: 2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x.

constexpr int normalize_player_walk_run_speed_multiplier(int value) {
    return (value == 2 || value == 3 || value == 4 || value == 6) ? value : 2;
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
    if (!value) return 0;
    if (std::strcmp(value, "1") == 0) return 2;
    if (std::strcmp(value, "1.5") == 0) return 3;
    if (std::strcmp(value, "2") == 0) return 4;
    if (std::strcmp(value, "3") == 0) return 6;
    return 0;
}

inline bool parse_legacy_player_walk_run_2x(const char* value) {
    return player_speed_ascii_equal(value, "true") ||
           player_speed_ascii_equal(value, "on") ||
           player_speed_ascii_equal(value, "yes") ||
           player_speed_ascii_equal(value, "1");
}

constexpr int resolve_player_walk_run_speed_multiplier(
    int persisted_multiplier, bool legacy_present, bool legacy_2x) {
    return (persisted_multiplier == 2 || persisted_multiplier == 3 ||
            persisted_multiplier == 4 || persisted_multiplier == 6)
        ? persisted_multiplier
        : (legacy_present && legacy_2x ? 4 : 2);
}

constexpr const char* player_walk_run_speed_persisted_value(int multiplier) {
    return multiplier == 3 ? "1.5" : multiplier == 4 ? "2" : multiplier == 6 ? "3" : "1";
}

}  // namespace gbarecomp
