// See memory_snapshot.h.
#include "memory_snapshot.h"

#include <cstdint>
#include <cstdio>

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
        wrote_mem = std::fclose(f) == 0;
    }
    return wrote_mem;
}
