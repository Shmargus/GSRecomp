// instant_text.h -- what both games' Instant text has in common: a fourth
// Message-speed value and the settings row that shows it as a word.
//
// Each game hooks its own routines (The Broken Seal text_speed_cheat.h and
// runner_main.cpp, The Lost Age src/tla_text_speed.cpp); the values below
// are the same in both ROMs.
#pragma once

#include <cstdint>

namespace gsr::instant_text {

// Message speed: 0 Slow, 1 Normal, 2 Fast; Instant is 3.
constexpr std::uint32_t kInstant = 3u;
// The dialogue routine's per-call allowance under Instant: far more than a
// page, so a call runs on to the routine's own stops.
constexpr std::uint32_t kAllowance = 255u;
// The settings screen's choice count for the Message-speed row.
constexpr std::uint32_t kChoiceCount = 4u;
// The Message-speed row on the settings screen, and its value in the
// screen's struct.
constexpr std::uint32_t kRow = 2u;
constexpr std::uint32_t kScreenValueOffset = 0x596u;
// An OBJ upload source that makes the game's upload clear the slot: the
// row's icons are uploaded blank and the word stands in their place.
constexpr std::uint32_t kBlankIconSource = 0xFFFFFFFFu;
// The caption: cleared from the first icon's x, centred on the next row's
// middle icon (Auto-Sleep and Speech icons at 0x64 and 0x7C).
constexpr std::uint32_t kCaptionClearX = 0x54u;
constexpr std::uint32_t kCaptionCentreX = 0x7Cu;
constexpr const char* kSpeedWords[4] = {"Slow", "Normal", "Fast", "Instant"};
// Around the word while the cursor is on the row.
constexpr char kArrowLeft[] = "< ";
constexpr char kArrowRight[] = " >";

}  // namespace gsr::instant_text
