#pragma once

#include <cstdint>
#include <iterator>
#include <span>

#include "game_options.h"
#include "gba_bus.h"
#include "host_config_ui.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif

namespace gsr::field_character {

struct Hero {
    unsigned id;
    const char* name;
};

inline constexpr Hero kHeroes[] = {
    {0, "Isaac"}, {1, "Garet"}, {2, "Ivan"}, {3, "Mia"},
    {4, "Felix"}, {5, "Jenna"}, {6, "Sheba"}, {7, "Piers"},
};
inline constexpr Hero kBrokenSealHeroes[] = {
    kHeroes[0], kHeroes[1], kHeroes[2], kHeroes[3], kHeroes[5],
};

struct Game {
    std::uint32_t animate, sprite_records, actor_context, party_order;
    unsigned pose_offset, dirty_offset;
    unsigned rope_action;  // diagonal rope; kNoAction where the game has none
    std::span<const Hero> heroes;
    std::span<const unsigned> alternate_bodies;
};

inline const Game* game = nullptr;
// Action IDs are below 0x40 (dispatcher TBS 0x0800CB40, TLA 0x08024016).
inline constexpr unsigned kNoAction = 0x40;
inline constexpr char kOption[] = "WalkingCharacter";
constexpr int kPartyLeader = 0, kOriginal = 1, kFirstHero = 2;

inline void install(const Game& config) { game = &config; }

inline int selection() {
    const int saved = gbarecomp::game_option_get(kOption, kPartyLeader);
    if (saved == kOriginal) return saved;
    for (const Hero& hero : game->heroes)
        if (saved == kFirstHero + static_cast<int>(hero.id)) return saved;
    return kPartyLeader;
}

inline void draw() {
#ifdef GBARECOMP_HAVE_IMGUI
    if (!game) return;
    const int current = selection();
    const char* preview = current == kOriginal ? "Original character" : "Party leader";
    for (const Hero& hero : game->heroes)
        if (current == kFirstHero + static_cast<int>(hero.id)) preview = hero.name;
    ImGui::TextUnformatted(gbarecomp::ui_text("Walking character"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##WalkingCharacter", gbarecomp::ui_text(preview))) {
        const auto choice = [current](const char* label, int value) {
            if (ImGui::Selectable(gbarecomp::ui_text(label), value == current))
                gbarecomp::game_option_set(kOption, value);
            if (value == current) ImGui::SetItemDefaultFocus();
        };
        choice("Party leader", kPartyLeader);
        choice("Original character", kOriginal);
        for (const Hero& hero : game->heroes)
            choice(hero.name, kFirstHero + static_cast<int>(hero.id));
        ImGui::EndCombo();
    }
#endif
}

inline bool ram(std::uint32_t addr, unsigned size) {
    return (addr & 3u) == 0 &&
        ((addr >= 0x02000000u && addr <= 0x02040000u - size) ||
         (addr >= 0x03000000u && addr <= 0x03008000u - size));
}

inline bool rom(const gba::GbaBus& bus, std::uint32_t addr, unsigned size) {
    return addr >= 0x08000000u &&
        static_cast<std::uint64_t>(addr - 0x08000000u) + size <= bus.rom_size();
}

inline std::uint32_t record(unsigned id) { return game->sprite_records + 20u * id; }

inline std::uint32_t pose_script(gba::GbaBus& bus, std::uint32_t rec, unsigned pose) {
    if (pose >= bus.read8(rec + 5u)) return 0;
    const auto entry = bus.read32(rec + 0x10u) + 4u * pose;
    if (!rom(bus, entry, 4u)) return 0;
    const auto script = bus.read32(entry);
    return rom(bus, script, 2u) ? script : 0;
}

// Both native animators draw the primary layer at +0x28. Keep the layer's
// sprite ID and animation table native, so actor scripts and new poses still
// address the original character. The frame table identifies our previous
// selection even after loading a state, without a separate host cache.
inline void on_entry(std::uint32_t pc) {
    if (!game || pc != game->animate) return;
    auto* bus = gbarecomp::active_bus();
    const auto sprite = g_cpu.R[0];
    if (!bus || !ram(sprite, 0x2Cu)) return;
    const auto layer = bus->read32(sprite + 0x28u);
    if (!ram(layer, 0x18u)) return;

    const unsigned body = bus->read16(layer);
    bool known_body = false;
    for (const auto& hero : game->heroes) known_body |= hero.id == body;
    for (unsigned alt : game->alternate_bodies) known_body |= alt == body;
    if (!known_body) return;
    const auto original = record(body);
    if (bus->read32(layer + 0xCu) != bus->read32(original + 0x10u)) return;

    const auto old_frames = bus->read32(layer + 8u);
    const auto original_frames = bus->read32(original + 0xCu);
    bool previously_changed = false;
    if (old_frames != original_frames) {
        for (const auto& hero : game->heroes)
            previously_changed |= old_frames == bus->read32(record(hero.id) + 0xCu);
        if (!previously_changed) return;
    }

    // The native field actor identity is the word before the battle order.
    // The first order byte can change independently when the party is moved.
    unsigned chosen = body;
    bool rope = false;
    const unsigned native_lead = bus->read32(game->party_order - 4u);
    const auto context = bus->read32(game->actor_context);
    if (native_lead < std::size(kHeroes) && ram(context, 0x34u)) {
        const auto actor = bus->read32(context + 0x14u + native_lead * 4u);
        // Every action, talking and story scenes included (Jimmy, 3 Nov);
        // one-off poses still fall back to the original body below.
        if (ram(actor, 0x54u) && bus->read32(actor + 0x50u) == sprite) {
            // Each action has its own program whose first word is the action
            // (TLA diagonal rope 0x0802F0C8).
            const auto program = bus->read32(actor);
            rope = rom(*bus, program, 4u) && bus->read32(program) == game->rope_action;
            const int mode = selection();
            const unsigned first = bus->read8(game->party_order);
            const bool member = first < std::size(kHeroes) &&
                (bus->read8(0x02000040u) & (1u << first));
            if (mode == kPartyLeader && member && first != native_lead) chosen = first;
            if (mode >= kFirstHero) chosen = static_cast<unsigned>(mode - kFirstHero);
        }
    }
    if (!previously_changed && chosen == body) return;

    const unsigned pose = bus->read8(sprite + game->pose_offset);
    // Poses 0-5 mean the same for every hero. Only the main heroes (TBS
    // Isaac, TLA Isaac/Felix) have 6-8 (jump; the game raises the actor for
    // the bounce) and 9 (a pose-1 idle); the others hold unrelated animations
    // there, so use their walk and stand. Field Psynergy casts 20 then 21
    // (F12 TBS 0069, TLA 0208/0209), the ring cast every hero has. Ladder
    // and vine poses 10-13 walk (below), and so do the room-exit stair
    // poses 23-26 (only Isaac draws them, sinking into or rising out of the
    // stairs; F12 TBS 0093/0094 Ivan: 26 climbing out under the facing
    // 0xC000, 24 arriving under 0x4000; TLA Isaac's 23-26 are the same ROM
    // drawings). Every other pose (pushing, story
    // gestures) stands, so the original body never shows and a held story
    // pose does not walk in place (Jimmy, 4 Nov; F12 TBS 0081 Sanctum pose 22).
    constexpr unsigned kSharedPose[] = {0, 1, 2, 3, 4, 5, 2, 2, 2, 1};
    constexpr unsigned kWalkPose = 2;
    constexpr unsigned kStandPose = 1;
    auto target = original;
    auto script = pose_script(*bus, original, pose);
    if (chosen != body) {
        for (const Hero& hero : game->heroes) {
            if (hero.id != chosen) continue;
            const auto replacement = record(chosen);
            const auto wanted = pose_script(*bus, replacement,
                pose < std::size(kSharedPose) ? kSharedPose[pose]
                : pose == 20 || pose == 21 ? pose
                : pose <= 13 || (pose >= 23 && pose <= 26) ? kWalkPose
                : kStandPose);
            if (wanted) {
                target = replacement;
                script = wanted;
            }
            break;
        }
    }
    if (target == original && !previously_changed) return;
    // R1 is the animator's view angle (direction table TBS 0x080130AC, TLA
    // 0x0802EAF4); the actor's own facing is untouched. Ladder and vine poses
    // 10-13 (getting on 13, climbing 10/12, hopping off 10; F12 TLA 0211) walk
    // with the back turned: 0xC000 is the back view (ROM frames drawn). On a
    // diagonal rope the facing stays fixed and the walk faces backwards
    // compared with the native hang (F12 TLA 0210), so mirror it.
    if (target != original) {
        if (pose >= 10 && pose <= 13) g_cpu.R[1] = 0xC000u;
        else if (rope) g_cpu.R[1] ^= 0x8000u;
    }
    const auto frames = bus->read32(target + 0xCu);
    const auto old_script = bus->read32(layer + 0x10u);
    if (!old_script && !previously_changed) return;
    // A hold-only pose can rely on a frame from the previous animation.
    // When crossing between characters, seed it with the new body's own
    // directional standing animation instead of keeping the old frame index.
    if (script && bus->read8(script) == 0xFEu) script = pose_script(*bus, target, 0);
    if (frames == old_frames && (script == old_script || !old_script)) return;

    bus->write32(layer + 8u, frames);
    bus->write8(layer + 7u, bus->read8(target + 0xAu));
    bus->write32(layer + 0x10u, old_script ? script : 0u);
    bus->write16(layer + 2u, 0);
    bus->write8(layer + 4u, bus->read8(target + 4u));
    bus->write8(layer + 0x14u, 0);
    bus->write8(layer + 0x16u, 0xFFu);
    bus->write8(layer + 0x17u, 0xFFu);
    bus->write8(sprite + game->dirty_offset, 1);
}

}  // namespace gsr::field_character
