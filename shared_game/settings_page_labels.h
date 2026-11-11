#pragma once

// Shared second-page wording. Both games speed up field casts and expand
// Reveal; each game retains its own option handlers and measured layout.
namespace gsr::settings_labels {
inline constexpr const char* kRows[] = {
    "Walking speed", "Encounters", "View size", "No slowdown", "Faster Psynergy"};
inline constexpr const char* kHelp[] = {
    "Set walking speed.", "Set encounter frequency.", "Set the view size.",
    "Remove in-game slowdown.", "Faster Psynergy, wider Reveal, Retreat travel."};
// Encounters are stored as 0 Normal, 1 Half, 2 Off, 3 Double (saved options keep
// meaning). The cursor walks Off, Half, Normal, Double.
inline constexpr int kEncounterOrder[4] = {2, 1, 0, 3};
inline int step_encounter(int value, int direction) {
    int at = 0;
    for (int i = 0; i < 4; ++i)
        if (kEncounterOrder[i] == value) at = i;
    return kEncounterOrder[(at + 4 + (direction < 0 ? -1 : 1)) % 4];
}
}
