// Authored route predicates; no remembered completion and no guest writes.
#pragma once
#include "adventure_steps.h"
#include "adventure_walkthrough_data.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gsr::adventure::walkthrough {
enum class Truth { Unknown, No, Yes };
// This choice filters advice; it never changes the game's collection flags.
inline std::atomic<bool> completionist{true};

inline Truth evaluate(unsigned index, const unsigned char* ram, const unsigned char* iwram = nullptr) {
    if (!ram || index >= std::size(conditions)) return Truth::Unknown;
    const auto& c = conditions[index];
    const auto yes = [](bool value) { return value ? Truth::Yes : Truth::No; };
    switch (c.op) {
    case Op::Unknown: return Truth::Unknown;
    case Op::Flag: return yes(read_flag(ram, c.value));
    case Op::Item: return yes(guide_has_item(ram, c.value));
    case Op::Completionist: return yes(completionist.load() == (c.value != 0));
    case Op::Room: return yes((ram[0x400] | (unsigned(ram[0x401]) << 8)) == c.value);
    case Op::Entrance: return yes((ram[0x402] | (unsigned(ram[0x403]) << 8)) == c.value);
    case Op::EntranceMin: return yes((ram[0x402] | (unsigned(ram[0x403]) << 8)) >= c.value);
    case Op::EntranceMax: return yes((ram[0x402] | (unsigned(ram[0x403]) << 8)) <= c.value);
    case Op::ActorTile: case Op::ActorTileMin: case Op::ActorTileMax: {
        const unsigned room = ram[0x400] | (unsigned(ram[0x401]) << 8);
        const unsigned actor = c.value & 0xffff;
        if (!iwram || room != (c.value >> 16) || actor > 0xbf || (c.first != 8 && c.first != 16))
            return Truth::Unknown;
        const auto word = [](const unsigned char* p) {
            return unsigned(p[0]) | (unsigned(p[1]) << 8) | (unsigned(p[2]) << 16) | (unsigned(p[3]) << 24);
        };
        // Original 8BA1C: IWRAM field context -> slot table -> actor.
        const unsigned context = word(iwram + 0x1ebc);
        const unsigned slot = 0x14 + actor * 4;
        if ((context & 3) || context < 0x02000000 || context > 0x02040000 - slot - 4)
            return Truth::Unknown;
        const unsigned pointer = word(ram + context - 0x02000000 + slot);
        if ((pointer & 3) || pointer < 0x02000000 || pointer > 0x02040000 - c.first - 4)
            return Truth::Unknown;
        const auto fixed = static_cast<std::int32_t>(word(ram + pointer - 0x02000000 + c.first));
        const auto tile = fixed >= 0 ? fixed / 0x100000 :
            -1 - static_cast<std::int32_t>((-1LL - fixed) / 0x100000);
        const auto target = static_cast<std::int32_t>(c.count);
        if (c.op == Op::ActorTileMin) return yes(tile >= target);
        if (c.op == Op::ActorTileMax) return yes(tile <= target);
        return yes(tile == target);
    }
    case Op::RoomState: {
        const unsigned room = ram[0x400] | (unsigned(ram[0x401]) << 8);
        const unsigned offset = c.value & 0xffff;
        if (room != (c.value >> 16) || offset < 0x1000 || offset > 0x1ffe || (offset & 1))
            return Truth::Unknown;
        return yes(((ram[offset] | (unsigned(ram[offset + 1]) << 8)) & c.first) == c.count);
    }
    case Op::Not: {
        const auto result = evaluate(children[c.first], ram, iwram);
        return result == Truth::Unknown ? result : yes(result == Truth::No);
    }
    case Op::All: case Op::Any: {
        bool unknown = false;
        for (unsigned i = 0; i < c.count; ++i) {
            const auto result = evaluate(children[c.first + i], ram, iwram);
            if (c.op == Op::All && result == Truth::No) return Truth::No;
            if (c.op == Op::Any && result == Truth::Yes) return Truth::Yes;
            unknown |= result == Truth::Unknown;
        }
        return unknown ? Truth::Unknown : yes(c.op == Op::All);
    }
    }
    return Truth::Unknown;
}

inline const Action* action_for(const unsigned char* ram, bool include_optional = completionist.load(), const unsigned char* iwram = nullptr) {
    if (!ram) return nullptr;
    for (const auto& action : actions)
        if ((!action.optional || include_optional) &&
            evaluate(action.when, ram, iwram) == Truth::Yes) return &action;
    return nullptr;
}

inline const Section* section_by_id(unsigned id) {
    for (const auto& section : sections) if (section.id == id) return &section;
    return nullptr;
}

// Area membership uses authored room scopes, independently of the next action's
// destination. Availability flags are deliberately not completion: keep acquired
// pickups in the checklist even after their interaction disappears.
inline const Section* area_for(const unsigned char* ram, const unsigned char* iwram = nullptr) {
    if (!ram) return nullptr;
    const unsigned room = ram[0x400] | (unsigned(ram[0x401]) << 8);
    const unsigned entrance = ram[0x402] | (unsigned(ram[0x403]) << 8);
    for (const auto& scope : locations) {
        if (scope.room != room || (scope.entrance >= 0 && unsigned(scope.entrance) != entrance)) continue;
        if (scope.when >= 0 && evaluate(unsigned(scope.when), ram, iwram) != Truth::Yes) continue;
        if (const auto* area = section_by_id(scope.section)) return area;
    }
    return nullptr;
}

inline const Section* section_for(const unsigned char* ram, bool include_optional = completionist.load(), const unsigned char* iwram = nullptr) {
    if (!ram) return nullptr;
    if (const auto* action = action_for(ram, include_optional, iwram))
        if (const auto* section = section_by_id(action->section)) return section;
    return area_for(ram, iwram);
}

struct Row { unsigned index; Truth done; Truth milestone = Truth::Unknown; };
struct Collectible { const char* label; const char* kind; Truth done; };
struct Snapshot {
    const Section* area = nullptr;
    std::vector<Collectible> collectibles;
    const Section* section = nullptr;
    const Action* action = nullptr;
    const Action* main_action = nullptr;
    const Action* journey = nullptr;
    const Action* return_action = nullptr;
    std::vector<const Action*> optional_actions;
    std::vector<Row> rows, treasure;
};
// Generated text is immutable. Only snapshots cross the emulation/UI boundary.
inline std::atomic<std::shared_ptr<const Snapshot>> current;

inline bool pickup_in_area(unsigned index, unsigned area) {
    const auto& c = conditions[index];
    if (c.op == Op::Room) {
        for (const auto& scope : locations)
            if (scope.room == c.value && scope.section == area) return true;
    } else if (c.op == Op::All || c.op == Op::Any) {
        for (unsigned i = 0; i < c.count; ++i)
            if (pickup_in_area(children[c.first + i], area)) return true;
    }
    return false;
}
inline bool item_names_djinni(std::string_view items, std::string_view name) {
    while (!items.empty()) {
        const auto end = items.find(';');
        auto item = items.substr(0, end);
        item = item.substr(0, item.find('('));
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (item == name) return true;
        if (end == std::string_view::npos) break;
        items.remove_prefix(end + 1);
    }
    return false;
}
inline void sample(const unsigned char* ram, const unsigned char* iwram = nullptr) {
    if (!ram) { current.store(std::shared_ptr<const Snapshot>{}); return; }
    const bool include_optional = completionist.load();
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->area = area_for(ram, iwram);
    const Action* journey = nullptr;
    for (const auto& action : actions) {
        const bool needed = action.optional ? include_optional : !snapshot->main_action;
        const bool eligible = needed &&
            evaluate(action.when, ram, iwram) == Truth::Yes;
        if (eligible) {
            if (!snapshot->action) snapshot->action = &action;
            if (!action.optional && !snapshot->main_action) snapshot->main_action = &action;
            if (include_optional && action.optional) {
                // Preserve authored priority within a detour, while showing independent
                // detours together. Later steps must not contradict the first next step.
                bool already_shown = false;
                for (const auto* shown : snapshot->optional_actions)
                    already_shown |= std::string_view(shown->optional_route) == action.optional_route;
                if (!already_shown) snapshot->optional_actions.push_back(&action);
            }
        }
        if (!snapshot->return_action && action.return_when >= 0 &&
            evaluate(unsigned(action.return_when), ram, iwram) == Truth::Yes)
            snapshot->return_action = &action;
        if (!snapshot->main_action && !journey && action.journey_when >= 0 &&
            evaluate(unsigned(action.journey_when), ram, iwram) == Truth::Yes)
            journey = &action;
    }
    if (!snapshot->main_action) snapshot->journey = journey;
    snapshot->section = snapshot->action ? section_by_id(snapshot->action->section) : nullptr;
    if (!snapshot->section) snapshot->section = snapshot->area;
    if (snapshot->section) for (unsigned i = snapshot->section->first; i < snapshot->section->end; ++i) {
        const auto& entry = entries[i];
        if (entry.when >= 0 && evaluate(unsigned(entry.when), ram, iwram) != Truth::Yes) continue;
        snapshot->rows.push_back({i, evaluate(entry.done, ram, iwram), evaluate(entry.milestone, ram, iwram)});
    }
    for (unsigned i = 0; i < std::size(treasures); ++i) {
        const auto& pickup = treasures[i];
        const auto done = evaluate(pickup.done, ram, iwram);
        if (evaluate(pickup.when, ram, iwram) == Truth::Yes)
            snapshot->treasure.push_back({i, done});
        if (snapshot->area && (pickup.area ? pickup.area == snapshot->area->id
                                           : pickup_in_area(pickup.when, snapshot->area->id)))
            snapshot->collectibles.push_back({pickup.label, pickup.kind, done});
    }
    if (snapshot->area) for (const auto& rule : rules) {
        const char* label = objectives[objective_index(rule.id)].text;
        const std::string_view full(label);
        const auto name = full.substr(0, full.find(':'));
        for (unsigned i = snapshot->area->first; i < snapshot->area->end; ++i) {
            if (!item_names_djinni(entries[i].item, name)) continue;
            snapshot->collectibles.push_back({label, "djinn", read_flag(ram, rule.flag) ? Truth::Yes : Truth::No});
            break;
        }
    }
    current.store(std::shared_ptr<const Snapshot>(std::move(snapshot)));
}
} // namespace gsr::adventure::walkthrough
