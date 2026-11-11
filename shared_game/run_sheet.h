// F1 > Game for a randomizer run, both games (Jimmy, 6 November 2026;
// docs/NEXT_PRIORITY_MODES.md section 5): the run's seed (run_code.h), to
// share, and the spoiler sheet, hidden until Show is pressed: each hero's
// Psynergy by level and what every weapon and armour piece became.
//
// The sheet is written on the emulation thread whenever the loaded file's
// run changes (the plans read the bus and the ROM image) and read by the F1
// window under a lock. The Show choice is not saved: every launch starts
// hidden, so nothing is seen by accident.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "challenge_mode.h"
#include "gs_text_encode.h"
#include "random_psynergy.h"
#include "random_weapons.h"
#include "run_code.h"
#include "host_config_ui.h"
#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif

namespace gsr::run_sheet {

struct Section {
    std::string title;
    std::vector<std::string> lines;
};

using ItemName = std::function<std::string(std::uint16_t)>;

struct Game {
    const random_psynergy::Game* psynergy;
    const random_weapons::Game* equipment;  // its text tables serve every name
    std::span<const char* const> hero_names;  // when a record has no name yet
    // A game's Random Treasure lines (none: the game has no Random Treasure).
    void (*treasure)(const random_psynergy::Config&, std::vector<Section>&, const ItemName&) = nullptr;
};

// The run a file's bytes describe: one seed (both blocks hold the same one)
// and the choices. 0 seed: not a randomizer run.
inline run_code::Run run_of(const random_psynergy::Config& p, const random_weapons::Config& e) {
    namespace rc = run_code;
    rc::Run r;
    r.seed = p.seed ? p.seed : e.seed;
    if (p.psynergy) r.choices |= rc::kPsynergy;
    if (p.psynergy && p.fair_psynergy) r.choices |= rc::kFairPsynergy;
    if (p.treasure && p.fair_items) r.choices |= rc::kFairItems;
    if (p.treasure && p.fair_shops) r.choices |= rc::kFairShops;
    if (p.coins && p.fair_coins) r.choices |= rc::kFairCoins;
    if (p.treasure) r.choices |= rc::kItems;
    if (p.treasure && p.bargains) r.choices |= rc::kBargains;
    if (p.coins) r.choices |= rc::kCoins;
    // Weapons and Armor: the finds (The Broken Seal) or the stat shuffle;
    // Stats: the stat shuffle with finds. A stat shuffle without finds is
    // The Lost Age's Weapons (or a Broken Seal run's, Stats on).
    if (p.weapon_finds || (e.seed && e.weapons)) r.choices |= rc::kWeapons;
    if (p.armour_finds || (e.seed && e.armour)) r.choices |= rc::kArmor;
    if (e.seed && (e.weapons || e.armour)) r.choices |= rc::kStats;
    if (e.seed && e.scaling) r.choices |= rc::kScaling;
    return r;
}

struct Sheet {
    std::string code;  // empty: no run
    std::vector<Section> sections;
};

struct State {
    const Game* game = nullptr;
    // What the sheet was written for (emulation thread).
    random_psynergy::Config psynergy;
    random_weapons::Config equipment;
    gsr::challenge::Config challenge;
    bool written = false;
    std::mutex lock;
    Sheet sheet;  // under lock
};
inline State state;

inline void install(const Game& g) { state.game = &g; }

// A game text as UTF-8 for the F1 window: the icon code (0x1D and its
// argument) and other control codes dropped, the font's Latin-1 letters
// converted.
inline std::string clean(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0x1Du) { ++i; continue; }
        if (c < 0x20u) continue;
        if (c < 0x80u) { out += static_cast<char>(c); continue; }
        out += static_cast<char>(0xC0u | (c >> 6));
        out += static_cast<char>(0x80u | (c & 0x3Fu));
    }
    return out;
}

inline std::string hero_name(gba::GbaBus& b, const Game& g, unsigned id) {
    std::string name;
    const std::uint32_t record = g.psynergy->records + id * g.psynergy->record_size;
    for (std::uint32_t i = 0; i < 14u; ++i) {
        const std::uint8_t c = b.read8(record + i);
        if (c == 0) break;
        if (c < 0x20u) { name.clear(); break; }
        name += static_cast<char>(c);
    }
    if (!name.empty()) return clean(name);
    return id < g.hero_names.size() ? g.hero_names[id] : std::to_string(id);
}

inline Sheet write(gba::GbaBus& b, const Game& g, const random_psynergy::Config& p,
                   const random_weapons::Config& e, const gsr::challenge::Config& challenge) {
    Sheet sheet;
    const run_code::Run run = run_of(p, e);
    // A run with no random choice (Scaled difficulty alone) has no code.
    if (run.seed == 0 || (run.choices & ~run_code::kFairBits) == 0 || !b.rom_ptr()) return sheet;
    sheet.code = run_code::spaced(run_code::encode(run));
    const random_weapons::Game& eg = *g.equipment;
    mods::TextEncoder text({b.rom_ptr(), b.rom_size()}, eg.text_table, eg.tree_table);
    const auto line = [&](std::uint32_t n) {
        std::string s;
        return text.decode(n, &s) ? clean(s) : std::string("?");
    };
    const auto spell = [&](std::uint16_t id) { return line(eg.ability_name_text + id); };
    const auto item = [&](std::uint16_t id) { return line(eg.item_name_text + id); };

    if (p.psynergy) {
        const random_psynergy::Plan plan =
            random_psynergy::make_plan(*g.psynergy, p.seed, challenge, p.fair_psynergy);
        for (int hero = 0; hero < g.psynergy->heroes; ++hero) {
            if (challenge.size > 0 && !gsr::challenge::picked(challenge, static_cast<std::uint8_t>(hero)))
                continue;
            Section s;
            s.title = std::string(gbarecomp::ui_text("Psynergy")) + ": " +
                      hero_name(b, g, static_cast<unsigned>(hero));
            std::string row;
            int level = -1;
            for (const auto& grant : plan.grants[static_cast<std::size_t>(hero)]) {
                if (grant.level != level) {
                    if (!row.empty()) s.lines.push_back(row);
                    level = grant.level;
                    row = std::string(gbarecomp::ui_text("Lv")) + " " + std::to_string(level) + ": ";
                } else {
                    row += ", ";
                }
                row += spell(grant.id);
            }
            if (!row.empty()) s.lines.push_back(row);
            sheet.sections.push_back(std::move(s));
        }
    }
    if (e.seed) {
        const random_weapons::Plan plan = random_weapons::plan_of(e);
        Section weapons{gbarecomp::ui_text("Weapons"), {}}, armour{gbarecomp::ui_text("Armor"), {}};
        for (const auto& w : plan.weapons) {
            if (w.donor == 0) continue;
            std::string row = item(w.id) + " <- " + item(w.donor) + " (";
            if (w.type == random_weapons::kWeapon) {
                row += std::string(gbarecomp::ui_text("Attack")) + " " +
                       std::to_string(random_weapons::attack_of(w.planned));
                if (w.unleash.id)
                    row += ", " + std::string(gbarecomp::ui_text("unleash")) + " " + spell(w.unleash.id);
                weapons.lines.push_back(row + ")");
            } else {
                row += std::string(gbarecomp::ui_text("Defense")) + " " +
                       std::to_string(random_weapons::defense_of(w.planned));
                if (const int a = random_weapons::attack_of(w.planned))
                    row += ", " + std::string(gbarecomp::ui_text("Attack")) + " " + std::to_string(a);
                armour.lines.push_back(row + ")");
            }
        }
        if (!weapons.lines.empty()) sheet.sections.push_back(std::move(weapons));
        if (!armour.lines.empty()) sheet.sections.push_back(std::move(armour));
    }
    if ((p.treasure || p.coins) && g.treasure) g.treasure(p, sheet.sections, item);
    return sheet;
}

inline bool same(const gsr::challenge::Config& a, const gsr::challenge::Config& b) {
    return a.size == b.size && a.picks == b.picks;
}

// Once a game frame, after the randomizers' own frames (the equipment's
// pristine records are captured there).
inline void frame() {
    State& s = state;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!s.game || !b) return;
    const random_psynergy::Config p = random_psynergy::current(*s.game->psynergy);
    const random_weapons::Config e = random_weapons::current(*s.game->equipment);
    const gsr::challenge::Config c = gsr::challenge::current();
    // The equipment plan needs its pristine records, captured on its frame.
    if (e.seed && !random_weapons::state.captured) return;
    if (s.written && p == s.psynergy &&
        e.seed == s.equipment.seed && e.scaling == s.equipment.scaling &&
        e.armour == s.equipment.armour && e.weapons == s.equipment.weapons && same(c, s.challenge))
        return;
    s.psynergy = p;
    s.equipment = e;
    s.challenge = c;
    s.written = true;
    Sheet sheet = write(*b, *s.game, p, e, c);
    std::lock_guard<std::mutex> hold(s.lock);
    s.sheet = std::move(sheet);
}

// F1 > Game: the code, then the sheet behind Show.
inline void draw() {
#ifdef GBARECOMP_HAVE_IMGUI
    State& s = state;
    Sheet sheet;
    {
        std::lock_guard<std::mutex> hold(s.lock);
        if (s.sheet.code.empty()) return;
        sheet = s.sheet;
    }
    static bool shown = false;
    ImGui::Spacing();
    ImGui::Text("%s: %s", gbarecomp::ui_text("Seed"), sheet.code.c_str());
    ImGui::TextWrapped("%s", gbarecomp::ui_text(
        "Share it: typed on the New Game list, it gives the same run."));
    if (ImGui::Button(gbarecomp::ui_text(shown ? "Hide spoilers" : "Show spoilers"))) shown = !shown;
    if (!shown) return;
    for (const Section& section : sheet.sections) {
        if (!ImGui::TreeNode(section.title.c_str())) continue;
        for (const std::string& row : section.lines) ImGui::TextWrapped("%s", row.c_str());
        ImGui::TreePop();
    }
#endif
}

}  // namespace gsr::run_sheet
