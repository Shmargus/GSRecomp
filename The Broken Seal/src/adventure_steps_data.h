// Generated from data/adventure_walkthrough.json. Do not edit.
#pragma once
namespace gsr::adventure {
struct GuideStep { const char* id; const char* area; const char* text; const char* objective; };
namespace guide_detail {
inline constexpr GuideStep battle{"battle", "Battle", "Finish the current battle. Location guidance resumes when you return to the field.", "Finish the current battle"};
inline constexpr GuideStep entrance{"entrance", "Sol Sanctum - entrance", "Follow the entrance passage into the sanctum's corridors.", "Enter Sol Sanctum"};
inline constexpr GuideStep first_jewel{"first-jewel", "Sol Sanctum - corridors", "Search the branching corridors and open the chest. Then return to the statue with an empty eye.", "Search the corridors and open the chest"};
inline constexpr GuideStep first_eye{"first-eye", "Sol Sanctum - first eye statue", "Find the statue with an empty eye. Face it, open Items, select the Small Jewel and choose Use to open the passage.", "Find the empty-eyed statue and use the Small Jewel"};
inline constexpr GuideStep move_wall{"move-wall", "Sol Sanctum - wall statues", "Continue through the opened passage. Use Isaac's Move on the wall statues to uncover the hidden route behind them.", "Use Move on the wall statues"};
inline constexpr GuideStep second_jewel{"second-jewel", "Sol Sanctum - corridors", "Go through the route behind the movable wall statues and open the chest. Return to the other empty-eyed statue after receiving its item.", "Search beyond the movable statues for a chest"};
inline constexpr GuideStep second_eye{"second-eye", "Sol Sanctum - second eye statue", "Find the other statue with an empty eye. Use the second Small Jewel on it to open the way deeper into the sanctum.", "Find the other empty-eyed statue and use the Small Jewel"};
inline constexpr GuideStep deeper{"deeper", "Sol Sanctum - inner passage", "Go through the second opened passage and continue to the sun and moon chambers with Kraden.", "Go through the opened passage"};
inline constexpr GuideStep water_first{"water-first", "Sol Sanctum - side passage", "Explore this passage and the adjoining corridors for the unopened chest. Return to the empty-eyed statue after receiving its item.", "Search the corridors and open the chest"};
inline constexpr GuideStep water_second{"water-second", "Sol Sanctum - side passage", "Explore this passage and the adjoining corridors beyond the movable statues. Open the chest, then return to the other empty-eyed statue.", "Search beyond the movable statues for a chest"};
inline constexpr GuideStep inspect{"inspect", "Sol Sanctum - sun and moon chambers", "Speak with Kraden and inspect the sun and moon chambers. Find the stairs leading to the statue puzzle above.", "Investigate the sun and moon chambers"};
inline constexpr GuideStep upstairs{"upstairs", "Sol Sanctum - sun and moon chambers", "Go to the statue floor. Set the two outer statues onto their switches before moving the central statue; then continue with the other statue switches.", "Reach the statue floor"};
inline constexpr GuideStep left_outer{"left-outer", "Sol Sanctum - statue puzzle", "Use Move to put the left outer statue onto its floor switch. Leave the central statue alone until both outer switches are activated.", "Activate the left outer switch"};
inline constexpr GuideStep right_outer{"right-outer", "Sol Sanctum - statue puzzle", "Use Move to put the right outer statue onto its floor switch. Both outer switches must be activated before moving the central statue.", "Activate the right outer switch"};
inline constexpr GuideStep center{"center", "Sol Sanctum - statue puzzle", "Use Move on the central statue to complete the mechanism; both outer switches are already activated.", "Move the central statue"};
inline constexpr GuideStep moon_statues{"moon-statues", "Sol Sanctum - statue puzzle", "Move the remaining statues onto their floor switches to change the moon chamber into a sun chamber.", "Complete the remaining statue switches"};
inline constexpr GuideStep portal{"portal", "Sol Sanctum - opened chamber", "Return to Kraden in the chamber below. Inspect the newly opened wall passage.", "Inspect the opened wall passage downstairs"};
inline constexpr GuideStep bags{"bags", "Sol Sanctum - Elemental Stars", "Speak with Kraden and finish receiving the offered items before exploring the platforms.", "Speak with Kraden"};
inline constexpr GuideStep venus{"venus", "Sol Sanctum - Elemental Stars", "Follow the platforms to the Venus Star. Examine it and collect it in a Mythril Bag.", "Collect the Venus Star"};
inline constexpr GuideStep mercury{"mercury", "Sol Sanctum - Elemental Stars", "Take the Mercury Star next. Follow the newly available platforms and examine the star.", "Collect the Mercury Star"};
inline constexpr GuideStep jupiter{"jupiter", "Sol Sanctum - Elemental Stars", "Take the Jupiter Star next, then finish any conversations before continuing.", "Collect the Jupiter Star"};
inline constexpr GuideStep mars{"mars", "Sol Sanctum - Elemental Stars", "Collect the Mars Star when the story allows it. Follow the platforms and examine the remaining star.", "Collect the Mars Star"};
inline constexpr GuideStep leave_stars{"leave-stars", "Sol Sanctum - Elemental Stars", "Finish the chamber conversation. Continue when control returns.", "Finish the Elemental Star chamber scene"};
inline constexpr GuideStep escape{"escape", "Sol Sanctum - eruption", "Follow the Wise One's instructions and finish the escape from Sol Sanctum.", "Escape Sol Sanctum"};
inline constexpr GuideStep flint{"flint", "World map - Vale", "Walk south from Vale and approach the Djinni. Finish the conversation before continuing.", "Walk south from Vale"};
}
}
