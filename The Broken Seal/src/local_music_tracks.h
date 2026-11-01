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
    {gs::rom(0x0817E8E8u), 1u}, // song 70: The First Book
    {gs::rom(0x0817FBD4u), 2u}, // song 74: Page One
    {gs::rom(0x0816868Cu), 3u}, // song 23: On that night, 3 years ago
    {gs::rom(0x08171AF8u), 4u}, // song 50: Isaac Battle
    {gs::rom(0x08179D5Cu), 5u}, // song 58: Victory
    {gs::rom(0x0817AD08u), 6u}, // song 61: Saturos and Menardi
    {gs::rom(0x08179F38u), 7u}, // song 59: Fallen Heroes (GS1)
    {gs::rom(0x0817C0D0u), 8u}, // song 64: Prelude
    {gs::rom(0x081604ACu), 9u}, // song 0: An Adept's Home
    {gs::rom(0x08165D5Cu), 10u}, // song 20: Sol Sanctum
    {gs::rom(0x0816776Cu), 11u}, // song 22: The Elemental Stars
    {gs::rom(0x0817BE30u), 12u}, // song 63: Sol Sanctum Erupts
    {gs::rom(0x081701FCu), 13u}, // song 42: The Angarian Journey
    {gs::rom(0x0817A574u), 14u}, // song 60: A Little Friendship
    {gs::rom(0x08169960u), 15u}, // song 24: Mysterious Caves
    {gs::rom(0x08161558u), 16u}, // song 2: Hopelessness
    {gs::rom(0x0816BB78u), 17u}, // song 27: Mercury Lighthouse
    {gs::rom(0x08173A14u), 18u}, // song 51: Saturos Battle
    {gs::rom(0x0816ACD8u), 19u}, // song 26: Winterly Imil
    {gs::rom(0x08161CACu), 20u}, // song 3: The Royal Palace
    {gs::rom(0x08166494u), 21u}, // song 21: Forest's Requiem
    {gs::rom(0x08162058u), 22u}, // song 4: Inside Tret - Altin Peak's Flood
    {gs::rom(0x08175380u), 23u}, // song 52: Boss Battle (GS1)
    {gs::rom(0x08160FA0u), 24u}, // song 1: Happy Towns
    {gs::rom(0x08162918u), 25u}, // song 5: Oriental
    {gs::rom(0x0816A5F8u), 26u}, // song 25: Desert Heat
    {gs::rom(0x081799C0u), 27u}, // song 55: Linked Battle
    {gs::rom(0x081632F8u), 28u}, // song 6: Kalay
    {gs::rom(0x0816F5F0u), 29u}, // song 41: Set Sail! Through the Karagol Sea
    {gs::rom(0x0816C780u), 30u}, // song 28: Crossbone Isle
    {gs::rom(0x081652BCu), 31u}, // song 10: Tolbi
    {gs::rom(0x08164BD8u), 32u}, // song 9: Having Fun
    {gs::rom(0x081643C0u), 33u}, // song 8: Colosso Starts
    {gs::rom(0x08163D54u), 34u}, // song 7: Ready for a Challenge
    {gs::rom(0x081805BCu), 35u}, // song 76: Colosso Battle
    {gs::rom(0x0816DA9Cu), 36u}, // song 29: Altmiller Cave - Tunnel Ruins
    {gs::rom(0x0816E090u), 37u}, // song 30: In the Presence of a Lord
    {gs::rom(0x08170FB4u), 38u}, // song 43: Venus Lighthouse
    {gs::rom(0x08176F88u), 39u}, // song 53: Saturos and Menardi Battle
    {gs::rom(0x08178EA0u), 40u}, // song 54: Fusion Dragon Battle
    {gs::rom(0x0817F0D0u), 41u}, // song 72: The Final Beacon
    {gs::rom(0x0817DE2Cu), 42u}, // song 67: The First Book Extended
    {gs::rom(0x0817FE48u), 43u}, // song 75: The Golden Sun Sets
}};

} // namespace gsr::intro_music
