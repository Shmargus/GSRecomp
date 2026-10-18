#pragma once

#include <cstdint>

#include "seamless.h"  // engine/src/runtime: option key, exit code

// Seamless mode, The Broken Seal's half: when the ship sails off, save the
// Clear Data to the player's chosen slot and exit for the launcher to start
// The Lost Age (engine seamless.h). The credits are skipped; the game's own
// save screen handles slot selection and overwrite confirmation.
//
// Measured, verified ROM (FACTS.md, "Ending and Clear Data save", 2026-10-15,
// and "Seamless mode: our own save", 2026-10-15):
//
//   0x0808AB48  loads the code of the room 0x02000400 names; the ship room
//               requests room 0 entrance 9, the credits, next.
//   0x080207C4  the save screen's routine. Without its questions, text and
//               sound it is: flash scan 0x080056CC (0 = ok), summaries
//               0x08005C68, Clear state 0x0801F818, map objects 0x0808A5B0,
//               block writes 0x08005920(slot, 0x02000000) and
//               0x08005920(slot + 3, 0x02001000) (0 = ok), then 0x08005CF8
//               (always, after the scan). The slot is u16 0x02002004 (0..2,
//               the one loaded or last saved; the picker opens on it).
//   0x0200044F  the Clear flag the save screen sets to 1 before saving;
//               0x0801F818 copies it into the save's header (+0x31).
//
// Room 1 entrance 2 is the ending save screen. At the save-end call from
// 0x080208AE (return 0x080208B2), r8 is zero only after successful writes;
// failures/cancellation are negative. See FACTS.md, 2026-10-18.
namespace gsr::seamless {

constexpr std::uint32_t kRoomCodeLoadPc = 0x0808AB48u;
constexpr std::uint32_t kRoomAddr = 0x02000400u;
constexpr std::uint32_t kEntranceAddr = 0x02000402u;
constexpr std::uint32_t kNextRoomAddr = 0x02000404u;
constexpr std::uint32_t kNextEntranceAddr = 0x02000406u;
constexpr std::uint16_t kCreditsRoom = 0u;
constexpr std::uint16_t kCreditsEntrance = 9u;
constexpr std::uint16_t kSaveRoom = 1u;
constexpr std::uint16_t kSaveEntrance = 2u;

constexpr std::uint32_t kSaveEndPc = 0x08005CF8u;
constexpr std::uint32_t kSaveReturnPc = 0x080208B2u;
constexpr std::uint32_t kCurrentSlotAddr = 0x02002004u;  // u16

// The new-game question, shown after "Hard Mode?" in the same window.
constexpr const char* kPromptTitle = "Seamless mode?";
constexpr const char* kPromptLines[3] = {
    "At the end, save and", "go on to The Lost Age.", "Change it with F1."};

// F1's Game page.
constexpr const char* kSwitchLabel = "Seamless mode";
constexpr const char* kSwitchCaption =
    "When the ending starts, skip the credits, choose a slot for your Clear "
    "Data and go straight on to The Lost Age with your party. Choose another "
    "slot to keep your playable save.";

}  // namespace gsr::seamless
