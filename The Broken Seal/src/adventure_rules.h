// TBS USA/EU: flag getter 08079338, bank literal 08079354 = 02000040.
// Djinn grant 0807A0F4 sets 0x30 + element*20 + index at 0807A194.
// Name lookup 08027C2C..3C uses text 0x45F + element*20 + index.
// All 28 grants/duplicates and all 4096 flag read/set/clear cases checked
// against original ROM execution. See FACTS.md, Adventure flag mapping.
#pragma once
#include "adventure_guide_data.h"
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace gsr::adventure {
enum class Progress { Unknown, Pending, Complete, InProgress };
struct Rule { const char* id; unsigned flag; };
inline constexpr Rule rules[] = {
    {"gs1-djinn-flint", 0x30},
    {"gs1-djinn-forge", 0x58},
    {"gs1-djinn-gust", 0x6c},
    {"gs1-djinn-granite", 0x31},
    {"gs1-djinn-breeze", 0x6d},
    {"gs1-djinn-fever", 0x59},
    {"gs1-djinn-fizz", 0x44},
    {"gs1-djinn-sleet", 0x45},
    {"gs1-djinn-zephyr", 0x6e},
    {"gs1-djinn-quartz", 0x32},
    {"gs1-djinn-mist", 0x46},
    {"gs1-djinn-corona", 0x5a},
    {"gs1-djinn-spritz", 0x47},
    {"gs1-djinn-smog", 0x6f},
    {"gs1-djinn-vine", 0x33},
    {"gs1-djinn-scorch", 0x5b},
    {"gs1-djinn-sap", 0x34},
    {"gs1-djinn-kite", 0x70},
    {"gs1-djinn-ember", 0x5c},
    {"gs1-djinn-hail", 0x48},
    {"gs1-djinn-ground", 0x35},
    {"gs1-djinn-squall", 0x71},
    {"gs1-djinn-tonic", 0x49},
    {"gs1-djinn-flash", 0x5d},
    {"gs1-djinn-bane", 0x36},
    {"gs1-djinn-dew", 0x4a},
    {"gs1-djinn-torch", 0x5e},
    {"gs1-djinn-luff", 0x72},
};
// Route entries share the acquisition rule; no second flag mapping to drift.
struct Alias { const char* id; const char* source; };
inline constexpr Alias aliases[] = {
    {"gs1-02-steps-1", "gs1-djinn-flint"},
    {"gs1-03-completion-1", "gs1-djinn-forge"},
    {"gs1-03-completion-2", "gs1-djinn-gust"},
    {"gs1-04-completion-1", "gs1-djinn-granite"},
    {"gs1-04-completion-2", "gs1-djinn-breeze"},
    {"gs1-06-completion-1", "gs1-djinn-sleet"},
    {"gs1-08-completion-3", "gs1-djinn-quartz"},
    {"gs1-09-completion-1", "gs1-djinn-spritz"},
    {"gs1-10-completion-2", "gs1-djinn-smog"},
    {"gs1-10-completion-3", "gs1-djinn-vine"},
    {"gs1-11-completion-1", "gs1-djinn-scorch"},
    {"gs1-11-completion-2", "gs1-djinn-sap"},
    {"gs1-13-completion-1", "gs1-djinn-ember"},
    {"gs1-13-completion-2", "gs1-djinn-ground"},
    {"gs1-13-completion-3", "gs1-djinn-hail"},
    {"gs1-14-completion-1", "gs1-djinn-squall"},
    {"gs1-15-completion-3", "gs1-djinn-tonic"},
    {"gs1-16-completion-1", "gs1-djinn-flash"},
    {"gs1-17-completion-1", "gs1-djinn-torch"},
    {"gs1-17-completion-2", "gs1-djinn-luff"},
};
constexpr std::size_t objective_index(std::string_view id) {
    for (std::size_t i = 0; i < std::size(objectives); ++i)
        if (id == objectives[i].id) return i;
    return std::size(objectives);
}
constexpr bool valid_rules() {
    for (const auto& rule : rules)
        if (objective_index(rule.id) == std::size(objectives) || rule.flag >= 4096) return false;
    for (const auto& alias : aliases) {
        if (objective_index(alias.id) == std::size(objectives)) return false;
        bool found = false;
        for (const auto& rule : rules) if (std::string_view(alias.source) == rule.id) found = true;
        if (!found) return false;
    }
    return true;
}
static_assert(valid_rules());
// Enemy initialization 08079478/080794EC..08079528 names ID via
// text (ID-8)+028F. Reward tally 080C2584..080C2592 sets 0600+ID.
// Boss death proves only the fight, not subsequent rewards/cutscenes.
struct BattleRule { const char* id; unsigned flag; bool whole_step; unsigned second_flag = 0; unsigned alternate_flag = 0; };
inline constexpr BattleRule battles[] = {
    {"gs1-02-steps-3", 0x61d, true, 0x61e}, // Bandit and Thief
    {"gs1-04-steps-4", 0x693, true},        // Tret
    {"gs1-06-steps-3", 0x67b, false, 0, 0x67c}, // Either Saturos record; never the scripted loss
    {"gs1-08-steps-3", 0x64e, false},       // Killer Ape; Douse Drop still to claim
    {"gs1-09-steps-4", 0x683, false},       // Hydros Statue; Lift reward/route remain
    {"gs1-10-steps-3", 0x659, true},        // Manticore
    {"gs1-12-steps-4", 0x679, false},       // Kraken; docking remains
    {"gs1-15-completion-1", 0x68d, false},  // Toadonpa; Hammet's rescue remains
    {"gs1-16-steps-3", 0x691, false},       // Storm Lizard; Gate remains
    {"gs1-18-steps-4", 0x695, false},       // Deadbeard; return remains
    {"gs1-19-steps-4", 0x694, true},        // Fusion Dragon
};
constexpr bool valid_battles() {
    for (const auto& rule : battles)
        if (objective_index(rule.id) == std::size(objectives) || rule.flag >= 4096 ||
            rule.second_flag >= 4096 || rule.alternate_flag >= 4096)
            return false;
    return true;
}
static_assert(valid_battles());
inline bool read_flag(const unsigned char* ram, unsigned flag) {
    // Match the original getter's low-12-bit aliasing, including upper bits.
    flag &= 0xfff;
    return (ram[0x40 + (flag >> 3)] & (1u << (flag & 7))) != 0;
}
inline bool prologue_complete(const unsigned char* ram) {
    // Storm Vale sets 842 at 0200C0CA in rom_77DD1C. The concluding
    // house cutscene clears the childhood-mode bit 834 at 0200978C in
    // rom_784360, before its room handoff. Require both: 834 is also clear
    // before a new adventure initializes. Existing opening recording
    // reaches this condition at frame33636, after the scripted fight.
    return read_flag(ram, 0x842) && !read_flag(ram, 0x834);
}
} // namespace gsr::adventure
