// no_bios_dispatch_table.cpp — the BIOS dispatch table of a build that links
// no recompiled BIOS (GBARECOMP_LINK_BIOS off, or no locally generated BIOS
// output). It is empty: such a build runs only in no-BIOS mode, where
// runtime_dispatch sends every PC below 0x4000 to the runtime's own
// stand-ins before this table is consulted (runtime_arm.h, "No-BIOS mode").
// It holds no BIOS-derived data, unlike generated_bios/bios_dispatch_table.cpp,
// which `gba_recompile --bios` writes locally for a comparison build.

#include <cstdint>

struct DispatchEntry { uint32_t addr; uint8_t thumb; void (*fn)(void); };
extern "C" const DispatchEntry kBiosDispatchTable[1] = {
    {0xFFFFFFFFu, 0u, nullptr},
};
extern "C" const unsigned kBiosDispatchTableLen = 0u;
