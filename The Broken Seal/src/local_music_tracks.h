#pragma once
#include <array>
#include <cstdint>
#include "local_music_m4a.h"

namespace gsr::intro_music {
using gbarecomp::m4a_music::Recording;
// Approved GS1 ROM sequences matched to the supplied recordings.
// Album track numbers are independent of the game song IDs.
inline constexpr std::array<Recording, 43> recordings{{
    {0x817E8E8u, 1u}, // song 70: The First Book
    {0x817FBD4u, 2u}, // song 74: Page One
    {0x816868Cu, 3u}, // song 23: On that night, 3 years ago
    {0x8171AF8u, 4u}, // song 50: Isaac Battle
    {0x8179D5Cu, 5u}, // song 58: Victory
    {0x817AD08u, 6u}, // song 61: Saturos and Menardi
    {0x8179F38u, 7u}, // song 59: Fallen Heroes (GS1)
    {0x817C0D0u, 8u}, // song 64: Prelude
    {0x81604ACu, 9u}, // song 0: An Adept's Home
    {0x8165D5Cu, 10u}, // song 20: Sol Sanctum
    {0x816776Cu, 11u}, // song 22: The Elemental Stars
    {0x817BE30u, 12u}, // song 63: Sol Sanctum Erupts
    {0x81701FCu, 13u}, // song 42: The Angarian Journey
    {0x817A574u, 14u}, // song 60: A Little Friendship
    {0x8169960u, 15u}, // song 24: Mysterious Caves
    {0x8161558u, 16u}, // song 2: Hopelessness
    {0x816BB78u, 17u}, // song 27: Mercury Lighthouse
    {0x8173A14u, 18u}, // song 51: Saturos Battle
    {0x816ACD8u, 19u}, // song 26: Winterly Imil
    {0x8161CACu, 20u}, // song 3: The Royal Palace
    {0x8166494u, 21u}, // song 21: Forest's Requiem
    {0x8162058u, 22u}, // song 4: Inside Tret - Altin Peak's Flood
    {0x8175380u, 23u}, // song 52: Boss Battle (GS1)
    {0x8160FA0u, 24u}, // song 1: Happy Towns
    {0x8162918u, 25u}, // song 5: Oriental
    {0x816A5F8u, 26u}, // song 25: Desert Heat
    {0x81799C0u, 27u}, // song 55: Linked Battle
    {0x81632F8u, 28u}, // song 6: Kalay
    {0x816F5F0u, 29u}, // song 41: Set Sail! Through the Karagol Sea
    {0x816C780u, 30u}, // song 28: Crossbone Isle
    {0x81652BCu, 31u}, // song 10: Tolbi
    {0x8164BD8u, 32u}, // song 9: Having Fun
    {0x81643C0u, 33u}, // song 8: Colosso Starts
    {0x8163D54u, 34u}, // song 7: Ready for a Challenge
    {0x81805BCu, 35u}, // song 76: Colosso Battle
    {0x816DA9Cu, 36u}, // song 29: Altmiller Cave - Tunnel Ruins
    {0x816E090u, 37u}, // song 30: In the Presence of a Lord
    {0x8170FB4u, 38u}, // song 43: Venus Lighthouse
    {0x8176F88u, 39u}, // song 53: Saturos and Menardi Battle
    {0x8178EA0u, 40u}, // song 54: Fusion Dragon Battle
    {0x817F0D0u, 41u}, // song 72: The Final Beacon
    {0x817DE2Cu, 42u}, // song 67: The First Book Extended
    {0x817FE48u, 43u}, // song 75: The Golden Sun Sets
}};

} // namespace gsr::intro_music
