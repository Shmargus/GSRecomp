// Golden Sun: The Lost Age -- Instant text, a fourth Message-speed choice.
//
// Message speed is the byte 0x0200046C (save block 0x02000240 + 0x22C):
// 0 Slow, 1 Normal, 2 Fast; Instant is 3. Measured addresses are in FACTS.md
// (2026-10-03); the hook sites are declared in config/tla/main.toml.
//
// The dialogue routine 0x08039864 draws until a per-call allowance at
// [sp+0x20] runs out. It seeds it from the table {1,1,10} (store at
// 0x08039890) and sets it to 1 on a space when not in battle (0x08039E8A),
// so Fast draws one word per call. Instant stores 255 at both, so a call
// runs on to the routine's own stops after control codes and full boxes
// (0x08039CFC, 0x080399B0), which are left alone: page waits and "press A"
// behave as before. Same design as Golden Sun 1's Instant text.
//
// On the settings screen the row is a text selector, "< Slow >" ..
// "< Instant >", in place of its three icons; Left/Right step through the
// four values (the game's own wrap).

#include <cstdint>
#include <string>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kMessageSpeedAddress = 0x0200046Cu;
constexpr uint32_t kInstant = 3u;

constexpr uint32_t kAllowanceSeedPc = 0x08039890u;
constexpr uint32_t kWordStopPc = 0x08039E8Au;
constexpr uint32_t kInstantAllowance = 255u;

constexpr uint32_t kChoiceCountPc = 0x0803FCBEu;
constexpr uint32_t kChoiceCountWithInstant = 4u;

// Settings screen struct: row values at +0x594 (Message speed +0x596).
constexpr uint32_t kScreenMessageSpeedOffset = 0x596u;
constexpr uint32_t kMessageSpeedRow = 2u;
constexpr uint32_t kBounceRowComparePc = 0x0803FC00u;

// Caption: text ID = value + 0x113B, drawn at x 0xA0 on line 0x28. The
// runner draws the whole caption in its place (src/tla_text.cpp): the speed
// word, with "< " / " >" while the cursor is on this row. The row's three
// icons are uploaded blank, so the caption is the selector, centred under
// where the icons were (Golden Sun 1's layout, re-measured).
constexpr uint32_t kCaptionBaseLiteralPc = 0x080402F4u;
constexpr uint32_t kCaptionBase = 0x113Bu;
constexpr const char* kSpeedWords[4] = {"Slow", "Normal", "Fast", "Instant"};
TlaValueCaption g_caption;

// Caption clear `movs r1,#0xa0` (to x 0xC8) and draw `movs r2,#0xa0`. The
// Message-speed icons sit at x 0x54 / 0x6C / 0x84, the Auto-Sleep and
// Speech icons at 0x64 / 0x7C: the clear starts at the first icon, the
// caption is centred on 0x7C (kTlaValueClearX / kTlaValueCentreX).
constexpr uint32_t kCaptionClearXPc = 0x080402FEu;
constexpr uint32_t kCaptionDrawXPc = 0x0804030Cu;
// While settings page two shows, the game still redraws this caption on
// every cursor move, on the line page two's Screen row uses; it showed
// through as "Instant" for a frame or two (F12 0025, 2026-10-05). Then the
// caption is empty and the clear starts at its own end, so nothing is drawn.
constexpr uint32_t kCaptionNoneX = 0xC8u;

// Rows drawn as words instead of icons: Message speed here and Hard Mode on
// the Auto-Sleep row (src/tla_hard_mode.cpp). Each row's icon table is
// loaded once by setup (then one icon per entry) and once per icon by the
// redraw: Message speed 0x0805EA7C {0x22, 0x23, 0x24} at 0x0803FFEA /
// 0x080401D8, Auto-Sleep 0x0805EA81 {0x27, 0x28} at 0x08040068 /
// 0x0804024E. The icon loader 0x08045048 hands its picture to the OBJ
// upload 0x080142D4 at `adds r2,r5,#0` (0x08045092); a source of
// 0xFFFFFFFF makes the upload zero-fill the slot (0x0801432A -> 0x03000258).
struct WordRow {
    uint32_t setup_pc, setup_count, redraw_pc, first_icon, last_icon;
};
constexpr WordRow kWordRows[] = {
    {0x0803FFEAu, 3u, 0x080401D8u, 0x22u, 0x24u},
    {0x08040068u, 2u, 0x0804024Eu, 0x27u, 0x28u},
};
constexpr uint32_t kIconSourcePc = 0x08045092u;
constexpr uint32_t kBlankIconSource = 0xFFFFFFFFu;
const WordRow* g_blank_row = nullptr;
uint32_t g_blank_icons_pending = 0;

bool message_speed_is_instant() {
    gba::GbaBus* bus = gbarecomp::active_bus();
    return bus && bus->ewram_ptr()[kMessageSpeedAddress - 0x02000000u] ==
                      kInstant;
}

uint32_t read_u8(uint32_t addr) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    return bus ? bus->read8(addr) : 0u;
}

uint32_t read_u16(uint32_t addr) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    return bus ? bus->read16(addr) : 0u;
}

int text_speed_store(uint32_t pc, uint32_t, uint32_t requested, uint32_t width,
                     uint32_t* out_value) {
    if (width != 4u || (pc != kAllowanceSeedPc && pc != kWordStopPc))
        return 0;
    if (!message_speed_is_instant()) return 0;
    *out_value = kInstantAllowance;
    return requested != kInstantAllowance;
}

int text_speed_alu_immediate(uint32_t pc, uint32_t original,
                             uint32_t* out_value) {
    if (pc == kCaptionClearXPc) {
        *out_value = tla_settings_page_two_showing() ? kCaptionNoneX : kTlaValueClearX;
        return 1;
    }
    if (pc == kCaptionDrawXPc) {
        *out_value = tla_settings_page_two_showing() ? kCaptionNoneX
                     : g_caption.armed ? g_caption.draw_x()
                                       : kTlaValueClearX;
        return 1;
    }
    if (pc == kIconSourcePc) {
        if (g_blank_icons_pending == 0u) return 0;
        const uint32_t icon = g_cpu.R[6];
        if (icon < g_blank_row->first_icon || icon > g_blank_row->last_icon) {
            g_blank_icons_pending = 0u;
            return 0;
        }
        --g_blank_icons_pending;
        // r2 = r5 + imm: the upload source becomes the clear marker.
        *out_value = kBlankIconSource - g_cpu.R[5];
        return 1;
    }
    if (pc == kBounceRowComparePc) {
        // Row 2 with value 3 has no icon to bounce: compare as if a slider
        // row, so the `ble` skips the bounce.
        const uint32_t screen = g_cpu.R[5];
        if (g_cpu.R[6] == kMessageSpeedRow &&
            read_u8(screen + kScreenMessageSpeedOffset) == kInstant) {
            *out_value = kMessageSpeedRow;
            return 1;
        }
        return 0;
    }
    if (pc == kChoiceCountPc) {
        *out_value = kChoiceCountWithInstant;
        return original != kChoiceCountWithInstant;
    }
    // 0x080D2898 `movs r3,#2` (2 - speed) and 0x080D2912 / 0x080D293C
    // `cmp r3,#2` (speed == Fast): with Instant they compare against 3, so
    // Instant takes Fast's path there.
    if ((pc == 0x080D2898u || pc == 0x080D2912u || pc == 0x080D293Cu) &&
        original == 2u && message_speed_is_instant()) {
        *out_value = kInstant;
        return 1;
    }
    return 0;
}

int text_speed_literal(uint32_t pc, uint32_t original, uint32_t* out_value) {
    for (const WordRow& row : kWordRows) {
        if (pc == row.setup_pc || pc == row.redraw_pc) {
            g_blank_row = &row;
            g_blank_icons_pending = pc == row.setup_pc ? row.setup_count : 1u;
            return 0;
        }
    }
    if (pc == kCaptionBaseLiteralPc) {
        // Only arms the caption; the text ID itself is unchanged. r7 is the
        // screen struct; the value is read right before this load.
        const uint32_t screen = g_cpu.R[7];
        const uint32_t value = read_u8(screen + kScreenMessageSpeedOffset);
        g_caption.armed = false;
        if (original == kCaptionBase && value <= kInstant) {
            g_caption.arm(kCaptionBase + value, kSpeedWords[value],
                          read_u16(screen + kTlaSettingsCursorRowOffset) == kMessageSpeedRow);
            if (tla_settings_page_two_showing()) g_caption.text.clear();
        }
        return 0;
    }
    // The three-entry tables indexed by Message speed (config/tla/main.toml):
    // with the byte at 3, one byte lower so index 3 reads Fast's entry.
    switch (pc) {
        case 0x0803988Au: case 0x080399E6u: case 0x08039BCAu:
        case 0x08039D2Cu: case 0x08039E0Eu: case 0x080D4366u:
            break;
        default:
            return 0;
    }
    if (!message_speed_is_instant()) return 0;
    *out_value = original - 1u;
    return 1;
}

}  // namespace

// The armed caption, drawn once (src/tla_text.cpp).
bool tla_text_speed_text(uint32_t text_id, std::string& out) {
    return g_caption.take(text_id, out);
}

// run_game clears the immediate and literal hooks and installs the ones in
// RunOptions (for the matching ROM only); the store hook is not reset.
void tla_install_text_speed(gbarecomp::RunOptions& options) {
    g_runtime_mem_write_override = text_speed_store;
    options.thumb_alu_immediate_override = text_speed_alu_immediate;
    options.thumb_literal_override = text_speed_literal;
}
