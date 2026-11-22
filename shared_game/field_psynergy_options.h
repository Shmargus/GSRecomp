#pragma once

#include <atomic>
#include <vector>

#include "game_options.h"
#include "quality_of_life_menu.h"

// Better Psynergy's parts, each its own switch: Pause > Quality of Life >
// Better Psynergy opens a page of them; settings page two turns them all on
// or off. The first part keeps the old single switch's key and the others
// start from its value, so a saved On keeps all three on.
namespace gsr::field_psy {

enum Part { kFaster, kReveal, kRetreat, kPartCount };
inline constexpr const char* kKeys[kPartCount] = {
    gbarecomp::kOptionFieldPsynergy, "FieldPsynergyReveal", "FieldPsynergyRetreat"};
inline constexpr const char* kLabels[kPartCount] = {"Faster casts", "Wider Reveal", "Retreat travel"};
inline constexpr const char* kHelp[kPartCount] = {
    "Field Psynergy plays at double speed.",
    "Reveal covers the whole view and lasts twice as far.",
    "Retreat on the world map takes you to a town you have visited."};
inline constexpr const char* kTitle = "Better Psynergy";

inline std::atomic<bool> g_on[kPartCount];

inline bool on(Part p) { return g_on[p].load(); }

inline void load() {
    const int old = gbarecomp::game_option_get(kKeys[kFaster], 0);
    for (int p = 0; p < kPartCount; ++p)
        g_on[p].store(gbarecomp::game_option_get(kKeys[p], old) != 0);
}

// Every key is written, so a part left at the old switch's value stays put.
inline void save() {
    for (int p = 0; p < kPartCount; ++p)
        gbarecomp::game_option_set(kKeys[p], g_on[p].load() ? 1 : 0);
}

inline void set(Part p, bool value) {
    g_on[p].store(value);
    save();
}

inline void set_all(bool value) {
    for (auto& part : g_on) part.store(value);
    save();
}

inline int count_on() {
    int n = 0;
    for (const auto& part : g_on) n += part.load() ? 1 : 0;
    return n;
}

// "On" with every part on, "Off" with none, else "Some".
inline const char* summary() {
    const int n = count_on();
    return n == 0 ? "Off" : n == kPartCount ? "On" : "Some";
}

// The Quality of Life row: A or Right opens the parts' page.
inline qol_menu::Row qol_row(const char* label, const char* help) {
    qol_menu::Row row{label, help, [] { return std::string(summary()); }, nullptr};
    row.page_title = kTitle;
    row.page = [] {
        std::vector<qol_menu::Row> rows;
        for (int p = 0; p < kPartCount; ++p) {
            const auto part = static_cast<Part>(p);
            rows.push_back({kLabels[p], kHelp[p], [part] { return std::string(on(part) ? "On" : "Off"); },
                [part](int) { set(part, !on(part)); }});
        }
        return rows;
    };
    return row;
}

}  // namespace gsr::field_psy
