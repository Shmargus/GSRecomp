// F1 > Cheats > Save editor: the loaded file's coins, each hero's level,
// items and Djinn, changed through the game's own routines.
//
// The F1 window runs on its own thread. It reads a copy of the party that
// the emulation thread writes once a game frame (frame(), on entry to the
// game's frame routine, where calling the game is safe) and leaves its
// requests for that same frame to carry out. Everything is held off while
// cheats are blocked (Challenge Mode).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "gs_text_encode.h"
#include "host_config_ui.h"
#include "random_weapons.h"
#include "run_sheet.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif

namespace gsr::save_editor {

struct Game {
    const random_weapons::Game* items;  // item table, names, character records
    std::uint32_t coins;                // u32, the game clamps it to max_coins
    std::uint32_t max_coins;
    std::uint32_t joined_flags;         // story flag n set: hero n has joined
    std::uint32_t level_offset;         // u8 in the character record
    std::uint32_t exp_offset;           // u32 in the character record
    unsigned max_level;
    std::uint32_t raise_pc;   // (id, level): one level at a time to level
    std::uint32_t give_pc;    // (id, item): slot, or -1 when full
    std::uint32_t remove_pc;  // (id, slot): one fewer (packs the bag), then the stat rebuild
    // Djinn. Record: owned bits +djinn_owned + element*4, owned count byte
    // +djinn_count + element. Owned flag djinn_flag + element*20 + index
    // (the bank at joined_flags); name text djinn_name_text + element*20 + index.
    std::uint32_t djinn_owned, djinn_count, djinn_flag, djinn_name_text;
    unsigned djinn_per_element;
    std::uint32_t djinni_give_pc;    // (id, element, index): 0, or -1 when refused
    std::uint32_t djinni_join_pc;    // (id, element, index): onto the game's Djinn list
    std::uint32_t djinni_unset_pc;   // (id, element, index): out of the set ones
    std::uint32_t djinni_leave_pc;   // (id, element, index): off the game's Djinn list
};

inline constexpr unsigned kElements = 4;
inline constexpr const char* kElementNames[kElements] = {"Venus", "Mercury", "Mars", "Jupiter"};

struct Hero {
    unsigned id = 0;
    std::string name;
    unsigned level = 0;
    std::uint32_t exp = 0;
    std::uint16_t slots[random_weapons::kItemSlots] = {};  // id | 0x200 equipped | (count - 1) << 11
    std::uint32_t djinn[kElements] = {};  // owned bits
};

struct Snapshot {
    bool valid = false;
    std::uint32_t coins = 0;
    std::vector<Hero> heroes;
    std::uint32_t owned[kElements] = {};  // every hero's owned bits together
};

enum class Op { Coins, Level, Give, Remove, GiveDjinni, TakeDjinni };
struct Request {
    Op op;
    unsigned hero;
    std::uint32_t value;
};

struct State {
    const Game* game = nullptr;
    std::mutex lock;
    Snapshot snapshot;                                    // under lock
    std::vector<std::pair<std::uint16_t, std::string>> names;  // under lock, once
    std::vector<std::string> djinn_names;  // under lock, once; element * per element + index
    std::vector<Request> requests;                        // under lock
    std::string message;                                  // under lock
};
inline State state;

inline void install(const Game& g) { state.game = &g; }

inline unsigned count_of(std::uint16_t word) { return (word >> 11) + 1u; }

// Emulation thread, on entry to the frame routine.
inline void frame() {
    State& s = state;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!s.game || !b) return;
    const Game& g = *s.game;
    const random_weapons::Game& ig = *g.items;
    const auto record = [&](unsigned id) { return ig.records + id * ig.record_size; };

    std::vector<Request> requests;
    bool need_names = false;
    {
        std::lock_guard<std::mutex> hold(s.lock);
        requests.swap(s.requests);
        need_names = s.names.empty();
    }
    if (need_names && b->rom_ptr()) {
        std::vector<std::pair<std::uint16_t, std::string>> names;
        mods::TextEncoder text({b->rom_ptr(), b->rom_size()}, ig.text_table, ig.tree_table);
        for (unsigned id = 1; id <= ig.last_item; ++id) {
            std::string raw;
            if (!text.decode(ig.item_name_text + id, &raw)) continue;
            std::string name = run_sheet::clean(raw);
            if (name.empty() || name == "?") continue;
            names.emplace_back(static_cast<std::uint16_t>(id), std::move(name));
        }
        std::vector<std::string> djinn_names;
        for (unsigned e = 0; e < kElements; ++e)
            for (unsigned i = 0; i < g.djinn_per_element; ++i) {
                std::string raw;
                text.decode(g.djinn_name_text + e * 20u + i, &raw);
                std::string name = run_sheet::clean(raw);
                djinn_names.push_back(name.empty() ? std::string(kElementNames[e]) + " " + std::to_string(i + 1) : name);
            }
        std::lock_guard<std::mutex> hold(s.lock);
        s.names = std::move(names);
        s.djinn_names = std::move(djinn_names);
    }
    const auto set_flag = [&](std::uint32_t flag, bool on) {
        const std::uint32_t at = g.joined_flags + (flag >> 3);
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << (flag & 7u));
        b->write8(at, static_cast<std::uint8_t>(on ? b->read8(at) | bit : b->read8(at) & ~bit));
    };
    const auto owner_of = [&](unsigned e, unsigned i) -> int {
        for (unsigned id = 0; id < static_cast<unsigned>(ig.heroes); ++id)
            if ((b->read32(record(id) + g.djinn_owned + e * 4u) >> i) & 1u) return static_cast<int>(id);
        return -1;
    };

    std::string message;
    if (!gbarecomp::cheats_blocked()) {
        for (const Request& r : requests) {
            std::uint32_t result = 0;
            switch (r.op) {
            case Op::Coins:
                b->write32(g.coins, std::min(r.value, g.max_coins));
                break;
            case Op::Level:
                if (r.value > b->read8(record(r.hero) + g.level_offset) && r.value <= g.max_level)
                    gbarecomp::guest_call(g.raise_pc | 1u, r.hero, r.value, 0, 0, &result);
                break;
            case Op::Give:
                gbarecomp::guest_call(g.give_pc | 1u, r.hero, r.value, 0, 0, &result);
                if (static_cast<std::int32_t>(result) < 0) message = "No room: that hero's bag is full.";
                break;
            case Op::Remove:
                if (r.value < random_weapons::kItemSlots &&
                    !(b->read16(record(r.hero) + random_weapons::kItems + r.value * 2u) &
                      random_weapons::kEquipped))
                    gbarecomp::guest_call(g.remove_pc | 1u, r.hero, r.value, 0, 0, &result);
                break;
            case Op::GiveDjinni: {
                // As the game's grant hands one over: give, onto the list;
                // then the stats, and the Djinni's owned flag so its spot in
                // the world is done.
                const unsigned e = r.value / 32u, i = r.value % 32u;
                if (e >= kElements || i >= g.djinn_per_element || owner_of(e, i) >= 0) break;
                gbarecomp::guest_call(g.djinni_give_pc | 1u, r.hero, e, i, 0, &result);
                if (static_cast<std::int32_t>(result) < 0) {
                    message = "That hero holds the most Djinn of that element.";
                    break;
                }
                gbarecomp::guest_call(g.djinni_join_pc | 1u, r.hero, e, i, 0, &result);
                gbarecomp::guest_call(ig.stats_pc | 1u, r.hero, 0, 0, 0, &result);
                set_flag(g.djinn_flag + e * 20u + i, true);
                break;
            }
            case Op::TakeDjinni: {
                // The giving half of the game's move: unset, clear the owned
                // bit and count, off the list; then the stats, and the owned
                // flag clears so the Djinni can be found again.
                const unsigned e = r.value / 32u, i = r.value % 32u;
                if (e >= kElements || owner_of(e, i) != static_cast<int>(r.hero)) break;
                const std::uint32_t rec = record(r.hero);
                gbarecomp::guest_call(g.djinni_unset_pc | 1u, r.hero, e, i, 0, &result);
                b->write32(rec + g.djinn_owned + e * 4u, b->read32(rec + g.djinn_owned + e * 4u) & ~(1u << i));
                b->write8(rec + g.djinn_count + e, static_cast<std::uint8_t>(b->read8(rec + g.djinn_count + e) - 1u));
                gbarecomp::guest_call(g.djinni_leave_pc | 1u, r.hero, e, i, 0, &result);
                gbarecomp::guest_call(ig.stats_pc | 1u, r.hero, 0, 0, 0, &result);
                set_flag(g.djinn_flag + e * 20u + i, false);
                break;
            }
            }
        }
    }

    Snapshot snap;
    snap.valid = true;
    snap.coins = b->read32(g.coins);
    for (unsigned id = 0; id < static_cast<unsigned>(ig.heroes); ++id) {
        Hero h;
        const std::uint32_t rec = record(id);
        for (unsigned e = 0; e < kElements; ++e) {
            h.djinn[e] = b->read32(rec + g.djinn_owned + e * 4u);
            snap.owned[e] |= h.djinn[e];
        }
        if (!((b->read8(g.joined_flags + (id >> 3)) >> (id & 7u)) & 1u)) continue;
        h.id = id;
        for (std::uint32_t i = 0; i < 14u; ++i) {
            const std::uint8_t c = b->read8(rec + i);
            if (c == 0) break;
            h.name += static_cast<char>(c);
        }
        h.name = run_sheet::clean(h.name);
        h.level = b->read8(rec + g.level_offset);
        h.exp = b->read32(rec + g.exp_offset);
        for (unsigned slot = 0; slot < random_weapons::kItemSlots; ++slot)
            h.slots[slot] = b->read16(rec + random_weapons::kItems + slot * 2u);
        snap.heroes.push_back(std::move(h));
    }
    std::lock_guard<std::mutex> hold(s.lock);
    s.snapshot = std::move(snap);
    if (!message.empty()) s.message = message;
}

// F1 > Cheats, below the engine's own cheats.
inline void draw() {
#ifdef GBARECOMP_HAVE_IMGUI
    State& s = state;
    if (!s.game) return;
    Snapshot snap;
    std::string message;
    {
        std::lock_guard<std::mutex> hold(s.lock);
        snap = s.snapshot;
        message = s.message;
    }
    const auto ask = [&](Op op, unsigned hero, std::uint32_t value) {
        std::lock_guard<std::mutex> hold(s.lock);
        s.requests.push_back({op, hero, value});
        s.message.clear();
    };
    using gbarecomp::ui_text;
    ImGui::Spacing();
    ImGui::SeparatorText(ui_text("Save editor"));
    if (!snap.valid || snap.heroes.empty()) {
        ImGui::TextWrapped("%s", ui_text("Load a file to change its coins, levels, items and Djinn."));
        return;
    }

    static int coins = -1;
    if (coins < 0) coins = static_cast<int>(snap.coins);
    ImGui::Text("%s: %u", ui_text("Coins"), snap.coins);
    // A number box filling the line beside its button.
    const auto box_width = [](const char* button) {
        return ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(button).x -
               ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x;
    };
    ImGui::SetNextItemWidth(box_width(ui_text("Set coins")));
    ImGui::InputInt("##EditCoins", &coins, 100, 1000);
    coins = std::clamp(coins, 0, static_cast<int>(s.game->max_coins));
    ImGui::SameLine();
    if (ImGui::Button(ui_text("Set coins"))) ask(Op::Coins, 0, static_cast<std::uint32_t>(coins));

    static unsigned chosen = 0;
    const Hero* hero = &snap.heroes.front();
    for (const Hero& h : snap.heroes)
        if (h.id == chosen) hero = &h;
    chosen = hero->id;
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##EditHero", hero->name.c_str())) {
        for (const Hero& h : snap.heroes) {
            ImGui::PushID(static_cast<int>(h.id));
            if (ImGui::Selectable(h.name.c_str(), h.id == chosen)) chosen = h.id;
            if (h.id == chosen) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // Level: up only, through the game's own level-up (stats grow as they
    // would have, experience is raised to the new level's).
    static int level = 0;
    static unsigned level_hero = ~0u;
    if (level_hero != hero->id || level < static_cast<int>(hero->level)) {
        level = static_cast<int>(hero->level);
        level_hero = hero->id;
    }
    ImGui::Text("%s %u   %s %u", ui_text("Level"), hero->level, ui_text("Experience"), hero->exp);
    ImGui::SetNextItemWidth(box_width(ui_text("Raise level")));
    ImGui::InputInt("##EditLevel", &level);
    level = std::clamp(level, static_cast<int>(hero->level), static_cast<int>(s.game->max_level));
    ImGui::SameLine();
    ImGui::BeginDisabled(level <= static_cast<int>(hero->level));
    if (ImGui::Button(ui_text("Raise level"))) ask(Op::Level, hero->id, static_cast<std::uint32_t>(level));
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", ui_text("Levels only go up, growing stats as the game does."));

    std::vector<std::pair<std::uint16_t, std::string>> names;
    {
        std::lock_guard<std::mutex> hold(s.lock);
        names = s.names;
    }
    const auto name_of = [&](std::uint16_t id) -> std::string {
        for (const auto& n : names)
            if (n.first == id) return n.second;
        return "#" + std::to_string(id);
    };

    ImGui::Spacing();
    ImGui::TextUnformatted(ui_text("Items"));
    bool any = false;
    for (unsigned slot = 0; slot < random_weapons::kItemSlots; ++slot) {
        const std::uint16_t word = hero->slots[slot];
        if (word == 0) continue;
        any = true;
        ImGui::PushID(static_cast<int>(slot));
        std::string row = name_of(word & random_weapons::kItemMask);
        if (count_of(word) > 1) row += " x" + std::to_string(count_of(word));
        if (word & random_weapons::kEquipped) row += std::string(" (") + ui_text("equipped") + ")";
        // Equipped items stay: the game's own menus take them off first.
        const bool equipped = (word & random_weapons::kEquipped) != 0;
        ImGui::BeginDisabled(equipped);
        if (ImGui::SmallButton(ui_text("Remove"))) ask(Op::Remove, hero->id, slot);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(row.c_str());
        ImGui::PopID();
    }
    if (!any) ImGui::TextUnformatted(ui_text("(none)"));

    static std::uint16_t pick = 0;
    static char filter[32] = {};
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##EditGive", pick ? name_of(pick).c_str() : ui_text("Choose an item"))) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##EditFilter", ui_text("Search"), filter, sizeof filter);
        std::string want = filter;
        std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& n : names) {
            std::string low = n.second;
            std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!want.empty() && low.find(want) == std::string::npos) continue;
            ImGui::PushID(n.first);
            if (ImGui::Selectable(n.second.c_str(), n.first == pick)) pick = n.first;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(pick == 0);
    if (ImGui::Button(ui_text("Give item"))) ask(Op::Give, hero->id, pick);
    ImGui::EndDisabled();

    // Djinn: this hero's, each with Remove; any Djinni nobody holds can be
    // given.
    std::vector<std::string> djinn_names;
    {
        std::lock_guard<std::mutex> hold(s.lock);
        djinn_names = s.djinn_names;
    }
    const unsigned per = s.game->djinn_per_element;
    const auto djinni_name = [&](unsigned e, unsigned i) -> std::string {
        const std::size_t at = e * per + i;
        return at < djinn_names.size() ? djinn_names[at] : std::to_string(i + 1);
    };
    ImGui::Spacing();
    ImGui::TextUnformatted(ui_text("Djinn"));
    bool any_djinni = false;
    for (unsigned e = 0; e < kElements; ++e)
        for (unsigned i = 0; i < per; ++i) {
            if (!((hero->djinn[e] >> i) & 1u)) continue;
            any_djinni = true;
            ImGui::PushID(static_cast<int>(1000 + e * 32u + i));
            if (ImGui::SmallButton(ui_text("Remove"))) ask(Op::TakeDjinni, hero->id, e * 32u + i);
            ImGui::SameLine();
            ImGui::Text("%s (%s)", djinni_name(e, i).c_str(), ui_text(kElementNames[e]));
            ImGui::PopID();
        }
    if (!any_djinni) ImGui::TextUnformatted(ui_text("(none)"));
    static int djinni = -1;
    if (djinni >= 0 && ((snap.owned[djinni / 32] >> (djinni % 32)) & 1u)) djinni = -1;
    ImGui::SetNextItemWidth(-1);
    const std::string djinni_label =
        djinni >= 0 ? djinni_name(static_cast<unsigned>(djinni) / 32u, static_cast<unsigned>(djinni) % 32u)
                    : std::string(ui_text("Choose a Djinni"));
    if (ImGui::BeginCombo("##EditDjinni", djinni_label.c_str())) {
        bool listed = false;
        for (unsigned e = 0; e < kElements; ++e)
            for (unsigned i = 0; i < per; ++i) {
                if ((snap.owned[e] >> i) & 1u) continue;
                listed = true;
                const int v = static_cast<int>(e * 32u + i);
                const std::string row = djinni_name(e, i) + " (" + ui_text(kElementNames[e]) + ")";
                ImGui::PushID(v);
                if (ImGui::Selectable(row.c_str(), v == djinni)) djinni = v;
                ImGui::PopID();
            }
        if (!listed) ImGui::TextUnformatted(ui_text("Every Djinni is already held."));
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(djinni < 0);
    if (ImGui::Button(ui_text("Give Djinni"))) ask(Op::GiveDjinni, hero->id, static_cast<std::uint32_t>(djinni));
    ImGui::EndDisabled();

    if (!message.empty()) ImGui::TextWrapped("%s", ui_text(message.c_str()));
#endif
}

}  // namespace gsr::save_editor
