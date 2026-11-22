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
// Phones have no tracker windows: their screen is the game and its buttons,
// and the F1 Game page does not list the tracker switches there.
inline void install(const djinn_tracker::Game& djinn, const treasure_tracker::Game& treasure,
                    const tracker_text::Config& text) {
#if defined(__ANDROID__)
    (void)djinn; (void)treasure; (void)text;
    return;
#endif
    tracker_text::config = &text;
    djinn_tracker::install(djinn);
    treasure_tracker::install(treasure);
}

// Emulation thread, once per frame.
inline void sample(const std::uint8_t* ewram, const std::uint8_t* rom, std::size_t rom_size,
                   unsigned (*planned_reward)(unsigned), unsigned (*planned_gift)(unsigned, unsigned)) {
#if defined(__ANDROID__)
    (void)ewram; (void)rom; (void)rom_size; (void)planned_reward; (void)planned_gift;
    return;
#endif
    if (rom && tracker_text::config && !tracker_text::ready.load(std::memory_order_relaxed)) {
        const auto& text = *tracker_text::config;
        const auto* djinn = djinn_tracker::game;
        const auto* treasure = treasure_tracker::game;
        if (djinn && treasure && text.language < 5 &&
            text.names.djinn.size() == std::size_t(djinn_tracker::kElements * djinn->per_element) &&
            text.names.places.size() == treasure->places.size()) {
            mods::TextEncoder decoder({rom, rom_size}, text.text_table, text.tree_table);
            for (std::size_t i = 0; i < text.names.djinn.size(); ++i)
                tracker_text::add(decoder, djinn->djinn[i].name, text.names.djinn[i]);
            for (std::size_t i = 0; i < text.names.places.size(); ++i)
                tracker_text::add(decoder, treasure->places[i], text.names.places[i]);
            for (const auto& token : text.names.tokens)
                tracker_text::add(decoder, token.english, token.ids);
            for (unsigned item = 1; item < text.item_count; ++item)
                tracker_text::add_reward(decoder, text.item_text + item);
            for (const auto& t : treasure->treasures)
                if (t.sub == treasure_tracker::kTablet)
                    tracker_text::add_reward(decoder, text.ability_text + t.reward);
        } else std::fprintf(stderr, "[trackers] invalid localized-name table\n");
        tracker_text::ready.store(true, std::memory_order_release);
    }
    djinn_tracker::sample(ewram, rom, rom_size);
    treasure_tracker::sample(ewram, rom, rom_size, planned_reward, planned_gift);
}

} // namespace gsr::trackers
