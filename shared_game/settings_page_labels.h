#pragma once

// Shared second-page wording. Both games speed up field casts and expand
// Reveal; each game retains its own option handlers and measured layout.
namespace gsr::settings_labels {
inline constexpr const char* kRows[] = {
    "Walking speed", "Encounters", "View size", "No slowdown", "Faster Psynergy"};
inline constexpr const char* kHelp[] = {
    "Set walking speed.", "Set encounter frequency.", "Set the view size.",
    "Remove in-game slowdown.", "Faster Psynergy, wider Reveal, Retreat travel."};
}
