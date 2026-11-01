#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gsr::custom_battle::text {

// Both games' native font advances are indexed by Latin-1 glyph minus 32.
inline unsigned width(std::string_view line, std::span<const std::uint8_t> advances) {
    unsigned pixels = 0;
    for (const unsigned char c : line)
        if (c >= 32u && c - 32u < advances.size()) pixels += advances[c - 32u];
    return pixels;
}

// Wrap translated dialogue and native monster names using the actual font.
// A long name can split inside a word; every non-space glyph is preserved.
inline std::vector<std::string> wrap(std::string_view value,
                                     std::span<const std::uint8_t> advances,
                                     unsigned limit) {
    std::vector<std::string> lines(1);
    for (std::size_t begin = 0; begin < value.size();) {
        if (value[begin] == ' ') { ++begin; continue; }
        const auto end = value.find(' ', begin);
        const auto word = value.substr(begin, end == value.npos ? value.size() - begin : end - begin);
        if (!lines.back().empty()) {
            if (width(lines.back(), advances) + width(" ", advances) + width(word, advances) > limit)
                lines.emplace_back();
            else
                lines.back() += ' ';
        }
        for (const char c : word) {
            if (!lines.back().empty() &&
                width(lines.back(), advances) + width(std::string_view(&c, 1), advances) > limit)
                lines.emplace_back();
            lines.back() += c;
        }
        begin = end == value.npos ? value.size() : end + 1;
    }
    return lines;
}

}  // namespace gsr::custom_battle::text
