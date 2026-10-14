// See unpacker_guard.h.

#include "unpacker_guard.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "gba_bus.h"
#include "guest_call.h"
#include "memory_snapshot.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" std::uint32_t g_irq_nest_depth;
extern "C" unsigned long long g_runtime_irq_entries;
extern "C" unsigned long long g_runtime_vblank_starts;
extern "C" std::uint32_t runtime_recent_dispatch_copy(std::uint32_t* pcs,
                                                      std::uint8_t* resumes,
                                                      std::uint32_t max);

namespace gsr {
namespace {

// Func_2808 (FACTS.md 2026-10-07/08), as offsets from wherever it was
// copied: TBS 0x03006000 (Func_5394), TLA its allocator's block + 4.
constexpr std::uint32_t kUnpackerSize = 0x4ECu;
// Every return target inside the unpacker lies at or above its prologue;
// +0x00..+0x0F is its entry-only format check.
constexpr std::uint32_t kBody = 0x10u;
// The main loop head, after a literal's `pop {r5,sb}`: only the prologue's
// frame is on the stack here.
constexpr std::uint32_t kLoopHead = 0x108u;
// The final `bx lr`, after `pop {r5-fp,lr}`: the stack is back at entry.
constexpr std::uint32_t kExit = 0x2FCu;
// Every reported crash went wrong at output byte 0x3E (FACTS.md); the forced
// fault waits for it too.
constexpr std::uint32_t kForceAfter = 0x3Eu;
// The audit keeps every dispatch of the current unpack (31,286 in the
// longest reported one), up to this many.
constexpr std::size_t kFullJournalMax = 400'000;
constexpr const char* kJournalFile = "unpacker_journal.csv";

std::uint32_t g_rom = 0;    // Func_2808 in this game's ROM
std::uint32_t g_start = 0;  // the resident copy being watched
std::uint32_t g_end = 0;
bool g_force_fault = false;
bool g_audit = false;
bool g_forced = false;      // this unpack's fault was injected
unsigned long long g_forced_total = 0;
// The prologue pushes r5-fp and lr; a literal adds `push {r5,sb}`.
constexpr std::uint32_t kEntryFrame = 0x20u;
constexpr std::uint32_t kLiteralFrame = 0x08u;
// The correct unpack of the crash input is 141,352 instructions.
constexpr std::uint64_t kRedoBudget = 4'000'000u;
constexpr unsigned kCapturesMax = 3;
// Recoveries written out in full; the Force unpacker fault option makes one
// per unpack, and later ones are only logged.
constexpr unsigned kReportsMax = 10;
constexpr unsigned long long kCaptureAfter = 30;
constexpr std::size_t kJournal = 128;
constexpr const char* kTextFile = "unpacker_recovered.txt";
constexpr const char* kMemoryFile = "unpacker_recovered.bin";

struct Entry {
    std::uint32_t r[16] = {};
    std::uint32_t cpsr = 0;
    std::vector<std::uint32_t> call_stack;
    unsigned long long frame = 0;
    unsigned long long irq_entries = 0;
    unsigned long long vblanks = 0;
};
Entry g_entry;

struct JournalEvent {
    std::uint32_t pc = 0, r1 = 0, sp = 0, sb = 0, lr = 0;
    std::uint32_t call_depth = 0;  // runtime call-return stack
    // g_runtime_resume_pc as the resolver is asked: an alias sets it only
    // after this, so any value here was left over by an earlier entry.
    std::uint32_t stale_resume = 0;
    unsigned long long irq_entries = 0;
    unsigned long long vblanks = 0;  // a change means a frame began meanwhile
    char path = '?';
    bool own = true;      // fn equals the unpacker's generated entry for pc
    bool resumed = false;  // this dispatch resumed a scheduler yield
};

struct Journal {
    std::array<JournalEvent, kJournal> ring{};
    std::size_t next = 0, count = 0;
    unsigned long long dispatches = 0, foreign = 0;
    bool have_foreign = false;
    JournalEvent first_foreign{};
    bool frozen = false;  // at the first dispatch with an impossible sp
    std::array<JournalEvent, kJournal> frozen_ring{};
    std::size_t frozen_count = 0, frozen_next = 0;
    unsigned long long frozen_at = 0;
};
Journal g_journal;

// Audit: every dispatch of the current unpack, all registers.
struct FullEvent {
    std::uint32_t pc = 0;
    std::uint32_t r[16] = {};
    std::uint32_t cpsr = 0;
    std::uint32_t call_depth = 0;
    unsigned long long irq_entries = 0, vblanks = 0;
    char path = '?';
    bool own = true, resumed = false;
};
std::vector<FullEvent> g_full;
bool g_full_overflow = false;
bool g_journal_written = false;
unsigned long long g_audited = 0, g_audit_skipped = 0, g_audit_mismatches = 0;
std::unique_ptr<gsr::builder::GuestMachine> g_sandbox;

unsigned g_recoveries = 0;
unsigned g_captures = 0;
unsigned long long g_capture_at = 0;  // guest frame; 0 = none due
std::string g_capture_note;

bool unpacker_resident(std::uint32_t start) {
    for (std::uint32_t off = 0; off < kUnpackerSize; off += 4u) {
        if (bus_read_u32(start + off) != bus_read_u32(g_rom + off))
            return false;
    }
    return true;
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1';
}

void journal_reset() {
    g_journal = Journal{};
}

// Oldest first.
template <typename Fn>
void journal_each(const std::array<JournalEvent, kJournal>& ring, std::size_t next,
                  std::size_t count, Fn&& fn) {
    for (std::size_t i = 0; i < count; ++i)
        fn(ring[(next + kJournal - count + i) % kJournal]);
}

void print_event(std::FILE* f, const JournalEvent& e) {
    std::fprintf(f,
        "  pc=0x%08X path=%c own=%d out=0x%08X (offset 0x%X) sp=0x%08X "
        "(entry-0x%X) sb=0x%08X lr=0x%08X irqs=+%llu frames=+%llu depth=%u%s",
        e.pc, e.path, e.own ? 1 : 0, e.r1, e.r1 - g_entry.r[1] + 1u, e.sp,
        g_entry.r[13] - e.sp, e.sb, e.lr, e.irq_entries - g_entry.irq_entries,
        e.vblanks - g_entry.vblanks, e.call_depth, e.resumed ? " RESUMED" : "");
    if (e.stale_resume != 0u)
        std::fprintf(f, " STALE-RESUME=0x%08X", e.stale_resume);
    std::fputc('\n', f);
}

// The audit's journal of the current unpack, once per session (the first
// mismatch or catch), beside unpacker_recovered.txt.
void write_full_journal(const char* why) {
    if (!g_audit || g_journal_written) return;
    g_journal_written = true;
    std::FILE* f = std::fopen(kJournalFile, "wb");
    if (!f) return;
    std::fprintf(f, "# %s; unpacker at 0x%08X (ROM 0x%08X); entry frame %llu "
                    "r0=0x%08X r1=0x%08X sp=0x%08X lr=0x%08X; %zu dispatches%s\n",
                 why, g_start, g_rom, g_entry.frame, g_entry.r[0], g_entry.r[1],
                 g_entry.r[13], g_entry.r[14], g_full.size(),
                 g_full_overflow ? " (journal full, later ones dropped)" : "");
    std::fprintf(f, "n,pc,offset,path,own,resumed,depth,irqs,frames,cpsr");
    for (int r = 0; r < 16; ++r) std::fprintf(f, ",r%d", r);
    std::fputc('\n', f);
    for (std::size_t i = 0; i < g_full.size(); ++i) {
        const FullEvent& e = g_full[i];
        std::fprintf(f, "%zu,0x%08X,0x%03X,%c,%d,%d,%u,%llu,%llu,0x%08X", i, e.pc,
                     e.pc - g_start, e.path, e.own ? 1 : 0, e.resumed ? 1 : 0,
                     e.call_depth, e.irq_entries - g_entry.irq_entries,
                     e.vblanks - g_entry.vblanks, e.cpsr);
        for (int r = 0; r < 16; ++r) std::fprintf(f, ",0x%08X", e.r[r]);
        std::fputc('\n', f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[unpacker-audit] wrote %s (%zu dispatches)\n",
                 kJournalFile, g_full.size());
}

// The audit's check of an unpack that returned normally: the same input, run
// from ROM in a sandbox, must give the same length and bytes. Observes only.
void audit_return() {
    const std::uint32_t src = g_entry.r[0], dst = g_entry.r[1];
    const bool rom_source = src >= 0x08000000u && src < 0x0E000000u;
    const std::uint32_t region = dst >> 24;
    if (!rom_source || (region != 0x02u && region != 0x03u)) {
        ++g_audit_skipped;
        return;
    }
    if (!g_sandbox) {
        const gba::GbaBus* bus = gbarecomp::active_bus();
        if (!bus || !bus->rom_ptr()) {
            ++g_audit_skipped;
            return;
        }
        g_sandbox = std::make_unique<gsr::builder::GuestMachine>(
            std::vector<std::uint8_t>(bus->rom_ptr(), bus->rom_ptr() + bus->rom_size()));
    }
    g_sandbox->clear_memory();
    std::uint32_t length = 0;
    std::string error;
    const std::uint32_t args[4] = {src, dst, g_entry.r[2], g_entry.r[3]};
    const bool returned = g_sandbox->call(g_rom, args, &length, &error,
                                          g_entry.r[13], kRedoBudget);
    ++g_audited;
    const std::uint32_t live_length = g_cpu.R[0];
    const std::vector<std::uint8_t>& mem =
        region == 0x02u ? g_sandbox->ewram() : g_sandbox->iwram();
    const std::uint32_t mask = region == 0x02u ? 0x3FFFFu : 0x7FFFu;
    const std::uint32_t n = returned ? length : 0u;
    std::uint32_t first_diff = 0xFFFFFFFFu, diffs = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (bus_read_u8(dst + i) != mem[(dst + i) & mask]) {
            if (first_diff == 0xFFFFFFFFu) first_diff = i;
            ++diffs;
        }
    }
    const bool ok = returned && length == live_length && diffs == 0;
    if (!ok) ++g_audit_mismatches;
    // The first check shows the audit is live; then every 25 (a short
    // session has a few dozen unpacks: 32 in 25 s of TBS, 2026-10-14).
    if (g_audited == 1u || g_audited % 25u == 0u || !ok) {
        std::fprintf(stderr, "[unpacker-audit] %llu unpacks checked, %llu "
                             "mismatched, %llu skipped (source not in ROM or output not in RAM)\n",
                     g_audited, g_audit_mismatches, g_audit_skipped);
    }
    if (ok) return;
    std::fprintf(stderr, "[unpacker-audit] MISMATCH at frame %llu: src=0x%08X "
                         "dst=0x%08X live length 0x%X, sandbox %s length 0x%X, "
                         "%u bytes differ, first at offset 0x%X\n",
                 runtime_current_frame(), src, dst, live_length,
                 returned ? "returned with" : error.c_str(), length, diffs,
                 first_diff);
    const bool first = g_recoveries == 0u && g_audit_mismatches == 1u;
    const bool memory = first && gsr_write_memory_snapshot(kMemoryFile);
    if (std::FILE* f = std::fopen(kTextFile, first ? "wb" : "ab")) {
        if (!first) std::fprintf(f, "\n\n");
        std::fprintf(f,
            "Golden Sun Recompiled -- unpacker audit mismatch %llu (Unpacker "
            "audit test option)\n"
            "The data unpacker (0x%08X..0x%08X) returned normally, but a sandbox "
            "run of the same input from ROM 0x%08X gives different output. "
            "Nothing was changed.\n\n"
            "entry: frame=%llu src=0x%08X dst=0x%08X sp=0x%08X lr=0x%08X\n"
            "live: length 0x%X; sandbox: %s, length 0x%X\n"
            "%u bytes differ, first at offset 0x%X\n",
            g_audit_mismatches, g_start, g_end, g_rom, g_entry.frame, src, dst,
            g_entry.r[13], g_entry.r[14], live_length,
            returned ? "returned" : error.c_str(), length, diffs, first_diff);
        if (first_diff != 0xFFFFFFFFu) {
            const std::uint32_t from = first_diff & ~0xFu;
            std::fprintf(f, "bytes from offset 0x%X (live/sandbox):\n ", from);
            for (std::uint32_t i = from; i < from + 32u && i < n; ++i)
                std::fprintf(f, " %02X/%02X", bus_read_u8(dst + i),
                             mem[(dst + i) & mask]);
            std::fputc('\n', f);
        }
        if (first)
            std::fprintf(f, "\n%s: %s\n", kMemoryFile, memory ? "written" : "FAILED");
        std::fclose(f);
    }
    write_full_journal("audit mismatch");
    if (g_captures < kCapturesMax) {
        ++g_captures;
        g_capture_at = runtime_current_frame() + kCaptureAfter;
        char note[200];
        std::snprintf(note, sizeof note,
            "unpacker audit mismatch at frame %llu: dst 0x%08X, %u bytes "
            "differ from offset 0x%X (see unpacker_recovered.txt)\n",
            runtime_current_frame(), dst, diffs, first_diff);
        g_capture_note = note;
    }
}

int unmatched_return(std::uint32_t target);
int recover(std::uint32_t at, std::uint32_t frames, bool early);

}  // namespace

void unpacker_guard_install(std::uint32_t rom_address) {
    g_rom = rom_address;
    g_force_fault = env_on("GSR_UNPACKER_FORCE_FAULT");
    g_audit = env_on("GSR_UNPACKER_AUDIT");
    g_runtime_unmatched_return_hook = &unmatched_return;
    if (g_force_fault || g_audit)
        std::fprintf(stderr, "[unpacker-guard] test options: force fault %s, "
                             "audit %s (unpacker in ROM at 0x%08X)\n",
                     g_force_fault ? "ON" : "off", g_audit ? "ON" : "off", g_rom);
}

void unpacker_guard_note_entry(std::uint32_t pc) {
    if (g_irq_nest_depth != 0u || g_rom == 0u) return;
    // Cheap first: most dispatches are some other routine.
    if (bus_read_u32(pc) != bus_read_u32(g_rom)) return;
    g_unpacker_guard_pending = false;
    if (!unpacker_resident(pc)) return;
    g_start = pc;
    g_end = pc + kUnpackerSize;
    g_forced = false;
    g_full.clear();
    g_full_overflow = false;
    for (int i = 0; i < 16; ++i) g_entry.r[i] = g_cpu.R[i];
    g_entry.cpsr = g_cpu.cpsr;
    const std::uint32_t depth = runtime_call_stack_depth();
    const std::uint32_t* data = runtime_call_stack_data();
    g_entry.call_stack.assign(data, data + depth);
    g_entry.frame = runtime_current_frame();
    g_entry.irq_entries = g_runtime_irq_entries;
    g_entry.vblanks = g_runtime_vblank_starts;
    journal_reset();
    g_unpacker_guard_pending = true;
}

bool unpacker_guard_note_dispatch(std::uint32_t pc, GuardGuestFn fn,
                                  GuardGuestFn expected, RamResolvePath path) {
    if (!g_unpacker_guard_pending || g_irq_nest_depth != 0u) return false;
    if (pc < g_start || pc >= g_end) return false;
    Journal& j = g_journal;
    JournalEvent& e = j.ring[j.next];
    e.pc = pc;
    e.r1 = g_cpu.R[1];
    e.sp = g_cpu.R[13];
    e.sb = g_cpu.R[9];
    e.lr = g_cpu.R[14];
    e.irq_entries = g_runtime_irq_entries;
    e.vblanks = g_runtime_vblank_starts;
    e.call_depth = runtime_call_stack_depth();
    e.stale_resume = g_runtime_resume_pc;
    // runtime_dispatch logged this dispatch, with its resume mark, just
    // before asking the resolver; the copy is oldest first.
    std::uint32_t recent_pcs[16];
    std::uint8_t recent_resumes[16];
    const std::uint32_t recent = runtime_recent_dispatch_copy(recent_pcs, recent_resumes, 16);
    e.resumed = recent != 0u && (recent_pcs[recent - 1u] & ~1u) == pc &&
                recent_resumes[recent - 1u] != 0u;
    e.path = static_cast<char>(path);
    e.own = fn == expected;
    j.next = (j.next + 1u) % kJournal;
    if (j.count < kJournal) ++j.count;
    ++j.dispatches;
    if (!e.own) {
        ++j.foreign;
        if (!j.have_foreign) {
            j.have_foreign = true;
            j.first_foreign = e;
        }
    }
    if (g_audit) {
        if (g_full.size() < kFullJournalMax) {
            FullEvent full;
            full.pc = pc;
            for (int r = 0; r < 16; ++r) full.r[r] = g_cpu.R[r];
            full.cpsr = g_cpu.cpsr;
            full.call_depth = e.call_depth;
            full.irq_entries = e.irq_entries;
            full.vblanks = e.vblanks;
            full.path = e.path;
            full.own = e.own;
            full.resumed = e.resumed;
            g_full.push_back(full);
        } else {
            g_full_overflow = true;
        }
    }
    const std::uint32_t entry_sp = g_entry.r[13];
    // The Force unpacker fault test option: inside a literal (its
    // `push {r5,sb}` frame live), once the output is past byte 0x3E, push one
    // more frame of the current r5/sb. The literal's pop then takes this one
    // and leaves its own: sb stays at the table value and sp 8 low, the state
    // of every reported crash. Nothing is checked at this dispatch; the code
    // runs on and the guard has to find it by itself.
    if (g_force_fault && !g_forced && pc > g_start + kBody &&
        pc != g_start + kLoopHead && pc != g_start + kExit &&
        e.sp == entry_sp - kEntryFrame - kLiteralFrame &&
        e.r1 - g_entry.r[1] + 1u >= kForceAfter) {
        g_forced = true;
        ++g_forced_total;
        const std::uint32_t sp = e.sp - kLiteralFrame;
        bus_write_u32(sp, g_cpu.R[5]);
        bus_write_u32(sp + 4u, g_cpu.R[9]);
        g_cpu.R[13] = sp;
        std::fprintf(stderr, "[unpacker-guard] FORCED FAULT %llu (test option): "
                             "extra frame at dispatch 0x%08X, output offset "
                             "0x%X, sp 0x%08X -> 0x%08X\n",
                     g_forced_total, pc, e.r1 - g_entry.r[1] + 1u, e.sp, sp);
        return false;
    }
    // +0x00 (format check) and +0x10 (the prologue's push) run
    // before the entry frame exists, the final `bx lr` after it is gone.
    // Checked at every instruction of four correct unpacks under Unicorn
    // (~924,000 steps, no exception); with the fault injected (byte 0x3E's
    // strb+pop skipped) the next table jump breaks it, at output 0x3E.
    const bool possible = pc <= g_start + kBody || pc == g_start + kExit
        ? e.sp == entry_sp
        : pc == g_start + kLoopHead
        ? e.sp == entry_sp - kEntryFrame
        : e.sp == entry_sp - kEntryFrame ||
          e.sp == entry_sp - kEntryFrame - kLiteralFrame;
    if (!possible && !j.frozen) {
        j.frozen = true;
        j.frozen_ring = j.ring;
        j.frozen_count = j.count;
        j.frozen_next = j.next;
        j.frozen_at = j.dispatches;
        return recover(pc, 0u, true) != 0;
    }
    return false;
}

void unpacker_guard_note_return(std::uint32_t return_pc) {
    if (!g_unpacker_guard_pending || g_irq_nest_depth != 0u) return;
    // A real return sets R15 to its target first; a yield's cancels see the
    // 0xFFFFFFF0 sentinel instead and must not end the watch.
    if ((g_cpu.R[15] & ~1u) != return_pc) return;
    if (return_pc != (g_entry.r[14] & ~1u)) return;
    g_unpacker_guard_pending = false;
    if (g_audit) audit_return();
}

bool unpacker_guard_capture_due(std::string* note) {
    if (g_capture_at == 0 || runtime_current_frame() < g_capture_at) return false;
    g_capture_at = 0;
    *note = g_capture_note;
    return true;
}

namespace {

void write_report(std::FILE* f, std::uint32_t target, std::uint32_t frames,
                  bool early, std::uint32_t bad_out,
                  const std::uint32_t* bad_regs, std::uint32_t bad_cpsr) {
    if (early) {
        std::fprintf(f,
            "Golden Sun Recompiled -- unpacker crash caught early (recovery %u)\n"
            "The data unpacker (0x%08X..0x%08X) dispatched 0x%08X with sp "
            "0x%08X (entry sp 0x%08X), a stack it cannot have. The unpack was "
            "redone on the interpreter; result below.\n\n",
            g_recoveries, g_start, g_end, target, bad_regs[13],
            g_entry.r[13]);
    } else {
        std::fprintf(f,
            "Golden Sun Recompiled -- unpacker crash caught (recovery %u)\n"
            "The data unpacker (0x%08X..0x%08X) tried to return to 0x%08X instead "
            "of its caller 0x%08X, with %u extra stack frame(s) of 8 bytes. The "
            "unpack was redone on the interpreter; result below.\n\n",
            g_recoveries, g_start, g_end, target, g_entry.r[14] & ~1u,
            frames);
    }
    if (g_forced)
        std::fprintf(f, "This fault was FORCED by the Force unpacker fault test "
                        "option, not a real one.\n\n");
    std::fprintf(f, "entry: frame=%llu src=0x%08X dst=0x%08X sp=0x%08X lr=0x%08X "
                    "cpsr=0x%08X call_stack_depth=%zu\n",
                 g_entry.frame, g_entry.r[0], g_entry.r[1], g_entry.r[13],
                 g_entry.r[14], g_entry.cpsr, g_entry.call_stack.size());
    std::fprintf(f, "  registers:");
    for (int r = 0; r < 16; ++r) std::fprintf(f, " r%d=0x%08X", r, g_entry.r[r]);
    std::fprintf(f, "\nbad exit: frame=%llu out=0x%08X (offset 0x%X) cpsr=0x%08X "
                    "irqs since entry=%llu\n  registers:",
                 runtime_current_frame(), bad_out, bad_out - g_entry.r[1] + 1u,
                 bad_cpsr, g_runtime_irq_entries - g_entry.irq_entries);
    for (int r = 0; r < 16; ++r) std::fprintf(f, " r%d=0x%08X", r, bad_regs[r]);
    std::fprintf(f, "\n\njournal: %llu dispatches inside the unpacker, %llu not "
                    "its own generated entry (path: C/c cache, I/i identity, R "
                    "relocatable, S stamp, D dynamic; lowercase = static table; "
                    "irqs/frames = interrupts and frame starts since entry; depth "
                    "= call-return stack; RESUMED = resumed after a pause)\n",
                 g_journal.dispatches, g_journal.foreign);
    if (g_journal.have_foreign) {
        std::fprintf(f, "first foreign dispatch:\n");
        print_event(f, g_journal.first_foreign);
    }
    if (g_journal.frozen) {
        std::fprintf(f, "\nlast %zu dispatches up to the first impossible sp "
                        "(dispatch #%llu), oldest first:\n",
                     g_journal.frozen_count, g_journal.frozen_at);
        journal_each(g_journal.frozen_ring, g_journal.frozen_next,
                     g_journal.frozen_count,
                     [&](const JournalEvent& e) { print_event(f, e); });
    } else {
        std::fprintf(f, "\nno dispatch had an impossible sp\n");
    }
    std::fprintf(f, "\nlast %zu dispatches before the bad exit, oldest first:\n",
                 g_journal.count);
    journal_each(g_journal.ring, g_journal.next, g_journal.count,
                 [&](const JournalEvent& e) { print_event(f, e); });
}

int unmatched_return(std::uint32_t target) {
    if (!g_unpacker_guard_pending || g_irq_nest_depth != 0u) return 0;
    const std::uint32_t caller = g_entry.r[14] & ~1u;
    if (target == caller) {  // an ordinary exit whose call frame was cancelled
        g_unpacker_guard_pending = false;
        return 0;
    }
    if (target >= g_start + kBody && target < g_end) return 0;

    // The shape of the known fault: `bx lr` straight after the final
    // `pop {r5-fp,lr}`, with 1..8 unpopped 8-byte frames under it.
    const std::uint32_t sp = g_cpu.R[13];
    const std::uint32_t entry_sp = g_entry.r[13];
    bool shape = target == (g_cpu.R[14] & ~1u) && sp < entry_sp &&
                 (entry_sp - sp) % kLiteralFrame == 0u &&
                 entry_sp - sp <= 8u * kLiteralFrame;
    for (int i = 0; shape && i < 8; ++i) {
        const std::uint32_t reg = i < 7 ? g_cpu.R[5 + i] : g_cpu.R[14];
        shape = bus_read_u32(sp - kEntryFrame + 4u * static_cast<std::uint32_t>(i)) == reg;
    }
    g_unpacker_guard_pending = false;
    if (!shape) {
        std::fprintf(stderr,
            "[unpacker-guard] the unpacker left for 0x%08X (caller 0x%08X, sp "
            "0x%08X, entry sp 0x%08X), not the known stack shape: not "
            "recovered\n", target, caller, sp, entry_sp);
        return 0;
    }
    return recover(target, (entry_sp - sp) / kLiteralFrame, false);
}

// `at` is the bad return target, or for an early catch the dispatched pc.
int recover(std::uint32_t at, std::uint32_t frames, bool early) {
    g_unpacker_guard_pending = false;
    ++g_recoveries;
    const std::uint32_t caller = g_entry.r[14] & ~1u;
    const std::uint32_t entry_sp = g_entry.r[13];
    const std::uint32_t bad_out = g_cpu.R[1];
    std::uint32_t bad_regs[16];
    for (int r = 0; r < 16; ++r) bad_regs[r] = g_cpu.R[r];
    const std::uint32_t bad_cpsr = g_cpu.cpsr;
    if (early) {
        std::fprintf(stderr,
            "[unpacker-guard] CAUGHT EARLY: the data unpacker dispatched 0x%08X "
            "with sp 0x%08X (entry sp 0x%08X), output at offset 0x%X of "
            "0x%08X; redoing the unpack on the interpreter (recovery %u)\n",
            at, bad_regs[13], entry_sp, bad_out - g_entry.r[1] + 1u,
            g_entry.r[1], g_recoveries);
    } else {
        std::fprintf(stderr,
            "[unpacker-guard] CAUGHT: the data unpacker returned to 0x%08X instead "
            "of 0x%08X (%u extra stack frame(s), output stopped at offset 0x%X of "
            "0x%08X); redoing the unpack on the interpreter (recovery %u)\n",
            at, caller, frames, bad_out - g_entry.r[1] + 1u, g_entry.r[1],
            g_recoveries);
    }
    // The session's first entry (an audit mismatch may have come first)
    // creates the files; later ones append.
    const bool first = g_recoveries == 1u && g_audit_mismatches == 0u;
    const bool memory = first && gsr_write_memory_snapshot(kMemoryFile);
    std::FILE* f = g_recoveries <= kReportsMax
        ? std::fopen(kTextFile, first ? "wb" : "ab") : nullptr;
    if (f) {
        if (!first) std::fprintf(f, "\n\n");
        write_report(f, at, frames, early, bad_out, bad_regs, bad_cpsr);
        if (first)
            std::fprintf(f, "\n%s: %s\n", kMemoryFile, memory ? "written" : "FAILED");
        std::fflush(f);
    }
    write_full_journal(g_forced ? "forced fault caught" : "catch");

    // The input must still be what the first run read: a ROM source, and the
    // unpacker itself intact.
    const std::uint32_t src = g_entry.r[0];
    const bool rom_source = src >= 0x08000000u && src < 0x0E000000u;
    const char* refused = !rom_source ? "the source is not in ROM"
                        : !unpacker_resident(g_start) ? "the unpacker's code was overwritten"
                        : nullptr;
    if (refused) {
        std::fprintf(stderr, "[unpacker-guard] not recovered: %s\n", refused);
        if (f) {
            std::fprintf(f, "\nNOT RECOVERED: %s\n", refused);
            std::fclose(f);
        }
        return 0;
    }

    for (int i = 0; i < 15; ++i) g_cpu.R[i] = g_entry.r[i];
    g_cpu.cpsr = g_entry.cpsr;
    runtime_call_stack_restore(g_entry.call_stack.data(),
                               static_cast<std::uint32_t>(g_entry.call_stack.size()));
    // Stops when the interpreted `bx lr` reaches the caller, and pops the
    // caller's call-return entry exactly as a matched return would.
    const int stopped = runtime_bridge_interpret(g_start, false, caller,
                                                 kRedoBudget);
    const std::uint32_t good_out = g_cpu.R[1];
    const bool balanced = (g_cpu.R[15] & ~1u) == caller && g_cpu.R[13] == entry_sp;
    // The faulty run wrote only from dst up to its own output pointer; past
    // the correct end nothing would put those bytes back.
    const bool contained = bad_out <= good_out;
    const bool ok = stopped == 1 && balanced && contained;
    std::fprintf(stderr,
        "[unpacker-guard] redo %s: returned to 0x%08X sp=0x%08X length 0x%X "
        "(faulty run stopped at offset 0x%X)\n",
        ok ? "OK, the game continues" : "FAILED",
        g_cpu.R[15], g_cpu.R[13], g_cpu.R[0], bad_out - g_entry.r[1] + 1u);
    if (f) {
        std::fprintf(f, "\nredo: %s; stopped=%d pc=0x%08X sp=0x%08X r0(length)=0x%X "
                        "out=0x%08X faulty_out=0x%08X\n",
                     ok ? "OK, the game continued" : "FAILED", stopped, g_cpu.R[15],
                     g_cpu.R[13], g_cpu.R[0], good_out, bad_out);
        std::fclose(f);
    }
    if (!ok) {
        std::fprintf(stderr, "[unpacker-guard] the redo did not restore a "
                             "consistent state; stopping the game\n");
        std::abort();
    }

    if (g_captures < kCapturesMax) {
        ++g_captures;
        g_capture_at = runtime_current_frame() + kCaptureAfter;
        char note[256];
        if (early) {
            std::snprintf(note, sizeof note,
                "%sunpacker crash caught early at frame %llu: dispatched 0x%08X "
                "with sp 0x%08X (entry sp 0x%08X); output offset 0x%X of "
                "0x%08X; redone on the interpreter (see unpacker_recovered.txt)\n",
                g_forced ? "FORCED " : "", runtime_current_frame(), at,
                bad_regs[13], entry_sp, bad_out - g_entry.r[1] + 1u, g_entry.r[1]);
        } else {
            std::snprintf(note, sizeof note,
                "unpacker crash caught at frame %llu: returned to 0x%08X instead of "
                "0x%08X with %u extra frame(s); output offset 0x%X of 0x%08X; redone "
                "on the interpreter (see unpacker_recovered.txt)\n",
                runtime_current_frame(), at, caller, frames,
                bad_out - g_entry.r[1] + 1u, g_entry.r[1]);
        }
        g_capture_note = note;
    }
    // R15 is the caller's return address: the generated call site that
    // pushed it continues, every one above it cancels. An early catch's
    // dispatch runs nothing and unwinds the same way.
    return 1;
}

}  // namespace
}  // namespace gsr
