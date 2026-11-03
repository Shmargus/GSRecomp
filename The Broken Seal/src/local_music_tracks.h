#pragma once
#include <array>
#include <cstdint>
#include "local_music_m4a.h"
#include "rom_version.h"

namespace gsr::intro_music {
using gbarecomp::m4a_music::Recording;
// Approved GS1 ROM sequences matched to the supplied recordings.
// Album track numbers are independent of the game song IDs.
// USA song headers; gs::rom() gives the loaded revision's (same song
// number, tools/build_rom_address_table.py).
inline const std::array<Recording, 43> recordings{{
    {gs::rom(0x0817E8E8u), 1u, "The First Book"}, // song 70: The First Book
    {gs::rom(0x0817FBD4u), 2u, "Page One"}, // song 74: Page One
    {gs::rom(0x0816868Cu), 3u, "On that night, 3 years ago"}, // song 23: On that night, 3 years ago
    {gs::rom(0x08171AF8u), 4u, "Isaac Battle"}, // song 50: Isaac Battle
    {gs::rom(0x08179D5Cu), 5u, "Victory"}, // song 58: Victory
    {gs::rom(0x0817AD08u), 6u, "Saturos and Menardi"}, // song 61: Saturos and Menardi
    {gs::rom(0x08179F38u), 7u, "Fallen Heroes (GS1)"}, // song 59: Fallen Heroes (GS1)
    {gs::rom(0x0817C0D0u), 8u, "Prelude"}, // song 64: Prelude
    {gs::rom(0x081604ACu), 9u, "An Adept's Home"}, // song 0: An Adept's Home
    {gs::rom(0x08165D5Cu), 10u, "Sol Sanctum"}, // song 20: Sol Sanctum
    {gs::rom(0x0816776Cu), 11u, "The Elemental Stars"}, // song 22: The Elemental Stars
    {gs::rom(0x0817BE30u), 12u, "Sol Sanctum Erupts"}, // song 63: Sol Sanctum Erupts
    {gs::rom(0x081701FCu), 13u, "The Angarian Journey"}, // song 42: The Angarian Journey
    {gs::rom(0x0817A574u), 14u, "A Little Friendship"}, // song 60: A Little Friendship
    {gs::rom(0x08169960u), 15u, "Mysterious Caves"}, // song 24: Mysterious Caves
    {gs::rom(0x08161558u), 16u, "Hopelessness"}, // song 2: Hopelessness
    {gs::rom(0x0816BB78u), 17u, "Mercury Lighthouse"}, // song 27: Mercury Lighthouse
    {gs::rom(0x08173A14u), 18u, "Saturos Battle"}, // song 51: Saturos Battle
    {gs::rom(0x0816ACD8u), 19u, "Winterly Imil"}, // song 26: Winterly Imil
    {gs::rom(0x08161CACu), 20u, "The Royal Palace"}, // song 3: The Royal Palace
    {gs::rom(0x08166494u), 21u, "Forest's Requiem"}, // song 21: Forest's Requiem
    {gs::rom(0x08162058u), 22u, "Inside Tret - Altin Peak's Flood"}, // song 4: Inside Tret - Altin Peak's Flood
    {gs::rom(0x08175380u), 23u, "Boss Battle (GS1)"}, // song 52: Boss Battle (GS1)
    {gs::rom(0x08160FA0u), 24u, "Happy Towns"}, // song 1: Happy Towns
    {gs::rom(0x08162918u), 25u, "Oriental"}, // song 5: Oriental
    {gs::rom(0x0816A5F8u), 26u, "Desert Heat"}, // song 25: Desert Heat
    {gs::rom(0x081799C0u), 27u, "Linked Battle"}, // song 55: Linked Battle
    {gs::rom(0x081632F8u), 28u, "Kalay"}, // song 6: Kalay
    {gs::rom(0x0816F5F0u), 29u, "Set Sail! Through the Karagol Sea"}, // song 41: Set Sail! Through the Karagol Sea
    {gs::rom(0x0816C780u), 30u, "Crossbone Isle"}, // song 28: Crossbone Isle
    {gs::rom(0x081652BCu), 31u, "Tolbi"}, // song 10: Tolbi
    {gs::rom(0x08164BD8u), 32u, "Having Fun"}, // song 9: Having Fun
    {gs::rom(0x081643C0u), 33u, "Colosso Starts"}, // song 8: Colosso Starts
    {gs::rom(0x08163D54u), 34u, "Ready for a Challenge"}, // song 7: Ready for a Challenge
    {gs::rom(0x081805BCu), 35u, "Colosso Battle"}, // song 76: Colosso Battle
    {gs::rom(0x0816DA9Cu), 36u, "Altmiller Cave - Tunnel Ruins"}, // song 29: Altmiller Cave - Tunnel Ruins
    {gs::rom(0x0816E090u), 37u, "In the Presence of a Lord"}, // song 30: In the Presence of a Lord
    {gs::rom(0x08170FB4u), 38u, "Venus Lighthouse"}, // song 43: Venus Lighthouse
    {gs::rom(0x08176F88u), 39u, "Saturos and Menardi Battle"}, // song 53: Saturos and Menardi Battle
    {gs::rom(0x08178EA0u), 40u, "Fusion Dragon Battle"}, // song 54: Fusion Dragon Battle
    {gs::rom(0x0817F0D0u), 41u, "The Final Beacon"}, // song 72: The Final Beacon
    {gs::rom(0x0817DE2Cu), 42u, "The First Book Extended"}, // song 67: The First Book Extended
    {gs::rom(0x0817FE48u), 43u, "The Golden Sun Sets"}, // song 75: The Golden Sun Sets
}};

// Existing album identities for battle themes and victory/defeat music.
// Separate rows keep an area's chosen theme from replacing battle music.
inline constexpr unsigned battle_tracks[] = {4u, 5u, 7u, 18u, 23u, 27u, 35u, 39u, 40u};

} // namespace gsr::intro_music
