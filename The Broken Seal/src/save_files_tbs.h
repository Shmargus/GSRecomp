// The Broken Seal's half of nine save files (shared_game/save_files_picker.h
// runs the file screens; this is the game's description and save layer).
//
// Measured, verified ROMs (FACTS.md "Nine save files", 3 November 2026):
//   The cartridge holds three files, each two flash blocks: slot s and s + 3
//   (0x1000-byte sectors, "CAMELOT" header, 0xFF0-byte payload). Every file
//   screen reaches them through these routines, all taking a block number:
//     0x08005A78 load(block, dst)   1 = absent, 0 = loaded (sector copied to
//                                   workspace +0x40, payload to dst)
//     0x08005920 write(block, src)  0 = written
//     0x08005AC0 erase(block)       1 = absent or failed, 0 = erased
//     0x08005C68 summaries()        workspace +0x1040 + row * 0x40: sector
//                                   bytes 0..0x3F of block row, +0x38 four
//                                   bytes at +0x110 of block row + 3; returns
//                                   the number of rows with block row
//   and the screens' own questions over the three summaries:
//     0x0801F704 first empty row (Copy's destination), 999 = none
//     0x0801F730 count(non_clear)   files, less Clear Data files if asked
//     0x0801F77C clear flags        0x02002010 / 0x0200200C, count (+100
//                                   when every file is Clear Data)
//   The file box is the picker 0x08020244(row, mode): Up/Down step the row
//   with 0x080022FC(row + 2 or + 4, 3); its rows are drawn once, before the
//   loop. The localized pickers are the same 0x580 bytes but for text
//   numbers, the name's x position and German RAM, so the drawing constants
//   are read from the loaded ROM at the USA offsets.
//   The workspace is [0x03001F1C] (German 0x03001F2C).
//
// Files 1-3 stay on the cartridge, untouched; files 4-9 are sector images in
// the battery extension, so a record is exactly what the game would have
// written. The game's slot numbers are rows of the window.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "../../shared_game/autosave.h"
#include "../../shared_game/challenge_mode.h"
#include "../../shared_game/save_files_picker.h"
#include "rom_version.h"

namespace gsr::save_files::tbs {

constexpr std::size_t kRecordBytes = 0x1000;   // one flash sector
constexpr std::size_t kChecksumBytes = 0xFE8;  // 0x08005AE0: all but the last 8
constexpr int kHalves = 2;                      // blocks s and s + 3
constexpr std::size_t kExtensionBytes =
    kHeaderBytes + (kFiles - kRows) * kHalves * kRecordBytes;

inline std::uint8_t* record(int file, int half);
inline Entry extension_entry(int file);
inline void set_flags(const Entries& all);
inline bool challenge_file(int file);

inline const Game& game() {
    static const Game g = {
        kExtensionBytes,
        gs::ram(0x03001F1Cu), 0x1040u, GS_ROM(0x08005C68u), GS_ROM(0x080056CCu),
        GS_ROM(0x08005CF8u),
        GS_ROM(0x0801F704u), GS_ROM(0x0801F730u), GS_ROM(0x0801F77Cu),
        GS_ROM(0x0801E74Cu), GS_ROM(0x0801E858u), GS_ROM(0x080164ACu),
        GS_ROM(0x08016498u), GS_ROM(0x080030F8u), GS_ROM(0x080022FCu), GS_ROM(0x0801FD98u),
        // Picker offsets (USA 0x08020244 + offset; same in every revision):
        // Up/Down after 0x080022FC, after the per-move 0x08016498(window),
        // after 0x080030F8(1) each frame (bl at +0x3E2), after 0x0801FD98.
        GS_ROM(0x08020244u), 0x45Cu, 0x4DAu, 0x3A8u, 0x3E6u, 0x532u,
        9, 10,       // mode r9, window r10
        4u, 8u, 0x200u,  // [sp+4] newest; [[sp+8] + =0xEA3] = 1 after a name
        0x1AAu, 0x1B6u, 0x1C2u, 0x1D4u, 0u,
        0x1D8u, 0x1E8u, 0x1F8u, 0x1F0u,
        // The Up/Down row tests that skip a file the mode cannot use (empty,
        // Clear Data for Continue, ...): config/<lang>/main.toml overrides.
        {0x40Au, 0x420u, 0x436u, 0x44Eu, 0x488u, 0x49Eu, 0x4B4u, 0x4CCu},
        extension_entry, set_flags, challenge_file,
        // The party leader's name (save_files_picker.h show_leaders):
        // records payload +0x500, 0x14C each (lookup 0x08077394); a file's
        // first block, one sector.
        0x500u, 0x14Cu, kRecordBytes,
        [](int file) -> const std::uint8_t* { return record(file, 0); },
    };
    return g;
}

// Challenge Mode's bytes are RAM 0x02000028 (challenge_mode.h), so sector
// +0x38 of a file's first block. On the cartridge the newest copy of the
// block wins (generation +0xA, as the save layer's own table keeps it).
constexpr std::size_t kChallengeSectorOffset = kRecordHeader + 0x28;
inline bool challenge_file(int file) {
    const std::uint8_t* bytes = nullptr;
    std::vector<std::uint8_t> flash;
    if (file >= kRows) {
        const std::uint8_t* r = record(file, 0);
        if (valid(r, kChecksumBytes)) bytes = r + kChallengeSectorOffset;
    } else if (gba::GbaBus* b = bus(); b && b->save().flash_enabled()) {
        flash = b->save().flash_bytes();
        int best = -1;
        for (std::size_t s = 0; s + kRecordBytes <= flash.size(); s += kRecordBytes) {
            const std::uint8_t* sector = flash.data() + s;
            if (!present(sector) || sector[7] != file) continue;
            const int generation = sector[0xA] | (sector[0xB] << 8);
            if (generation > best) {
                best = generation;
                bytes = sector + kChallengeSectorOffset;
            }
        }
    }
    return bytes && gsr::challenge::decode(bytes).size > 0;
}

inline std::uint8_t* record(int file, int half) {
    return extension(game()) + kHeaderBytes +
           ((file - kRows) * kHalves + half) * kRecordBytes;
}

// A summary as 0x08005C68 builds it, for a file in the extension.
inline Entry extension_entry(int file) {
    Entry e{};
    const std::uint8_t* first = record(file, 0);
    const std::uint8_t* second = record(file, 1);
    if (valid(first, kChecksumBytes)) std::memcpy(e.data(), first, kSummary);
    std::memset(e.data() + 0x38, 0, 4);
    if (valid(second, kChecksumBytes)) std::memcpy(e.data() + 0x38, second + 0x110, 4);
    return e;
}

// 0x0801F77C's flags over nine files.
inline void set_flags(const Entries& all) {
    gba::GbaBus* b = bus();
    constexpr std::uint32_t kClearData = 0x02002010u, kTransfer = 0x0200200Cu;
    b->write16(kClearData, 0);
    b->write16(kTransfer, 0);
    for (const Entry& e : all) {
        if (e[0x31] != 0) b->write16(kClearData, 1);
        if (e[0x32] != 0) b->write16(kTransfer, 1);
    }
    if ((b->read32(gs::ram(0x03001AE8u)) & 0x120u) != 0x120u) b->write16(kClearData, 0);
}

// Block load/write/erase: a row's file on the cartridge gets its own block
// number; a file in the extension is answered here.
inline void block(std::uint32_t pc) {
    const std::uint32_t number = g_cpu.R[0];
    if (number >= kRows * kHalves) return;
    const int file = row_file(game(), static_cast<int>(number % kRows));
    const int half = static_cast<int>(number / kRows);
    if (file < kRows) {
        g_cpu.R[0] = static_cast<std::uint32_t>(file + kRows * half);
        return;
    }
    const RecordCall op = pc == GS_ROM(0x08005A78u) ? RecordCall::Load
                        : pc == GS_ROM(0x08005AC0u) ? RecordCall::Erase : RecordCall::Write;
    // Numbered as its row's block, as 0x08005920 writes it.
    answer_record(pc, op, record(file, half), kRecordBytes, kChecksumBytes,
                  static_cast<std::uint8_t>(file % kRows + kRows * half),
                  workspace(game()) + 0x40);
}

// The Autosave (shared_game/autosave.h): the field Save's own steps (save
// routine 0x080207C4, FACTS "Seamless mode: our own save"): header
// 0x0801F818, map objects 0x0808A5B0, then blocks from 0x02000000 and
// 0x02001000. Neither step reads the flash workspace, so the record is
// filled here directly, numbered as its row's blocks as 0x08005920 numbers
// them. The flash scan, sounds and windows around them are the menu's.
inline void save_autosave() {
    if (!extension(game())) return;
    call(GS_ROM(0x0801F818u));
    call(GS_ROM(0x0808A5B0u));
    for (int half = 0; half < kHalves; ++half)
        fill_record(record(kAutosaveFile, half), kRecordBytes, kChecksumBytes,
                    static_cast<std::uint8_t>(kAutosaveFile % kRows + kRows * half),
                    0x02000000u + static_cast<std::uint32_t>(half) * 0x1000u);
}

// Area changes are noticed at the field button poll 0x0800EAF8 (radial
// Psynergy's poll), while [0x03001EBC] is set; room s16 0x02000400.
inline void install_autosave() {
    static const gsr::autosave::Game g = {
        GS_ROM(0x0800EAF8u), gs::ram(0x03001EBCu), 0x02000400u, save_autosave};
    gsr::autosave::install(g);
}

// The runner's hooks.
// The file the game's slot (a row of the box) stores.
inline int slot_file(int row) { return row_file(game(), row); }
inline void on_entry(std::uint32_t pc) {
    if (g_inner || !extension(game())) return;
    if (pc == GS_ROM(0x08005A78u) || pc == GS_ROM(0x08005920u) || pc == GS_ROM(0x08005AC0u))
        return block(pc);
    save_files::on_entry(game(), pc);
}
inline int branch(std::uint32_t pc, std::uint32_t original, std::uint32_t* out_decision) {
    return save_files::branch(game(), pc, original, out_decision);
}
using save_files::keyinput;
using save_files::g_picker;
template <class Put, class Pixel>
void paint(Put& put, Pixel& pixel) { save_files::paint(game(), put, pixel); }

}  // namespace gsr::save_files::tbs
