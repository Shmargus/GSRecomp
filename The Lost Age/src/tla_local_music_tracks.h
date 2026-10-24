#pragma once
#include <array>
#include <cstdint>
#include "local_music_m4a.h"

namespace tla_local_music {
using gbarecomp::m4a_music::Recording;
// Lost Age songs (song table 0x081C4530, music player 1) matched to the
// album's track numbers (Music/TLA "2-NN - <title>.flac") by measurement:
// note onsets, pitch content and exact loop length; FACTS.md "Local music".
// Two have no other candidate with their loop length and are marked. The
// bonus tracks 2-55..2-57 are alternate takes of 5, 18 and 37 and unused.
// Songs carried over from The Broken Seal play its album (Music/TBS, the
// launcher's second folder, "1-NN"): GS1's sequences note for note except
// the two marked. Song 68 (The First Book Extended) loops here but ends in
// GS1, unlike its recording, so it keeps the original.
constexpr unsigned kTbs = gbarecomp::kLocalMusicFolderTracks;
inline constexpr std::array<Recording, 97> recordings{{
    {0x82C80A8u, 1u}, // song 40: The Second Book
    {0x82D7E00u, 2u}, // song 71: Moving Worlds
    {0x82CA8D4u, 3u}, // song 49: Jenna Battle
    {0x82C74A8u, 4u}, // song 39: Trouble is brewing
    {0x82C8EFCu, 5u}, // song 42: Traversing Weyard
    {0x82CB414u, 6u}, // song 50: Felix Battle
    {0x82D1990u, 7u}, // song 59: Fallen Heroes (GS2 version)
    {0x82BBD20u, 8u}, // song 4: Drums of Daila (by elimination: only free 48 s loop; confirmed in Daila by Jimmy)
    {0x82BB850u, 9u}, // song 3: Kandorean Temple Entrance
    {0x82BD3D4u, 10u}, // song 9: Kandorean Temple Labyrinth
    {0x82CCA08u, 11u}, // song 52: Boss Battle (GS2)
    {0x82C0F84u, 12u}, // song 23: Cavernous Shadows
    {0x82BE398u, 13u}, // song 13: Little Madra
    {0x82C09B0u, 14u}, // song 22: Beneath the Surface
    {0x82BB394u, 15u}, // song 2: Alhafra
    {0x82CC064u, 16u}, // song 51: Non-Adept Battle
    {0x82BFA34u, 17u}, // song 20: Air's Rock
    {0x82BEBB0u, 18u}, // song 15: A Full Moon in Garoh
    {0x82BDA44u, 19u}, // song 10: Scaling Mountains
    {0x82BDC98u, 20u}, // song 11: Nocturnal Ritual
    {0x82C1DD0u, 21u}, // song 25: Inside the Great Gabomba
    {0x82C87C4u, 22u}, // song 41: Full Speed Ahead
    {0x82D0B44u, 23u}, // song 56: Ship Battle
    {0x82BCB68u, 24u}, // song 7: Izumo in Despair
    {0x82C16E8u, 25u}, // song 24: Gaia Rock
    {0x82CD074u, 26u}, // song 53: Dragons Battle
    {0x82BC5ACu, 27u}, // song 6: Festival in Izumo
    {0x82BC108u, 28u}, // song 5: Apojii Islands
    {0x82C03F8u, 29u}, // song 21: Aqua Rock
    {0x82BEF24u, 30u}, // song 16: Yallam
    {0x82BF0C8u, 31u}, // song 17: Yepp's Song
    {0x82BF1BCu, 31u}, // song 18: Yepp's Song (song 17's first two tracks)
    {0x82C495Cu, 32u}, // song 30: Taopo Swamp
    {0x82C4174u, 33u}, // song 29: Tundarian Tower
    {0x82C4DB4u, 34u}, // song 31: Ankohl Ruins
    {0x82C597Cu, 35u}, // song 35: There Goes Briggs
    {0x82D40CCu, 36u}, // song 65: Sea of Time
    {0x82BE0D8u, 37u}, // song 12: Ruins of Lemuria
    {0x82BE7B4u, 38u}, // song 14: Freezing Kalt
    {0x82C60C0u, 39u}, // song 37: Mystical Shaman Village
    {0x82BAE04u, 40u}, // song 1: Place your bets
    {0x82C22A4u, 41u}, // song 26: Jupiter Lighthouse
    {0x82C53A4u, 42u}, // song 34: Agatio and Karst
    {0x82D12CCu, 43u}, // song 57: Agatio and Karst Battle
    {0x82C6B54u, 44u}, // song 38: Alone
    {0x82C9864u, 45u}, // song 43: Walking forward with determination
    {0x82C2904u, 46u}, // song 27: Magma Rock
    {0x82D2AD4u, 47u}, // song 62: Frozen in Prox
    {0x82C3694u, 48u}, // song 28: Mars Lighthouse
    {0x82C5E38u, 49u}, // song 36: Enemy Appearance
    {0x82CF778u, 50u}, // song 54: Doom Dragon Battle
    {0x82BCDB8u, 51u}, // song 8: Sorrow and Regret
    {0x82CA1F0u, 52u}, // song 44: The Golden Sun Rises (by elimination: only free 64 s loop)
    {0x82D4910u, 53u}, // song 67: Welcome Home (Not)
    {0x82D8DB8u, 54u}, // song 73: Welcome Home
    {0x82D71C0u, kTbs + 1u}, // song 70: The First Book
    {0x82D98BCu, kTbs + 2u}, // song 74: Page One
    {0x82E87BCu, kTbs + 3u}, // song 723: On that night, 3 years ago (same notes, tracks split differently)
    {0x82F11B8u, kTbs + 4u}, // song 750: Isaac Battle
    {0x82D1670u, kTbs + 5u}, // song 58: Victory
    {0x82D2774u, kTbs + 6u}, // song 61: Saturos and Menardi
    {0x82D3B4Cu, kTbs + 8u}, // song 64: Prelude
    {0x82E02A4u, kTbs + 9u}, // song 700: An Adept's Home
    {0x82E5B90u, kTbs + 10u}, // song 720: Sol Sanctum
    {0x82E75A0u, kTbs + 11u}, // song 722: The Elemental Stars
    {0x82D38ACu, kTbs + 12u}, // song 63: Sol Sanctum Erupts
    {0x82EF8ACu, kTbs + 13u}, // song 742: The Angarian Journey
    {0x82D1FD4u, kTbs + 14u}, // song 60: A Little Friendship
    {0x82E9A98u, kTbs + 15u}, // song 724: Mysterious Caves
    {0x82E1350u, kTbs + 16u}, // song 702: Hopelessness
    {0x82EBCB8u, kTbs + 17u}, // song 727: Mercury Lighthouse
    {0x82F38D8u, kTbs + 18u}, // song 751: Saturos Battle (84% of GS1 notes; same loop and tempo)
    {0x82EAE10u, kTbs + 19u}, // song 726: Winterly Imil
    {0x82E1AA4u, kTbs + 20u}, // song 703: The Royal Palace
    {0x82E62C8u, kTbs + 21u}, // song 721: Forest's Requiem
    {0x82E1E50u, kTbs + 22u}, // song 704: Inside Tret - Altin Peak's Flood
    {0x82F525Cu, kTbs + 23u}, // song 752: Boss Battle (GS1)
    {0x82E0D98u, kTbs + 24u}, // song 701: Happy Towns
    {0x82E2710u, kTbs + 25u}, // song 705: Oriental
    {0x82EA730u, kTbs + 26u}, // song 725: Desert Heat
    {0x82D0298u, kTbs + 27u}, // song 55: Linked Battle
    {0x82E30FCu, kTbs + 28u}, // song 706: Kalay
    {0x82EECA0u, kTbs + 29u}, // song 741: Set Sail! Through the Karagol Sea
    {0x82EC8C8u, kTbs + 30u}, // song 728: Crossbone Isle
    {0x82E50F8u, kTbs + 31u}, // song 710: Tolbi
    {0x82E4A0Cu, kTbs + 32u}, // song 709: Having Fun
    {0x82E41E4u, kTbs + 33u}, // song 708: Colosso Starts
    {0x82E3B6Cu, kTbs + 34u}, // song 707: Ready for a Challenge
    {0x82DA2A8u, kTbs + 35u}, // song 76: Colosso Battle
    {0x82EDBE8u, kTbs + 36u}, // song 729: Altmiller Cave - Tunnel Ruins
    {0x82EE1DCu, kTbs + 37u}, // song 730: In the Presence of a Lord
    {0x82F0674u, kTbs + 38u}, // song 743: Venus Lighthouse
    {0x82F6E7Cu, kTbs + 39u}, // song 753: Saturos and Menardi Battle
    {0x82F8DA4u, kTbs + 40u}, // song 754: Fusion Dragon Battle
    {0x82D85E4u, kTbs + 41u}, // song 72: The Final Beacon
    {0x82D9B30u, kTbs + 43u}, // song 75: The Golden Sun Sets
    {0x82F900Cu, kTbs + 43u}, // song 799: The Golden Sun Sets
}};

} // namespace tla_local_music
