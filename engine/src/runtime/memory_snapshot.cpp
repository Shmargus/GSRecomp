// See memory_snapshot.h.
#include "memory_snapshot.h"

#include <cstdint>
#include <cstdio>

#include "crash_handler.h"
#include "gba_bus.h"
#include "gba_io.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" bool gsr_write_memory_snapshot(const char* path) {
    bool wrote_mem = false;
    if (std::FILE* f = std::fopen(path, "wb")) {
        const gba::GbaBus* bus = gbarecomp::active_bus();
        auto put32 = [&](std::uint32_t v) {
            const unsigned char b[4] = {
                (unsigned char)(v), (unsigned char)(v >> 8),
                (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
            std::fwrite(b, 1, 4, f);
        };
        auto region = [&](const char* name, std::uint32_t addr,
                          const void* data, std::uint32_t size) {
            char nm[8] = {};
            for (int i = 0; i < 8 && name[i]; ++i) nm[i] = name[i];
            std::fwrite(nm, 1, 8, f);
            put32(addr);
            put32(size);
            std::fwrite(data, 1, size, f);
        };
        static const char magic[8] = {'G','S','R','M','E','M','1','\0'};
        std::fwrite(magic, 1, 8, f);
        put32(bus ? 8u : 2u);
        const std::uint64_t frame = runtime_current_frame();
        unsigned char fb[8];
        for (int i = 0; i < 8; ++i) fb[i] = (unsigned char)(frame >> (8 * i));
        region("FRAME", 0, fb, 8);
        region("CPU", 0, &g_cpu, (std::uint32_t)sizeof(g_cpu));
        if (bus) {
            region("IO", 0x04000000u, bus->io().raw(), 0x400);
            region("IWRAM", 0x03000000u, bus->iwram_ptr(), 32u * 1024u);
            region("EWRAM", 0x02000000u, bus->ewram_ptr(), 256u * 1024u);
            region("PAL", 0x05000000u, bus->pal_ptr(), 1024u);
            region("VRAM", 0x06000000u, bus->vram_ptr(), 96u * 1024u);
            region("OAM", 0x07000000u, bus->oam_ptr(), 1024u);
        }
        const bool ok = std::ferror(f) == 0;
        wrote_mem = std::fclose(f) == 0 && ok;
    }
    return wrote_mem;
}

GsrDiagnosticFiles gsr_write_diagnostic_files(const char* dir, const char* stem) {
    GsrDiagnosticFiles result{};
    const auto frame = runtime_current_frame();
    const ArmCpuState cpu = g_cpu;
    const auto depth = runtime_call_stack_depth();
    const auto* returns = runtime_call_stack_data();
    const auto trace_count = runtime_fp_count();
    result.trail_available = trace_count != 0;
    char path[300];
    std::snprintf(path, sizeof(path), "%s/%s_context.txt", dir, stem);
    if (std::FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "diagnostic_context_version=1\n"
                        "engine_diagnostics_built=%s %s\n"
                        "state=live_at_capture_not_historical_picture\n"
                        "guest_frame=%llu\nactive_bus=%d\n"
                        "cpsr=0x%08x\nresume_pc=0x%08x\n"
                        "recorded_instruction_entries=%u\n",
                     __DATE__, __TIME__, frame, gbarecomp::active_bus() ? 1 : 0,
                     cpu.cpsr, g_runtime_resume_pc, trace_count);
        for (unsigned i = 0; i < 16; ++i)
            std::fprintf(f, "r%u=0x%08x\n", i, cpu.R[i]);
        std::fprintf(f, "pending_guest_returns=%u\n"
                        "return_order=oldest_first_not_a_complete_guest_backtrace\n", depth);
        for (unsigned i = 0; i < depth; ++i)
            std::fprintf(f, "return[%u]=0x%08x\n", i, returns[i]);
        const bool ok = std::ferror(f) == 0;
        result.context_written = std::fclose(f) == 0 && ok;
    }
    if (result.trail_available) {
        std::snprintf(path, sizeof(path), "%s/%s_trail.csv", dir, stem);
        result.trail_written = runtime_fp_save_tail_csv(path, 20000) != 0;
    }
    return result;
}

extern "C" void gsr_install_crash_snapshot_writer() {
    gbarecomp::crash_handler_set_extra_writer([](const char* dir) -> const char* {
        static char desc[256];
        char path[300];
        std::snprintf(path, sizeof(path), "%s/crash_memory.bin", dir);
        const bool wrote_mem = gsr_write_memory_snapshot(path);
        const char* mem = wrote_mem ? "crash_memory.bin" : "crash_memory.bin FAILED";
        const auto extra = gsr_write_diagnostic_files(dir, "crash");
        const char* context = extra.context_written ? "crash_context.txt" : "crash_context.txt FAILED";
        if (!extra.trail_available) {
            std::snprintf(desc, sizeof(desc),
                          "%s, %s (no recorded game trail; F1 Crash log is optional)",
                          mem, context);
        } else {
            std::snprintf(desc, sizeof(desc), "%s, %s, %s", mem, context,
                          extra.trail_written ? "crash_trail.csv" : "crash_trail.csv FAILED");
        }
        return desc;
    });
}
