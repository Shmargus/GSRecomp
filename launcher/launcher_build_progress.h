#pragma once
#include <algorithm>
#include <cmath>

namespace gsr_launcher {
struct ProgressColor { unsigned char r, g, b, a; };
inline constexpr ProgressColor progress_gold{224, 181, 79, 255};
inline constexpr ProgressColor progress_dark_purple{77, 35, 116, 255};
inline constexpr ProgressColor progress_deep_background{22, 13, 35, 255};
inline constexpr ProgressColor progress_light_purple{172, 118, 220, 90};
inline constexpr float progress_corner_radius = 6.0f;
inline constexpr float progress_height = 28.0f;
inline float progress_fraction(int done, int total) {
    return total > 0 ? std::clamp(static_cast<float>(done) / total, 0.0f, 1.0f) : 0.0f;
}
inline float energy_y(float x, float width, float height, double seconds, int strand) {
    const double phase = (width > 0 ? x / width : 0) * 6.28318530718;
    return height * (0.5f + 0.18f * static_cast<float>(std::sin(
        phase + seconds * 0.65 + strand * 2.09439510239)));
}
} // namespace gsr_launcher
