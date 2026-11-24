// Fair placement for free routes in The Lost Age. Each entry is a visit,
// so later treasure in the same place keeps its own progression section.
#pragma once
#include <array>
#include <iterator>
#include <string_view>

#include "tla_treasure_randomizer_data.h"

namespace gsr::treasure::tla_data {

struct RegionalVisit {
    std::string_view place;
    std::uint16_t section;
    std::uint16_t tier;
};

// Representatives are the upper medians of the participating guide sections.
// See build_tla_treasure_randomizer.py --measure-regions for the native
// encounter levels and vanilla treasure values for each visit.
inline constexpr RegionalVisit regional_visits[] = {
    // Early Eastern Sea visits, with dungeon encounters at levels 22–25.
    {"Yallam", 12, 15},
    {"Taopo Swamp", 12, 15},
    {"West Indra Islet", 12, 15},
    {"Kalt Island", 12, 15},
    {"Apojii Islands", 14, 15},
    {"Aqua Rock", 15, 15},
    {"Izumo", 17, 15},
    {"Gaia Rock", 17, 15},
    // Tougher Eastern Sea caves and ruins, with encounters at levels 25–31.
    // Treasure Isle section 12 is only the entry area. The Grind and Lift
    // rewards retain sections 24 and 28.
    {"Treasure Isle", 12, 18},
    {"Tundaria Tower", 16, 18},
    {"Izumo Ruins", 18, 18},
    {"Ankohl Ruins", 19, 18},
    {"Islet Cave", 20, 18},
};

inline constexpr auto region_sections = [] {
    std::array<std::uint16_t, std::size(spots)> sections;
    sections.fill(kNoSection);
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const Spot& spot = spots[i];
        if (spot.fixed) continue;
        for (const RegionalVisit& visit : regional_visits) {
            if (spot.section == visit.section && spot.place == visit.place) {
                sections[i] = visit.tier;
                break;
            }
        }
    }
    return sections;
}();

}  // namespace gsr::treasure::tla_data
