#include "enemy_hp_bar.h"

#include <atomic>

#include "game_options.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace gsr::enemy_hp_bar {
namespace {

Game g_game{};
bool g_installed = false;

// Both taken in the target-selection loop (emulation thread): the hovered
// enemy at its call to the frame routine, the name's place when it draws the
// name. Read at present time.
std::atomic<unsigned long long> g_seen_frame{0};
std::atomic<std::uint32_t> g_unit{0};
std::atomic<std::uint32_t> g_name_unit{0};
std::atomic<std::int32_t> g_name_centre{0};

// The enemy under the cursor, read from the loop's own stack; 0 if the
// cursor is on a party member. sp must be the loop's.
std::uint16_t hovered(std::uint32_t sp) {
    const auto count = static_cast<std::int32_t>(bus_read_u32(sp + kCandidateCountSp));
    const auto index = static_cast<std::int32_t>(bus_read_u32(sp + kCursorIndexSp));
    if (index < 0 || index >= count) return 0;
    const std::uint16_t unit = bus_read_u16(
        bus_read_u32(sp + g_game.candidate_list_sp) + static_cast<std::uint32_t>(index) * 2u);
    return unit >= kFirstEnemy && unit <= kLastEnemy ? unit : 0;
}

}  // namespace

void install(const Game& game) {
    g_game = game;
    g_installed = true;
}

void on_entry(std::uint32_t pc, int (*name_width)(std::uint32_t name)) {
    if (!g_installed) return;
    const std::uint32_t lr = g_cpu.R[14] & ~1u;
    if (pc == g_game.frame_routine_pc && lr == g_game.target_loop_return) {
        const std::uint16_t unit = hovered(g_cpu.R[13]);
        if (unit == 0) return;
        g_unit.store(unit);
        g_seen_frame.store(runtime_current_frame());
    } else if (pc == g_game.name_draw_pc && lr == g_game.name_draw_return) {
        // The name's centre on screen, measured in the menu font it is
        // drawn with.
        const std::uint16_t unit = hovered(g_cpu.R[13]);
        if (unit == 0) return;
        g_name_centre.store(kNameInsideX + static_cast<std::int32_t>(g_cpu.R[2]) +
                            name_width(g_cpu.R[0]) / 2);
        g_name_unit.store(unit);
    }
}

bool bar(Bar* out) {
    const unsigned long long now = runtime_current_frame();
    const unsigned long long seen = g_seen_frame.load();
    if (!g_installed || seen == 0 || now < seen || now > seen + 1u ||
        g_name_unit.load() != g_unit.load() ||
        gbarecomp::game_option_get(kOptionKey, 0) == 0)
        return false;
    const std::uint32_t record = bus_read_u32(g_game.enemy_base_slot) +
        (g_unit.load() - kFirstEnemy) * kUnitStride;
    out->max_hp = bus_read_u16(record + kMaxHpOffset);
    if (out->max_hp <= 0) return false;
    out->hp = std::min<int>(bus_read_u16(record + kCurrentHpOffset), out->max_hp);
    out->left = std::clamp(g_name_centre.load() - kBarWidth / 2, 0, kScreenWidth - kBarWidth);
    out->top = kNameTop - kBarGap - kBarHeight;
    return true;
}

}  // namespace gsr::enemy_hp_bar
