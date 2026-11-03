// Golden Sun: The Lost Age -- the Djinn Tracker and the Treasure Tracker.
// The windows and the rules both games share are in ../../The Broken
// Seal/src/ (trackers.h, djinn_tracker.h, treasure_tracker.h; one copy);
// this file installs The Lost Age's tables. F1 Game switches
// (src/tla_walk_speed.cpp lists them) and launcher checkboxes, off by default.
//
// Djinn Tracker.
// Measured, verified ROM b500663220cb9bf56b9f8e8c0c544f1d6fa3a824 (FACTS.md,
// "Djinn Tracker", 2026-10-30):
//   Names: text 0x6D3 + element * 20 + index (tables 0x080A9F54/0x08060C30),
//   18 per element: The Broken Seal's seven, then this game's eleven.
//   Owned: party records 0x02000520 + character * 0x14C, all eight (Isaac's
//   four, Felix's four), +0xF8; Jimmy's save: bits 0..16 in use, each
//   record's count at +0x118 equals its bits, nothing above bit 17.
//   Pictures: sprite records 0x08300000 + sprite * 20 (0x080227E0); the
//   Djinni are sprites 243 Venus, 244 Mercury, 245 Mars, 246 Jupiter, records
//   byte-identical to The Broken Seal's 90..93, and frame 0 decodes pixel for
//   pixel the same with the palette at 0x08017B10 (its 0xE0 colours equal
//   The Broken Seal's 0x0800779C).
// Locations (Kraden's, checked against a public Djinn list, 2026-10-30;
// unconfirmed in game). Four of The Broken Seal's Djinn can also be found
// here when a game starts without a transfer.
//
// Treasure Tracker.
// Measured, verified ROM b500663220cb9bf56b9f8e8c0c544f1d6fa3a824 (FACTS.md,
// "Treasure Tracker", 2026-10-30): the table is the ROM's treasure table
// 0x080F2204 (tla_treasure_tracker_data.h); location lookup 0x080CA280 over
// the records at 0x080EF4A4, room keys 0x080F17A8 + room * 8 + 2; current
// room/entrance 0x02000420/0x02000422.

#include "../../shared_game/trackers.h"
#include "tla_treasure_tracker_data.h"
#include "tla_tracker_name_ids.h"
#include "gba_bus.h"
#include "runtime_bus_bridge.h"
#include "tla.h"
#include "tla_rom_version.h"

namespace {

namespace dt = gsr::djinn_tracker;
namespace tt = gsr::treasure_tracker;

constexpr dt::Djinni kDjinn[72] = {
    {"Flint", "From {Golden Sun}, with {Isaac}'s party"},
    {"Granite", "From {Golden Sun}, with {Isaac}'s party"},
    {"Quartz", "From {Golden Sun}, with {Isaac}'s party"},
    {"Vine", "From {Golden Sun}, with {Isaac}'s party"},
    {"Sap", "From {Golden Sun}; without a transfer, {Treasure Isle} beside the door to the {summon tablet} room"},
    {"Ground", "From {Golden Sun}, with {Isaac}'s party"},
    {"Bane", "From {Golden Sun}, with {Isaac}'s party"},
    {"Echo", "World map south of {Daila}: joins as you leave town"},
    {"Iron", "World map southeast of {Gondowan Cliffs} by the river (battle)"},
    {"Steel", "{Gabomba Statue}: reverse the cogs, ride the left cog down"},
    {"Mud", "{Gabomba Catacombs}: {Cyclone} the grass in the northeast"},
    {"Flower", "{Taopo Swamp}: block the vent with the pillar, then the slide"},
    {"Meld", "Islet cave of the animal trader: across two logs"},
    {"Petra", "World map: up the river near {Shaman Village Cave}"},
    {"Salt", "{Contigo}: {Scoop} the circle in the grass by the south entrance"},
    {"Geode", "{Atteka Inlet}: {Lift} the round rock, {Cyclone} the grass"},
    {"Mold", "{Prox}: up the frozen river, slide the ice, {Scoop} the snow"},
    {"Crystal", "{Yampi Desert Cave}: {Scoop} the moving sand pile"},
    {"Fizz", "From {Golden Sun}, with {Isaac}'s party"},
    {"Sleet", "From {Golden Sun}, with {Isaac}'s party"},
    {"Mist", "From {Golden Sun}, with {Isaac}'s party"},
    {"Spritz", "From {Golden Sun}, with {Isaac}'s party"},
    {"Hail", "From {Golden Sun}, with {Isaac}'s party"},
    {"Tonic", "From {Golden Sun}; without a transfer, {Prox}'s lone snowy house northeast of town"},
    {"Dew", "From {Golden Sun}, with {Isaac}'s party"},
    {"Fog", "{Kandorean Temple} near the end: {Lash} the rope in front of it"},
    {"Sour", "World map south of {Osenia Cavern} (battle)"},
    {"Spring", "{Kibombo}: joins with {Piers}"},
    {"Shade", "{Kibombo}: joins with {Piers}"},
    {"Chill", "World map coast south of {Gondowan Cliffs} and {Naribwe} (battle)"},
    {"Steam", "{Aqua Rock}: top of the big waterfall room"},
    {"Rime", "{Lemuria}: {Tremor} the statue, {Cyclone} the southwest grass"},
    {"Gel", "{Kalt Island}: {Lash} across the frozen river"},
    {"Eddy", "{Shaman Village Cave}: {Lift} three boulders, {Frost}, {Whirlwind}"},
    {"Balm", "{Mars Lighthouse}: push the statue into the corner"},
    {"Serac", "{Sea of Time Islet}: {Tremor} the statue on the pillar"},
    {"Forge", "From {Golden Sun}, with {Isaac}'s party"},
    {"Fever", "From {Golden Sun}, with {Isaac}'s party"},
    {"Corona", "From {Golden Sun}, with {Isaac}'s party"},
    {"Scorch", "From {Golden Sun}, with {Isaac}'s party"},
    {"Ember", "From {Golden Sun}; without a transfer, {Magma Rock} entrance by the first dam"},
    {"Flash", "From {Golden Sun}, with {Isaac}'s party"},
    {"Torch", "From {Golden Sun}, with {Isaac}'s party"},
    {"Cannon", "{Dehkan Plateau}: trap it in the last room with {Pound}"},
    {"Spark", "{Mikasalla}: {Scoop} in the farm area at the bottom left"},
    {"Kindle", "{Gondowan Cliffs}: up the vine by the tightrope, east"},
    {"Char", "{Madra}: the old man south of the shop, after the {Healing Fungus}"},
    {"Coal", "{Izumo}: {Uzume} gives it for the {Dancing Idol}"},
    {"Reflux", "{Tundaria Tower} fourth floor: the ice slides"},
    {"Core", "World map forest between two rivers, south-central continent"},
    {"Tinder", "{Hesperia Settlement}: {Growth} up the cliff, crate puzzle"},
    {"Shine", "{Contigo}: the stump behind the inn ({Force})"},
    {"Fury", "{Magma Rock} deep: south of the blue gem room, then west"},
    {"Fugue", "{Mars Lighthouse} past the frozen dragon head ({Blaze}, {Burst})"},
    {"Gust", "From {Golden Sun}, with {Isaac}'s party"},
    {"Breeze", "From {Golden Sun}, with {Isaac}'s party"},
    {"Zephyr", "From {Golden Sun}; without a transfer, southwest Atteka islet past the stumps"},
    {"Smog", "From {Golden Sun}, with {Isaac}'s party"},
    {"Kite", "From {Golden Sun}, with {Isaac}'s party"},
    {"Squall", "From {Golden Sun}, with {Isaac}'s party"},
    {"Luff", "From {Golden Sun}, with {Isaac}'s party"},
    {"Breath", "{Shrine of the Sea God}: corner it with the torch (battle)"},
    {"Blitz", "{Yampi Desert}: {Pound} the three pillars to its platform"},
    {"Ether", "{Garoh}: {Reveal} the northeast rock, come back the next morning"},
    {"Waft", "{Kibombo Mountains}: {Frost} the puddle, {Growth} the plant"},
    {"Haze", "{Apojii Islands}: south beach behind the waterfall"},
    {"Wheeze", "{Tundaria} world map, southwest of {Tundaria Tower} (battle)"},
    {"Aroma", "{Shaman Village}: through {Moapa}'s house, {Lash} across the cliff"},
    {"Whorl", "{Jupiter Lighthouse}: right tower, the {Hover} puzzle corner"},
    {"Gasp", "{Trial Road} summit: {Reveal} at the water pools"},
    {"Lull", "{Loho}: past the wall the cannon breaks, up the vines"},
    {"Gale", "{Treasure Isle} second half: {Lift} the centre boulder"},
};

const dt::Game kDjinnGame = {kDjinn, 18, 9, 0x520u, 8, TLA_ROM(0x08300000u) - 0x08000000u, TLA_ROM(0x08017B10u) - 0x08000000u, {243, 244, 245, 246}};

const tt::Game kTreasureGame = {tt::tla_data::places, tt::tla_data::place_names, tt::tla_data::containers,
                                    tt::tla_data::treasures, TLA_ROM(0x080F17A8u) - 0x08000000u, TLA_ROM(0x080EF4A4u) - 0x08000000u, 0x420u};

const gsr::tracker_text::Config kTrackerText = {gsr::tracker_text::tla_data::names,
    TLA_ROM(0x080A9F54u) - 0x08000000u, TLA_ROM(0x08060C30u) - 0x08000000u,
    static_cast<unsigned>(tla_rom::version())};

}  // namespace

void tla_trackers_sample() {
    if (const auto* bus = gbarecomp::active_bus())
        gsr::trackers::sample(bus->ewram_ptr(), bus->rom_ptr(), bus->rom_size());
}

void tla_install_trackers() { gsr::trackers::install(kDjinnGame, kTreasureGame, kTrackerText); }
