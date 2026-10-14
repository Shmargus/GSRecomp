#pragma once

// A snapshot of the GBA's memory and CPU registers, for crash reports
// (crash_memory.bin) and the games' unpacker catchers. Format, little-endian:
//   char magic[8] = "GSRMEM1\0"; u32 region_count;
//   per region: char name[8] (zero padded); u32 gba_address; u32 size;
//               u8 data[size];
// Regions in order: FRAME (addr 0, u64 frame counter), CPU (addr 0, raw
// g_cpu bytes), IO 0x04000000 0x400, IWRAM 0x03000000 32K, EWRAM
// 0x02000000 256K, PAL 0x05000000 1K, VRAM 0x06000000 96K, OAM 0x07000000
// 1K. Only FRAME and CPU are written when no bus is active.
extern "C" bool gsr_write_memory_snapshot(const char* path);
