#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif

namespace gsr::adventure_ui {

// Copies the menu font once, on the emulation thread. Tiles are the same
// 256 8x8 4bpp tiles used by the settings page; advances cover bytes 0x20-FF.
inline std::array<std::uint8_t, 256 * 32> font_tiles{};
inline std::array<std::uint8_t, 256 - 0x20> font_widths{};
inline std::atomic<bool> font_ready{false};
inline std::mutex font_mutex;

inline void publish_font(const std::uint8_t* tiles, const std::uint8_t* widths) {
    if (!tiles || !widths || font_ready.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(font_mutex);
    if (font_ready.load(std::memory_order_relaxed)) return;
    for (std::size_t i = 0; i < font_tiles.size(); ++i) font_tiles[i] = tiles[i];
    for (std::size_t i = 0; i < font_widths.size(); ++i) font_widths[i] = widths[i];
    font_ready.store(true, std::memory_order_release);
}

inline std::uint8_t glyph_advance(std::uint8_t code) {
    if (code < 0x20) code = '?';
    const unsigned width = font_widths[code - 0x20];
    return static_cast<std::uint8_t>(width > 8 ? 8 : width);
}

// Convert UTF-8 to the game's byte font. Common guide punctuation is mapped
// explicitly; malformed or unsupported input becomes a readable question mark.
inline std::vector<std::uint8_t> normalize_utf8(const char* text) {
    std::vector<std::uint8_t> out;
    if (!text) return out;
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) {
        if (*p < 0x80) {
            const unsigned char c = *p++;
            out.push_back(c < 0x20 && c != '\n' ? '?' : c);
            continue;
        }
        unsigned cp = 0, n = 0;
        if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 3; }
        else { out.push_back('?'); ++p; continue; }
        ++p;
        bool valid = true;
        for (unsigned i = 0; i < n; ++i) {
            if (p[i] == 0 || (p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid || (n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
            (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back('?');
            continue;
        }
        p += n;
        switch (cp) {
            case 0x2018: case 0x2019: case 0x201A: case 0x201B: out.push_back('\''); break;
            case 0x201C: case 0x201D: case 0x201E: case 0x201F: out.push_back('"'); break;
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: out.push_back('-'); break;
            case 0x2026: out.insert(out.end(), 3, '.'); break;
            case 0x00A0: case 0x2002: case 0x2003: case 0x2009: out.push_back(' '); break;
            default: out.push_back('?'); break;
        }
    }
    return out;
}

#ifdef GBARECOMP_HAVE_IMGUI
struct WrappedText {
    std::vector<std::vector<std::uint8_t>> lines;
    float width = 0;
};

inline WrappedText wrap_text(const char* text, float max_width, int scale) {
    WrappedText result;
    const auto bytes = normalize_utf8(text);
    result.lines.emplace_back();
    const float limit = max_width > 0 ? max_width : 1.0f;
    const float pixel = static_cast<float>(scale);
    auto line_width = [&]() {
        unsigned w = 0;
        for (auto c : result.lines.back()) w += glyph_advance(c);
        return static_cast<float>(w) * pixel;
    };
    for (std::size_t i = 0; i < bytes.size();) {
        if (bytes[i] == '\n') { result.lines.emplace_back(); ++i; continue; }
        if (bytes[i] == ' ') {
            if (!result.lines.back().empty() && line_width() + glyph_advance(' ') * pixel > limit)
                result.lines.emplace_back();
            else result.lines.back().push_back(' ');
            ++i;
            continue;
        }
        std::size_t end = i;
        unsigned word_width = 0;
        while (end < bytes.size() && bytes[end] != ' ' && bytes[end] != '\n')
            word_width += glyph_advance(bytes[end++]);
        if (!result.lines.back().empty() && line_width() + word_width * pixel > limit)
            result.lines.emplace_back();
        for (; i < end; ++i) {
            if (!result.lines.back().empty() && line_width() + glyph_advance(bytes[i]) * pixel > limit)
                result.lines.emplace_back();
            result.lines.back().push_back(bytes[i]);
        }
    }
    for (const auto& line : result.lines) {
        unsigned w = 0;
        for (auto c : line) w += glyph_advance(c);
        if (w * pixel > result.width) result.width = w * pixel;
    }
    return result;
}

inline float measure_text_height(const char* text, float max_width, int scale) {
    if (font_ready.load(std::memory_order_acquire))
        return static_cast<float>(wrap_text(text, max_width, scale).lines.size()) * 8.0f * scale;
    return ImGui::CalcTextSize(text ? text : "", nullptr, false, max_width).y;
}

inline float draw_text(const char* text, float max_width, int scale,
                       ImU32 letter = IM_COL32(255, 255, 255, 255),
                       ImU32 shadow = IM_COL32(0, 0, 0, 255)) {
    if (!font_ready.load(std::memory_order_acquire)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + max_width);
        ImGui::TextWrapped("%s", text ? text : "");
        ImGui::PopTextWrapPos();
        return ImGui::GetTextLineHeightWithSpacing();
    }
    const auto wrapped = wrap_text(text, max_width, scale);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float s = static_cast<float>(scale);
    float y = origin.y;
    for (const auto& line : wrapped.lines) {
        float x = origin.x;
        for (const auto code : line) {
            const std::size_t base = std::size_t(code) * 32;
            for (unsigned py = 0; py < 8; ++py) {
                for (unsigned px = 0; px < 8; ++px) {
                    const unsigned packed = font_tiles[base + py * 4 + px / 2];
                    const unsigned index = (px & 1) ? packed >> 4 : packed & 0x0F;
                    if (index != 1 && index != 3) continue;
                    const ImU32 color = index == 1 ? letter : shadow;
                    draw->AddRectFilled(ImVec2(x + px * s, y + py * s),
                        ImVec2(x + (px + 1) * s, y + (py + 1) * s), color);
                }
            }
            x += glyph_advance(code) * s;
        }
        y += 8.0f * s;
    }
    const float height = static_cast<float>(wrapped.lines.size()) * 8.0f * s;
    ImGui::Dummy(ImVec2(wrapped.width, height));
    return height;
}
#endif

} // namespace gsr::adventure_ui
