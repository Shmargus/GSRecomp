// Golden Sun: The Lost Age -- battle speed-up: Select in battle toggles 2x.
//
// Same design as Golden Sun 1's, re-measured on this ROM (FACTS.md,
// 2026-10-03). The frame wait 0x080134B0 has two paths, chosen by the byte
// 0x0300113A (`beq` at 0x080134B8):
//   0 (taken)  clears bit 0 of the VBlank flag 0x0300121C (mask 0xFFFE),
//              runs the sound engine (0x081C0088, 0x08000630 / mixer
//              0x03000D40) and spins until the VBlank handler sets the bit;
//   else       clears the bit (mask built by `adds r2,#0xfe` at
//              0x080134C2) and spins, without the sound engine.
// On every other battle frame the runner sends the wait down the second
// path with the mask 0xFFFF: the bit the last VBlank set stays set, the
// spin falls straight through and the next frame's work starts at once.
// Two battle frames then run per VBlank, and the sound engine still runs
// once per VBlank, so music keeps its tempo.
//
// Better Field Psy (settings page two) runs a field Psynergy cast the same
// way, outside battle, while a field Psynergy dispatcher is running (see
// dispatcher_on_stack; FACTS.md, 2026-10-08).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "frame_history.h"
#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kWaitPathBranchPc = 0x080134B8u;
constexpr uint32_t kWaitShortMaskPc = 0x080134C2u;
constexpr uint32_t kShortMaskLowByte = 0xFEu;   // 0xFF00 + 0xFE = 0xFFFE
constexpr uint32_t kKeepFlagLowByte = 0xFFu;    // 0xFFFF
constexpr uint32_t kVBlankFlag = 0x0300121Cu;

// Allocation slot 0xE0: the field Psynergy cast's object (below).
constexpr uint32_t kFieldCastSlot = 0x030000E0u;

// The cast blocks inside a field Psynergy dispatcher, called from
// 0x080CE61C: 0x080DB670 (bl at 0x080CEA6E) or 0x080DB4B8 (bl at
// 0x080CEA74), chosen by flags 0x140/0x141; both read the object in slot
// 0xE0 and switch on its kind at +0x1E. So the return into 0x080CE61C sits
// in a live stack frame for as long as the cast runs (F12 0054-0057, 0059:
// 0x080CEA73 at 0x03007E74). After the dispatcher returns, 0x080CE61C runs
// the target's reaction (0x080CE458 / 0x080CEAFC). Slot 0xE0 alone
// outlives the cast into that reaction (a measured run: set until
// frame 8484, Kraden's box from 8439; the box stays on screen at 2x) and is
// set in the Djinn tutorial (0062/0063, no dispatcher frame).
constexpr uint32_t kDispatcherReturns[] = {0x080CEA73u, 0x080CEA79u};
constexpr uint32_t kStackTop = 0x03007F00u;

bool dispatcher_on_stack(gba::GbaBus* b) {
    const uint32_t sp = g_cpu.R[13] & ~3u;
    if (sp < 0x03007000u || sp >= kStackTop) return false;
    for (uint32_t a = sp; a < kStackTop; a += 4u)
        for (const uint32_t r : kDispatcherReturns)
            if (b->read32(a) == r) return true;
    return false;
}

constexpr uint32_t kKeyInput = 0x04000130u;
constexpr uint16_t kKeySelect = 0x0004u;

// Two battle frames must fit in one VBlank period of guest time, or the
// VBlank handler commits a half-built second frame (Golden Sun 1, flicker
// at 1x). While 2x runs the guest CPU gets at least this overclock.
constexpr unsigned kMinOverclock = 10u;

std::atomic<bool> g_on{false};
std::atomic<bool> g_field_fast{false};
bool g_was_casting = false;
bool g_select_held = false;
bool g_was_in_battle = false;
int64_t g_arena_front = -1;
int64_t g_arena_pending = -1;
int64_t g_arena_restored = -1;
bool g_skip_this_pass = false;
bool g_drop_armed = false;
std::atomic<unsigned long long> g_seen_frame{0};
unsigned g_saved_overclock_q8 = 0;

gba::GbaBus* bus() { return gbarecomp::active_bus(); }

constexpr const char* kOptionsKey = gbarecomp::kOptionBattleSpeed;
constexpr const char* kFieldOptionsKey = gbarecomp::kOptionFieldPsynergy;

void save_options() { gbarecomp::game_option_set(kOptionsKey, g_on.load() ? 1 : 0); }

void hold_overclock(bool hold) {
    constexpr unsigned kQ8 = 256u;
    const unsigned minimum = kMinOverclock * kQ8;
    const unsigned current = runtime_get_overclock_factor_q8();
    if (hold) {
        if (g_saved_overclock_q8 == 0u && current < minimum) {
            g_saved_overclock_q8 = current;
            runtime_set_overclock_factor_q8(minimum);
        }
    } else if (g_saved_overclock_q8 != 0u) {
        if (current == minimum) runtime_set_overclock_factor_q8(g_saved_overclock_q8);
        g_saved_overclock_q8 = 0u;
    }
}

// The arena's BG2CNT line table (FACTS.md, 2026-10-04): block at
// [0x03000028], index word at +0, tables at block + 0x20 + index * 0x140.
// At VBlank 0x08125C1E points HBlank DMA0 at the table the index names; the
// game fills the other one and flips the index once per frame of work. With
// two frames per VBlank the second fills the table on screen (a misplaced
// band near the top while the camera moves). So on every skipped wait the index is put
// back to the one the last VBlank used, and before the next real wait the
// game's own choice is handed back if nothing flipped it since: that table
// is the complete one. Golden Sun 1's fix for the same scheme.
constexpr uint32_t kArenaBlockSlot = 0x03000028u;

uint32_t arena_index_address(gba::GbaBus* b) {
    const uint32_t block = b->read32(kArenaBlockSlot);
    const bool ram = (block >= 0x02000000u && block < 0x02040000u) ||
                     (block >= 0x03000000u && block < 0x03008000u);
    return ram && (block & 3u) == 0u ? block : 0u;
}

void keep_arena_table(gba::GbaBus* b) {
    const uint32_t index = arena_index_address(b);
    if (!index) {
        g_arena_front = g_arena_pending = -1;
        return;
    }
    if (!g_skip_this_pass) {
        // This wait halts; the VBlank that follows scans out this index.
        if (g_arena_pending >= 0 &&
            b->read32(index) == static_cast<uint32_t>(g_arena_restored))
            b->write32(index, static_cast<uint32_t>(g_arena_pending));
        g_arena_pending = -1;
        g_arena_front = b->read32(index);
        return;
    }
    if (g_arena_front >= 0 &&
        b->read32(index) != static_cast<uint32_t>(g_arena_front)) {
        g_arena_pending = b->read32(index);
        g_arena_restored = g_arena_front;
        b->write32(index, static_cast<uint32_t>(g_arena_front));
    }
}

// The OAM flush reloc_18b4 (ROM 0x080018B4) writes this frame's N entries to
// the shadow [0x03000050], parks the slots from N up to the last frame's
// count (halfword 0x0300124A, in words) at (192, 192), then stores the copy
// length max(last, 2N) at 0x03001248 and 2N as the new last; the VBlank
// handler DMAs only that length to OAM (0x08013A04). On a skipped wait the
// frame's copy never happens, so the next frame would park only from its N
// up to this one's: slots the screen still shows past both stayed there
// (Better Field Psy, F12 0076: Move's hand pieces and steam puffs left in
// OAM slots 81-94; FACTS.md, 2026-10-08). So the next frame parks up to
// this one's copy length, which already covers what the screen shows.
constexpr uint32_t kOamCopyWords = 0x03001248u;
constexpr uint32_t kOamLastWords = 0x0300124Au;

void keep_oam_parking(gba::GbaBus* b) {
    b->write16(kOamLastWords, b->read16(kOamCopyWords));
}

// The field canvas uploader (0x080EB594, VBlank task group 12) runs once
// per real VBlank, while a fast cast runs two logic passes. Command 3 copies
// its 4096-byte canvas, then 0x080EB51C fades it through the ROM's table.
// Retire that fade on a skipped presentation too; leave the upload pending
// for the next real VBlank. Otherwise Cyclone's final 16 fade requests get
// only eight fades, and its last ring stays visible until task removal
// (0114 f18..66). No artwork needs publishing for the skipped picture.
void advance_skipped_field_fade(gba::GbaBus* b) {
    constexpr uint32_t kCanvasSlot = 0x03000060u;
    constexpr uint32_t kEffectSlot = 0x030000F0u;
    constexpr uint32_t kCanvasBytes = 0x1000u;
    constexpr uint32_t kFadeTable = 0x080F3BDCu;
    const auto ram_contains = [](uint32_t at, uint32_t bytes) {
        return (at >= 0x02000000u && at <= 0x02040000u - bytes) ||
               (at >= 0x03000000u && at <= 0x03008000u - bytes);
    };
    const uint32_t effect = b->read32(kEffectSlot);
    const uint32_t canvas = b->read32(kCanvasSlot);
    if (!ram_contains(effect, 0xC4u) || !ram_contains(canvas, kCanvasBytes) ||
        b->read8(effect + 0xC1u) != 3u ||
        b->rom_size() < kFadeTable - 0x08000000u + 32u)
        return;
    for (uint32_t i = 0; i < kCanvasBytes; ++i) {
        const uint8_t index = b->read8(canvas + i);
        if (index) b->write8(canvas + i, b->read8(kFadeTable + (index & 31u)));
    }
}

// The battle entry is paced by the display: while it runs the game keeps a
// VCount interrupt at line 32 (IE bit 2, DISPSTAT bit 5, LYC 0x20) and opens
// BG1's black entry stripes in step with it. Two frames per VBlank left every
// eighth row of the arena black for the rest of the battle (FACTS.md,
// 2026-10-04). So 2x waits for the entry to end, as Golden Sun 1's does.
constexpr uint32_t kIe = 0x04000200u;
constexpr uint16_t kIeVCount = 0x0004u;
constexpr uint32_t kDispstat = 0x04000004u;
constexpr uint16_t kDispstatVCountIrq = 0x0020u;
constexpr uint16_t kEntryVCountLine = 0x20u;

bool entry_running(gba::GbaBus* b) {
    const uint16_t dispstat = b->read16(kDispstat);
    return (b->read16(kIe) & kIeVCount) != 0u &&
           (dispstat & kDispstatVCountIrq) != 0u &&
           (dispstat >> 8) == kEntryVCountLine;
}

// Once per frame wait: reads Select and decides whether this pass skips.
void decide_pass() {
    gba::GbaBus* b = bus();
    if (!b) return;
    const bool in_battle = b->read32(kTlaBattleSlot) != 0u;
    if (in_battle != g_was_in_battle) {
        g_was_in_battle = in_battle;
        std::fprintf(stderr, "[battle-speed] battle %s frame=%llu\n",
                     in_battle ? "start" : "end", runtime_current_frame());
    }
    const bool select = (~b->read16(kKeyInput) & kKeySelect) != 0u;
    if (in_battle && select && !g_select_held) {
        g_on.store(!g_on.load());
        save_options();
        std::fprintf(stderr, "[battle-speed] %s frame=%llu\n",
                     g_on.load() ? "2x" : "1x", runtime_current_frame());
    }
    g_select_held = select;
    const bool battle_2x = in_battle && g_on.load();
    const bool field_2x = !in_battle && g_field_fast.load() &&
                          b->read32(kFieldCastSlot) != 0u && dispatcher_on_stack(b);
    if (field_2x != g_was_casting) {
        g_was_casting = field_2x;
        std::fprintf(stderr, "[field-psy-fast] %s frame=%llu\n",
                     field_2x ? "on" : "off", runtime_current_frame());
    }
    gba::g_hblank_dma_latch.store(battle_2x, std::memory_order_relaxed);
    gba::g_hblank_dma_cpu_write_wins.store(battle_2x, std::memory_order_relaxed);
    if (!battle_2x) g_arena_front = g_arena_pending = -1;
    if (!battle_2x && !field_2x) {
        g_skip_this_pass = false;
        hold_overclock(false);
        return;
    }
    hold_overclock(true);
    if (battle_2x) g_seen_frame.store(runtime_current_frame());
    // Only a pass whose VBlank bit is already set can fall through.
    const bool flag_set = (b->read16(kVBlankFlag) & 1u) != 0u;
    g_skip_this_pass = !g_skip_this_pass && flag_set &&
                       !(battle_2x && entry_running(b));
    if (g_skip_this_pass) keep_oam_parking(b);
    if (g_skip_this_pass && field_2x) advance_skipped_field_fade(b);
    if (battle_2x) keep_arena_table(b);
}

// Reveal's range (0x080DD754, run every frame while Reveal is up; FACTS.md,
// 2026-10-15), The Broken Seal's routine on this ROM: with S = [0x0300006C],
// countdown S+0xCA6 (600 at the cast), cast spot S+0xCA8/+0xCAA; r2 =
// dx*dx + dy*dy (dx scaled by 0xD105 >> 16) against r1 = 0xE10 (60 px
// squared, built by movs/lsls); `bge` 0x080DD7F4 then stores 0x2090 to
// S+0x166, which ends Reveal (as does the countdown reaching 0). Better
// Field Psy doubles the distance, as in The Broken Seal.
constexpr uint32_t kRevealRangeBranchPc = 0x080DD7F4u;
constexpr uint32_t kRevealRangeSquared = 0xE10u;

int battle_speed_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    if (pc == kRevealRangeBranchPc) {
        if (!g_field_fast.load() || g_cpu.R[1] != kRevealRangeSquared) return 0;
        *out_decision = static_cast<int32_t>(g_cpu.R[2]) >=
                        static_cast<int32_t>(4u * kRevealRangeSquared) ? 1u : 0u;
        return *out_decision != original;
    }
    if (pc != kWaitPathBranchPc) return 0;
    decide_pass();
    g_drop_armed = g_skip_this_pass;
    if (!g_skip_this_pass) return 0;
    *out_decision = 0u;  // not taken: the short path, no sound engine
    return original != 0u;
}

int (*g_next_alu_immediate)(uint32_t, uint32_t, uint32_t*) = nullptr;

int battle_speed_alu_immediate(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc == kWaitShortMaskPc && original == kShortMaskLowByte) {
        if (!g_drop_armed) return 0;
        g_drop_armed = false;
        *out_value = kKeepFlagLowByte;
        return 1;
    }
    return g_next_alu_immediate ? g_next_alu_immediate(pc, original, out_value) : 0;
}

bool indicator_visible() {
    // Within a frame of the last battle pass at 2x; a loaded state moves the
    // frame counter back, so not before it either.
    const unsigned long long now = runtime_current_frame(), seen = g_seen_frame.load();
    return g_on.load() && now >= seen && now <= seen + 1u;
}

}  // namespace

// run_game clears the branch and immediate hooks and installs the ones in
// RunOptions; this chains in front of whatever immediate hook is there.
void tla_install_battle_speed(gbarecomp::RunOptions& options) {
    g_on.store(gbarecomp::game_option_get(kOptionsKey, 0) != 0);
    g_field_fast.store(gbarecomp::game_option_get(kFieldOptionsKey, 0) != 0);
    g_next_alu_immediate = options.thumb_alu_immediate_override;
    options.thumb_alu_immediate_override = battle_speed_alu_immediate;
    options.conditional_branch_override = battle_speed_branch;
}

// Called by the present hook (src/tla_rewind.cpp): ">>" at the native top
// left while 2x runs, the two chevrons touching.
bool tla_battle_speed_paint(uint8_t* rgb, uint32_t width, uint32_t height) {
    if (!indicator_visible()) return false;
    tla_paint_text(rgb, width, height, 4, 4, ">>", 5);
    return true;
}

bool tla_battle_speed_active() { return indicator_visible(); }

// A spell's setup (stores of 0 to BG2X/BG2Y at e.g. 0x0813E86A, 0x0813FEFA,
// 0x0814356C) tears one frame when it runs before row 0 (F12 0088 frame 97,
// 0089 frames 31 and 99; FACTS.md, 2026-10-08); the test is shared with The
// Broken Seal (gbarecomp::spell_setup_torn_frame). Such a frame repeats the
// last picture. At any speed, not only 2x.
bool tla_battle_spell_setup_frame() {
    gba::GbaBus* b = bus();
    const gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!b || !ppu || b->read32(kTlaBattleSlot) == 0u) return false;
    return gbarecomp::spell_setup_torn_frame(*ppu);
}

// Better Field Psy (settings page two).
bool tla_field_psy_fast() { return g_field_fast.load(); }

void tla_set_field_psy_fast(bool on) {
    g_field_fast.store(on);
    gbarecomp::game_option_set(kFieldOptionsKey, on ? 1 : 0);
}
