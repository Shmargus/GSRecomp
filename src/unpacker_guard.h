#pragma once

#include <cstdint>
#include <string>

// Safety net and journal for the data unpacker crash (FACTS.md "Two 0.4.1
// Steam Deck crashes: one unpopped push {r5,sb}"). Always on.
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
// the real fix.
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

void unpacker_guard_install();

// verified_ram_dispatch, for every ARM dispatch it handles.
void unpacker_guard_note_entry(std::uint32_t pc);
void unpacker_guard_note_dispatch(std::uint32_t pc, GuardGuestFn fn,
                                  GuardGuestFn expected, RamResolvePath path);
// The call-return hook (verified_ram_dispatch_return_hook).
void unpacker_guard_note_return(std::uint32_t return_pc);

// Frame loop: true once per recovery, 30 frames after it, with the note for
// the rewind capture.
bool unpacker_guard_capture_due(std::string* note);

}  // namespace gsr
