#pragma once

#include <cstdint>
#include <string>

// Safety net and journal for the data unpacker crash (FACTS.md "Two 0.4.1
// Steam Deck crashes: one unpopped push {r5,sb}"). Always on. Both games:
// The Lost Age's ROM holds the same routine byte for byte (0x08012B74, copied
// behind a Thumb `bx pc` stub to wherever its allocator puts it; 0x03005300
// in every TLA session logged so far, FACTS.md 2026-10-14), so this file is
// compiled into both runners and keys on the routine's own offsets, with
// each game naming where the routine sits in its ROM.
//
// Golden Sun's Func_5394 copies Func_2808 to 0x03006000 and calls it with a
// BL to a `bx r6` veneer. Some sessions leave one of its `push {r5,sb}`
// frames on the stack, so its final `pop {r5-fp,lr}` loads a saved register
// as the return address and the game jumps into data and aborts. This keeps
// the unpacker's starting state at each entry; when the unpacker's return
// matches no live call and lands outside it with exactly that stack shape, it
// puts the starting state back, redoes the whole unpack on the reference
// interpreter, and returns to the caller as the console would have. Each
// recovery is logged ([unpacker-guard]), written to unpacker_recovered.txt
// (with unpacker_recovered.bin, the memory as found, for the first one) in
// the game's working folder, and saves the rewind frames 30 frames later.
// The launchers delete the pair before every start, add it to the bug report
// and ask for the report to be sent.
//
// While the unpacker runs it also journals every dispatch inside it: which
// resolver path answered, whether the function equals the unpacker's own
// generated entry, and r1/sp/sb, frozen at the first dispatch whose sp is
// neither of the two the unpacker can have. That journal is the evidence for
// the real fix. That first impossible sp is also the earliest sign of the
// fault (the next table jump after the lost byte): the same redo runs there,
// before the bad run can write past its output into its own code (FACTS.md
// "0.4.3 player crash: same byte 0x3E, third data set").
//
// Two launcher test options (off in normal play):
// - GSR_UNPACKER_FORCE_FAULT ("Force unpacker fault"): once per unpack, after
//   output byte 0x3E, leaves one extra 8-byte frame on the stack inside a
//   literal, the state every crash report shows (one unpopped push {r5,sb},
//   sb left at a scratch value), so the safety net can be tested in play.
// - GSR_UNPACKER_AUDIT ("Unpacker audit"): every unpack that returns normally
//   is run again from ROM in a sandbox (player_builder/guest_call.h) and its
//   output compared; every dispatch is kept with all registers. The first
//   mismatch or catch writes unpacker_journal.csv beside the other files.
namespace gsr {

using GuardGuestFn = void (*)(void);

// Resolver paths for unpacker_guard_note_dispatch (runner_main.cpp sets them
// in verified_ram_dispatch).
enum class RamResolvePath : char {
    None = '?',
    CacheNative = 'C',
    CacheStatic = 'c',
    IdentityNative = 'I',
    IdentityStatic = 'i',
    Relocatable = 'R',
    Stamp = 'S',
    Dynamic = 'D',
};

inline bool g_unpacker_guard_pending = false;

// `rom_address`: Func_2808 in this game's ROM (the bytes a resident copy
// must equal).
void unpacker_guard_install(std::uint32_t rom_address);

// The RAM dispatch hook, for every ARM dispatch it handles. note_entry starts
// a watch when `pc` holds a resident copy of the unpacker (its first word is
// checked first, so any pc may be passed). note_dispatch returns true when it
// redid the unpack: g_cpu is then at the caller and the dispatch must run
// nothing.
void unpacker_guard_note_entry(std::uint32_t pc);
bool unpacker_guard_note_dispatch(std::uint32_t pc, GuardGuestFn fn,
                                  GuardGuestFn expected, RamResolvePath path);
// The call-return hook (verified_ram_dispatch_return_hook).
void unpacker_guard_note_return(std::uint32_t return_pc);

// Frame loop: true once per recovery, 30 frames after it, with the note for
// the rewind capture.
bool unpacker_guard_capture_due(std::string* note);

}  // namespace gsr
