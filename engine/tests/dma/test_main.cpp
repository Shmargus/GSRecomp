#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "gba_io.h"
#include "gba_vram_trace.h"
#include "runtime_arm.h"

extern "C" unsigned long long g_runtime_vblank_starts;

namespace {

class TestBus final : public armv4t::Bus {
public:
    uint8_t read8(uint32_t addr) override { return byte(addr); }
    uint16_t read16(uint32_t addr) override {
        return static_cast<uint16_t>(read8(addr) | (read8(addr + 1) << 8));
    }
    uint32_t read32(uint32_t addr) override {
        return static_cast<uint32_t>(read8(addr)) |
               (static_cast<uint32_t>(read8(addr + 1)) << 8) |
               (static_cast<uint32_t>(read8(addr + 2)) << 16) |
               (static_cast<uint32_t>(read8(addr + 3)) << 24);
    }
    void write8(uint32_t addr, uint8_t value) override { byte(addr) = value; }
    void write16(uint32_t addr, uint16_t value) override {
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
    }
    void write32(uint32_t addr, uint32_t value) override {
        write8(addr, static_cast<uint8_t>(value));
        write8(addr + 1, static_cast<uint8_t>(value >> 8));
        write8(addr + 2, static_cast<uint8_t>(value >> 16));
        write8(addr + 3, static_cast<uint8_t>(value >> 24));
    }

private:
    uint8_t& byte(uint32_t addr) {
        if ((addr & 0xFF000000u) == 0x02000000u) {
            return ewram_[addr & (ewram_.size() - 1)];
        }
        return iwram_[addr & (iwram_.size() - 1)];
    }

    std::array<uint8_t, 256 * 1024> ewram_{};
    std::array<uint8_t, 32 * 1024> iwram_{};
};

int failures = 0;

struct RamDmaEvent {
    uint64_t frame = 0;
    uint8_t completed = 0;
    uint32_t source = 0;
    uint32_t dest = 0;
    uint32_t size = 0;
    uint32_t writer_pc = 0;
    uint32_t crc32 = 0;
};

RamDmaEvent ram_dma_events[16]{};
unsigned ram_dma_event_count = 0;

void record_ram_dma(uint64_t frame, uint8_t completed, uint32_t source,
                    uint32_t dest, uint32_t size, uint32_t writer_pc,
                    uint8_t, uint32_t crc32, uint32_t) {
    if (ram_dma_event_count >= 16u) return;
    ram_dma_events[ram_dma_event_count++] =
        {frame, completed, source, dest, size, writer_pc, crc32};
}

void reset_ram_dma_probe() {
    ram_dma_event_count = 0;
    for (auto& event : ram_dma_events) event = {};
    g_runtime_ram_image_dma_probe = &record_ram_dma;
    g_runtime_vblank_starts = 41u;
    g_cpu.R[15] = 0x08001234u;
    g_cpu.cpsr = 0u;
}

void check_eq(const char* test, uint32_t got, uint32_t expected) {
    if (got != expected) {
        std::printf("FAIL %s: got 0x%08X, expected 0x%08X\n",
                    test, got, expected);
        ++failures;
    }
}

void configure_dma(gba::GbaIo& io, uint32_t source, uint32_t dest,
                   uint16_t count, uint16_t control) {
    io.write32(0xB0, source);
    io.write32(0xB4, dest);
    io.write16(0xB8, count);
    io.write16(0xBA, control);
}

void test_immediate_word_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02000000u, 0x78563412u);
    bus.write32(0x02000004u, 0xAABBCCDDu);

    configure_dma(io, 0x02000001u, 0x03000003u, 1, 0x8400u);

    check_eq("immediate_word_aligned_dest", bus.read32(0x03000000u),
             0x78563412u);
    check_eq("immediate_word_no_unaligned_write", bus.read32(0x03000004u), 0u);
    check_eq("immediate_word_run_count", static_cast<uint32_t>(io.dma_runs(0)), 1u);
    check_eq("immediate_word_unit_count", static_cast<uint32_t>(io.dma_words(0)), 1u);
}

void test_immediate_halfword_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write16(0x02000010u, 0xBEEFu);
    bus.write16(0x02000012u, 0x1234u);

    configure_dma(io, 0x02000011u, 0x03000021u, 1, 0x8000u);

    check_eq("immediate_halfword_aligned_dest", bus.read16(0x03000020u),
             0xBEEFu);
    check_eq("immediate_halfword_no_unaligned_write", bus.read16(0x03000022u), 0u);
}

void test_timed_word_alignment() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02000040u, 0xDEADBEEFu);
    bus.write32(0x02000044u, 0x01020304u);

    configure_dma(io, 0x02000041u, 0x03000043u, 1, 0x9400u);
    check_eq("timed_word_not_immediate", bus.read32(0x03000040u), 0u);
    io.run_timed_dma(1);

    check_eq("timed_word_aligned_dest", bus.read32(0x03000040u), 0xDEADBEEFu);
    check_eq("timed_word_no_unaligned_write", bus.read32(0x03000044u), 0u);
}

void test_probe_detects_partial_immediate_dma() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02001000u, 0x11223344u);
    bus.write32(0x02001004u, 0x55667788u);

    reset_ram_dma_probe();
    // Two words begin four bytes before the measured image and overlap its
    // first word. This is deliberately not the old exact 592-byte shape.
    configure_dma(io, 0x02001000u, 0x03005B08u, 2, 0x8400u);

    check_eq("ram_probe_partial_immediate_events",
             ram_dma_event_count, 2u);
    check_eq("ram_probe_partial_immediate_trigger_dest",
             ram_dma_events[0].dest, 0x03005B08u);
    check_eq("ram_probe_partial_immediate_size",
             ram_dma_events[0].size, 8u);
    check_eq("ram_probe_partial_immediate_completion",
             ram_dma_events[1].completed, 1u);
    check_eq("ram_probe_partial_immediate_pc",
             ram_dma_events[0].writer_pc, 0x08001234u);
    g_runtime_ram_image_dma_probe = nullptr;
}

void test_probe_detects_split_timed_dma() {
    TestBus bus;
    gba::GbaIo io;
    io.set_bus(&bus);
    bus.write32(0x02002000u, 0xAABBCCDDu);
    bus.write32(0x02002004u, 0xEEFF0011u);

    reset_ram_dma_probe();
    // Repeat-enabled VBlank DMA writes one word per trigger. The two blocks
    // together cover the image start, and each block must be attributed.
    configure_dma(io, 0x02002000u, 0x03005B0Cu, 1, 0x9600u);
    io.run_timed_dma(1);
    io.run_timed_dma(1);

    check_eq("ram_probe_split_timed_events", ram_dma_event_count, 4u);
    check_eq("ram_probe_split_timed_first_dest",
             ram_dma_events[0].dest, 0x03005B0Cu);
    check_eq("ram_probe_split_timed_second_dest",
             ram_dma_events[2].dest, 0x03005B10u);
    check_eq("ram_probe_split_timed_first_completion",
             ram_dma_events[1].completed, 1u);
    check_eq("ram_probe_split_timed_second_completion",
             ram_dma_events[3].completed, 1u);
    g_runtime_ram_image_dma_probe = nullptr;
}

struct DmaDescriptorEvent {
    int channel = -1;
    uint32_t pc = 0;
    uint32_t source = 0;
    uint32_t destination = 0;
    uint32_t bytes = 0;
    uint16_t control = 0;
    int start_mode = -1;
};
DmaDescriptorEvent descriptor_event{};
unsigned descriptor_events = 0;

void record_dma_descriptor(int channel, uint32_t pc, uint32_t source,
                           uint32_t destination, uint32_t bytes,
                           uint16_t control, int start_mode) {
    ++descriptor_events;
    descriptor_event = {channel, pc, source, destination, bytes, control,
                        start_mode};
}

void test_payload_free_dma_descriptor_observer() {
    gba::vram_trace::set_dma_descriptor_observer(&record_dma_descriptor);
    gba::vram_trace::trace_dma(nullptr, 2, 0x08001234u, 0x02001000u,
                              0x02020000u, 0x8000u, 0x8400u, 1);
    check_eq("dma_descriptor_count", descriptor_events, 1u);
    check_eq("dma_descriptor_channel", descriptor_event.channel, 2u);
    check_eq("dma_descriptor_pc", descriptor_event.pc, 0x08001234u);
    check_eq("dma_descriptor_source", descriptor_event.source, 0x02001000u);
    check_eq("dma_descriptor_destination", descriptor_event.destination,
             0x02020000u);
    check_eq("dma_descriptor_bytes", descriptor_event.bytes, 0x8000u);
    check_eq("dma_descriptor_control", descriptor_event.control, 0x8400u);
    check_eq("dma_descriptor_start_mode", descriptor_event.start_mode, 1u);
    gba::vram_trace::set_dma_descriptor_observer(nullptr);

    unsigned rearms = 0;
    while (gba::vram_trace::rearm_bounded_window()) ++rearms;
    check_eq("vram_trace_bounded_rearms", rearms, 15u);
    check_eq("vram_trace_rearm_stays_exhausted",
             gba::vram_trace::rearm_bounded_window(), 0u);
}

void test_payload_free_oam_handoff_trace() {
    TestBus bus;
    gba::vram_trace::reset_oam_trace_window();
    // One enabled, non-empty sprite and one explicitly disabled slot. The
    // trace reads only post-DMA attributes to produce counters; it does not
    // retain or print these values.
    bus.write16(0x07000000u, 0x0000u);
    bus.write16(0x07000002u, 10u);
    bus.write16(0x07000004u, 1u);
    bus.write16(0x07000008u, 0x0200u);
    gba::vram_trace::trace_oam_dma(
        &bus, 3, 0x080036A4u, 0x02010000u, 0x07000000u, 16u, 0x8000u, 0);

    gba::vram_trace::OamDmaTraceStats dma{};
    gba::vram_trace::get_oam_dma_trace_stats(&dma);
    check_eq("oam_dma_transfer_count", static_cast<uint32_t>(dma.transfers), 1u);
    check_eq("oam_dma_used_slots", dma.last_used_slots, 1u);
    check_eq("oam_dma_visible_slots", dma.last_visible_slots, 1u);
    check_eq("oam_dma_nonzero_slots", dma.last_nonzero_slots, 2u);
    check_eq("oam_dma_raw_x_count", dma.last_raw_x_ge_240, 0u);
    check_eq("oam_dma_raw_y_count", dma.last_raw_y_ge_160, 0u);

    gba::vram_trace::trace_oam_shadow_write(
        0x08001234u, 0x0300347Cu, 8u);
    gba::vram_trace::trace_oam_shadow_write(
        0x08001236u, 0x0300347Cu, 8u);
    gba::vram_trace::trace_oam_shadow_dma(
        nullptr, 3, 0x080036A4u, 0x02010000u, 0x03003484u, 8u, 0x8000u, 0);
    gba::vram_trace::OamShadowTraceStats shadow{};
    gba::vram_trace::get_oam_shadow_trace_stats(&shadow);
    check_eq("oam_shadow_all_write_calls",
             static_cast<uint32_t>(shadow.write_calls), 3u);
    check_eq("oam_shadow_unique_slots",
             static_cast<uint32_t>(shadow.unique_slots), 2u);
    check_eq("oam_shadow_slot_overwrites",
             static_cast<uint32_t>(shadow.slot_overwrite_events), 1u);
    check_eq("oam_shadow_overwritten_slots",
             static_cast<uint32_t>(shadow.overwritten_slots), 1u);
    check_eq("oam_shadow_dma_write_calls",
             static_cast<uint32_t>(shadow.dma_write_calls), 1u);

    // Exact measured producer + handoff: the 708-byte producer covers shadow
    // slots 0..88, then the final 1024-byte shadow->OAM DMA presents all 128
    // slots. A raw-192 candidate in unseen slot 89 must remain unseen.
    gba::vram_trace::reset_oam_trace_window();
    gba::vram_trace::trace_oam_shadow_dma(
        &bus, 3, 0x0800C674u, 0x08009BB8u, 0x0300347Cu, 708u,
        0x8400u, 0);
    bus.write16(0x07000000u, 192u);
    bus.write16(0x07000008u, 193u);
    bus.write16(0x070002C8u, 192u);  // slot 89, outside the producer span
    gba::vram_trace::trace_oam_dma(
        &bus, 3, 0x080036A4u, 0x0300347Cu, 0x07000000u, 1024u,
        0x8400u, 0);
    gba::vram_trace::OamAttr0TraceStats attr0{};
    gba::vram_trace::get_oam_attr0_trace_stats(&attr0);
    check_eq("oam_attr0_exact_copy", static_cast<uint32_t>(attr0.post_copies), 1u);
    check_eq("oam_attr0_candidates", static_cast<uint32_t>(attr0.candidate_slots), 3u);
    check_eq("oam_attr0_exact_192", static_cast<uint32_t>(attr0.exact_192_slots), 2u);
    check_eq("oam_attr0_other", static_cast<uint32_t>(attr0.other_slots), 1u);
    check_eq("oam_attr0_unseen", static_cast<uint32_t>(attr0.unseen_slots), 1u);
    gba::vram_trace::OamAttr0Provenance provenance{};
    check_eq("oam_attr0_provenance_slot0",
          gba::vram_trace::get_oam_attr0_provenance(0u, &provenance) &&
          provenance.kind == gba::vram_trace::OamAttr0WriterKind::Dma, 1u);
    check_eq("oam_attr0_slot89_unseen",
          gba::vram_trace::get_oam_attr0_provenance(89u, &provenance) &&
          provenance.kind == gba::vram_trace::OamAttr0WriterKind::Unseen, 1u);
    gba::vram_trace::trace_oam_shadow_write(
        0x08001234u, 0x0300347Du, 1u);
    check_eq("oam_attr0_cpu_overwrite",
          gba::vram_trace::get_oam_attr0_provenance(0u, &provenance) &&
          provenance.kind == gba::vram_trace::OamAttr0WriterKind::Cpu &&
          provenance.writer_pc == 0x08001234u && provenance.touched_bytes == 2u,
          1u);
}

void test_mode0_field_map_trace_scope() {
    gba::GbaIo io;

    // Mode 0, BG1 enabled with a 256x256 map at screenblock 3.
    io.write16(gba::IoReg::DISPCNT, 1u << 9);
    // Priority/color fields are intentionally non-zero; map size lives in
    // bits 14..15, not in the low priority bits.
    io.write16(gba::IoReg::BG1CNT, 0x0709u);
    check_eq("map_trace_bg1_start",
             gba::vram_trace::overlaps_active_mode0_bg123(
                 io.raw(), 0x06003800u, 2u), 1u);
    check_eq("map_trace_bg1_next_block",
             gba::vram_trace::overlaps_active_mode0_bg123(
                 io.raw(), 0x06004000u, 2u), 0u);

    // A vertical 512x256 map uses screenblocks N and N+2.
    io.write16(gba::IoReg::DISPCNT, 1u << 10);
    io.write16(gba::IoReg::BG2CNT, (5u << 8) | (2u << 14));
    check_eq("map_trace_bg2_vertical_second",
             gba::vram_trace::overlaps_active_mode0_bg123(
                 io.raw(), 0x06003800u, 2u), 1u);
    check_eq("map_trace_bg2_vertical_gap",
             gba::vram_trace::overlaps_active_mode0_bg123(
                 io.raw(), 0x06003000u, 2u), 0u);

    // DMA scope uses the same live screenblocks and catches a range crossing
    // the map boundary without retaining any transfer payload.
    check_eq("map_trace_dma_crossing",
             gba::vram_trace::dma_overlaps_active_mode0_bg123(
                 io.raw(), 0x06003FFCu, 4u, 2u, 0u), 1u);
    check_eq("map_trace_dma_outside",
             gba::vram_trace::dma_overlaps_active_mode0_bg123(
                 io.raw(), 0x06004000u, 4u, 2u, 0u), 0u);
}

}  // namespace

int main() {
    // Arm the opt-in payload-free trace before any DMA path can cache the
    // environment decision. The production runner leaves this unset unless
    // the WIDE-01 diagnostics toggle is selected.
#if defined(_WIN32)
    _putenv_s("GBARECOMP_VRAM_MAP_TRACE", "1");
#else
    setenv("GBARECOMP_VRAM_MAP_TRACE", "1", 1);
#endif
    test_immediate_word_alignment();
    test_immediate_halfword_alignment();
    test_timed_word_alignment();
    test_probe_detects_partial_immediate_dma();
    test_probe_detects_split_timed_dma();
    test_mode0_field_map_trace_scope();
    test_payload_free_dma_descriptor_observer();
    test_payload_free_oam_handoff_trace();
    if (failures) {
        std::printf("dma_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("dma_tests: OK\n");
    return 0;
}
