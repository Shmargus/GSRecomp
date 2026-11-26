// Djinn Tracker, both games: a window in the F1 menu's own style with every
// Djinni, one foldable section per element with its count. Djinn the save
// has are bright; the rest are question marks in their element, or, with
// "show all", their own picture dimmed in place. Hovering a Djinni shows its
// name and where it is found. F1 > Game switch, off by default. Where the
// graphics card draws (OpenGL, ImGui viewports) it is its own desktop window,
// dragged by its title and resized from any edge; otherwise it floats inside
// the game window. Position, size and folds are saved. A phone shows it on
// the menu's Trackers page instead (trackers.h). One copy; each runner
// installs its game's table through trackers.h (trackers_tbs.h, The Lost
// Age's src/tla_trackers.cpp) and samples once per frame.
//
// Measured, both ROMs verified:
//   Ownership: party record + 0xF8, four u32 (Venus, Mercury, Mars,
//   Jupiter), bit i = the element's Djinni i; the Djinn screen's counts at
//   +0x118 agree. A Djinni is owned when any record holds it.
//   Pictures: field sprite record (+0 width, +1 height, +0xA frame codec,
//   +0xC frame pointer table), frame 0 (facing front), 32x32, codec 3: the
//   unpacker of The Broken Seal's 0800A97C, then its 08009BB8 colour stream,
//   colours from the OBJ palette. The art is read from the player's ROM in
//   memory at run time; nothing is stored.
#pragma once
#include "game_options.h"
#include "host_config_ui.h"
#include "tracker_text.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "tracker_window.h"

namespace gsr::djinn_tracker {

constexpr const char* kOptionKey = "DjinnTracker";
constexpr const char* kSwitchLabel = "Djinn Tracker";
constexpr const char* kSwitchCaption =
    "A window with every Djinni, counted per element. Djinn you have not "
    "found are question marks; point at one to see where it is. Drag it by "
    "its title, resize it from an edge, click an element to fold it.";
constexpr const char* kShowAllKey = "DjinnTrackerShowAll";
constexpr const char* kShowAllLabel = "Djinn Tracker: show all Djinn";
constexpr const char* kShowAllCaption =
    "Djinn you have not found yet keep their name and picture, dimmed, in "
    "their element instead of hiding as question marks.";
// Bit e set = element e folded.
constexpr const char* kFoldedKey = "DjinnTrackerFolded";

constexpr int kElements = 4;
inline constexpr const char* kElementNames[kElements] = {"Venus", "Mercury", "Mars", "Jupiter"};

struct Djinni { const char* name; const char* location; };
struct Game {
    const Djinni* djinn;          // element-major, per_element each
    int per_element;
    int columns;                  // Djinn per row
    std::uint32_t party_records;  // EWRAM offset of record 0
    int party_count;
    std::uint32_t sprite_records; // ROM offsets (address - 0x08000000)
    std::uint32_t obj_palette;
    unsigned sprites[kElements];
};
constexpr std::uint32_t kRecordStride = 0x14Cu;
constexpr std::uint32_t kOwnedOffset = 0xF8u;
constexpr std::uint32_t kSpriteRecordSize = 20u;
constexpr unsigned kLastColour = 0xDFu;
constexpr unsigned kLzCodec = 3u;

struct Picture {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;  // 0xAARRGGBB; 0 = clear
};
inline const Game* game = nullptr;
inline std::array<Picture, kElements> pictures;
inline std::atomic<bool> pictures_ready{false};
inline bool pictures_tried = false;
// Bit i of owned[e] = the element's Djinni i in the loaded save.
inline std::array<std::atomic<std::uint32_t>, kElements> owned{};
// Random Djinn (random_djinn.h), written by the emulation thread. Slots are
// then spots: gives[element * 20 + index] is 1 + the Djinni (element * 20 +
// index) the spot of Djinni (element, index) gives, 0 where the spot is not
// shuffled; bit index of spot_taken[element] is that spot's flag (0x30 +
// element * 20 + index), set once it has been taken.
inline std::array<std::atomic<std::int16_t>, kElements * 20> gives{};
inline std::array<std::atomic<std::uint32_t>, kElements> spot_taken{};

// Frame 0 of field sprite `sprite`, as The Broken Seal's loader 0800AE80
// builds it for codec 3: 0800A97C unpacks it, then 08009BB8's colour stream.
inline bool decode(const std::uint8_t* rom, std::size_t size, const Game& g, unsigned sprite, Picture& pic) {
    const auto u8 = [&](std::uint32_t at) -> unsigned { return at < size ? rom[at] : 0u; };
    const auto u16 = [&](std::uint32_t at) { return u8(at) | (u8(at + 1) << 8); };
    const auto u32 = [&](std::uint32_t at) { return u16(at) | (std::uint32_t(u16(at + 2)) << 16); };
    const auto offset = [](std::uint32_t address) { return address - 0x08000000u; };
    const std::uint32_t record = g.sprite_records + sprite * kSpriteRecordSize;
    const int width = int(u8(record)), height = int(u8(record + 1));
    const std::uint32_t frames = u32(record + 0xC);
    if (record + kSpriteRecordSize > size || !width || !height || frames < 0x08000000u ||
        u8(record + 0xA) != kLzCodec)
        return false;
    const std::uint32_t at = offset(u32(offset(frames)));
    if (at >= size) return false;
    // 0800A97C: halfword offset to the flag bytes (0 = not packed), literal
    // bytes from at+2; flags low bit first, 1 a literal, 0 a big-endian
    // halfword copy: count (top 4 bits, or 0 then the next byte + 0x10) + 2
    // bytes from (at+2) - (low 12 bits); copy 0 ends it.
    std::vector<std::uint8_t> stream;
    const std::size_t limit = std::size_t(width) * height * 4u;
    const std::uint32_t literal_base = at + 2;
    if (const unsigned control_offset = u16(at); control_offset == 0) {
        for (std::uint32_t i = literal_base; i < size && stream.size() < limit; ++i) {
            stream.push_back(rom[i]);
            if (!rom[i]) break;
        }
    } else {
        std::uint32_t control = at + control_offset, literal = literal_base;
        unsigned flags = u8(control++) | 0x100u;
        for (;;) {
            if (flags == 1u) flags = u8(control++) | 0x100u;
            const bool is_literal = flags & 1u;
            flags >>= 1;
            if (stream.size() > limit) return false;
            if (is_literal) { stream.push_back(std::uint8_t(u8(literal++))); continue; }
            const unsigned word = (u8(control) << 8) | u8(control + 1);
            control += 2;
            if (!word) break;
            unsigned count = word >> 12;
            if (!count) count = u8(control++) + 0x10u;
            count += 2;
            const std::uint32_t distance = word & 0xFFFu;
            if (distance > literal_base) return false;
            for (unsigned k = 0; k < count; ++k)
                stream.push_back(std::uint8_t(u8(literal_base - distance + k)));
        }
    }
    // 08009BB8: 0 ends the frame, above 0xDF skips (byte - 0xDF) clear
    // pixels, anything else is a colour of the OBJ palette.
    pic.width = width;
    pic.height = height;
    pic.pixels.assign(std::size_t(width) * height, 0u);
    std::size_t p = 0;
    for (const unsigned b : stream) {
        if (!b || p >= pic.pixels.size()) break;
        if (b > kLastColour) { p += b - kLastColour; continue; }
        const unsigned c = u16(g.obj_palette + b * 2) & 0x7FFFu;
        const unsigned r = c & 31u, gr = (c >> 5) & 31u, bl = (c >> 10) & 31u;
        pic.pixels[p++] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) |
                          (((gr << 3) | (gr >> 2)) << 8) | ((bl << 3) | (bl >> 2));
    }
    return true;
}

// Emulation thread, once per frame.
inline void sample(const std::uint8_t* ewram, const std::uint8_t* rom, std::size_t rom_size) {
    if (!game) return;
    if (!pictures_tried && rom) {
        pictures_tried = true;
        bool ok = true;
        for (int e = 0; e < kElements; ++e) ok &= decode(rom, rom_size, *game, game->sprites[e], pictures[e]);
        pictures_ready.store(ok, std::memory_order_release);
    }
    if (!ewram) return;
    for (int e = 0; e < kElements; ++e) {
        std::uint32_t mask = 0;
        for (int c = 0; c < game->party_count; ++c) {
            const std::uint8_t* p = ewram + game->party_records + c * kRecordStride + kOwnedOffset + e * 4;
            mask |= p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
        }
        owned[e].store(mask & ((1u << game->per_element) - 1u), std::memory_order_relaxed);
    }
}

#ifdef GBARECOMP_HAVE_IMGUI
constexpr float kDefaultCell = 56.0f;
using tracker_window::kGold;
inline tracker_window::Window window{"DjinnTracker", 0.0f};
inline int folded = -1;  // -1: not read yet

// One Djinni, its rows merged into runs of one colour. Dimmed: darker and
// partly see-through, so the shape still shows what is missing.
inline void draw_picture(ImDrawList* draw, const Picture& pic, ImVec2 pos, float scale, bool bright) {
    for (int y = 0; y < pic.height; ++y)
        for (int x = 0; x < pic.width;) {
            const std::uint32_t c = pic.pixels[std::size_t(y) * pic.width + x];
            int end = x + 1;
            while (end < pic.width && pic.pixels[std::size_t(y) * pic.width + end] == c) ++end;
            if (c) {
                unsigned r = (c >> 16) & 255u, g = (c >> 8) & 255u, b = c & 255u, a = 255u;
                if (!bright) { r = r * 3 / 10; g = g * 3 / 10; b = b * 3 / 10; a = 150u; }
                draw->AddRectFilled(ImVec2(pos.x + x * scale, pos.y + y * scale),
                                    ImVec2(pos.x + end * scale, pos.y + (y + 1) * scale),
                                    IM_COL32(r, g, b, a));
            }
            x = end;
        }
}

inline void draw_question_mark(ImDrawList* draw, ImVec2 lo, float cell) {
    const float font_size = cell * 0.6f;
    const ImVec2 mark = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "?");
    draw->AddRect(ImVec2(lo.x + 3, lo.y + 3), ImVec2(lo.x + cell - 3, lo.y + cell - 3),
                  ImGui::GetColorU32(ImGuiCol_Border, 0.5f), cell * 0.15f);
    draw->AddText(ImGui::GetFont(), font_size,
                  ImVec2(lo.x + (cell - mark.x) * 0.5f, lo.y + (cell - mark.y) * 0.5f),
                  IM_COL32(200, 200, 200, 170), "?");
}

// What the save has: each slot found or not, and the Djinni it shows (its
// own, or in a Random Djinn run the one its spot gives).
struct Found {
    std::array<std::uint32_t, kElements> have{};
    std::array<int, kElements> found{};
    std::vector<int> shows;
    int total = 0;
};
inline Found found_now(const Game& g) {
    Found f;
    f.shows.resize(std::size_t(kElements * g.per_element));
    for (int e = 0; e < kElements; ++e) {
        f.have[e] = owned[e].load(std::memory_order_relaxed);
        const std::uint32_t spots = spot_taken[e].load(std::memory_order_relaxed);
        for (int i = 0; i < g.per_element; ++i) {
            int& slot = f.shows[std::size_t(e * g.per_element + i)];
            slot = e * g.per_element + i;
            if (const int given = gives[std::size_t(e * 20 + i)].load(std::memory_order_relaxed) - 1;
                given >= 0 && given % 20 < g.per_element) {
                slot = given / 20 * g.per_element + given % 20;
                f.have[e] = (f.have[e] & ~(1u << i)) | (((spots >> i) & 1u) << i);
            }
            f.found[e] += (f.have[e] >> i) & 1u;
        }
        f.total += f.found[e];
    }
    return f;
}

inline std::string title(const Game& g, const Found& f) {
    return tracker_text::format(gbarecomp::ui_text("Djinn  %d/%d"), f.total, kElements * g.per_element);
}

// One section per element: a header line, tapped or clicked to fold it, over
// rows of `columns`; the Djinn shrink or grow to fill the space left. Unfound
// Djinn are question marks unless showing all. Pointing at (or tapping) one
// shows its name and where it is found.
inline void draw_contents(const Game& g, const Found& f) {
    if (folded < 0) folded = gbarecomp::game_option_get(kFoldedKey, 0);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line_h = ImGui::GetTextLineHeight() + style.ItemSpacing.y;
    const int element_rows = (g.per_element + g.columns - 1) / g.columns;
    const bool show_all = gbarecomp::game_option_get(kShowAllKey, 0) != 0;
    float fixed_h = 0.0f;
    int rows = 0;
    for (int e = 0; e < kElements; ++e) {
        fixed_h += line_h;
        if (!(folded & (1 << e))) {
            rows += element_rows;
            fixed_h += style.ItemSpacing.y;
        }
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cell = std::max(8.0f, rows ? std::min(avail.x / g.columns, (avail.y - fixed_h) / rows)
                                           : avail.x / g.columns);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const bool ready = pictures_ready.load(std::memory_order_acquire);
    int hovered = -1;
    for (int e = 0; e < kElements; ++e) {
        const bool open = !(folded & (1 << e));
        const auto text = tracker_text::format(gbarecomp::ui_text("%s Djinn  %d/%d"),
            gbarecomp::ui_text(kElementNames[e]), f.found[e], g.per_element);
        if (tracker_window::fold_header(e, open, text.c_str())) {
            folded ^= 1 << e;
            gbarecomp::game_option_set(kFoldedKey, folded);
        }
        if (!open) continue;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        for (int i = 0; i < g.per_element; ++i) {
            const bool bright = (f.have[e] >> i) & 1u;
            const ImVec2 lo(origin.x + (i % g.columns) * cell, origin.y + (i / g.columns) * cell);
            if (!bright && !show_all) {
                draw_question_mark(list, lo, cell);
            } else if (ready) {
                const Picture& pic = pictures[f.shows[std::size_t(e * g.per_element + i)] / g.per_element];
                const float scale = cell / float(std::max(pic.width, pic.height));
                draw_picture(list, pic, ImVec2(lo.x + (cell - pic.width * scale) * 0.5f,
                                               lo.y + (cell - pic.height * scale) * 0.5f), scale, bright);
            } else {
                list->AddRect(ImVec2(lo.x + 2, lo.y + 2), ImVec2(lo.x + cell - 2, lo.y + cell - 2),
                              ImGui::GetColorU32(ImGuiCol_Border, bright ? 1.0f : 0.35f));
            }
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(lo, ImVec2(lo.x + cell, lo.y + cell)))
                hovered = e * g.per_element + i;
        }
        ImGui::Dummy(ImVec2(cell * g.columns, cell * element_rows));
    }
    if (hovered >= 0) {
        const bool have_it = (f.have[hovered / g.per_element] >> (hovered % g.per_element)) & 1u;
        ImGui::BeginTooltip();
        if (show_all || have_it)
            ImGui::TextColored(kGold, "%s%s", tracker_text::name(g.djinn[f.shows[std::size_t(hovered)]].name).c_str(),
                have_it ? "" : gbarecomp::ui_text(" (not found yet)"));
        else
            ImGui::TextColored(kGold, "%s", gbarecomp::ui_text("Unfound Djinni"));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
            std::max(1.0f, ImGui::GetMainViewport()->WorkSize.x - style.WindowPadding.x * 2));
        // The slot is the spot, so its location is the spot's own.
        ImGui::TextUnformatted(tracker_text::route(g.djinn[hovered].location).c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// The computer's window (F1 > Game switch).
inline void draw() {
    if (window.previous) window.previous();
    if (!game || gbarecomp::game_option_get(kOptionKey, 0) == 0) return;
    const Game& g = *game;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line_h = ImGui::GetTextLineHeight() + style.ItemSpacing.y;
    const int element_rows = (g.per_element + g.columns - 1) / g.columns;
    // Default: every element open at kDefaultCell.
    const ImVec2 size(g.columns * kDefaultCell + style.WindowPadding.x * 2,
                      (kElements + 2) * line_h + kElements * (element_rows * kDefaultCell + style.ItemSpacing.y) +
                          style.WindowPadding.y * 2);
    const Found f = found_now(g);
    if (tracker_window::begin(window, size, title(g, f).c_str(), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        draw_contents(g, f);
    tracker_window::end(window);
}

// The phone's Trackers page (trackers.h): the title with the "show all"
// switch, then the Djinn filling the rest.
inline void draw_page() {
    if (!game) return;
    const Game& g = *game;
    const Found f = found_now(g);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kGold, "%s", title(g, f).c_str());
    ImGui::SameLine();
    bool show_all = gbarecomp::game_option_get(kShowAllKey, 0) != 0;
    if (ImGui::Checkbox(gbarecomp::ui_text("Show all###DjinnShowAll"), &show_all))
        gbarecomp::game_option_set(kShowAllKey, show_all ? 1 : 0);
    draw_contents(g, f);
}
#endif

// Once at startup, after anything else that installs extra ImGui drawing.
// trackers.h then shows it: a window on a computer, a page on a phone.
inline void install(const Game& g) { game = &g; }

} // namespace gsr::djinn_tracker
