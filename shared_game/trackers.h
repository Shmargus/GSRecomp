// The trackers both games show (djinn_tracker.h, treasure_tracker.h): one
// install and one per-frame sample for each runner, which passes its game's
// tables (trackers_tbs.h; The Lost Age's src/tla_trackers.cpp).
#pragma once
#include "djinn_tracker.h"
#include "treasure_tracker.h"
#include "tracker_text.h"
#include <cstddef>
#include <cstdint>

namespace gsr::trackers {

// Once at startup, after anything else that installs extra ImGui drawing.
inline void install(const djinn_tracker::Game& djinn, const treasure_tracker::Game& treasure,
                    const tracker_text::Config& text) {
    tracker_text::config = &text;
    djinn_tracker::install(djinn);
    treasure_tracker::install(treasure);
}

// Emulation thread, once per frame.
inline void sample(const std::uint8_t* ewram, const std::uint8_t* rom, std::size_t rom_size) {
    if (rom && tracker_text::config && !tracker_text::ready.load(std::memory_order_relaxed)) {
        const auto& text = *tracker_text::config;
        const auto* djinn = djinn_tracker::game;
        const auto* treasure = treasure_tracker::game;
        if (djinn && treasure && text.language < 5 &&
            text.names.djinn.size() == std::size_t(djinn_tracker::kElements * djinn->per_element) &&
            text.names.places.size() == treasure->places.size() &&
            text.names.contents.size() == treasure->treasures.size()) {
            mods::TextEncoder decoder({rom, rom_size}, text.text_table, text.tree_table);
            for (std::size_t i = 0; i < text.names.djinn.size(); ++i)
                tracker_text::add(decoder, djinn->djinn[i].name, text.names.djinn[i]);
            for (std::size_t i = 0; i < text.names.places.size(); ++i)
                tracker_text::add(decoder, treasure->places[i], text.names.places[i]);
            for (std::size_t i = 0; i < text.names.contents.size(); ++i)
                tracker_text::add(decoder, treasure->treasures[i].contents, text.names.contents[i]);
            for (const auto& token : text.names.tokens)
                tracker_text::add(decoder, token.english, token.ids);
        } else std::fprintf(stderr, "[trackers] invalid localized-name table\n");
        tracker_text::ready.store(true, std::memory_order_release);
    }
    djinn_tracker::sample(ewram, rom, rom_size);
    treasure_tracker::sample(ewram, rom, rom_size);
}

} // namespace gsr::trackers
