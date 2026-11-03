// The Treasure Tracker's table types (treasure_tracker.h); each game's
// generated table (tools/build_treasure_tracker.py) fills them.
#pragma once

namespace gsr::treasure_tracker {
// A location name offset (the game's location records, word +4) and the
// place it is listed under.
struct PlaceName { unsigned offset, place; };
// Sub-type -> what the game says was searched ("checked the chest...").
struct Container { unsigned sub; const char* name; };
constexpr unsigned kChest = 1;
struct Treasure { unsigned place, flag; const char* contents; unsigned sub; };
} // namespace gsr::treasure_tracker
