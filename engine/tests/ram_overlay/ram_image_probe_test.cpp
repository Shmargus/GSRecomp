// Synthetic coverage for the opt-in, no-bytes pointer attribution probe.

#include <cstdint>
#include <cstdio>

#include "runtime_arm.h"

extern "C" void runtime_ram_image_dma_begin(void);
extern "C" void runtime_ram_image_dma_end(void);
extern "C" void runtime_note_ram_image_bus_write(uint32_t addr,
                                                   uint32_t width);

extern "C" unsigned long long g_runtime_vblank_starts;

namespace {

struct Event {
    std::uint64_t frame = 0;
    std::uint32_t dest = 0;
    std::uint32_t writer_pc = 0;
    std::uint8_t writer_thumb = 0;
    std::uint32_t pointer_target = 0;
    std::uint8_t pointer_thumb = 0;
};

Event g_events[8]{};
unsigned g_event_count = 0;

struct DmaEvent {
    std::uint64_t frame = 0;
    std::uint8_t completed = 0;
    std::uint32_t source = 0;
    std::uint32_t dest = 0;
    std::uint32_t size = 0;
    std::uint32_t writer_pc = 0;
    std::uint8_t writer_thumb = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t channel = 0;
};

DmaEvent g_dma_events[4]{};
unsigned g_dma_event_count = 0;

struct WriteEvent {
    std::uint64_t frame = 0;
    std::uint32_t pc = 0;
    std::uint8_t thumb = 0;
    std::uint32_t addr = 0;
    std::uint32_t width = 0;
};

WriteEvent g_write_events[8]{};
unsigned g_write_event_count = 0;

struct BoundaryEvent {
    std::uint8_t outer = 0;
    std::uint32_t return_pc = 0;
    std::uint32_t call_depth = 0;
};

BoundaryEvent g_boundary_events[8]{};
unsigned g_boundary_event_count = 0;

void record(std::uint64_t frame, std::uint32_t dest, std::uint32_t writer_pc,
            std::uint8_t writer_thumb, std::uint32_t pointer_target,
            std::uint8_t pointer_thumb) {
    if (g_event_count >= 8u) return;
    g_events[g_event_count++] =
        {frame, dest, writer_pc, writer_thumb, pointer_target, pointer_thumb};
}

void record_dma(std::uint64_t frame, std::uint8_t completed,
                std::uint32_t source, std::uint32_t dest, std::uint32_t size,
                std::uint32_t writer_pc, std::uint8_t writer_thumb,
                std::uint32_t crc32, std::uint32_t channel) {
    if (g_dma_event_count >= 4u) return;
    g_dma_events[g_dma_event_count++] = {
        frame, completed, source, dest, size, writer_pc, writer_thumb, crc32,
        channel};
}

void record_write(std::uint64_t frame, std::uint32_t pc, std::uint8_t thumb,
                  std::uint32_t addr, std::uint32_t width) {
    if (g_write_event_count >= 8u) return;
    g_write_events[g_write_event_count++] = {frame, pc, thumb, addr, width};
}

void record_boundary(std::uint8_t outer, std::uint32_t return_pc,
                     std::uint32_t call_depth) {
    if (g_boundary_event_count >= 8u) return;
    g_boundary_events[g_boundary_event_count++] =
        {outer, return_pc, call_depth};
}

bool check(bool ok, const char* what) {
    if (ok) return true;
    std::printf("FAIL: %s\n", what);
    return false;
}

}  // namespace

int main() {
    g_runtime_ram_pointer_write_probe = &record;
    g_cpu.R[15] = 0x08001234u;
    g_cpu.cpsr = 0u;

    runtime_note_ram_pointer_write(0x02001234u, 0x03005B0Du, 4u);
    runtime_note_ram_pointer_write(0x02001238u, 0x03005D5Cu, 4u);
    runtime_note_ram_pointer_write(0x0200123Cu, 0x03005D5Bu, 4u);
    runtime_note_ram_pointer_write(0x02001240u, 0x03005B0Cu, 2u);

    bool ok = true;
    ok &= check(g_event_count == 2u, "pointer probe range/width gate");
    ok &= check(g_events[0].frame == 0u &&
                    g_events[0].dest == 0x02001234u &&
                    g_events[0].writer_pc == 0x08001234u &&
                    g_events[0].pointer_target == 0x03005B0Cu,
                "pointer probe first event");
    ok &= check(g_events[1].pointer_target == 0x03005D5Au,
                "pointer probe normalizes target mode bit");

    g_runtime_ram_pointer_write_probe = nullptr;
    runtime_note_ram_pointer_write(0x02001244u, 0x03005B0Cu, 4u);
    ok &= check(g_event_count == 2u, "disabled pointer probe is inert");

    g_runtime_vblank_starts = 17u;
    g_cpu.R[15] = 0x0800C8B2u;
    g_cpu.cpsr = CPSR_T_BIT;
    g_runtime_ram_image_dma_probe = &record_dma;
    runtime_note_ram_image_dma(
        0u, 0x08009BE4u, 0x03005B0Cu, 0x250u, 0u, 3u);
    runtime_note_ram_image_dma(
        1u, 0x08009BE4u, 0x03005B0Cu, 0x250u, 0xAFBFFE10u, 3u);
    ok &= check(g_dma_event_count == 2u, "DMA lifecycle trigger/completion");
    ok &= check(g_dma_events[0].frame == 17u &&
                    g_dma_events[0].completed == 0u &&
                    g_dma_events[0].writer_pc == 0x0800C8B2u &&
                    g_dma_events[0].writer_thumb == 1u,
                "DMA lifecycle writer attribution");
    ok &= check(g_dma_events[1].completed == 1u &&
                    g_dma_events[1].crc32 == 0xAFBFFE10u &&
                    g_dma_events[1].channel == 3u,
                "DMA lifecycle completion CRC");
    g_runtime_ram_image_dma_probe = nullptr;
    runtime_note_ram_image_dma(
        1u, 0x08009BE4u, 0x03005B0Cu, 0x250u, 0x12345678u, 3u);
    ok &= check(g_dma_event_count == 2u, "disabled DMA lifecycle is inert");

    g_runtime_vblank_starts = 23u;
    g_runtime_ram_image_write_probe = &record_write;
    // The common committed-store hook covers both generated fast stores and
    // interpreter/GbaBus stores; it supplies the current guest PC/mode and
    // deliberately has no value parameter.
    g_cpu.R[15] = 0x03000198u;
    g_cpu.cpsr = 0u;
    runtime_note_ram_image_bus_write(0x03005B0Au, 4u);
    g_cpu.R[15] = 0x0300019Cu;
    runtime_note_ram_image_bus_write(0x03005D58u, 4u);
    runtime_ram_image_dma_begin();
    runtime_note_ram_image_bus_write(0x03005B10u, 4u);
    runtime_ram_image_dma_end();
    runtime_note_ram_image_bus_write(0x02000000u, 4u);
    ok &= check(g_write_event_count == 2u,
                "CPU writer metadata range gate");
    ok &= check(g_write_events[0].frame == 23u &&
                    g_write_events[0].pc == 0x03000198u &&
                    g_write_events[0].addr == 0x03005B0Cu &&
                    g_write_events[0].width == 2u,
                "CPU writer metadata clips overlap and omits value");
    ok &= check(g_write_events[1].addr == 0x03005D58u &&
                    g_write_events[1].width == 4u,
                "CPU writer metadata final word");
    g_runtime_ram_image_write_probe = nullptr;

    g_runtime_ram_image_boundary_probe = &record_boundary;
    runtime_call_should_return(0x03003908u);
    ok &= check(g_boundary_event_count == 1u &&
                    g_boundary_events[0].outer == 0u &&
                    g_boundary_events[0].return_pc == 0x03003908u,
                "return boundary lifecycle callback");
    g_runtime_ram_image_boundary_probe = nullptr;
    std::printf("ram_image_probe_test: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
