#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "menu_text.h"
#include "settings_help.h"

namespace gsr::qol_menu {

struct Row {
    const char* label = "";
    const char* help = "";
    std::function<std::string()> value;
    std::function<void(int)> change;
};

struct Widget {
    std::vector<Row> rows;
    bool showing = false;
    bool swallowing = false;
    int cursor = 0;
    int first = 0;
    std::uint16_t held = 0;
    std::vector<unsigned> sounds;
    // Presentation-thread state for the scrolling help line.
    mutable int help_cursor = -1;
    mutable double help_started = -1;

    void open(std::vector<Row> new_rows);
    void reset();
    // Input is the game's active-low ten-button value. The widget consumes
    // keys while open and keeps swallowing them until all are released.
    std::uint16_t keyinput(std::uint16_t value);

private:
    void keep_visible();
};

// Match Retreat's native 28x15-tile window and six visible 12-pixel rows.
constexpr int kTileX = 1, kTileY = 2, kTiles = 28, kRows = 15, kVisible = 6;
constexpr unsigned kA = 1, kB = 2, kStart = 8, kRight = 16, kLeft = 32;
constexpr unsigned kUp = 64, kDown = 128, kR = 256, kL = 512;
constexpr unsigned kMove = 0x6F, kBack = 0x71;

inline void Widget::open(std::vector<Row> new_rows) {
    rows = std::move(new_rows);
    showing = true;
    swallowing = false;
    cursor = 0;
    first = 0;
    held = 0x3FFu;
    help_started = -1;
    sounds.clear();
}

inline void Widget::reset() {
    rows.clear();
    showing = swallowing = false;
    cursor = first = 0;
    held = 0;
    sounds.clear();
}

inline void Widget::keep_visible() {
    if (cursor < first) first = cursor;
    else if (cursor >= first + kVisible)
        first = cursor - kVisible + 1;
    const int last = std::max(0, static_cast<int>(rows.size()) - kVisible);
    first = std::clamp(first, 0, last);
}

inline std::uint16_t Widget::keyinput(std::uint16_t value) {
    const auto pressed = static_cast<std::uint16_t>(~value & 0x3FFu);
    if (showing) {
        const unsigned down = pressed & ~held;
        held = pressed;
        if (down & (kB | kStart)) {
            showing = false;
            swallowing = true;
            sounds.push_back(kBack);
        } else if (!rows.empty()) {
            const int count = static_cast<int>(rows.size());
            bool moved = false;
            if (down & kUp) {
                cursor = (cursor + count - 1) % count;
                moved = true;
            } else if (down & kDown) {
                cursor = (cursor + 1) % count;
                moved = true;
            } else if (down & kL) {
                const int before = cursor;
                cursor = std::max(0, cursor - kVisible);
                moved = cursor != before;
            } else if (down & kR) {
                const int before = cursor;
                cursor = std::min(count - 1, cursor + kVisible);
                moved = cursor != before;
            } else if (down & (kA | kRight | kLeft)) {
                const int direction = (down & kLeft) && !(down & (kA | kRight)) ? -1 : 1;
                if (rows[cursor].change)
                    rows[cursor].change(direction);
                moved = true;
            }
            if (moved) {
                keep_visible();
                sounds.push_back(kMove);
            }
        }
        return 0x3FFu;
    }
    if (swallowing) {
        if (pressed) return 0x3FFu;
        swallowing = false;
    }
    return value;
}

template<class Draw, class Width>
inline void paint(const Widget& widget, Draw& draw, Width& width, int x, int y) {
    constexpr int inner_width = (kTiles - 2) * 8;
    const auto fit = [&](std::string s, int available) {
        if (width(s) <= available) return s;
        while (!s.empty() && width(s + "...") > available) s.pop_back();
        return s + "...";
    };
    const auto title = gsr::menu_text("Quality of Life");
    draw(title, x + (inner_width - width(title)) / 2, y);
    if (!widget.rows.empty()) {
        const auto counter = std::to_string(widget.cursor + 1) + "/" +
            std::to_string(widget.rows.size());
        draw(counter, x + inner_width - width(counter) - 8, y);
    }

    for (int line = 0; line < kVisible; ++line) {
        const int index = widget.first + line;
        if (index >= static_cast<int>(widget.rows.size())) break;
        const auto& row = widget.rows[index];
        const bool selected = index == widget.cursor;
        const std::string marker = selected ? "> " : "  ";
        const auto label = gsr::menu_text(row.label ? row.label : "");
        const auto value = row.value ? gsr::menu_text(row.value().c_str()) : std::string{};
        const int left = x + 8;
        const int right = x + inner_width - 8;
        const auto fitted_value = value.empty() ? std::string{} : fit(value, right - left);
        const int value_width = fitted_value.empty() ? 0 : width(fitted_value);
        const int label_room = std::max(0, right - left - value_width -
            (fitted_value.empty() ? 0 : 8));
        const auto fitted_label = fit(marker + label, label_room);
        draw(fitted_label, left, y + 14 + line * 12);
        if (!fitted_value.empty())
            draw(fitted_value, right - value_width, y + 14 + line * 12);
    }

    if (widget.cursor >= 0 && widget.cursor < static_cast<int>(widget.rows.size())) {
        const char* help = widget.rows[widget.cursor].help;
        if (help && *help) {
            // A long line scrolls back and forth at the Settings help pace.
            // No clip is available here: whole characters leave on the left
            // and enter on the right, sliding inside the 8-pixel padding.
            auto footer = gsr::menu_text(help);
            const int left = x + 8, room = inner_width - 16;
            const double time = settings_help::Line::now();
            if (widget.help_cursor != widget.cursor || widget.help_started < 0) {
                widget.help_cursor = widget.cursor;
                widget.help_started = time;
            }
            int shift = settings_help::Line::offset(
                width(footer) - room, time - widget.help_started);
            std::size_t dropped = 0;
            while (dropped < footer.size() && width(footer.substr(0, dropped + 1)) <= shift)
                ++dropped;
            shift -= dropped ? width(footer.substr(0, dropped)) : 0;
            footer.erase(0, dropped);
            while (!footer.empty() && width(footer) - shift > room + 8) footer.pop_back();
            draw(footer, left - shift, y + (kRows - 2) * 8 - 8);
        }
    }
}

} // namespace gsr::qol_menu
