#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace gsr::settings_help {

// The description font is NOT the 8x8 menu font. Each character is a
// 16-bit advance followed by fifteen 16-bit bitmap rows (MSB on the left).
// Its ordinary space advances five pixels. See FACTS, F12 0192.
class Line {
public:
    template<class Read>
    void load(Read read, std::uint32_t address) {
        if (ready_.load(std::memory_order_acquire)) return;
        for (std::size_t i = 0; i < font_.size(); ++i)
            font_[i] = read(address + static_cast<std::uint32_t>(i));
        ready_.store(true, std::memory_order_release);
    }

    // Updated on the emulation thread; painting only reads published data.
    void colours(std::uint16_t letter, std::uint16_t shadow) {
        colours_.store(letter | (std::uint32_t(shadow) << 16));
    }

    void reset() { text_.clear(); }

    static double now() {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
               int ox, int oy, const std::string& text, int row,
               std::uint32_t window) {
        draw(text, row, window, now(), [&](int x, int y, std::uint16_t colour) {
            x += ox;
            y += oy;
            if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
                y >= static_cast<int>(height)) return;
            auto* p = rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
            for (int c = 0; c < 3; ++c) {
                const int v = (colour >> (c * 5)) & 31;
                p[c] = static_cast<std::uint8_t>((v << 3) | (v >> 2));
            }
        });
    }

    // UI pacing choices, not measured reading-speed claims. Wall time keeps
    // Turbo and walking-speed settings from changing the reading pace.
    static int offset(int overflow, double elapsed) {
        if (overflow <= 0) return 0;
        constexpr double pixels_per_second = 30.0;
        constexpr double hold_seconds = 1.0;
        const double travel = overflow / pixels_per_second;
        const double cycle = 2.0 * (hold_seconds + travel);
        double phase = std::max(0.0, elapsed);
        phase -= static_cast<unsigned long long>(phase / cycle) * cycle;
        if (phase <= hold_seconds) return 0;
        phase -= hold_seconds;
        if (phase < travel) return static_cast<int>(phase * pixels_per_second);
        phase -= travel;
        if (phase <= hold_seconds) return overflow;
        return std::clamp(overflow - static_cast<int>(
            (phase - hold_seconds) * pixels_per_second), 0, overflow);
    }

    template<class Put>
    void draw(const std::string& text, int row, std::uint32_t window,
              double time, Put put) {
        if (!ready_.load(std::memory_order_acquire)) return;
        const int wx = window & 0xFFu, wy = (window >> 8) & 0xFFu;
        const int ww = (window >> 16) & 0xFFu, wh = window >> 24;
        if (ww < 3 || wh < 3 || wx + ww > 32 || wy + wh > 32) return;
        const int left = (wx + 1) * 8, right = (wx + ww - 1) * 8;
        const int top = (wy + 1) * 8;
        // Native shaded descriptions reach into the frame's four shaded
        // pixel rows. Keep that baseline; clip horizontally to the interior.
        const int baseline = top - 3;
        if (text != text_ || row != row_ || window != window_) {
            text_ = text;
            row_ = row;
            window_ = window;
            started_ = time;
        }
        int extent = 0;
        glyphs(text, [&](int x, int, bool) { extent = std::max(extent, x + 1); });
        const int shift = offset(extent - (right - left), time - started_);
        const std::uint32_t colours = colours_.load();
        glyphs(text, [&](int x, int y, bool shadow) {
            x += left - shift;
            y += baseline;
            if (x < left || x >= right || y < top - 4 ||
                y >= (wy + wh - 1) * 8 + 4) return;
            put(x, y, static_cast<std::uint16_t>(shadow ? colours >> 16 : colours));
        });
    }

private:
    std::uint16_t word(std::size_t at) const {
        return static_cast<std::uint16_t>(font_[at] | (font_[at + 1] << 8));
    }

    template<class Put>
    void glyphs(const std::string& text, Put put) const {
        // Paint all shadows first so neighbouring letters never get covered
        // by the next character's shadow.
        for (const bool shadow : {true, false}) {
            int x = 0;
            for (const unsigned char c : text) {
                if (c < 0x20u) continue;
                const std::size_t at = (c - 0x20u) * 32u;
                if (c != ' ') {
                    for (int y = 0; y < 15; ++y) {
                        const std::uint16_t bits = word(at + 2u + y * 2u);
                        for (int gx = 0; gx < 16; ++gx)
                            if (bits & (0x8000u >> gx))
                                put(x + gx + int(shadow), y + int(shadow), shadow);
                    }
                }
                x += c == ' ' ? 5 : word(at);
            }
        }
    }

    std::array<std::uint8_t, (256 - 0x20) * 32> font_{};
    std::atomic<bool> ready_{false};
    std::atomic<std::uint32_t> colours_{0x00007FFFu};
    // Presentation-thread state only.
    std::string text_;
    int row_ = -1;
    std::uint32_t window_ = 0;
    double started_ = 0;
};

}  // namespace gsr::settings_help
