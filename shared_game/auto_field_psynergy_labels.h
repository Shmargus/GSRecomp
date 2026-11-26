#pragma once

// Auto Field Psynergy (radial_psynergy.h): F1 > Game switch, off by default.
namespace gsr::auto_field_psynergy {
constexpr const char* kOptionKey = "AutoFieldPsynergy";
constexpr const char* kSwitchLabel = "Auto Field Psynergy";
constexpr const char* kSwitchCaption =
    "Press A in front of something that reacts to Psynergy, such as a pillar "
    "for Move, to cast it. Talking and checking still come first, and the "
    "Psynergy costs PP as usual.";
}  // namespace gsr::auto_field_psynergy

// Select-only menu (radial_psynergy.h): F1 > Game switch, off by default.
// A with nobody to talk to and nothing to check does nothing instead of
// opening the menu; Select still opens it.
namespace gsr::select_menu {
constexpr const char* kOptionKey = "SelectOnlyMenu";
constexpr const char* kSwitchLabel = "Menu on Select only";
constexpr const char* kSwitchCaption =
    "A no longer opens the menu when there is nothing to talk to or check; "
    "Select still does. A keeps working for talking, checking and everything "
    "else.";
}  // namespace gsr::select_menu
