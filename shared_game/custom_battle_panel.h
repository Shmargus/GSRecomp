#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include "custom_battle.h"
#include "menu_text.h"

// The opponent panel's contents (custom_battle.h), one copy for both runners:
// the Monsters/Bosses/Arena tabs, the list, the highlighted preview, and
// the picks so far. Each runner paints the window in
// the game's own pieces first and passes its menu font and pixels, in native
// coordinates: draw_text(text, x, y), text_width(text), pixel(x, y) ->
// std::uint8_t* RGB or nullptr. inner_x/inner_y: the window's inside.
namespace gsr::custom_battle {

template <class DrawText, class TextWidth, class Pixel>
void paint_panel(const PanelView& view, DrawText& draw_text, TextWidth& text_width,
                 int inner_x, int inner_y, Pixel& pixel) {
    constexpr int kInnerWidth = (kPanelTiles - 2) * 8;
    constexpr int kInnerHeight = (kPanelRows - 2) * 8;
    constexpr int kListX = 8, kListY = 14, kRowPitch = 12, kListWidth = 112;
    constexpr int kPictureX = 128, kPictureCentreY = 47, kFrameHeight = 72;
    constexpr int kFooterY = kInnerHeight - 11;
    const auto centred = [&](const std::string& text, int centre_x, int y) {
        draw_text(text, inner_x + centre_x - text_width(text) / 2, inner_y + y);
    };
    const auto fitted = [&](std::string text, int room) {
        if (text_width(text) <= room) return text;
        while (!text.empty() && text_width(text + "...") > room) text.pop_back();
        return text + "...";
    };
    // Darkens the window inside, as a highlight or a picture frame.
    const auto shade = [&](int x, int y, int w, int h, int keep) {
        for (int dy = 0; dy < h; ++dy)
            for (int dx = 0; dx < w; ++dx)
                if (std::uint8_t* p = pixel(inner_x + x + dx, inner_y + y + dy))
                    for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(p[c] * keep / 8);
    };
    // A small white arrow with the text's shadow, its point at row 0 of
    // a 3-pixel triangle: up, down, left or right.
    enum Point { kUp, kDown, kLeft, kRight };
    const auto arrow = [&](int centre_x, int y, Point point) {
        for (int pass = 0; pass < 2; ++pass) {
            const std::uint8_t level = pass ? 0xFFu : 0x00u;
            for (int row = 0; row < 3; ++row) {
                const int half = point == kUp || point == kLeft ? row : 2 - row;
                for (int d = -half; d <= half; ++d) {
                    const bool across = point == kUp || point == kDown;
                    const int x = centre_x + (across ? d : row - 1);
                    const int ay = y + (across ? row : d + 1);
                    if (std::uint8_t* p = pixel(inner_x + x + (pass ? 0 : 1),
                                                inner_y + ay + (pass ? 0 : 1)))
                        p[0] = p[1] = p[2] = level;
                }
            }
        }
    };
    // The pages as tabs, Left/Right between them; the shaded one is open.
    const std::string tabs[] = {gsr::menu_text("Monsters"), gsr::menu_text("Bosses"),
                                gsr::menu_text("Arena")};
    int tab_x = kListX;
    arrow(tab_x - 6, 2, kLeft);
    for (int t = 0; t < static_cast<int>(Page::Count); ++t) {
        const int w = text_width(tabs[t]);
        if (t == static_cast<int>(view.page)) shade(tab_x - 3, -2, w + 6, 12, 5);
        draw_text(tabs[t], inner_x + tab_x, inner_y);
        tab_x += w + 10;
    }
    arrow(tab_x - 5, 2, kRight);
    const auto hint = gsr::menu_text("L/R: page");
    draw_text(hint, inner_x + kInnerWidth - 4 - text_width(hint), inner_y);
    if (view.rows.empty())
        draw_text(fitted(gsr::menu_text(view.page == Page::Bosses ? "No boss fits." : "No monster fits."), kListWidth),
                  inner_x + kListX, inner_y + kListY);
    for (int i = 0; i < static_cast<int>(view.rows.size()); ++i) {
        const int y = kListY + i * kRowPitch;
        if (i == view.selected) shade(kListX - 4, y - 2, kListWidth + 4, kRowPitch, 5);
        draw_text(fitted(view.rows[i], kListWidth), inner_x + kListX, inner_y + y);
    }
    if (view.more_above) arrow(kListX + kListWidth - 8, kListY - 6, kUp);
    if (view.more_below)
        arrow(kListX + kListWidth - 8, kListY + kPanelVisibleRows * kRowPitch - 4, kDown);
    // The picture, in a darker frame, centred; one wider or taller than
    // the frame (two-piece Kraken, Fusion Dragon) shrinks to fit it.
    const int frame_w = kInnerWidth - kPictureX - 4;
    shade(kPictureX, kListY - 3, frame_w, kFrameHeight, 6);
    int shown_w = view.picture_width, shown_h = view.picture_height;
    if (shown_w > frame_w || shown_h > kFrameHeight) {
        const double fit = std::min(double(frame_w) / view.picture_width,
                                    double(kFrameHeight) / view.picture_height);
        shown_w = static_cast<int>(view.picture_width * fit);
        shown_h = static_cast<int>(view.picture_height * fit);
    }
    const int px = kPictureX + (frame_w - shown_w) / 2;
    const int py = kPictureCentreY - shown_h / 2;
    for (int y = 0; y < shown_h; ++y)
        for (int x = 0; x < shown_w; ++x) {
            const int sx = x * view.picture_width / shown_w;
            const int sy = y * view.picture_height / shown_h;
            const std::uint32_t c =
                view.picture[static_cast<std::size_t>(sy * view.picture_width + sx)];
            if (!(c >> 24)) continue;
            if (std::uint8_t* p = pixel(inner_x + px + x, inner_y + py + y)) {
                p[0] = static_cast<std::uint8_t>(c >> 16);
                p[1] = static_cast<std::uint8_t>(c >> 8);
                p[2] = static_cast<std::uint8_t>(c);
            }
        }
    if (!view.level.empty()) centred(view.level, kPictureX + frame_w / 2,
                                     kListY - 3 + kFrameHeight + 2);
    if (!view.chosen.empty()) {
        const int room = kInnerWidth - 2 * kListX;
        const auto line = fitted(gsr::menu_text("Chosen:") + " " + view.chosen, room);
        draw_text(line, inner_x + kListX, inner_y + kFooterY);
    }
}

}  // namespace gsr::custom_battle
