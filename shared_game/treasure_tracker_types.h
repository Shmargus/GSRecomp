// The Treasure Tracker's table types (treasure_tracker.h); each game's
// generated table (The Broken Seal/tools/build_treasure_tracker.py) fills them.
#pragma once

namespace gsr::treasure_tracker {
// A location name offset (the game's location records, word +4) and the
// place it is listed under.
struct PlaceName { unsigned offset, place; };
struct EntranceName { unsigned room, entrance, offset; };
// Sub-type -> what the game says was searched ("checked the chest...").
struct Container { unsigned sub; const char* name; };
constexpr unsigned kChest = 1;
constexpr unsigned kTablet = 0x1A;
constexpr unsigned kCoins = 1u << 31;
struct Treasure {
    unsigned place, flag, sub;
    unsigned reward;           // item ID, summon ability ID, or kCoins | amount
    unsigned reward_word = 0;  // offset of the original reward in the ROM table, if any
};
} // namespace gsr::treasure_tracker
