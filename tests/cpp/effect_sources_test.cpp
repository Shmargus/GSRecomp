// Does the particle reader find real sparks, and refuse everything else?
//
// The refusal half matters more than the finding half. 0x02010000 and the
// structure around it are shared scratch: the same bytes are a particle array
// while one system runs and a staging image while another does (FACTS.md,
// 2026-09-18). A reader that trusts an address would draw an image buffer as a
// spray of sparks. So every test below that builds plausible memory is paired
// with one that builds memory which must be rejected.

#include "effect_sources.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::size_t kEwramBytes = 256 * 1024;
constexpr std::size_t kIwramBytes = 32 * 1024;
constexpr std::uint32_t kStructure = 0x02035924u;

struct Memory {
    std::vector<std::uint8_t> ewram = std::vector<std::uint8_t>(kEwramBytes, 0);
    std::vector<std::uint8_t> iwram = std::vector<std::uint8_t>(kIwramBytes, 0);

    gsr::GuestMemory view() const {
        gsr::GuestMemory m;
        m.ewram = ewram.data();
        m.ewram_bytes = ewram.size();
        m.iwram = iwram.data();
        m.iwram_bytes = iwram.size();
        return m;
    }
    void put32(std::vector<std::uint8_t>& block, std::size_t at,
               std::uint32_t v) {
        block[at] = static_cast<std::uint8_t>(v);
        block[at + 1] = static_cast<std::uint8_t>(v >> 8);
        block[at + 2] = static_cast<std::uint8_t>(v >> 16);
        block[at + 3] = static_cast<std::uint8_t>(v >> 24);
    }
    void set_structure(std::uint32_t pointer) {
        put32(iwram, gsr::kStructurePointer - gsr::kIwramBase, pointer);
    }
    void write_record(std::uint32_t address, std::int32_t x, std::int32_t y,
                      std::int32_t life) {
        const std::size_t at = address - gsr::kEwramBase;
        put32(ewram, at + gsr::kRecordX, static_cast<std::uint32_t>(x));
        put32(ewram, at + gsr::kRecordY, static_cast<std::uint32_t>(y));
        put32(ewram, at + gsr::kRecordLife, static_cast<std::uint32_t>(life));
    }
};

void test_reads_the_offset_system() {
    Memory m;
    m.set_structure(kStructure);
    const std::uint32_t array = kStructure + gsr::kOffset16Array;
    // (v >> 10) + 64: a value of 10*1024 lands at x = 74.
    for (int i = 0; i < 12; ++i) {
        m.write_record(array + i * gsr::kRecordBytes, (i - 6) * 1024,
                       (i - 3) * 1024, i % 5);
    }
    const gsr::EffectRead read =
        gsr::read_sparks(m.view(), gsr::EffectSource::Offset16, 128, 128);
    assert(read.source == gsr::EffectSource::Offset16);
    assert(read.live == 12);
    // Record 0: (0 - 6) * 1024 >> 10 = -6, + 64 = 58.
    assert(read.sparks[0].x == 58);
    assert(read.sparks[0].y == 61);
}

void test_reads_the_cartesian_system() {
    Memory m;
    m.set_structure(kStructure);
    const std::uint32_t array = kStructure + gsr::kCartesianArray;
    // 16.16: 40 << 16 is x = 40.
    for (int i = 0; i < 20; ++i) {
        m.write_record(array + i * gsr::kRecordBytes, (20 + i) << 16,
                       (30 + i) << 16, 3);
    }
    const gsr::EffectRead read =
        gsr::read_sparks(m.view(), gsr::EffectSource::Cartesian, 256, 120);
    assert(read.source == gsr::EffectSource::Cartesian);
    assert(read.live == 20);
    assert(read.sparks[0].x == 20);
    assert(read.sparks[0].y == 30);
}

void test_refuses_a_staging_image() {
    // The case this reader exists to survive: the array address holds image
    // pixels, not records. Bytes of artwork read as 32-bit words give enormous
    // coordinates, which must be refused rather than drawn.
    Memory m;
    m.set_structure(kStructure);
    const std::uint32_t array = kStructure + gsr::kCartesianArray;
    for (std::size_t i = 0; i < 64 * gsr::kRecordBytes; ++i) {
        m.ewram[array - gsr::kEwramBase + i] =
            static_cast<std::uint8_t>(0x20 + (i * 7) % 0xC0);
    }
    const gsr::EffectRead read =
        gsr::read_sparks(m.view(), gsr::EffectSource::Cartesian, 256, 120);
    assert(read.source == gsr::EffectSource::None);
    assert(read.sparks.empty());
}

void test_refuses_an_empty_array() {
    Memory m;
    m.set_structure(kStructure);
    const gsr::EffectRead read =
        gsr::read_sparks(m.view(), gsr::EffectSource::Offset16, 128, 128);
    assert(read.source == gsr::EffectSource::None);
}

void test_refuses_a_structure_pointer_that_is_not_set_up() {
    Memory m;
    m.set_structure(0);  // no effect running
    const gsr::EffectRead read =
        gsr::read_sparks(m.view(), gsr::EffectSource::Offset16, 128, 128);
    assert(read.source == gsr::EffectSource::None);
    Memory stale;
    stale.set_structure(0x03007F00u);  // an IWRAM value where EWRAM belongs
    assert(gsr::read_sparks(stale.view(), gsr::EffectSource::Offset16, 128, 128)
               .source == gsr::EffectSource::None);
}

void test_too_few_live_records_is_not_believed() {
    Memory m;
    m.set_structure(kStructure);
    const std::uint32_t array = kStructure + gsr::kOffset16Array;
    // Below the minimum: could be luck inside unrelated memory.
    for (int i = 0; i < 3; ++i)
        m.write_record(array + i * gsr::kRecordBytes, 1024, 1024, 1);
    assert(gsr::read_sparks(m.view(), gsr::EffectSource::Offset16, 128, 128)
               .source == gsr::EffectSource::None);
}

void test_detect_picks_the_live_system() {
    Memory m;
    m.set_structure(kStructure);
    const std::uint32_t array = kStructure + gsr::kCartesianArray;
    for (int i = 0; i < 20; ++i)
        m.write_record(array + i * gsr::kRecordBytes, (40 + i) << 16, 50 << 16,
                       2);
    const gsr::EffectRead read = gsr::detect_and_read(m.view(), 256, 120);
    assert(read.source == gsr::EffectSource::Cartesian);
    assert(read.live == 20);
}

void test_sparks_outside_the_canvas_are_kept() {
    // The entire point: a particle the game would have clipped must survive
    // the read, because drawing it is what this is for.
    Memory m;
    m.set_structure(kStructure);
    const std::uint32_t array = kStructure + gsr::kCartesianArray;
    for (int i = 0; i < 12; ++i)
        m.write_record(array + i * gsr::kRecordBytes, (-40 + i) << 16, 60 << 16,
                       1);
    const gsr::EffectRead read =
        gsr::read_sparks(m.view(), gsr::EffectSource::Cartesian, 128, 128);
    assert(read.source == gsr::EffectSource::Cartesian);
    assert(read.live == 12);
    assert(read.sparks[0].x == -40);  // off the game's canvas, kept by us
}

}  // namespace

int main() {
    test_reads_the_offset_system();
    test_reads_the_cartesian_system();
    test_refuses_a_staging_image();
    test_refuses_an_empty_array();
    test_refuses_a_structure_pointer_that_is_not_set_up();
    test_too_few_live_records_is_not_believed();
    test_detect_picks_the_live_system();
    test_sparks_outside_the_canvas_are_kept();
    std::printf("effect_sources: all checks passed\n");
    return 0;
}
