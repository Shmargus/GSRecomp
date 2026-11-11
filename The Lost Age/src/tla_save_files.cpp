// The Lost Age's half of nine save files (shared_game/save_files_picker.h
// runs the file screens; this is the game's description and save layer).
//
// Measured, verified ROM (FACTS.md "Nine save files", 4 November 2026):
//   A file is one group of three 0x1000-byte flash sectors (0x3000 bytes:
//   "CAMELOT", +7 slot, +8 u16 sum of the 0x2FF0 payload bytes, +0xA
//   generation); five groups hold the three files and the game's spares.
//   Every screen reaches them by slot number 0..2 (slot 3 is a separate
//   record, 0x080160D4, left alone):
//     0x08015E44 load(slot, dst)    1 = absent, 0 = loaded (group read to
//                                   workspace +0x3C, payload to dst)
//     0x08015CA4 write(slot, src)   0 = written
//     0x08015E8C erase(slot)        1 = absent or failed, 0 = erased
//     0x08016054 summaries()        workspace +0x303C + row * 0x40: group
//                                   bytes 0..0x3F, +0x38 four bytes at group
//                                   +0x1010; returns the files present
//   and the screens' own questions:
//     0x08043230 first empty row (Copy's destination), 999 = none
//     0x0804325C count(non_clear)
//     0x080432A4 clear flags        0x02003860 / 0x020036D8 / 0x02005350
//   The file box is the picker 0x08043CD8(row, mode), the same build as The
//   Broken Seal's with a sixth mode (6: refuses +0x33). The workspace is
//   [0x030000CC]; the game's current file is the row at 0x020036D0.
//
// Files 1-3 stay on the cartridge, untouched; files 4-9 are group images in
// the battery extension, so a record is exactly what the game would have
// written. The game's slot numbers are rows of the window.
#include "tla_rom_version.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include "../../shared_game/autosave.h"
#include "../../shared_game/save_files_picker.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace sf = gsr::save_files;

namespace {

constexpr std::size_t kRecordBytes = 0x3000;  // one group
constexpr std::size_t kExtensionBytes =
    sf::kHeaderBytes + (sf::kFiles - sf::kRows) * kRecordBytes;

std::uint8_t* record(int file);
sf::Entry extension_entry(int file);
void set_flags(const sf::Entries& all);
bool challenge_file(int file);

const sf::Game& game() {
    static const sf::Game g = {
        kExtensionBytes,
        0x030000CCu, 0x303Cu, TLA_ROM(0x08016054u), TLA_ROM(0x0801596Cu),
        TLA_ROM(0x0801613Cu),
        TLA_ROM(0x08043230u), TLA_ROM(0x0804325Cu), TLA_ROM(0x080432A4u),
        TLA_ROM(0x08041FA4u), TLA_ROM(0x080420A8u), TLA_ROM(0x08039430u),
        TLA_ROM(0x08039418u), TLA_ROM(0x08013560u), TLA_ROM(0x08002064u), TLA_ROM(0x080439D8u),
        // Picker offsets: Up/Down after 0x08002064, after the per-move
        // 0x08039418(window), after 0x08013560(1) each frame, after 0x080439D8.
        TLA_ROM(0x08043CD8u), 0x512u, 0x5DCu, 0x438u, 0x476u, 0x632u,
        10, 9,       // mode r10, window r9
        8u, 0u, 0u,  // [sp+8] newest; no dirty flag
        0x220u, 0x234u, 0x248u, 0x262u, 0x27Cu,
        0x224u, 0x290u, 0x2A2u, 0x29Au,
        // The Up/Down row tests that skip a file the mode cannot use:
        // config/tla/main.toml overrides.
        {0x49Eu, 0x4B4u, 0x4CEu, 0x4E8u, 0x504u, 0x568u, 0x57Eu, 0x598u, 0x5B2u, 0x5CEu},
        extension_entry, set_flags, challenge_file,
        // The party leader's name (save_files_picker.h show_leaders):
        // records payload +0x520, 0x14C each (lookup 0x08016CA4); a file is
        // one group.
        0x520u, 0x14Cu, kRecordBytes,
        [](int file) -> const std::uint8_t* { return record(file); },
    };
    return g;
}

std::uint8_t* record(int file) {
    return sf::extension(game()) + sf::kHeaderBytes + (file - sf::kRows) * kRecordBytes;
}

// A summary as 0x08016054 builds it, for a file in the extension.
sf::Entry extension_entry(int file) {
    sf::Entry e{};
    const std::uint8_t* r = record(file);
    if (!sf::valid(r, kRecordBytes - sf::kRecordHeader)) return e;
    std::memcpy(e.data(), r, sf::kSummary);
    std::memcpy(e.data() + 0x38, r + 0x1010, 4);
    return e;
}

// Challenge Mode's bytes are RAM 0x02000028 (src/tla_new_game.cpp), so
// payload +0x28, group +0x38. On the cartridge a file is the group whose
// first sector carries its slot; the newest generation (+0xA) wins, as the
// save layer's own table keeps it.
constexpr std::size_t kChallengeOffset = sf::kRecordHeader + 0x28;
bool challenge_file(int file) {
    if (file >= sf::kRows) {
        const std::uint8_t* r = record(file);
        return sf::valid(r, kRecordBytes - sf::kRecordHeader) &&
               tla_challenge_bytes(r + kChallengeOffset);
    }
    gba::GbaBus* b = sf::bus();
    if (!b || !b->save().flash_enabled()) return false;
    const std::vector<std::uint8_t> flash = b->save().flash_bytes();
    const std::uint8_t* bytes = nullptr;
    int best = -1;
    for (std::size_t s = 0; s + kRecordBytes <= flash.size(); s += kRecordBytes) {
        const std::uint8_t* group = flash.data() + s;
        if (!sf::present(group) || group[7] != file) continue;
        const int generation = group[0xA] | (group[0xB] << 8);
        if (generation > best) {
            best = generation;
            bytes = group + kChallengeOffset;
        }
    }
    return tla_challenge_bytes(bytes);
}

// 0x080432A4's flags over nine files.
void set_flags(const sf::Entries& all) {
    gba::GbaBus* b = sf::bus();
    constexpr std::uint32_t kClearData = 0x02003860u, kTransfer = 0x020036D8u,
                            kBeforeReunion = 0x02005350u;
    b->write16(kClearData, 0);
    b->write16(kTransfer, 0);
    b->write16(kBeforeReunion, 0);
    for (const sf::Entry& e : all) {
        if (e[0x31] != 0) b->write16(kClearData, 1);
        if (e[0x32] != 0) b->write16(kTransfer, 1);
        if (e[0x1C] != 0 && e[0x33] == 0) b->write16(kBeforeReunion, 1);
    }
}

// Load/write/erase: a row's file on the cartridge gets its own slot; a file
// in the extension is answered here.
void slot_call(std::uint32_t pc) {
    const std::uint32_t slot = g_cpu.R[0];
    if (slot >= static_cast<std::uint32_t>(sf::kRows)) return;
    const int file = sf::row_file(game(), static_cast<int>(slot));
    if (file < sf::kRows) {
        g_cpu.R[0] = static_cast<std::uint32_t>(file);
        return;
    }
    const sf::RecordCall op = pc == TLA_ROM(0x08015E44u) ? sf::RecordCall::Load
                            : pc == TLA_ROM(0x08015E8Cu) ? sf::RecordCall::Erase
                                                         : sf::RecordCall::Write;
    // Numbered as its row's slot and summed whole, as 0x08015CA4 writes it.
    sf::answer_record(pc, op, record(file), kRecordBytes, kRecordBytes - sf::kRecordHeader,
                      static_cast<std::uint8_t>(file % sf::kRows),
                      sf::workspace(game()) + 0x3Cu);
}

// The Autosave (shared_game/autosave.h): the field Save's own steps
// (0x080436E8.., 0x080435FC..): header 0x08043358, map objects 0x080C8628
// (veneer to 0x080CAD9C, DMA to 0x02001024..), then 0x08042F10(slot,
// 0x02000000), whose flash write is ours for file 9. Neither step reads the
// flash workspace, so the record is filled here directly.
void save_autosave() {
    if (!sf::extension(game())) return;
    sf::call(TLA_ROM(0x08043358u));
    sf::call(TLA_ROM(0x080C8628u));
    sf::fill_record(record(sf::kAutosaveFile), kRecordBytes, kRecordBytes - sf::kRecordHeader,
                    static_cast<std::uint8_t>(sf::kAutosaveFile % sf::kRows), 0x02000000u);
}

std::uint16_t (*next_keys)(std::uint16_t) = nullptr;
std::uint16_t keys(std::uint16_t value) {
    value = sf::keyinput(value);
    return next_keys ? next_keys(value) : value;
}

int (*next_branch)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;
int branch(std::uint32_t pc, std::uint32_t original, std::uint32_t* out_decision) {
    if (const int r = sf::branch(game(), pc, original, out_decision); r >= 0) return r;
    return next_branch ? next_branch(pc, original, out_decision) : -1;
}

}  // namespace

void tla_install_save_files(gbarecomp::RunOptions& options) {
    options.save_extension_bytes = kExtensionBytes;
    // Area changes at the field button poll 0x08026E60 (radial Psynergy's
    // poll) while [0x0300006C] is set; room s16 0x02000420.
    static const gsr::autosave::Game autosave = {
        TLA_ROM(0x08026E60u), 0x0300006Cu, 0x02000420u, save_autosave};
    gsr::autosave::install(autosave);
    next_keys = options.keyinput_filter;
    options.keyinput_filter = keys;
    next_branch = options.conditional_branch_override;
    options.conditional_branch_override = branch;
}

void tla_save_files_on_entry(std::uint32_t pc) {
    if (sf::g_inner || !sf::extension(game())) return;
    if (pc == TLA_ROM(0x08015E44u) || pc == TLA_ROM(0x08015CA4u) || pc == TLA_ROM(0x08015E8Cu))
        return slot_call(pc);
    sf::on_entry(game(), pc);
}

bool tla_save_files_text(std::uint32_t text_id, std::string& out) {
    return sf::autosave_line(text_id, out);
}

bool tla_save_files_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    if (!sf::g_picker.open) return false;
    auto put = tla_menu_pixel_writer(rgb, width, height);
    // A native pixel of the shown picture (the view's margins around it).
    int ox = 0, oy = 0;
    if (const gba::GbaPpu* ppu = gbarecomp::active_ppu()) {
        ox = static_cast<int>(ppu->view_extra_left());
        oy = static_cast<int>(ppu->view_extra_top());
    }
    auto pixel = [=](int x, int y) -> std::uint8_t* {
        x += ox;
        y += oy;
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
            return nullptr;
        return rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
    };
    return sf::paint(game(), put, pixel);
}
