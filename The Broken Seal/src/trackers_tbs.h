// The Broken Seal's tracker tables (trackers.h; the windows and the rules
// both games share are in djinn_tracker.h and treasure_tracker.h).
//
// Djinn Tracker.
// Measured, verified ROM 5c4695205413df7db52b9a184815a07783999971 (FACTS.md,
// "Djinn Tracker", 2026-10-30):
//   Party records 02000500 + character * 0x14C (story_progress.h has_item),
//   Isaac, Garet, Ivan, Mia; owned +0xF8 matches Clear Data's Djinn screen.
//   Names in text 0x45F + element * 20 + index (the same order as the bits
//   and as the Djinn flags 0x30 + element * 20 + index, grant 0807A0F4).
//   Pictures: field sprites 90 Venus, 91 Mercury, 92 Mars, 93 Jupiter in the
//   sprite records at 08185024; OBJ palette 0800779C.
// Locations: the former guide's authored Djinn routes
// (Archive/2026-10-30-tbs-guide).
//
// Treasure Tracker.
// Measured, verified ROM 5c4695205413df7db52b9a184815a07783999971 (FACTS.md,
// "Treasure Tracker", 2026-10-30): location lookup 0808B158 over the records
// at 0809DDD8, room keys 0809F1A8 + room * 8 + 2; current room/entrance
// 02000400/02000402.
#pragma once
#include "rom_version.h"
#include "../../shared_game/trackers.h"
#include "treasure_tracker_data.h"
#include "tracker_name_ids.h"

namespace gsr::djinn_tracker::tbs {

inline constexpr Djinni kDjinn[28] = {
    {"Flint", "World map south of {Vale}"},
    {"Granite", "{Kolima} tree-house basement"},
    {"Quartz", "{Mogall Forest} southwestern grass route"},
    {"Vine", "World-map forest beyond {Lamakan Desert}"},
    {"Sap", "{Vault Cave} lower route"},
    {"Ground", "Northern {Kalay Docks}"},
    {"Bane", "{Crossbone Isle} sixth floor"},
    {"Fizz", "{Mercury Lighthouse} entrance route"},
    {"Sleet", "{Mercury Lighthouse} hidden waterfall room"},
    {"Mist", "{Xian} eastern cliff"},
    {"Spritz", "{Altin Peak} cart route"},
    {"Hail", "World-map forest beyond {Tolbi}'s western bridge"},
    {"Tonic", "{Lunpa Fortress} bedroom passage"},
    {"Dew", "{Suhalla Gate} third-slide route"},
    {"Forge", "{Goma Cave} eastern ledge"},
    {"Fever", "{Imil} ice cave"},
    {"Corona", "World-map island north of {Xian}"},
    {"Scorch", "{Kalay Tunnel} eastern waterfall cavern"},
    {"Ember", "{Tolbi} southern cliff route"},
    {"Flash", "{Suhalla Desert} western stump crossing"},
    {"Torch", "{Lalivero} rooftops"},
    {"Gust", "{Bilibin} underground passage"},
    {"Breeze", "{Tret Tree} outer branches"},
    {"Zephyr", "{Fuchin Falls Cave} log route"},
    {"Smog", "{Lamakan Desert} stone formation"},
    {"Kite", "{Vale Cave} chase route"},
    {"Squall", "{Altmiller Cave} log chamber"},
    {"Luff", "{Babi Lighthouse} isolated ledge"},
};

inline const Game kGame = {kDjinn, 7, 7, 0x500u, 4, gs::rom(0x08185024u) - 0x08000000u,
                           gs::rom(0x0800779Cu) - 0x08000000u, {90, 91, 92, 93}};

} // namespace gsr::djinn_tracker::tbs

namespace gsr::treasure_tracker::tbs {

inline const Game kGame = {tbs_data::places, tbs_data::place_names, tbs_data::containers,
                           tbs_data::treasures, gs::rom(0x0809F1A8u) - 0x08000000u,
                           gs::rom(0x0809DDD8u) - 0x08000000u, 0x400u};

} // namespace gsr::treasure_tracker::tbs

namespace gsr::trackers::tbs {
inline const tracker_text::Config kText = {tracker_text::tbs_data::names,
    gs::rom(0x080736B8u) - 0x08000000u, gs::rom(0x0803842Cu) - 0x08000000u,
    static_cast<unsigned>(gs::rom_version())};
}
