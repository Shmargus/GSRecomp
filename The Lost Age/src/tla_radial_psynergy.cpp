// The Lost Age's radial Psynergy menu places (shared_game/radial_psynergy.h).
// Evidence: FACTS "Radial Psynergy menu".
#include "tla.h"
#include "../../shared_game/radial_psynergy.h"

namespace {
namespace radial = gsr::radial_psynergy;

std::uint16_t (*next_keys)(std::uint16_t) = nullptr;
std::uint16_t keys(std::uint16_t value) {
    value = radial::keyinput(value);
    return next_keys ? next_keys(value) : value;
}
// Auto Field Psynergy (FACTS "Auto Field Psynergy"). How the field loop handles the A
// button (0x080CC192..0x080CC22E):
//   - The actor is actor 8 while the halfword 0x020004B6 is set, else [0x02000454].
//   - It talks when 0x080CD91C(actor) != -1 and either the result is above 0x3F or
//     0x080CCE94 of it is nonzero.
//   - Otherwise it checks when 0x080CC54C() is nonzero.
// The dispatcher finds the target with 0x080CE31C(kind). It then searches the room's
// events ([field] + 0x10) with 0x080CE458(category, kind, target) for 0x80000005,
// 0x10000005, 5 and 0x50000005 (0x080CE7F0..0x080CE86E). Event bit 0x80 matches
// anywhere in the room.
bool a_acts() {
    auto* b = gbarecomp::active_bus();
    const std::uint32_t actor = b->read16(0x020004B6u) ? 8u : b->read32(0x02000454u);
    const auto target = static_cast<std::int32_t>(radial::guest(TLA_ROM(0x080CD91Cu), actor));
    if (target != -1 && (target > 0x3F || radial::guest(TLA_ROM(0x080CCE94u), target))) return true;
    return radial::guest(TLA_ROM(0x080CC54Cu)) != 0;
}
// Both searches walk the whole table on every call (85 records on the world
// map), and auto_word asks per known Psynergy and category. Measured: with 4+
// Psynergy known on the world map (about 16 in a town) the searches run past
// the frame and that frame's A press is lost, ship boarding included. Both
// only ever return a record with type 5 (word & 0xF) whose ability, byte 1 of
// the word (getter 0x080AF43C), has field kind +6 == kind (0x080CE4C2..D8,
// 0x080CE384..96). So a room without one is answered here, without them.
bool room_reacts_to(gba::GbaBus& b, unsigned kind) {
    for (std::uint32_t at = b.read32(b.read32(0x0300006Cu) + 0x10u);; at += 12u) {
        const std::uint32_t word = b.read32(at);
        if (word == 0xFFFFFFFFu) return false;
        if ((word & 0xFu) == 5u && b.read8(radial::ability((word >> 8) & 0xFFu) + 6u) == kind)
            return true;
    }
}
std::uint32_t find(std::uint32_t category, unsigned kind) {
    if (!room_reacts_to(*gbarecomp::active_bus(), kind)) return 0;
    const std::uint32_t target = radial::guest(TLA_ROM(0x080CE31Cu), kind);
    return radial::guest(TLA_ROM(0x080CE458u), category, kind, target);
}
}  // namespace

// The field button poll, 0x08026E60.
//   - It returns 0 while [0x0300006C] is 0.
//   - While the byte at field + 0xCAC is set, every button queues one fixed action
//     instead (0x08026E72..0x08026EEA).
//   - Otherwise the masks at 0x02000470.. (L 0x200 at 0x02000478, R 0x100 at
//     0x0200047A) pick the shortcut words 0x02000480 / 0x02000482, which the game's
//     setter 0x080FD52C writes. Those go to the cast routine 0x08026DD0. It queues
//     field + 0x166, as the Psynergy menu does, for the dispatcher 0x080CE61C.
// Ability records: 0x080B7C14 (12 bytes, ids to 0x2DE). The field kind at +6 is what
// dispatcher 0x080CE64A reads.
// Icons: 0x08054A14 (260 entries), in the common OBJ colours 0x08017B10. The count
// routine is 0x0803D4D0. It is asked at build time because Reloaded moves the table
// and returns 0xFFFF.
// Names: text 0x5A7 + id.
void tla_install_radial_psynergy(gbarecomp::RunOptions& options) {
    static const radial::AutoCast auto_cast = {
        0x02000470u, 0x03001154u, 0x10u, a_acts, find,
        {0x80000005u, 0x10000005u, 0x5u, 0x50000005u}, 4u, 0x80u,
        // Menu on Select only: the poll's first mask 0x02000474 (Select, 4
        // in every capture) sets the menu request field + 0x15A; A with no
        // talk or check sets the same request (0x080CC228..0x080CC22C).
        0x02000474u,
    };
    static const radial::Game profile = {
        TLA_ROM(0x08026E60u), 0x0300006Cu, 0xCACu,
        TLA_ROM(0x08026DD0u), TLA_ROM(0x080CE61Cu), TLA_ROM(0x081C0010u),
        TLA_ROM(0x080B7C14u), 12u, 0x2DFu, 6u, true,
        TLA_ROM(0x08054A14u), 260u, TLA_ROM(0x08017B10u),
        0x02000520u, 0x02000040u, 0x02000458u, 0x02000480u, 0x02000482u,
        TLA_TEXT(0x5A7u),
        TLA_ROM(0x080A9F54u) - 0x08000000u, TLA_ROM(0x08060C30u) - 0x08000000u,
        tla_publish_menu_art, &auto_cast, TLA_ROM(0x0803D4D0u),
        // 0x08026DFA..E08: byte field + 0x18A == 3 takes the world-map
        // branch; otherwise the word goes to field + 0x166 (0x08026E4C).
        0x18Au, 1u, 0x166u,
    };
    radial::install(profile);
    next_keys = options.keyinput_filter;
    options.keyinput_filter = keys;
}

bool tla_radial_psynergy_on_entry(std::uint32_t pc) { return radial::on_entry(pc); }

void tla_radial_psynergy_reset() { radial::reset(); }

bool tla_radial_psynergy_paint(std::uint8_t* rgb, std::uint32_t w, std::uint32_t h) {
    return radial::paint(rgb, w, h, [&](int tile_x, int tile_y, int tiles, int rows, auto content) {
        tla_menu_paint_window(rgb, w, h, tile_x, tile_y, tiles, rows);
        auto draw = [&](const std::string& s, int x, int y) { tla_menu_paint_text(rgb, w, h, s, x, y); };
        auto width = [](const std::string& s) { return int(tla_menu_text_width(s)); };
        content(draw, width, (tile_x + 1) * 8, (tile_y + 1) * 8);
    });
}

bool tla_radial_psynergy_visible() {
    std::lock_guard<std::mutex> guard(radial::lock);
    return radial::open;
}
