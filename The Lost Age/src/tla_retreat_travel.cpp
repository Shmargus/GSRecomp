// TLA's native Teleport destinations and dock positions, with Retreat's
// own PP cost. Native ship unlocks/ownership flags are never changed.
#include "tla.h"
#include "gba_bus.h"
#include "../../shared_game/retreat_travel.h"
#include "../../shared_game/treasure_tracker.h"

namespace {
namespace travel = gsr::retreat_travel;
struct Town { const char* name; unsigned area; };
constexpr Town towns[] = {
    {"Daila", 2}, {"Madra", 9}, {"Mikasalla", 14}, {"Garoh", 15},
    {"Alhafra", 20}, {"Naribwe", 23}, {"Kibombo", 27}, {"Yallam", 33},
    {"Apojii Islands", 36}, {"Izumo", 38}, {"Champa", 40}, {"Lemuria", 45},
    {"Contigo", 54}, {"Shaman Village", 58}, {"Loho", 59}, {"Prox", 62},
};
bool rom_range(gba::GbaBus* b, std::uint32_t p, unsigned n) {
    return p >= 0x08000000u && std::uint64_t(p - 0x08000000u) + n <= b->rom_size();
}
unsigned location(gba::GbaBus* b, unsigned room, unsigned entrance) {
    namespace tt = gsr::treasure_tracker;
    if (!tt::game) return ~0u;
    bool found = false;
    const auto label = tt::location(*tt::game, b->rom_ptr(), b->rom_size(), room, entrance, &found);
    return found ? label : ~0u;
}
// Prefer the exact native town dock. Lemuria's map target is its city room
// (199), whereas its native mooring row names its dock (202). Resolve that
// through the native location-name lookup already used by the tracker;
// its city and dock have DIFFERENT room keys but the same location label.
bool mooring(gba::GbaBus* b, unsigned room, unsigned entrance, unsigned& result) {
    unsigned sibling = 0, siblings = 0;
    const auto label = location(b, room, entrance);
    for (auto at = TLA_ROM(0x080EF824u); rom_range(b, at, 16); at += 16u) {
        const auto dock = b->read16(at);
        if (dock == 0xFFFFu) break;
        const auto entry = b->read16(at + 2u);
        if (dock == room && (entry == entrance || entry == 0xFFFFu || entrance == 0xFFFFu)) {
            result = dock; return true;
        }
        if (entry == 0xFFFFu && label != ~0u && location(b, dock, entry) == label) {
            sibling = dock; ++siblings;
        }
    }
    if (siblings != 1u) return false;
    result = sibling; return true;
}
std::vector<travel::Destination> list() {
    auto* b = gbarecomp::active_bus();
    std::vector<travel::Destination> result;
    for (const auto& town : towns) {
        if (!gsr::read_flag(b->ewram_ptr(), 0x180u + town.area)) continue;
        for (auto at = TLA_ROM(0x080F1140u); rom_range(b, at, 20); at += 20u) {
            const auto id = b->read8(at);
            if (!id) break;
            const auto icon = b->read8(at + 1u) >> 4;
            if (id != town.area || icon == 0u || icon == 15u) continue;
            const auto room = b->read16(at + 16u), entrance = b->read16(at + 18u);
            unsigned dock = 0;
            if (!mooring(b, room, entrance, dock)) break;
            result.push_back({town.name, 0x180u + town.area, at, room, entrance, dock});
            break;
        }
    }
    return result;
}
bool arrive(const travel::Destination& d) {
    auto* b = gbarecomp::active_bus();
    unsigned dock = 0;
    if (!rom_range(b, d.route, 20) || b->read16(d.route + 16u) != d.room ||
        b->read16(d.route + 18u) != d.entrance ||
        !mooring(b, d.room, d.entrance, dock) || dock != d.mooring) return false;
    // Capture the visible actors BEFORE Retreat hides/lifts Felix. Teleport's
    // map coroutine must restore this snapshot, not the finished effect.
    return travel::call(TLA_ROM(0x080CAD9Cu), 0u, 0u);
}
bool native_running = false, native_casting = false;
std::vector<std::uint8_t> world_image;
std::uint32_t image_width = 0, image_height = 0;
// ED2A4's measured prologue saves eight words and reserves 0x38 bytes.
constexpr unsigned kNativeStack = 0x58u;
bool native_hold(gba::GbaBus* b, travel::Hold& h, unsigned depth) {
    return travel::read_hold_at(b, h, g_cpu.R[13] + depth) && h[5] != 2u;
}
bool native_entry(std::uint32_t pc) {
    auto* b = gbarecomp::active_bus();
    if (!b || !travel::game || travel::busy) return false;
    travel::Hold h{};
    if (pc == travel::game->dispatcher && native_hold(b, h, 0u) && travel::phase(h) == 0u) {
        native_casting = true;
    } else if (pc == TLA_ROM(0x080ED2A4u) && native_hold(b, h, 0u) && travel::phase(h) == 2u) {
        native_running = true;
        g_cpu.R[14] = travel::game->frame | 1u;
    } else if (pc == travel::game->frame && native_hold(b, h, 0u)) {
        native_casting = false;
        if (travel::phase(h) == 3u) {
            // Retreat's real effect completed. Travel does not depend on
            // Teleport's separate command ever being queued by that cast.
            travel::call(TLA_ROM(0x080D2260u), 0u, 0u);
            travel::set_phase(b, g_cpu.R[13], h, 2u);
            g_cpu.R[14] = TLA_ROM(0x080ED2A4u) | 1u;
            gbarecomp::guest_return_now(pc, 1u);
            return true;
        }
        const auto room = b->read16(0x020004FEu);
        if (travel::phase(h) == 2u && room != 0xFFFFu) {
            // ED2A4 already requested a field reload and positioned the ship.
            // Give that reload the selected destination now. FC89's stage
            // byte would reload the old room first and play Teleport's effects.
            const auto town_flags = b->read8(travel::route(h) + 1u);
            if (room != 2u || (town_flags & 0x10u))
                b->write16(0x020004B6u, 0u);
            else if (town_flags & 0x20u)
                b->write16(0x020004B6u, 1u);
            // Teleport clears this flag after reloading the departure scene.
            // Retreat loads the destination first, so let native field setup
            // place the party there instead of restoring the departure actors.
            constexpr unsigned restore_actors_flag = 0x109u;
            constexpr auto flag_byte = 0x02000000u + gsr::kFlagBank + (restore_actors_flag >> 3);
            constexpr auto flag_mask = 1u << (restore_actors_flag & 7u);
            b->write8(flag_byte, b->read8(flag_byte) & ~flag_mask);
            b->write16(travel::game->room, room);
            b->write16(travel::game->room + 2u, b->read16(0x02000500u));
        }
        native_running = false; // Native UI has returned; shared core unwinds.
        travel::swallowing = true;
    } else if ((pc == travel::game->frame || pc == TLA_ROM(0x080CA368u) ||
                pc == TLA_ROM(0x080CAD9Cu)) &&
               native_hold(b, h, kNativeStack) && travel::phase(h) == 2u) {
        native_running = true;
        const auto row = travel::route(h);
        if (pc == TLA_ROM(0x080CAD9Cu)) {
            // The complete, pre-effect snapshot was already taken above.
            gbarecomp::guest_return_now(pc, 0u);
            return true;
        }
        if (pc == travel::game->frame && g_cpu.R[14] == TLA_ROM(0x080ED4A9u)) {
            const auto table = TLA_ROM(0x080F1140u);
            // Selection is already validated by the picker. Let native A
            // confirmation perform docking, cleanup, flags and field exit.
            if (row >= table && (row - table) % 20u == 0u && rom_range(b, row, 20u)) {
                b->write16(0x0202A012u, static_cast<std::uint16_t>((row - table) / 20u));
                b->write32(g_cpu.R[5], 0u);
                b->write32(g_cpu.R[5] + 12u, 1u);
                gbarecomp::guest_return_now(pc, 0u);
                return true;
            }
        } else if (pc == TLA_ROM(0x080CA368u)) {
            unsigned dock = 0;
            if (mooring(b, b->read16(row + 16u), b->read16(row + 18u), dock)) {
                g_cpu.R[0] = dock;
                g_cpu.R[1] = 0xFFFFFFFFu;
            }
        }
    }
    return false;
}
std::uint16_t (*next_keys)(std::uint16_t) = nullptr;
std::uint16_t keys(std::uint16_t value) {
    value = native_running ? 0x3FFu : travel::keyinput(value);
    return next_keys ? next_keys(value) : value;
}
} // namespace

void tla_install_retreat_travel(gbarecomp::RunOptions& options) {
    static const travel::Cast cast = {TLA_ROM(0x08016CE4u), TLA_ROM(0x08038120u),
        TLA_ROM(0x08038040u), TLA_ROM(0x08038140u), TLA_ROM(0x080D295Cu),
        TLA_ROM(0x080AD078u), TLA_ROM(0x080CE8C3u), TLA_ROM(0x080E0DD4u),
        0x30u, 0x40u};
    static const travel::Game profile = {TLA_ROM(0x080CE61Cu), TLA_ROM(0x080CC343u),
        TLA_ROM(0x08013560u), TLA_ROM(0x081C0010u), 0x02000428u,
        0x02000420u, 0x0300006Cu, 0x02000520u, 0x158u, 6u,
        tla_field_psy_fast, list, arrive, tla_publish_menu_art, TLA_ROM(0x080CE61Cu), 6u, &cast};
    travel::install(profile);
    next_keys = options.keyinput_filter;
    options.keyinput_filter = keys;
}
bool tla_retreat_travel_on_entry(std::uint32_t pc) {
    return native_entry(pc) || travel::cast_entry(pc) || travel::on_entry(pc);
}
void tla_retreat_travel_reset() {
    travel::reset(); native_running = native_casting = false;
    world_image.clear(); image_width = image_height = 0;
}
bool tla_retreat_travel_paint(std::uint8_t* rgb, std::uint32_t w, std::uint32_t h) {
    // Replace the native map's internal frames with the last world view;
    // the destination list is the only travel UI the player sees.
    const auto bytes = std::size_t(w) * h * 3u;
    if (native_running) {
        if (image_width == w && image_height == h && world_image.size() == bytes)
            std::copy(world_image.begin(), world_image.end(), rgb);
        else
            std::fill(rgb, rgb + bytes, 0u);
        return true;
    }
    if (!travel::showing && !native_casting) return false;
    world_image.assign(rgb, rgb + bytes); image_width = w; image_height = h;
    if (!travel::showing) return false;
    tla_menu_paint_window(rgb, w, h, travel::kTileX, travel::kTileY, travel::kTiles, travel::kRows);
    auto draw = [&](const std::string& s, int x, int y) { tla_menu_paint_text(rgb, w, h, s, x, y); };
    // Shared painter already supplies Latin-1; tla_menu_text_width accepts
    // the same native strings as the existing Custom Battle panel.
    auto width = [](const std::string& s) { return int(tla_menu_text_width(s)); };
    travel::paint(draw, width, (travel::kTileX + 1) * 8, (travel::kTileY + 1) * 8);
    return true;
}
