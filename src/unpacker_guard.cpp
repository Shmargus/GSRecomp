// See unpacker_guard.h.

#include "unpacker_guard.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" bool gsr_write_memory_snapshot(const char* path);
extern "C" std::uint32_t g_irq_nest_depth;
extern "C" unsigned long long g_runtime_irq_entries;
extern "C" unsigned long long g_runtime_vblank_starts;
extern "C" std::uint32_t runtime_recent_dispatch_copy(std::uint32_t* pcs,
                                                      std::uint8_t* resumes,
                                                      std::uint32_t max);

namespace gsr {
namespace {

// Func_2808 as Func_5394 copies it (FACTS.md 2026-10-07/08).
constexpr std::uint32_t kUnpackerStart = 0x03006000u;
constexpr std::uint32_t kUnpackerEnd = 0x030064ECu;
constexpr std::uint32_t kUnpackerRom = 0x08002808u;
// Every return target inside the unpacker lies at or above its prologue;
// 0x03006000..0x0300600F is its entry-only format check.
constexpr std::uint32_t kUnpackerBody = 0x03006010u;
// The prologue pushes r5-fp and lr; a literal adds `push {r5,sb}`.
constexpr std::uint32_t kEntryFrame = 0x20u;
constexpr std::uint32_t kLiteralFrame = 0x08u;
// The correct unpack of the crash input is 141,352 instructions.
constexpr std::uint64_t kRedoBudget = 4'000'000u;
constexpr unsigned kCapturesMax = 3;
constexpr unsigned long long kCaptureAfter = 30;
constexpr std::size_t kJournal = 64;
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

unsigned g_recoveries = 0;
unsigned g_captures = 0;
unsigned long long g_capture_at = 0;  // guest frame; 0 = none due
std::string g_capture_note;

bool unpacker_resident() {
    for (std::uint32_t off = 0; off < kUnpackerEnd - kUnpackerStart; off += 4u) {
        if (bus_read_u32(kUnpackerStart + off) != bus_read_u32(kUnpackerRom + off))
            return false;
    }
    return true;
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
        "(entry-0x%X) sb=0x%08X lr=0x%08X irqs=+%llu frames=+%llu depth=%u%s\n",
        e.pc, e.path, e.own ? 1 : 0, e.r1, e.r1 - g_entry.r[1] + 1u, e.sp,
        g_entry.r[13] - e.sp, e.sb, e.lr, e.irq_entries - g_entry.irq_entries,
        e.vblanks - g_entry.vblanks, e.call_depth, e.resumed ? " RESUMED" : "");
}

int unmatched_return(std::uint32_t target);

}  // namespace

void unpacker_guard_install() {
    g_runtime_unmatched_return_hook = &unmatched_return;
}

void unpacker_guard_note_entry(std::uint32_t pc) {
    if (pc != kUnpackerStart || g_irq_nest_depth != 0u) return;
    g_unpacker_guard_pending = false;
    if (!unpacker_resident()) return;
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

void unpacker_guard_note_dispatch(std::uint32_t pc, GuardGuestFn fn,
                                  GuardGuestFn expected, RamResolvePath path) {
    if (!g_unpacker_guard_pending || g_irq_nest_depth != 0u) return;
    if (pc < kUnpackerStart || pc >= kUnpackerEnd) return;
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
    const std::uint32_t entry_sp = g_entry.r[13];
    const bool possible = e.sp == entry_sp - kEntryFrame ||
                          e.sp == entry_sp - kEntryFrame - kLiteralFrame;
    if (!possible && !j.frozen && pc != kUnpackerStart) {
        j.frozen = true;
        j.frozen_ring = j.ring;
        j.frozen_count = j.count;
        j.frozen_next = j.next;
        j.frozen_at = j.dispatches;
    }
}

void unpacker_guard_note_return(std::uint32_t return_pc) {
    if (!g_unpacker_guard_pending || g_irq_nest_depth != 0u) return;
    // A real return sets R15 to its target first; a yield's cancels see the
    // 0xFFFFFFF0 sentinel instead and must not end the watch.
    if ((g_cpu.R[15] & ~1u) != return_pc) return;
    if (return_pc == (g_entry.r[14] & ~1u)) g_unpacker_guard_pending = false;
}

bool unpacker_guard_capture_due(std::string* note) {
    if (g_capture_at == 0 || runtime_current_frame() < g_capture_at) return false;
    g_capture_at = 0;
    *note = g_capture_note;
    return true;
}

namespace {

void write_report(std::FILE* f, std::uint32_t target, std::uint32_t frames,
                  std::uint32_t bad_out, const std::uint32_t* bad_regs,
                  std::uint32_t bad_cpsr) {
    std::fprintf(f,
        "Golden Sun Recompiled -- unpacker crash caught (recovery %u)\n"
        "The data unpacker (0x%08X..0x%08X) tried to return to 0x%08X instead "
        "of its caller 0x%08X, with %u extra stack frame(s) of 8 bytes. The "
        "unpack was redone on the interpreter; result below.\n\n",
        g_recoveries, kUnpackerStart, kUnpackerEnd, target, g_entry.r[14] & ~1u,
        frames);
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
    if (target >= kUnpackerBody && target < kUnpackerEnd) return 0;

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

    ++g_recoveries;
    const std::uint32_t frames = (entry_sp - sp) / kLiteralFrame;
    const std::uint32_t bad_out = g_cpu.R[1];
    std::uint32_t bad_regs[16];
    for (int r = 0; r < 16; ++r) bad_regs[r] = g_cpu.R[r];
    const std::uint32_t bad_cpsr = g_cpu.cpsr;
    std::fprintf(stderr,
        "[unpacker-guard] CAUGHT: the data unpacker returned to 0x%08X instead "
        "of 0x%08X (%u extra stack frame(s), output stopped at offset 0x%X of "
        "0x%08X); redoing the unpack on the interpreter (recovery %u)\n",
        target, caller, frames, bad_out - g_entry.r[1] + 1u, g_entry.r[1],
        g_recoveries);
    const bool memory = g_recoveries == 1u && gsr_write_memory_snapshot(kMemoryFile);
    std::FILE* f = std::fopen(kTextFile, g_recoveries == 1u ? "wb" : "ab");
    if (f) {
        if (g_recoveries > 1u) std::fprintf(f, "\n\n");
        write_report(f, target, frames, bad_out, bad_regs, bad_cpsr);
        if (g_recoveries == 1u)
            std::fprintf(f, "\n%s: %s\n", kMemoryFile, memory ? "written" : "FAILED");
        std::fflush(f);
    }

    // The input must still be what the first run read: a ROM source, and the
    // unpacker itself intact.
    const std::uint32_t src = g_entry.r[0];
    const bool rom_source = src >= 0x08000000u && src < 0x0E000000u;
    const char* refused = !rom_source ? "the source is not in ROM"
                        : !unpacker_resident() ? "the unpacker's code was overwritten"
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
    const int stopped = runtime_bridge_interpret(kUnpackerStart, false, caller,
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
        std::snprintf(note, sizeof note,
            "unpacker crash caught at frame %llu: returned to 0x%08X instead of "
            "0x%08X with %u extra frame(s); output offset 0x%X of 0x%08X; redone "
            "on the interpreter (see unpacker_recovered.txt)\n",
            runtime_current_frame(), target, caller, frames,
            bad_out - g_entry.r[1] + 1u, g_entry.r[1]);
        g_capture_note = note;
    }
    // R15 is the caller's return address: the generated call site that
    // pushed it continues, every one above it cancels.
    return 1;
}

}  // namespace
}  // namespace gsr
