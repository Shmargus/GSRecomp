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
// Not a game sub-type: an item a person or scene gives (Gift below).
constexpr unsigned kGift = 0x10000u;
struct Treasure {
    unsigned place, flag, sub;
    unsigned reward;           // item ID, summon ability ID, or kCoins | amount
    unsigned reward_word = 0;  // offset of the original reward in the ROM table, if any
    unsigned pack = 0;         // a gift's scene code pack or file (Random Treasure's key)
};
// A gift: its place's name (as in the places list), the flag the game sets
// when it is given, and
// Random Treasure's key: the scene's code pack or file and the original item.
struct Gift { const char* place; unsigned flag, pack, item; };
} // namespace gsr::treasure_tracker
