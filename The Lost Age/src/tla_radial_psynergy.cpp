// The Lost Age's radial Psynergy menu places (shared_game/radial_psynergy.h).
// Evidence: FACTS "Radial Psynergy menu" (4 November 2026).
#include "tla.h"
#include "../../shared_game/radial_psynergy.h"

namespace {
namespace radial = gsr::radial_psynergy;

std::uint16_t (*next_keys)(std::uint16_t) = nullptr;
std::uint16_t keys(std::uint16_t value) {
    value = radial::keyinput(value);
    return next_keys ? next_keys(value) : value;
}
// Auto Field Psynergy (FACTS "Auto Field Psynergy", 5 November 2026). The
// field loop's A (0x080CC192..0x080CC22E): actor 8 while the halfword
// 0x020004B6 is set, else [0x02000454]; talk when 0x080CD91C(actor) != -1 and
// is above 0x3F or 0x080CCE94 of it is nonzero; else check when 0x080CC54C()
// is nonzero. The dispatcher finds the target with 0x080CE31C(kind), then
// searches the room's events ([field] + 0x10) with 0x080CE458(category,
// kind, target) for 0x80000005, 0x10000005, 5, 0x50000005
// (0x080CE7F0..0x080CE86E); event bit 0x80 matches anywhere in the room.
bool a_acts() {
    auto* b = gbarecomp::active_bus();
    const std::uint32_t actor = b->read16(0x020004B6u) ? 8u : b->read32(0x02000454u);
    const auto target = static_cast<std::int32_t>(radial::guest(TLA_ROM(0x080CD91Cu), actor));
    if (target != -1 && (target > 0x3F || radial::guest(TLA_ROM(0x080CCE94u), target))) return true;
    return radial::guest(TLA_ROM(0x080CC54Cu)) != 0;
}
std::uint32_t find(std::uint32_t category, unsigned kind) {
    const std::uint32_t target = radial::guest(TLA_ROM(0x080CE31Cu), kind);
    return radial::guest(TLA_ROM(0x080CE458u), category, kind, target);
}
}  // namespace

// Field button poll 0x08026E60 (returns 0 while [0x0300006C] is 0; while the
// byte at field + 0xCAC is set every button queues one fixed action instead,
// 0x08026E72..0x08026EEA): masks 0x02000470.. (L 0x200 at 0x02000478,
// R 0x100 at 0x0200047A) pick the shortcut words 0x02000480 / 0x02000482
// (written by the game's setter 0x080FD52C) for the cast routine 0x08026DD0,
// which queues field + 0x166 as the Psynergy menu does, for the dispatcher
// 0x080CE61C. Ability records
// 0x080B7C14 (12 bytes, ids to 0x2DE, field kind +6 as dispatcher 0x080CE64A
// reads it); icons 0x08054A14 (260, count 0x0803D4D0) in the common OBJ
// colours 0x08017B10; names text 0x5A7 + id.
void tla_install_radial_psynergy(gbarecomp::RunOptions& options) {
    static const radial::AutoCast auto_cast = {
        0x02000470u, 0x03001154u, 0x10u, a_acts, find,
        {0x80000005u, 0x10000005u, 0x5u, 0x50000005u}, 4u, 0x80u,
    };
    static const radial::Game profile = {
        TLA_ROM(0x08026E60u), 0x0300006Cu, 0xCACu,
        TLA_ROM(0x08026DD0u), TLA_ROM(0x080CE61Cu), TLA_ROM(0x081C0010u),
        TLA_ROM(0x080B7C14u), 12u, 0x2DFu, 6u, true,
        TLA_ROM(0x08054A14u), 260u, TLA_ROM(0x08017B10u),
        0x02000520u, 0x02000040u, 0x02000458u, 0x02000480u, 0x02000482u,
        TLA_TEXT(0x5A7u),
        TLA_ROM(0x080A9F54u) - 0x08000000u, TLA_ROM(0x08060C30u) - 0x08000000u,
        tla_publish_menu_art, &auto_cast,
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
