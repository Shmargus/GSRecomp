// Golden Sun: The Lost Age -- settings screen hooks. Shared wording is in
// shared_game/settings_page_labels.h.
//
// The old second settings page is gone: its rows live in the Pause menu's
// Quality of Life menu (shared_game/quality_of_life*.h), built from
// quality_of_life_rows below. What is left here is the settings loop's key
// hook (literal 0x080403C4, key block 0x03001150), which forwards repeat keys
// to the Difficulty row (tla_hard_mode.cpp), and the Quality of Life menu's
// present-time painter and install.

#include "../../shared_game/settings_page_labels.h"
#include "tla_rom_version.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gba_bus.h"
#include "runtime.h"
#include "view_config.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"
#include "../../shared_game/quality_of_life.h"

namespace {

const uint32_t kKeyBlockLiteralPc = TLA_ROM(0x080403C4u);
constexpr uint32_t kKeyBlock = 0x03001150u;
constexpr uint32_t kPressedOffset = 0x4u;
constexpr uint32_t kRepeatOffset = 0xCu;
constexpr uint32_t kKeyA = 0x01u, kKeyB = 0x02u, kKeyStart = 0x08u;
constexpr int kRowCount = 5;
constexpr int kWalkSpeedRow = 0, kEncounterRow = 1, kScreenRow = 2,
              kNoSlowdownRow = 3, kFieldPsyRow = 4;
constexpr auto& kRowLabels = gsr::settings_labels::kRows;
constexpr auto& kRowHelp = gsr::settings_labels::kHelp;
constexpr int kSpeedCount = 4;
constexpr int kSpeedHalves[kSpeedCount] = {2, 3, 4, 6};
constexpr const char* kSpeedWords[kSpeedCount] = {"Normal", "1.5x", "2x", "3x"};
constexpr const char* kEncounterWords[4] = {"Normal", "Half", "Off", "Double"};
// View mode 0 is Native 240x160, 1 the expanded view (src/tla_runner.cpp).
constexpr auto& kScreenWords = gbarecomp::kViewModeLabels;
constexpr const char* kOnOffWords[2] = {"Off", "On"};

gba::GbaBus* bus() { return gbarecomp::active_bus(); }

int speed_index() {
    const int halves = runtime_get_mem_write_override_enabled();
    for (int i = 0; i < kSpeedCount; ++i)
        if (kSpeedHalves[i] == halves) return i;
    return 0;
}

std::string row_value(int row) {
    switch (row) {
    case kWalkSpeedRow: return kSpeedWords[speed_index()];
    case kEncounterRow: return kEncounterWords[tla_encounter_rate() & 3];
    case kScreenRow: return kScreenWords[std::clamp(runtime_get_view_mode(), 0, gbarecomp::kViewModeCount - 1)];
    case kNoSlowdownRow: return kOnOffWords[runtime_get_no_slowdown() != 0 ? 1 : 0];
    case kFieldPsyRow: return kOnOffWords[tla_field_psy_fast() ? 1 : 0];
    }
    return "";
}

void change_row(int row, bool right) {
    auto step = [&](int value, int count) {
        return right ? (value + 1) % count : (value + count - 1) % count;
    };
    switch (row) {
    case kWalkSpeedRow:
        runtime_set_mem_write_override_enabled(
            kSpeedHalves[step(speed_index(), kSpeedCount)]);
        break;
    case kEncounterRow:
        tla_set_encounter_rate(gsr::settings_labels::step_encounter(tla_encounter_rate(), right ? 1 : -1));
        break;
    case kScreenRow:
        runtime_request_view_mode(step(runtime_get_view_mode(), gbarecomp::kViewModeCount));
        break;
    case kNoSlowdownRow:
        runtime_request_no_slowdown(runtime_get_no_slowdown() != 0 ? 0 : 1);
        break;
    case kFieldPsyRow:
        tla_set_field_psy_fast(!tla_field_psy_fast());
        break;
    }
}

int keys_literal(uint32_t* out_value) {
    gba::GbaBus* b = bus();
    if (!b) return 0;
    const uint32_t row = g_cpu.R[9];
    const uint32_t pressed = b->read32(kKeyBlock + kPressedOffset);
    if (pressed & (kKeyA | kKeyB | kKeyStart)) {
        return 0;  // the game closes the screen
    }
    // The game's own five rows, Difficulty last (tla_hard_mode.cpp); the old
    // page two now lives in the Quality of Life menu.
    tla_difficulty_row_keys(row, b->read32(kKeyBlock + kRepeatOffset));
    return 0;
}

int (*g_next_literal)(uint32_t, uint32_t, uint32_t*) = nullptr;

int settings_literal(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc == kKeyBlockLiteralPc && original == kKeyBlock)
        return keys_literal(out_value);
    return g_next_literal ? g_next_literal(pc, original, out_value) : 0;
}

}  // namespace

bool tla_settings_page_overlay_visible() {
    return gsr::qol::menu.showing;
}

// Called by the present hook (src/tla_rewind.cpp).
bool tla_settings_page_paint(uint8_t* rgb, uint32_t width, uint32_t height) {
    if (!gsr::qol::menu.showing) return false;
    namespace qm = gsr::qol_menu;
    tla_menu_paint_window(rgb, width, height, qm::kTileX, qm::kTileY, qm::kTiles, qm::kRows);
    auto draw = [&](const std::string& text, int x, int y) { tla_menu_paint_text(rgb, width, height, text, x, y); };
    auto measure = [](const std::string& text) { return static_cast<int>(tla_menu_text_width(text)); };
    qm::paint(gsr::qol::menu, draw, measure, (qm::kTileX + 1) * 8, (qm::kTileY + 1) * 8);
    return true;
}

namespace {
std::vector<gsr::qol_menu::Row> quality_of_life_rows() {
    std::vector<gsr::qol_menu::Row> rows;
    for (int i = 0; i < kRowCount; ++i)
        rows.push_back({kRowLabels[i], kRowHelp[i], [i] { return row_value(i); },
            [i](int direction) { change_row(i, direction > 0); }});
    gsr::qol::append_switches(rows);
    return rows;
}
std::uint16_t (*next_quality_keys)(std::uint16_t) = nullptr;
std::uint16_t quality_keys(std::uint16_t value) {
    value = gsr::qol::menu.keyinput(value);
    return next_quality_keys ? next_quality_keys(value) : value;
}
} // namespace

// Chains in front of the literal hook already in RunOptions
// (Instant text's, battle speed's).
void tla_install_settings_page(gbarecomp::RunOptions& options) {
    static const gsr::qol::Game profile = {
        TLA_ROM(0x0803A7ACu), TLA_ROM(0x0804E127u), TLA_ROM(0x0804E0D4u),
        TLA_ROM(0x08013560u), TLA_ROM(0x081C0010u),
        TLA_TEXT(0x1155u), TLA_TEXT(0x1159u), TLA_ROM(0x08045048u),
        42u, 23u, tla_publish_menu_art, quality_of_life_rows}; // Sleep 42 (pause icon table); 23 picked by Jimmy from the icon sheet, 11 November 2026. TODO-EVIDENCE: sheet not confirmed as TLA's.
    gsr::qol::install(profile);
    next_quality_keys = options.keyinput_filter;
    options.keyinput_filter = quality_keys;
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = settings_literal;
}
