// See unpacker_catch.h.

#include "unpacker_catch.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "env_flag.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#endif

extern "C" void (*g_runtime_ram_fallback_probe)(std::uint32_t pc,
                                                std::uint32_t entry_pc,
                                                int thumb, const char* why);
extern "C" std::uint32_t runtime_recent_dispatch_copy(std::uint32_t* pcs,
                                                      std::uint8_t* resumes,
                                                      std::uint32_t max);
extern "C" void runtime_unpacker_slot_dump(const char* why);
extern "C" bool gsr_write_memory_snapshot(const char* path);
extern "C" std::uint32_t g_irq_nest_depth;
extern "C" std::uint32_t g_yield_resume_pc;
extern "C" bool g_yield_resume_pending;

namespace gsr {
namespace {

// Func_2808 as Func_5394 copies it (FACTS.md 2026-10-07/08). The first 16
// bytes are its entry-only format check, which its own output may overwrite.
constexpr std::uint32_t kUnpackerStart = 0x03006000u;
constexpr std::uint32_t kUnpackerEnd = 0x030064ECu;
constexpr std::uint32_t kUnpackerRom = 0x08002808u;
constexpr std::uint32_t kVolatilePrefix = 16u;

bool unpacker_resident() {
    for (std::uint32_t off = kVolatilePrefix;
         off < kUnpackerEnd - kUnpackerStart; ++off) {
        if (bus_read_u8(kUnpackerStart + off) != bus_read_u8(kUnpackerRom + off))
            return false;
    }
    return true;
}

void write_host_backtrace(std::FILE* f) {
    void* frames[64];
#if defined(_WIN32)
    const int count = CaptureStackBackTrace(0, 64, frames, nullptr);
#else
    const int count = backtrace(frames, 64);
#endif
    std::fprintf(f, "\nhost call chain (innermost first; module+offset):\n");
    for (int i = 0; i < count; ++i) {
        const char* name = "?";
        std::uintptr_t base = 0;
#if defined(_WIN32)
        char path[MAX_PATH] = {};
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module) &&
            GetModuleFileNameA(module, path, MAX_PATH) != 0) {
            name = path;
            base = reinterpret_cast<std::uintptr_t>(module);
        }
#else
        Dl_info info{};
        if (dladdr(frames[i], &info) != 0 && info.dli_fname) {
            name = info.dli_fname;
            base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
        }
#endif
        const std::string module_name =
            std::filesystem::path(name).filename().string();
        std::fprintf(f, "  #%2d %s+0x%llx\n", i, module_name.c_str(),
                     static_cast<unsigned long long>(
                         reinterpret_cast<std::uintptr_t>(frames[i]) - base));
    }
}

void write_catch(std::uint32_t pc, std::uint32_t entry_pc, int thumb,
                 const char* why) {
    // One fixed pair beside the crash files, overwritten by a later catch;
    // the launcher deletes both at every launch and adds them to a report.
    const bool memory = gsr_write_memory_snapshot("unpacker_catch.bin");

    if (std::FILE* f = std::fopen("unpacker_catch.txt", "wb")) {
        std::fprintf(f,
            "Golden Sun Recompiled -- unpacker catch\n"
            "The data unpacker (0x%08X..0x%08X) is in memory and our engine "
            "was about to fall back to the interpreter inside it. Nothing has "
            "run since.\n\n",
            kUnpackerStart, kUnpackerEnd);
        std::fprintf(f, "frame=%llu why=%s pc=0x%08X entry_pc=0x%08X mode=%s\n",
                     runtime_current_frame(), why, pc, entry_pc,
                     thumb ? "thumb" : "arm");
        std::fprintf(f,
            "image_base=0x%08X call_stack_depth=%u irq_nest_depth=%u "
            "yield_resume_pending=%d yield_resume_pc=0x%08X unpacker_catch.bin=%s\n\n",
            g_runtime_image_base, runtime_call_stack_depth(), g_irq_nest_depth,
            g_yield_resume_pending ? 1 : 0, g_yield_resume_pc,
            memory ? "written" : "FAILED");

        std::fprintf(f, "registers:\n");
        for (int r = 0; r < 16; ++r)
            std::fprintf(f, "  r%-2d=0x%08X%s", r, g_cpu.R[r],
                         (r % 4 == 3) ? "\n" : "");
        std::fprintf(f, "  cpsr=0x%08X\n  banked sp/lr/spsr:", g_cpu.cpsr);
        for (unsigned b = 0; b < ARM_BANK_COUNT; ++b)
            std::fprintf(f, " [%u] 0x%08X/0x%08X/0x%08X", b, g_cpu.banked_sp[b],
                         g_cpu.banked_lr[b], g_cpu.banked_spsr[b]);
        std::fprintf(f, "\n\n");

        std::uint32_t pcs[16];
        std::uint8_t resumes[16];
        const std::uint32_t n = runtime_recent_dispatch_copy(pcs, resumes, 16);
        std::fprintf(f, "last dispatches (oldest first; R = resumed after a "
                        "pause):\n");
        for (std::uint32_t i = 0; i < n; ++i)
            std::fprintf(f, "  0x%08X%s\n", pcs[i], resumes[i] ? " R" : "");

        const std::uint32_t sp = g_cpu.R[13];
        std::fprintf(f, "\nstack around sp:\n");
        for (std::int32_t off = -0x40; off <= 0x40; off += 4) {
            const std::uint32_t a = sp + static_cast<std::uint32_t>(off);
            std::fprintf(f, "  0x%08X%s 0x%08X\n", a, off == 0 ? " sp" : "   ",
                         bus_read_u32(a));
        }
        write_host_backtrace(f);
        std::fclose(f);
    }
    std::fprintf(stderr, "[unpacker-catch] %s at pc=0x%08X (entry 0x%08X): "
                         "saved unpacker_catch.txt and unpacker_catch.bin\n",
                 why, pc, entry_pc);
    runtime_unpacker_slot_dump("unpacker-catch");
}

void probe(std::uint32_t pc, std::uint32_t entry_pc, int thumb,
           const char* why) {
    static bool caught = false;
    if (caught) return;
    const bool inside = (pc >= kUnpackerStart && pc < kUnpackerEnd) ||
                        (entry_pc >= kUnpackerStart && entry_pc < kUnpackerEnd);
    if (!inside || !unpacker_resident()) return;
    caught = true;
    write_catch(pc, entry_pc, thumb, why);
}

}  // namespace

void unpacker_catch_install() {
    if (!gbarecomp::env_flag("GSR_UNPACKER_CATCH")) return;
    g_runtime_ram_fallback_probe = &probe;
    std::fprintf(stderr, "[unpacker-catch] armed\n");
}

}  // namespace gsr
