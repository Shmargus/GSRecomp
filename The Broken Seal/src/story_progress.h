// Custom Battle story progress rules extracted from the authored guide data:
// each room's earliest guide section, and the flag/room/state conditions
// that decide the furthest section reached by a save in EWRAM.
// Frozen tables, extracted once (27 November 2026) from the retired
// Adventure Guide (Archive/2026-11-27-adventure-guide/) and checked equal to
// it over all 65,536 room IDs and 7,862 recorded states. Do not hand-edit
// without repeating that check against the archived guide. Repeated conditions
// (the same flag at two indices) are the guide's own and harmless.
// No guest writes.
#pragma once

#include "../../shared_game/flag_bank.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace gsr::story_progress {

enum class Op : std::uint8_t { Unknown, Flag, Room, RoomState, All, Any };

struct Condition {
    Op op;
    std::uint32_t value;
    std::uint16_t first;
    std::uint16_t count;
};

struct RoomSection {
    std::uint16_t room;
    std::uint16_t section;
};

struct SectionCheck {
    std::uint16_t section;
    std::uint16_t cond;
};

inline constexpr RoomSection kRoomSections[] = {
    {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1},
    {9, 1}, {10, 1}, {11, 2}, {12, 2}, {13, 2}, {14, 2},
    {15, 2}, {16, 2}, {17, 2}, {18, 2}, {19, 2}, {20, 4},
    {21, 4}, {22, 4}, {23, 4}, {24, 4}, {25, 5}, {26, 5},
    {27, 5}, {28, 5}, {29, 5}, {30, 6}, {31, 6}, {32, 6},
    {33, 6}, {34, 6}, {35, 7}, {36, 7}, {37, 7}, {38, 7},
    {39, 12}, {40, 8}, {41, 8}, {42, 8}, {43, 8}, {44, 8},
    {45, 8}, {46, 8}, {47, 9}, {48, 9}, {49, 9}, {50, 10},
    {51, 10}, {52, 10}, {53, 11}, {54, 11}, {55, 11}, {56, 11},
    {57, 11}, {58, 11}, {59, 11}, {60, 13}, {61, 13}, {62, 13},
    {63, 13}, {64, 13}, {65, 13}, {66, 13}, {67, 13}, {68, 14},
    {69, 14}, {70, 14}, {71, 14}, {72, 15}, {73, 15}, {74, 16},
    {75, 17}, {76, 17}, {77, 18}, {78, 18}, {79, 18}, {80, 16},
    {81, 18}, {82, 18}, {83, 17}, {84, 18}, {85, 18}, {86, 18},
    {87, 18}, {88, 19}, {89, 20}, {90, 20}, {91, 20}, {92, 20},
    {93, 24}, {94, 24}, {95, 24}, {96, 22}, {97, 22}, {98, 22},
    {99, 21}, {100, 21}, {101, 21}, {102, 21}, {103, 21}, {104, 32},
    {105, 32}, {106, 32}, {107, 25}, {108, 25}, {109, 25}, {110, 25},
    {111, 25}, {112, 26}, {113, 42}, {114, 42}, {115, 42}, {116, 42},
    {117, 42}, {118, 42}, {119, 42}, {120, 42}, {121, 42}, {122, 42},
    {123, 42}, {124, 42}, {125, 42}, {126, 42}, {127, 42}, {128, 42},
    {129, 42}, {130, 42}, {131, 42}, {132, 42}, {133, 42}, {134, 42},
    {135, 27}, {136, 27}, {137, 27}, {138, 27}, {139, 27}, {140, 30},
    {141, 30}, {142, 30}, {143, 30}, {144, 30}, {145, 30}, {146, 29},
    {147, 29}, {148, 29}, {149, 29}, {150, 29}, {151, 29}, {152, 31},
    {153, 33}, {154, 33}, {155, 33}, {156, 33}, {157, 31}, {158, 31},
    {159, 32}, {160, 32}, {161, 32}, {162, 32}, {163, 32}, {164, 35},
    {165, 35}, {166, 35}, {167, 34}, {168, 34}, {169, 36}, {170, 36},
    {171, 36}, {172, 39}, {173, 39}, {174, 39}, {175, 39}, {176, 39},
    {177, 39}, {178, 38}, {179, 38}, {180, 38}, {181, 37}, {182, 37},
    {183, 40}, {184, 40}, {185, 40}, {186, 40}, {187, 43}, {188, 44},
    {189, 27},
};

inline constexpr Condition kConditions[] = {
    {Op::Unknown, 0u, 0u, 0u},
    {Op::Flag, 2117u, 0u, 0u},
    {Op::Flag, 2134u, 0u, 0u},
    {Op::Flag, 2131u, 0u, 0u},
    {Op::Flag, 2132u, 0u, 0u},
    {Op::Flag, 2135u, 0u, 0u},
    {Op::Flag, 2144u, 0u, 0u},
    {Op::Flag, 2049u, 0u, 0u},
    {Op::Flag, 2544u, 0u, 0u},
    {Op::Flag, 2103u, 0u, 0u},
    {Op::Flag, 2106u, 0u, 0u},
    {Op::Flag, 2113u, 0u, 0u},
    {Op::Flag, 2114u, 0u, 0u},
    {Op::Room, 27u, 0u, 0u},
    {Op::Flag, 768u, 0u, 0u},
    {Op::All, 0u, 0u, 2u},
    {Op::Room, 28u, 0u, 0u},
    {Op::Room, 29u, 0u, 0u},
    {Op::Any, 0u, 2u, 2u},
    {Op::Flag, 2148u, 0u, 0u},
    {Op::All, 0u, 4u, 2u},
    {Op::Flag, 2143u, 0u, 0u},
    {Op::Flag, 2120u, 0u, 0u},
    {Op::RoomState, 2822146u, 65535u, 1u},
    {Op::RoomState, 2822148u, 65535u, 205u},
    {Op::RoomState, 2822150u, 65535u, 10u},
    {Op::RoomState, 2822152u, 65535u, 9u},
    {Op::RoomState, 2822154u, 65535u, 1u},
    {Op::All, 0u, 6u, 4u},
    {Op::Flag, 2091u, 0u, 0u},
    {Op::Flag, 2164u, 0u, 0u},
    {Op::Flag, 2193u, 0u, 0u},
    {Op::Flag, 2194u, 0u, 0u},
    {Op::Flag, 2195u, 0u, 0u},
    {Op::Flag, 2080u, 0u, 0u},
    {Op::Flag, 3947u, 0u, 0u},
    {Op::Flag, 2436u, 0u, 0u},
    {Op::Flag, 48u, 0u, 0u},
    {Op::Flag, 48u, 0u, 0u},
    {Op::Flag, 3919u, 0u, 0u},
    {Op::Flag, 88u, 0u, 0u},
    {Op::Flag, 108u, 0u, 0u},
    {Op::Flag, 49u, 0u, 0u},
    {Op::All, 0u, 10u, 2u},
    {Op::Flag, 3931u, 0u, 0u},
    {Op::Flag, 109u, 0u, 0u},
    {Op::Flag, 3878u, 0u, 0u},
    {Op::Flag, 89u, 0u, 0u},
    {Op::Flag, 89u, 0u, 0u},
    {Op::Flag, 2160u, 0u, 0u},
    {Op::Flag, 2161u, 0u, 0u},
    {Op::Flag, 2162u, 0u, 0u},
    {Op::Flag, 2163u, 0u, 0u},
    {Op::All, 0u, 12u, 4u},
    {Op::Flag, 2161u, 0u, 0u},
    {Op::Flag, 68u, 0u, 0u},
    {Op::Flag, 2165u, 0u, 0u},
    {Op::Flag, 3941u, 0u, 0u},
    {Op::Flag, 2166u, 0u, 0u},
    {Op::Flag, 2167u, 0u, 0u},
    {Op::Flag, 2168u, 0u, 0u},
    {Op::All, 0u, 16u, 2u},
    {Op::Flag, 2167u, 0u, 0u},
    {Op::Flag, 69u, 0u, 0u},
    {Op::All, 0u, 18u, 2u},
    {Op::Flag, 3944u, 0u, 0u},
    {Op::Flag, 2194u, 0u, 0u},
    {Op::All, 0u, 20u, 2u},
    {Op::Flag, 3860u, 0u, 0u},
    {Op::Flag, 110u, 0u, 0u},
    {Op::All, 0u, 22u, 2u},
    {Op::Flag, 50u, 0u, 0u},
    {Op::Flag, 1614u, 0u, 0u},
    {Op::Flag, 70u, 0u, 0u},
    {Op::Flag, 70u, 0u, 0u},
    {Op::Flag, 2203u, 0u, 0u},
    {Op::Flag, 90u, 0u, 0u},
    {Op::Flag, 3847u, 0u, 0u},
    {Op::Flag, 2301u, 0u, 0u},
    {Op::Flag, 2302u, 0u, 0u},
    {Op::Flag, 2302u, 0u, 0u},
    {Op::Flag, 2311u, 0u, 0u},
    {Op::Flag, 71u, 0u, 0u},
    {Op::All, 0u, 24u, 2u},
    {Op::Flag, 71u, 0u, 0u},
    {Op::Flag, 2312u, 0u, 0u},
    {Op::Flag, 1667u, 0u, 0u},
    {Op::Flag, 3848u, 0u, 0u},
    {Op::All, 0u, 26u, 2u},
    {Op::Flag, 3958u, 0u, 0u},
    {Op::Flag, 2197u, 0u, 0u},
    {Op::Flag, 3962u, 0u, 0u},
    {Op::Flag, 2226u, 0u, 0u},
    {Op::Flag, 3966u, 0u, 0u},
    {Op::Flag, 111u, 0u, 0u},
    {Op::Flag, 1625u, 0u, 0u},
    {Op::Flag, 51u, 0u, 0u},
    {Op::Flag, 2322u, 0u, 0u},
    {Op::Flag, 2320u, 0u, 0u},
    {Op::All, 0u, 28u, 2u},
    {Op::Flag, 2321u, 0u, 0u},
    {Op::Flag, 2473u, 0u, 0u},
    {Op::Flag, 2472u, 0u, 0u},
    {Op::Flag, 2474u, 0u, 0u},
    {Op::All, 0u, 30u, 2u},
    {Op::Flag, 52u, 0u, 0u},
    {Op::Flag, 4038u, 0u, 0u},
    {Op::Flag, 4039u, 0u, 0u},
    {Op::All, 0u, 32u, 2u},
    {Op::Flag, 3880u, 0u, 0u},
    {Op::Flag, 4034u, 0u, 0u},
    {Op::Flag, 112u, 0u, 0u},
    {Op::Flag, 2213u, 0u, 0u},
    {Op::Flag, 2341u, 0u, 0u},
    {Op::Flag, 1657u, 0u, 0u},
    {Op::Flag, 3990u, 0u, 0u},
    {Op::Flag, 92u, 0u, 0u},
    {Op::Flag, 72u, 0u, 0u},
    {Op::Flag, 53u, 0u, 0u},
    {Op::Flag, 53u, 0u, 0u},
    {Op::Flag, 2400u, 0u, 0u},
    {Op::Flag, 113u, 0u, 0u},
    {Op::Flag, 3888u, 0u, 0u},
    {Op::Flag, 2402u, 0u, 0u},
    {Op::Flag, 4000u, 0u, 0u},
    {Op::Flag, 4001u, 0u, 0u},
    {Op::All, 0u, 34u, 2u},
    {Op::Flag, 3980u, 0u, 0u},
    {Op::Flag, 3981u, 0u, 0u},
    {Op::Flag, 3982u, 0u, 0u},
    {Op::Flag, 3983u, 0u, 0u},
    {Op::Flag, 3984u, 0u, 0u},
    {Op::Flag, 3985u, 0u, 0u},
    {Op::Flag, 3879u, 0u, 0u},
    {Op::All, 0u, 36u, 7u},
    {Op::Flag, 4007u, 0u, 0u},
    {Op::Flag, 73u, 0u, 0u},
    {Op::Flag, 3974u, 0u, 0u},
    {Op::Flag, 91u, 0u, 0u},
    {Op::Flag, 93u, 0u, 0u},
    {Op::Flag, 4020u, 0u, 0u},
    {Op::Flag, 74u, 0u, 0u},
    {Op::Flag, 3895u, 0u, 0u},
    {Op::Flag, 3896u, 0u, 0u},
    {Op::Flag, 4022u, 0u, 0u},
    {Op::Flag, 94u, 0u, 0u},
    {Op::Flag, 4024u, 0u, 0u},
    {Op::Flag, 114u, 0u, 0u},
    {Op::Flag, 3890u, 0u, 0u},
    {Op::Flag, 3891u, 0u, 0u},
    {Op::Flag, 3893u, 0u, 0u},
    {Op::All, 0u, 43u, 2u},
    {Op::Flag, 3782u, 0u, 0u},
    {Op::Flag, 3783u, 0u, 0u},
    {Op::Flag, 3784u, 0u, 0u},
    {Op::Flag, 3785u, 0u, 0u},
    {Op::All, 0u, 45u, 4u},
    {Op::Flag, 3790u, 0u, 0u},
    {Op::Flag, 3791u, 0u, 0u},
    {Op::Flag, 3792u, 0u, 0u},
    {Op::Flag, 3793u, 0u, 0u},
    {Op::All, 0u, 49u, 4u},
    {Op::Flag, 3794u, 0u, 0u},
    {Op::Flag, 3795u, 0u, 0u},
    {Op::Flag, 3796u, 0u, 0u},
    {Op::Flag, 3797u, 0u, 0u},
    {Op::All, 0u, 53u, 4u},
    {Op::Flag, 3798u, 0u, 0u},
    {Op::Flag, 3799u, 0u, 0u},
    {Op::Flag, 3800u, 0u, 0u},
    {Op::Flag, 3801u, 0u, 0u},
    {Op::All, 0u, 57u, 4u},
    {Op::Flag, 54u, 0u, 0u},
    {Op::All, 0u, 61u, 2u},
    {Op::Flag, 3806u, 0u, 0u},
    {Op::Flag, 3807u, 0u, 0u},
    {Op::Flag, 3808u, 0u, 0u},
    {Op::Flag, 3809u, 0u, 0u},
    {Op::All, 0u, 63u, 4u},
};

inline constexpr std::uint16_t kChildren[] = {
    13, 14, 16, 17, 18, 19, 24, 25, 26, 27, 23, 28, 49, 50, 51, 52,
    59, 60, 31, 32, 65, 66, 69, 33, 81, 82, 86, 87, 97, 98, 102, 103,
    106, 107, 124, 125, 127, 128, 129, 130, 131, 132, 133, 149, 150, 152, 153, 154,
    155, 157, 158, 159, 160, 162, 163, 164, 165, 167, 168, 169, 170, 171, 172, 174,
    175, 176, 177,
};

inline constexpr SectionCheck kSectionChecks[] = {
    {1u, 7u}, {1u, 9u}, {1u, 10u}, {1u, 11u}, {1u, 12u}, {3u, 8u},
    {3u, 37u}, {3u, 38u}, {4u, 2u}, {4u, 3u}, {4u, 4u}, {4u, 5u},
    {5u, 6u}, {5u, 15u}, {5u, 20u}, {5u, 39u}, {5u, 40u}, {6u, 21u},
    {6u, 41u}, {7u, 42u}, {8u, 43u}, {8u, 44u}, {8u, 45u}, {9u, 22u},
    {10u, 29u}, {10u, 46u}, {10u, 47u}, {10u, 48u}, {11u, 30u}, {11u, 53u},
    {11u, 54u}, {11u, 55u}, {11u, 56u}, {11u, 57u}, {11u, 58u}, {11u, 61u},
    {11u, 62u}, {11u, 63u}, {12u, 1u}, {13u, 34u}, {13u, 35u}, {13u, 64u},
    {13u, 67u}, {13u, 68u}, {13u, 70u}, {14u, 71u}, {14u, 72u}, {15u, 73u},
    {15u, 74u}, {15u, 75u}, {16u, 76u}, {17u, 77u}, {17u, 78u}, {18u, 79u},
    {18u, 80u}, {18u, 83u}, {18u, 84u}, {18u, 85u}, {18u, 88u}, {18u, 89u},
    {19u, 90u}, {19u, 91u}, {19u, 92u}, {20u, 93u}, {20u, 94u}, {20u, 95u},
    {20u, 96u}, {21u, 99u}, {21u, 100u}, {22u, 101u}, {22u, 104u}, {22u, 105u},
    {23u, 108u}, {24u, 109u}, {24u, 110u}, {24u, 111u}, {25u, 112u}, {25u, 113u},
    {25u, 114u}, {26u, 115u}, {27u, 116u}, {28u, 117u}, {28u, 118u}, {28u, 119u},
    {29u, 120u}, {29u, 121u}, {29u, 122u}, {29u, 123u}, {31u, 126u}, {32u, 134u},
    {32u, 135u}, {32u, 136u}, {33u, 137u}, {33u, 138u}, {35u, 139u}, {36u, 140u},
    {36u, 141u}, {37u, 142u}, {37u, 143u}, {38u, 144u}, {38u, 145u}, {38u, 146u},
    {39u, 147u}, {40u, 36u}, {40u, 148u}, {40u, 151u}, {42u, 156u}, {42u, 161u},
    {42u, 166u}, {42u, 173u}, {42u, 178u},
};

enum class Truth { Unknown, No, Yes };

inline unsigned room_section(unsigned room) {
    const auto it = std::lower_bound(
        std::begin(kRoomSections), std::end(kRoomSections), room,
        [](const RoomSection& entry, unsigned target) { return entry.room < target; });
    return (it != std::end(kRoomSections) && it->room == room) ? it->section : ~0u;
}

inline Truth evaluate(unsigned index, const unsigned char* ram) {
    if (!ram || index >= std::size(kConditions)) return Truth::Unknown;
    const auto& c = kConditions[index];
    const auto yes = [](bool value) { return value ? Truth::Yes : Truth::No; };
    const unsigned room = ram[0x400] | (unsigned(ram[0x401]) << 8);
    switch (c.op) {
    case Op::Flag: return yes(gsr::read_flag(ram, c.value));
    case Op::Room: return yes(room == c.value);
    case Op::RoomState: {
        const unsigned offset = c.value & 0xffffu;
        if (room != (c.value >> 16) || offset < 0x1000u || offset > 0x1ffeu || (offset & 1u))
            return Truth::Unknown;
        return yes(((ram[offset] | (unsigned(ram[offset + 1]) << 8)) & c.first) == c.count);
    }
    case Op::All:
    case Op::Any: {
        bool unknown = false;
        for (unsigned i = 0; i < c.count; ++i) {
            const auto result = evaluate(kChildren[c.first + i], ram);
            if (c.op == Op::All && result == Truth::No) return Truth::No;
            if (c.op == Op::Any && result == Truth::Yes) return Truth::Yes;
            unknown |= result == Truth::Unknown;
        }
        return unknown ? Truth::Unknown : yes(c.op == Op::All);
    }
    default: return Truth::Unknown;
    }
}

inline unsigned reached_section(const unsigned char* ram) {
    if (!ram) return 0;
    unsigned reached = 0;
    for (const auto& check : kSectionChecks) {
        if (check.section > reached && evaluate(check.cond, ram) == Truth::Yes)
            reached = check.section;
    }
    return reached;
}

}  // namespace gsr::story_progress
