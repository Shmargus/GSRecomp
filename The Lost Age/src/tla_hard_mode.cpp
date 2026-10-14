// Golden Sun: The Lost Age -- Hard Mode.
//
// The game has three difficulty levels in the byte 0x020004C7 (save block
// 0x02000240 + 0x287): 0 Normal, 1 Easy, 2 Hard ("Monsters are stronger").
// It only offers them on a new game when a save slot holds Clear Data
// (FACTS.md, 2026-10-07), and nothing in the game ever reads the byte
// (FACTS.md, 2026-10-08), so Hard alone changes nothing. The runner makes it
// Golden Sun 1's Hard Mode: enemies get 1.5x HP and 1.25x Attack and
// Defence. It offers Hard Mode on every new game, with the game's own
// questions, and turns the settings screen's Auto-Sleep row into a Hard Mode
// switch. That one byte is the only switch, so answering Hard on a Clear
// Data file and turning the row on are the same thing and never stack.
//
// Enemies. Setup 0x080AFBEC copies the monster's table entry into its unit
// (r5): base HP +0x10, also max HP +0x34 and HP +0x38; base Attack +0x18;
// base Defence +0x1A (as Golden Sun 1's units). It then rebuilds the unit's
// stats with 0x080AD3F8 (`bl` at 0x080AFD60). On that call's entry the
// runner scales those fields (measured: the Wild Wolf's unit 0x020308C8,
// HP 45 written at 0x080AFC52..0x080AFC56, max HP rewritten by the rebuild
// at 0x080ADB4A).
//
// New game. The title menu's code is unpacked to 0x02008000; after "Transfer
// data from Golden Sun?" it asks for a level only when the halfword
// 0x02003860 (any slot with Clear Data, set by 0x080432A4) is non-zero:
// "Play on EASY level?" (text 0x25, at 0x020085B2), "Play on HARD level?"
// (0x27, at 0x02008648; Yes stores 2) and "Play on Normal level?" (0x28; No
// asks again from Easy). Easy needs Clear Data, so without it the runner
// sets the flag just before the check (on entry to 0x080382F8 called from
// 0x02008580), and when the Easy question is about to show (entry to
// 0x08038040 with r0 = 0x25, called from 0x020085B8) clears it again and
// shows the Hard question in its place: r0 = 0x27 and the return address of
// the Hard question's own call (0x0200864C), so the game carries on there.
//
// Settings. Setup 0x0803FC38 copies Auto-Sleep [0x0200048A] into the screen;
// A stores it back there and into the live flag 0x03001200 and branches
// (0x08040528) past B's call to 0x0803F9C0; both then call 0x0803FD14 from
// 0x0804058E. The runner shows Hard Mode in that byte while the screen is
// open and takes it back on that last call, so Auto-Sleep itself stays 0
// (after B the byte still holds what setup showed: no change). The idle count that sleeps after 10800 frames
// (0x08013610 `beq` on the live flag) is always skipped, and the "In Sleep
// Mode..." notice (0x1161, shown when the row is turned on, 0x080404D2) is
// never shown. The row's label and help line are replaced (src/tla_text.cpp),
// and its value is a word selector like Message speed's: "No" / "Yes", with
// "< " / " >" while the cursor is on the row, centred on x 0x7C where the
// row's two icons were (blanked, src/tla_text_speed.cpp). The redraw loads
// the value's text ID 0x1144 + value at 0x08040346 (value in r2, screen r7,
// cursor row halfword at +0x574), clears from x 0xA0 (0x08040350) and draws
// at x 0xA0 (0x08040360).

#include <cstdint>
#include <string>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kDifficulty = 0x020004C7u;
constexpr uint8_t kHard = 2u;
constexpr uint32_t kClearDataFlag = 0x02003860u;

// Enemy stats: rebuild entered from setup's `bl` at 0x080AFD60.
constexpr uint32_t kStatRebuildEntry = 0x080AD3F8u;
constexpr uint32_t kEnemySetupReturn = 0x080AFD65u;
constexpr uint32_t kBaseHp = 0x10u, kMaxHp = 0x34u, kHp = 0x38u;
constexpr uint32_t kBaseAttack = 0x18u, kBaseDefence = 0x1Au;
constexpr uint32_t kStatMax = 0x7FFFu;

constexpr uint32_t kChoiceEntry = 0x080382F8u;
constexpr uint32_t kBeforeLevelCheckReturn = 0x02008585u;
constexpr uint32_t kShowTextEntry = 0x08038040u;
constexpr uint32_t kEasyQuestionReturn = 0x020085BDu;
constexpr uint32_t kEasyQuestionText = 0x25u;
constexpr uint32_t kHardQuestionText = 0x27u;
constexpr uint32_t kHardQuestionReturn = 0x02008651u;

constexpr uint32_t kAutoSleepSaved = 0x0200048Au;
constexpr uint32_t kAutoSleepLive = 0x03001200u;
constexpr uint32_t kSettingsSetupEntry = 0x0803FC38u;
constexpr uint32_t kSettingsCloseEntry = 0x0803FD14u;
constexpr uint32_t kSettingsCloseReturn = 0x08040593u;

constexpr uint32_t kIdleBranchPc = 0x08013616u;
constexpr uint32_t kSleepNoticeBranchPc = 0x080404D2u;

constexpr uint32_t kLabelText = 0x1143u;
constexpr uint32_t kHelpText = 0x114Au;

constexpr uint32_t kValueTextLiteralPc = 0x08040346u;
constexpr uint32_t kValueTextBase = 0x1144u;
constexpr uint32_t kValueDrawXPc = 0x08040360u;
constexpr uint32_t kHardModeRow = 4u;
constexpr const char* kWords[2] = {"No", "Yes"};
TlaValueCaption g_caption;

// The flag 0x02003860 was set by the runner for this new game.
bool g_forced = false;

int (*g_next_branch)(uint32_t, uint32_t, uint32_t*) = nullptr;

// One of the unit's halfwords times num / den, rounded down.
void scale(gba::GbaBus* b, uint32_t addr, uint32_t num, uint32_t den) {
    const uint32_t value = b->read16(addr) * num / den;
    b->write16(addr, static_cast<uint16_t>(value > kStatMax ? kStatMax : value));
}
int (*g_next_literal)(uint32_t, uint32_t, uint32_t*) = nullptr;
int (*g_next_alu_immediate)(uint32_t, uint32_t, uint32_t*) = nullptr;

int hard_mode_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    if (pc == kIdleBranchPc || pc == kSleepNoticeBranchPc) {
        *out_decision = 1u;  // taken: no idle count, no notice
        return original != 1u;
    }
    return g_next_branch ? g_next_branch(pc, original, out_decision) : 0;
}

int hard_mode_literal(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc == kValueTextLiteralPc && original == kValueTextBase) {
        gba::GbaBus* b = gbarecomp::active_bus();
        const uint32_t value = g_cpu.R[2] != 0u ? 1u : 0u;
        g_caption.arm(kValueTextBase + value, kWords[value],
                      b && b->read16(g_cpu.R[7] + kTlaSettingsCursorRowOffset) == kHardModeRow);
        return 0;
    }
    return g_next_literal ? g_next_literal(pc, original, out_value) : 0;
}

// On settings page two the value is not drawn (src/tla_settings_page.cpp).
int hard_mode_alu_immediate(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if ((pc == kTlaAutoSleepClearXPc || pc == kValueDrawXPc) &&
        !tla_settings_page_two_showing()) {
        *out_value = pc == kTlaAutoSleepClearXPc ? kTlaValueClearX : g_caption.draw_x();
        return 1;
    }
    return g_next_alu_immediate ? g_next_alu_immediate(pc, original, out_value) : 0;
}

}  // namespace

void tla_hard_mode_on_entry(uint32_t pc) {
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return;
    const uint32_t lr = g_cpu.R[14];
    if (pc == kStatRebuildEntry && lr == kEnemySetupReturn) {
        if (b->read8(kDifficulty) != kHard) return;
        const uint32_t unit = g_cpu.R[5];
        scale(b, unit + kBaseHp, 3u, 2u);
        scale(b, unit + kMaxHp, 3u, 2u);
        scale(b, unit + kHp, 3u, 2u);
        scale(b, unit + kBaseAttack, 5u, 4u);
        scale(b, unit + kBaseDefence, 5u, 4u);
    } else if (pc == kChoiceEntry && lr == kBeforeLevelCheckReturn) {
        g_forced = b->read16(kClearDataFlag) == 0u;
        if (g_forced) b->write16(kClearDataFlag, 1u);
    } else if (pc == kShowTextEntry && lr == kEasyQuestionReturn &&
               g_cpu.R[0] == kEasyQuestionText && g_forced) {
        b->write16(kClearDataFlag, 0u);
        g_cpu.R[0] = kHardQuestionText;
        g_cpu.R[14] = kHardQuestionReturn;
    } else if (pc == kSettingsSetupEntry) {
        b->write8(kAutoSleepSaved, b->read8(kDifficulty) == kHard ? 1u : 0u);
    } else if (pc == kSettingsCloseEntry && lr == kSettingsCloseReturn) {
        if (b->read8(kAutoSleepSaved) != 0u)
            b->write8(kDifficulty, kHard);
        else if (b->read8(kDifficulty) == kHard)
            b->write8(kDifficulty, 0u);
        b->write8(kAutoSleepSaved, 0u);
        b->write8(kAutoSleepLive, 0u);
    }
}

// The Auto-Sleep row's label, help line and armed value (src/tla_text.cpp).
bool tla_hard_mode_text(uint32_t text_id, std::string& out) {
    if (g_caption.take(text_id, out)) return true;
    if (text_id == kLabelText) out = "Hard Mode";
    else if (text_id == kHelpText) out = "Make monsters stronger.";
    else return false;
    return true;
}

// Chains in front of the branch, literal and immediate hooks already in
// RunOptions.
void tla_install_hard_mode(gbarecomp::RunOptions& options) {
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = hard_mode_branch;
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = hard_mode_literal;
    g_next_alu_immediate = options.thumb_alu_immediate_override;
    options.thumb_alu_immediate_override = hard_mode_alu_immediate;
}
