// Treasure Tracker, both games: every treasure the field gives from an
// object, listed per location, each marked once the save has taken it. The
// location the party is in opens by itself; the others fold. Options in the
// window: chests only, and showing what each holds. F1 > Game switch, off by
// default; a tracker_window.h window like the Djinn Tracker. One copy; each
// runner installs its game's table through trackers.h (trackers_tbs.h, The
// Lost Age's src/tla_trackers.cpp) and samples once per frame.
//
// Measured, both ROMs verified (each game's FACTS.md, "Treasure Tracker",
// 2026-10-30); the tables come from The Broken Seal/tools/build_treasure_tracker.py:
//   Taken: the treasure's own flag (flag_bank.h).
//   Chest or not: the sub-type whose "checked the chest..." message the game
//   shows (kChest).
//   Location: the game's own lookup (room, entrance) -> name offset over its
//   eight-byte location records, with the room's key at room table + room *
//   8 + 2; fed the current room/entrance.
#pragma once
#include "flag_bank.h"
#include "game_options.h"
#include "host_config_ui.h"
#include "tracker_window.h"
#include "treasure_tracker_types.h"
#include "tracker_text.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace gsr::treasure_tracker {

constexpr const char* kOptionKey = "TreasureTracker";
constexpr const char* kSwitchLabel = "Treasure Tracker";
constexpr const char* kSwitchCaption =
    "A window listing every treasure per location, ticked once taken; the "
    "place you are in opens by itself. In the window: chests only, and "
    "what each one holds.";
constexpr const char* kChestsOnlyKey = "TreasureTrackerChestsOnly";
constexpr const char* kContentsKey = "TreasureTrackerContents";

struct Game {
    std::span<const char* const> places;
    std::span<const PlaceName> place_names;
    std::span<const Container> containers;
    std::span<const Treasure> treasures;  // grouped by place, in place order
    std::uint32_t room_records;           // ROM offsets (address - 0x08000000)
    std::uint32_t location_records;
    std::uint32_t current_room;           // EWRAM offset: room, then entrance (u16)
};

constexpr std::size_t kMaxTreasures = 512;
inline const Game* game = nullptr;
// Bit i of taken[i / 32] = treasures[i] taken in the loaded save.
inline std::array<std::atomic<std::uint32_t>, kMaxTreasures / 32> taken{};
inline std::atomic<int> current_place{-1};

// The game's location lookup: the name offset for (room, entrance), 0 if none.
inline std::uint32_t location(const Game& g, const std::uint8_t* rom, std::size_t size, unsigned room,
                              unsigned entrance, bool* found = nullptr) {
    if (found) *found = false;
    const auto u16 = [&](std::uint32_t at) -> unsigned { return at + 1 < size ? rom[at] | (rom[at + 1] << 8) : 0xFFFFu; };
    if (g.room_records + room * 8 + 2 >= size) return 0;
    const int room_key = std::int8_t(rom[g.room_records + room * 8 + 2]);
    const int door = int(std::int16_t(entrance));
    for (std::uint32_t at = g.location_records; at + 8 <= size; at += 8) {
        const int key = std::int16_t(u16(at));
        if (key == -1) return 0;
        const unsigned door_bits = u16(at + 2);
        if (key != ((door_bits & 0x8000u) ? int(room) : room_key)) continue;
        const unsigned low = door_bits & 0x7FFFu;
        if (low == 0x7FFFu || int((low ^ 0x4000u)) - 0x4000 == door) {
            if (found) *found = true;
            return u16(at + 4) | (std::uint32_t(u16(at + 6)) << 16);
        }
    }
    return 0;
}

// Emulation thread, once per frame.
inline void sample(const std::uint8_t* ewram, const std::uint8_t* rom, std::size_t rom_size) {
    if (!game || !ewram) return;
    const Game& g = *game;
    for (std::size_t w = 0; w < taken.size(); ++w) {
        std::uint32_t bits = 0;
        for (std::size_t i = w * 32; i < std::min(g.treasures.size(), w * 32 + 32); ++i)
            if (read_flag(ewram, g.treasures[i].flag)) bits |= 1u << (i - w * 32);
        taken[w].store(bits, std::memory_order_relaxed);
    }
    if (!rom) return;
    const std::uint8_t* at = ewram + g.current_room;
    const std::uint32_t name = location(g, rom, rom_size, at[0] | (at[1] << 8), at[2] | (at[3] << 8));
    int place = -1;
    for (const auto& p : g.place_names)
        if (p.offset == name) place = int(p.place);
    current_place.store(place, std::memory_order_relaxed);
}

#ifdef GBARECOMP_HAVE_IMGUI
inline tracker_window::Window window{"TreasureTracker", 360.0f};
inline std::vector<bool> open;
inline int shown_place = -2;  // the place `open` last followed

inline const char* container(const Game& g, unsigned sub) {
    for (const auto& c : g.containers)
        if (c.sub == sub) return gbarecomp::ui_text(c.name);
    return gbarecomp::ui_text("Treasure");
}

inline void draw() {
    if (window.previous) window.previous();
    if (!game || gbarecomp::game_option_get(kOptionKey, 0) == 0) return;
    const Game& g = *game;
    const std::size_t count_all = g.treasures.size(), places = g.places.size();
    const bool chests_only = gbarecomp::game_option_get(kChestsOnlyKey, 0) != 0;
    const bool contents = gbarecomp::game_option_get(kContentsKey, 0) != 0;
    const auto is_taken = [](std::size_t i) {
        return (taken[i / 32].load(std::memory_order_relaxed) >> (i % 32)) & 1u;
    };
    std::vector<int> count(places, 0), done(places, 0);
    int total = 0, total_done = 0;
    for (std::size_t i = 0; i < count_all; ++i) {
        const auto& t = g.treasures[i];
        if (chests_only && t.sub != kChest) continue;
        ++count[t.place];
        ++total;
        if (is_taken(i)) { ++done[t.place]; ++total_done; }
    }
    // Arriving somewhere opens that place and folds the rest.
    open.resize(places, false);
    const int here = current_place.load(std::memory_order_relaxed);
    const bool arrived = here >= 0 && here != shown_place;
    if (arrived) {
        shown_place = here;
        for (std::size_t p = 0; p < places; ++p) open[p] = int(p) == here;
    }
    const auto title = tracker_text::format("%s  %d/%d",
        gbarecomp::ui_text(chests_only ? "Chests" : "Treasures"), total_done, total);
    if (tracker_window::begin(window, ImVec2(300.0f, 420.0f), title.c_str())) {
        bool value = chests_only;
        if (ImGui::Checkbox(gbarecomp::ui_text("Chests only###ChestsOnly"), &value)) gbarecomp::game_option_set(kChestsOnlyKey, value ? 1 : 0);
        ImGui::SameLine();
        value = contents;
        if (ImGui::Checkbox(gbarecomp::ui_text("Contents###Contents"), &value)) gbarecomp::game_option_set(kContentsKey, value ? 1 : 0);
        ImGui::BeginChild("##places");
        const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        std::size_t i = 0;
        for (std::size_t p = 0; p < places; ++p) {
            const std::size_t first = i;
            while (i < count_all && g.treasures[i].place == p) ++i;
            if (!count[p]) continue;
            if (arrived && int(p) == here) ImGui::SetScrollHereY(0.0f);
            const auto header = tracker_text::format("%s  %d/%d", tracker_text::name(g.places[p]).c_str(), done[p], count[p]);
            if (tracker_window::fold_header(int(p), open[p], header.c_str())) open[p] = !open[p];
            if (!open[p]) continue;
            ImGui::Indent();
            for (std::size_t k = first; k < i; ++k) {
                const auto& t = g.treasures[k];
                if (chests_only && t.sub != kChest) continue;
                const bool got = is_taken(k);
                if (contents) ImGui::TextColored(got ? dim : text, "%s %s: %s", got ? "x" : "-", container(g, t.sub), tracker_text::contents(t.contents).c_str());
                else ImGui::TextColored(got ? dim : text, "%s %s", got ? "x" : "-", container(g, t.sub));
            }
            ImGui::Unindent();
        }
        ImGui::EndChild();
    }
    tracker_window::end(window);
}
#endif

// Once at startup, after anything else that installs extra ImGui drawing.
inline void install(const Game& g) {
    if (g.treasures.size() > kMaxTreasures) return;
    game = &g;
#ifdef GBARECOMP_HAVE_IMGUI
    tracker_window::chain(window, draw);
#endif
}

} // namespace gsr::treasure_tracker
