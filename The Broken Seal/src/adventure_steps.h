// Location-aware TBS USA/EU instructions; no guest writes or saved checkboxes.
// Evidence: FACTS.md "Guide mode"; root local/guide-mode and existing recordings.
#pragma once
#include "adventure_rules.h"
namespace gsr::adventure {
struct GuideStep { const char* id; const char* area; const char* text; const char* objective; };
namespace guide_detail {
inline constexpr GuideStep battle{"battle", "Battle", "Finish the current battle. Location guidance resumes when you return to the field.", "Finish the current battle"};
inline constexpr GuideStep entrance{"entrance", "Sol Sanctum - entrance", "Follow the entrance passage into the sanctum's corridors.", "Enter Sol Sanctum"};
inline constexpr GuideStep first_jewel{"first-jewel", "Sol Sanctum - corridors", "Search the branching corridors for the treasure chest containing a Small Jewel. Open it before returning to the statue with an empty eye.", "Search the corridors and open the chest"};
inline constexpr GuideStep first_eye{"first-eye", "Sol Sanctum - first eye statue", "Find the statue with an empty eye. Face it, open Items, select the Small Jewel and choose Use to open the passage.", "Find the empty-eyed statue and use the Small Jewel"};
inline constexpr GuideStep move_wall{"move-wall", "Sol Sanctum - wall statues", "Continue through the opened passage. Use Isaac's Move on the wall statues to uncover the hidden route behind them.", "Use Move on the wall statues"};
inline constexpr GuideStep second_jewel{"second-jewel", "Sol Sanctum - corridors", "Explore the passages beyond the movable wall statues. Find and open the chest containing the second Small Jewel.", "Search beyond the movable statues for a chest"};
inline constexpr GuideStep second_eye{"second-eye", "Sol Sanctum - second eye statue", "Find the other statue with an empty eye. Use the second Small Jewel on it to open the way deeper into the sanctum.", "Find the other empty-eyed statue and use the Small Jewel"};
inline constexpr GuideStep deeper{"deeper", "Sol Sanctum - inner passage", "Go through the second opened passage and continue to the sun and moon chambers with Kraden.", "Go through the opened passage"};
inline constexpr GuideStep water_first{"water-first", "Sol Sanctum - side passage", "Explore this passage and continue into the adjoining corridors. You still need the first Small Jewel chest.", "Search the corridors and open the chest"};
inline constexpr GuideStep water_second{"water-second", "Sol Sanctum - side passage", "Explore this passage and the adjoining corridors for the second Small Jewel. Return to the eye statue after opening its chest.", "Search beyond the movable statues for a chest"};
inline constexpr GuideStep inspect{"inspect", "Sol Sanctum - sun and moon chambers", "Speak with Kraden and inspect the sun and moon chambers. Find the stairs leading to the statue puzzle above.", "Investigate the sun and moon chambers"};
inline constexpr GuideStep upstairs{"upstairs", "Sol Sanctum - sun and moon chambers", "Go to the statue floor. Set the two outer statues onto their switches before moving the central statue; this prevents the trap.", "Reach the statue floor"};
inline constexpr GuideStep left_outer{"left-outer", "Sol Sanctum - statue puzzle", "Use Move to put the left outer statue onto its floor switch. Leave the central statue alone until both outer switches are activated.", "Activate the left outer switch"};
inline constexpr GuideStep right_outer{"right-outer", "Sol Sanctum - statue puzzle", "Use Move to put the right outer statue onto its floor switch. Both outer switches must be activated before moving the central statue.", "Activate the right outer switch"};
inline constexpr GuideStep center{"center", "Sol Sanctum - statue puzzle", "Both outer switches are activated. Use Move on the central statue to complete the trap-prevention mechanism.", "Move the central statue"};
inline constexpr GuideStep moon_statues{"moon-statues", "Sol Sanctum - statue puzzle", "The trap-prevention mechanism is complete. Move the remaining statues onto their floor switches to change the moon chamber into a sun chamber.", "Complete the remaining statue switches"};
inline constexpr GuideStep portal{"portal", "Sol Sanctum - opened chamber", "The statue puzzle is solved. Return to Kraden in the chamber below and inspect the newly opened wall passage to reach the Elemental Stars.", "Inspect the opened wall passage downstairs"};
inline constexpr GuideStep bags{"bags", "Sol Sanctum - Elemental Stars", "Speak with Kraden and collect the Mythril Bags before taking the Elemental Stars.", "Speak with Kraden"};
inline constexpr GuideStep venus{"venus", "Sol Sanctum - Elemental Stars", "Follow the platforms to the Venus Star. Examine it and collect it in a Mythril Bag.", "Collect the Venus Star"};
inline constexpr GuideStep mercury{"mercury", "Sol Sanctum - Elemental Stars", "Take the Mercury Star next. Follow the newly available platforms and examine the star.", "Collect the Mercury Star"};
inline constexpr GuideStep jupiter{"jupiter", "Sol Sanctum - Elemental Stars", "Take the Jupiter Star next, then finish the scene with Saturos and Menardi.", "Collect the Jupiter Star"};
inline constexpr GuideStep mars{"mars", "Sol Sanctum - Elemental Stars", "Collect the Mars Star when the story allows it. Follow the platforms and examine the remaining star.", "Collect the Mars Star"};
inline constexpr GuideStep leave_stars{"leave-stars", "Sol Sanctum - Elemental Stars", "The Mars Star is collected. Finish the chamber scene and follow the Wise One's instructions to escape the sanctum.", "Finish the Elemental Star chamber scene"};
inline constexpr GuideStep escape{"escape", "Sol Sanctum - eruption", "Follow the Wise One's instructions and finish the escape from Sol Sanctum.", "Escape Sol Sanctum"};
inline constexpr GuideStep flint{"flint", "World map - Vale", "Walk south from Vale and speak to Flint, the Venus Djinni, to accept him into your party.", "Walk south from Vale"};
}
// Original character getter08077394:02000500 + character*014C.
// Inventory lookup08078664:15 halfwords at+D8, item word &01FF.
inline bool guide_has_item(const unsigned char* ram, unsigned item) {
    for (unsigned hero = 0; hero < 8; ++hero)
        for (unsigned slot = 0; slot < 15; ++slot) {
            const unsigned p = 0x500 + hero * 0x14c + 0xd8 + slot * 2;
            const unsigned word = ram[p] | (unsigned(ram[p + 1]) << 8);
            if ((word & 0x1ff) == item) return true;
        }
    return false;
}
inline const GuideStep* guide_for(const unsigned char* ram) {
    if (!ram) return nullptr;
    using namespace guide_detail;
    const unsigned room = ram[0x400] | (unsigned(ram[0x401]) << 8);
    const unsigned map = ram[0x408] | (unsigned(ram[0x409]) << 8);
    if (room == 510) return &battle;
    // Room table0809F1A8 + room*8 feeds loader0808AB48; asset table08320000.
    // Original overlays verified for rooms11..19, including shared16/19 code.
    if (room >= 11 && room <= 19 && guide_has_item(ram, 0xde))
        return room == 17 ? &leave_stars : &escape;
    if (room == 19) return &entrance;
    if (room == 14 || room == 15) {
        if (read_flag(ram, 0x821)) return &deeper;
        if (!read_flag(ram, 0x81a))
            return read_flag(ram, 0xf01) || guide_has_item(ram, 0xe1) ? &first_eye : &water_first;
        return read_flag(ram, 0xf02) || guide_has_item(ram, 0xe1) ? &second_eye : &water_second;
    }
    if (room == 16) {
        if (!read_flag(ram, 0x81a))
            return read_flag(ram, 0xf01) || guide_has_item(ram, 0xe1) ? &first_eye : &first_jewel;
        if (!read_flag(ram, 0x804)) return &move_wall;
        if (!read_flag(ram, 0x821))
            return read_flag(ram, 0xf02) || guide_has_item(ram, 0xe1) ? &second_eye : &second_jewel;
        return &deeper;
    }
    if (room == 11 || room == 13) {
        if (read_flag(ram, 0x80f)) return &portal;
        if (!read_flag(ram, 0x809)) return &inspect;
        return read_flag(ram, 0x818) ? &moon_statues : &upstairs;
    }
    if (room == 12) {
        if (read_flag(ram, 0x80f)) return &portal;
        if (!read_flag(ram, 0x818)) {
            if (!read_flag(ram, 0x816)) return &left_outer;
            if (!read_flag(ram, 0x817)) return &right_outer;
            return &center;
        }
        return &moon_statues;
    }
    if (room == 17) {
        if (read_flag(ram, 0x83e)) return &mars;
        if (!guide_has_item(ram, 0xe0)) return &bags;
        if (!read_flag(ram, 0x83c) && !guide_has_item(ram, 0xdc)) return &venus;
        if (!read_flag(ram, 0x83d) && !guide_has_item(ram, 0xdd)) return &mercury;
        if (!guide_has_item(ram, 0xdf)) return &jupiter;
        return &mars;
    }
    if (room == 18) return &escape;
    if (!prologue_complete(ram)) return nullptr;
    if (map == 2 && !read_flag(ram, 0x30)) return &flint;
    return nullptr;
}
} // namespace gsr::adventure
