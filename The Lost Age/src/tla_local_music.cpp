// Golden Sun: The Lost Age -- Music mod: the album recordings in Music/TLA
// (and Music/TBS for songs carried over from The Broken Seal) play in place
// of the game's songs (launcher cog, its first-run question, F1 > Audio).
//
// The Lost Age's m4a driver is a modified copy of Golden Sun 1's, so the
// shared policy (engine/src/runtime/local_music_m4a.h) applies with this
// ROM's own addresses (FACTS.md, 2026-10-24 "Local music"):
// - song table 0x081C4530, player table 0x081C44D0; music is player 1,
//   info 0x02006A90, ten 0x50-byte tracks at 0x02006CE0 (literal pool at
//   0x081C1FE4/0x081C1FE8; song table entries for BGM name player 1);
// - MPlayOpen stores 0x081C09D1 as the main routine (literal 0x081C26F0), so
//   0x081C09D0 runs once a frame with R0 = info (GS1's routine is also in
//   the ROM, at 0x081C192C, but nothing calls it);
// - MPlayStart 0x081C26F4 stores the accepted header with `str r7,[r5]` at
//   0x081C2740;
// - the gain helper 0x081C077A (called from 0x081C0962 and 0x081C0B9A with
//   the channel in R4 and the track in R5) saturates left gain with
//   `cmp r0,#0xff; bgt 0x081C0772` -> `movs r0,#0xff` and right gain with
//   `bgt 0x081C0776` at 0x081C079C. Taking the branch and storing 0 there
//   silences a voice (config/tla/main.toml).

#include <cstdint>

#include "local_music_m4a.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "tla.h"
#include "tla_local_music_tracks.h"

namespace {

constexpr gbarecomp::m4a_music::Player kPlayer{
    0x02006A90u, 0x02006CE0u, 10u, 0x50u,
    0x081C09D0u,                     // main routine
    0x081C2740u,                     // MPlayStart's accepted header store
    {0x081C078Eu, 1u, 0x081C0772u},  // left: BGT taken reaches MOVS #255
    {0x081C079Cu, 1u, 0x081C0776u}}; // right
gbarecomp::m4a_music::Replacement g_music{kPlayer, tla_local_music::recordings.data(),
                                          tla_local_music::recordings.size()};

int (*g_next_branch)(uint32_t, uint32_t, uint32_t*) = nullptr;
int (*g_next_alu_immediate)(uint32_t, uint32_t, uint32_t*) = nullptr;
RuntimeMemWriteOverrideHook g_next_write_override = nullptr;

int music_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    if (g_music.branch(pc, g_cpu.R[5], out_decision)) return 1;
    return g_next_branch ? g_next_branch(pc, original, out_decision) : 0;
}

int music_alu_immediate(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (g_music.immediate(pc, g_cpu.R[5], out_value)) return 1;
    return g_next_alu_immediate ? g_next_alu_immediate(pc, original, out_value) : 0;
}

// Only observes the header store; the write itself is unchanged.
int music_write(uint32_t pc, uint32_t addr, uint32_t requested, uint32_t width,
                uint32_t* out_value) {
    g_music.header_write(pc, addr, requested, width);
    return g_next_write_override
               ? g_next_write_override(pc, addr, requested, width, out_value)
               : 0;
}

}  // namespace

void tla_local_music_on_entry(uint32_t pc) { g_music.on_entry(pc); }

void tla_local_music_on_savestate_load() { g_music.update(true); }

// Chains in front of the hooks already in RunOptions and the write override.
// Always installed: F1 > Audio can switch the Music mod on mid-game.
void tla_install_local_music(gbarecomp::RunOptions& options) {
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = music_branch;
    g_next_alu_immediate = options.thumb_alu_immediate_override;
    options.thumb_alu_immediate_override = music_alu_immediate;
    g_next_write_override = g_runtime_mem_write_override;
    g_runtime_mem_write_override = music_write;
}
