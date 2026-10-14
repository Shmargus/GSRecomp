#pragma once

// Diagnostic "unpacker catcher" (env GSR_UNPACKER_CATCH): on for players
// (both release launchers), a test toggle "Catch unpacker faults" in the
// developer launcher. While Golden Sun's data unpacker (Func_2808, copied to
// 0x03006000..0x030064EC) is resident, the first time our engine falls back
// to the interpreter at a pc inside it, this saves the moment before anything
// runs, in the game's working folder: unpacker_catch.txt (registers, the last
// dispatches and which were pause resumes, the stack, the host call chain)
// and unpacker_catch.bin (crash_memory.bin format). The launchers delete the
// pair before every start and put it in the bug report, so at most one pair
// exists. The 2026-10-07 crash leaving Sol Sanctum began with exactly such a
// fallback (FACTS.md "0.4 player crash: the unpacker returned into compressed
// data"). Does nothing when unset; catches once per session.
namespace gsr {

void unpacker_catch_install();

}  // namespace gsr
