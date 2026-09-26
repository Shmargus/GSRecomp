// Bounded VRAM map-writer diagnostics used by widescreen map investigation.
// The trace is opt-in and records only metadata, never guest bytes.

#pragma once

#include <cstddef>
#include <cstdint>

namespace armv4t { struct Bus; }

namespace gba::vram_trace {

// GBA VRAM is the 96 KiB window 0x06000000..0x06017FFF.
constexpr std::uint64_t kVramStart = 0x06000000ull;
constexpr std::uint64_t kVramEnd   = 0x06018000ull;

// Return whether [address, address+size) intersects the physical VRAM
// window. The widened arithmetic makes malformed/overflowing diagnostics
// fail closed instead of wrapping into the window.
constexpr bool overlaps(std::uint32_t address, std::uint64_t size) {
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    return first < kVramEnd && last > kVramStart;
}

// Return whether a DMA destination sequence intersects VRAM. `step` is the
// transfer unit (2 or 4), and `dest_control` follows DMA CNT_H: 0 increment,
// 1 decrement, 2 fixed, 3 increment/reload (increment for this block).
constexpr bool dma_overlaps(std::uint32_t destination,
                            std::uint32_t step,
                            std::uint32_t units,
                            std::uint32_t dest_control) {
    if (step == 0 || units == 0) return false;
    const std::uint64_t span = static_cast<std::uint64_t>(step) * units;
    std::uint64_t first = destination;
    std::uint64_t last = destination;
    if (dest_control == 1u) {
        // The first transfer lands at destination; the sequence walks down.
        first = destination >= span - step
            ? static_cast<std::uint64_t>(destination) - (span - step)
            : 0u;
        last = static_cast<std::uint64_t>(destination) + step;
    } else if (dest_control == 2u) {
        last = static_cast<std::uint64_t>(destination) + step;
    } else {
        last = static_cast<std::uint64_t>(destination) + span;
    }
    return first < kVramEnd && last > kVramStart;
}

// The live Mode 0 field maps are derived from the guest's current IO image,
// not from a game-specific address guess. `io` points at GBA IO offset 0.
bool overlaps_active_mode0_bg123(const std::uint8_t* io,
                                  std::uint32_t address,
                                  std::uint64_t size);
bool dma_overlaps_active_mode0_bg123(const std::uint8_t* io,
                                     std::uint32_t destination,
                                     std::uint32_t step,
                                     std::uint32_t units,
                                     std::uint32_t dest_control);

// Mode 2 affine sibling of the Mode 0 field predicates above. Affine BG2/BG3
// maps are one contiguous run of (16 << size_code)^2 bytes (one byte per map
// entry, no 2-byte text-mode flip/palette bits) starting at the BGCNT screen
// base; both the base and size code are read live from BGCNT, never assumed.
bool overlaps_active_affine_bg23(const std::uint8_t* io,
                                 std::uint32_t address,
                                 std::uint64_t size);
bool dma_overlaps_active_affine_bg23(const std::uint8_t* io,
                                     std::uint32_t destination,
                                     std::uint32_t step,
                                     std::uint32_t units,
                                     std::uint32_t dest_control);

// Game runners may arm the bounded probe by default for a temporary,
// identity-gated investigation build. An explicit
// GBARECOMP_VRAM_MAP_TRACE=0 still disables it.
void set_default_enabled(bool enabled);

// Enable the payload-free BG0 text-layer write trace in the supplied session
// directory. The game-side text recorder owns the directory and calls this
// only while GSR_TEXT_RECORD is enabled. The resulting
// `text_vram_writes.csv` contains CPU/DMA writer metadata and live BG0
// register/range metadata, never guest VRAM contents. `address` and
// `range_size` describe the touched address span; DMA rows also preserve the
// descriptor's transfer length in `dma_bytes`.
void set_text_trace_directory(const char* directory);

// Start a fresh bounded diagnostic window. This is intentionally generic:
// game policy decides what constitutes a scene/authentication window. At most
// 16 windows can be armed in one process, so repeated scene transitions cannot
// turn the metadata-only probe into unbounded logging. Returns false once the
// lifetime window budget is exhausted.
bool rearm_bounded_window();

// Optional payload-free DMA descriptor observer for game-owned provenance
// policy. It receives metadata before the transfer and never owns or exposes
// guest bytes. The observer is independent of the VRAM-map print filter.
using DmaDescriptorObserver = void (*)(int channel, std::uint32_t pc,
    std::uint32_t source, std::uint32_t destination, std::uint32_t bytes,
    std::uint16_t control, int start_mode);
void set_dma_descriptor_observer(DmaDescriptorObserver observer);

// Optional payload-free callback after an IWRAM OAM-shadow write commits.
// The callback receives only writer PC/address/size; the owning runner may
// inspect its own live context independently of diagnostic print tracing.
using OamShadowWriteObserver = void (*)(std::uint32_t pc,
    std::uint32_t address, std::uint32_t size);
void set_oam_shadow_write_observer(OamShadowWriteObserver observer);

// Optional payload-free observer of every CPU write and DMA into palette RAM
// or VRAM: writer PC, destination, byte count, DMA source (0 for a CPU
// write) and DMA channel (-1 for a CPU write). DMA-driven stores are
// reported once per transfer, not per unit. Null by default.
using VideoWriteObserver = void (*)(std::uint32_t pc, std::uint32_t address,
                                    std::uint32_t bytes, std::uint32_t source,
                                    int channel);
void set_video_write_observer(VideoWriteObserver observer);
// Palette RAM CPU writes, from gba_bus.cpp (VRAM ones use trace_cpu_write).
void note_palette_cpu_write(std::uint32_t pc, std::uint32_t address,
                            std::uint32_t size);

// Optional payload-free range gate for the committed observer. When set, it
// runs before the observer and keeps game-specific callbacks from seeing
// unrelated IWRAM writes. A null gate preserves the observer's prior
// behavior for generic users.
using OamShadowWriteRangePredicate = bool (*)(std::uint32_t address,
                                              std::uint32_t size);
void set_oam_shadow_write_range_predicate(
    OamShadowWriteRangePredicate predicate);

// The hooks are no-ops unless a runner arms the default or
// GBARECOMP_VRAM_MAP_TRACE is set. Each stream is independently bounded; only
// writer PC/address/size (CPU) or source/dest/size (DMA), plus BG1-3 CNT/scroll
// metadata, is printed. Mode 0 field hits print as [vram-map-cpu]/
// [vram-map-dma]; Mode 2 affine BG2/BG3 hits print as [vram-affine-cpu]/
// [vram-affine-dma] with their own bounded counters so one field cannot
// exhaust the other's record budget. The independent text trace writes
// metadata CSV rows for the selected BG0 character/screen-map ranges in
// modes 0/1, including while BG0 is hidden, when enabled through
// set_text_trace_directory().
void trace_cpu_write(const std::uint8_t* io, std::uint32_t pc,
                     std::uint32_t address, std::uint32_t size);
void trace_dma(const std::uint8_t* io, int channel, std::uint32_t pc,
               std::uint32_t source, std::uint32_t destination,
               std::uint32_t bytes, std::uint16_t control, int start_mode);

// Payload-free OAM handoff diagnostics. A completed DMA whose destination
// intersects OAM is summarized after the copy, so `used_slots` describes the
// actual post-DMA OAM image rather than the pre-transfer destination. The
// shadow counters treat a slot touched by a later write as an overwrite; no
// attribute or tile values are retained or printed.
struct OamShadowTraceStats {
    std::uint64_t write_calls = 0;
    std::uint64_t bytes = 0;
    std::uint64_t dma_write_calls = 0;
    std::uint64_t dma_bytes = 0;
    std::uint64_t slot_write_events = 0;
    std::uint64_t slot_overwrite_events = 0;
    std::uint64_t unique_slots = 0;
    std::uint64_t overwritten_slots = 0;
    std::uint64_t records_dropped = 0;
};

struct OamDmaTraceStats {
    std::uint64_t transfers = 0;
    std::uint64_t bytes = 0;
    std::uint64_t used_slot_total = 0;
    std::uint64_t visible_slot_total = 0;
    std::uint64_t nonzero_slot_total = 0;
    std::uint64_t records_dropped = 0;
    std::uint32_t last_source = 0;
    std::uint32_t last_destination = 0;
    std::uint32_t last_size = 0;
    std::uint32_t last_used_slots = 0;
    std::uint32_t last_visible_slots = 0;
    std::uint32_t last_nonzero_slots = 0;
    std::uint32_t last_raw_x_ge_240 = 0;
    std::uint32_t last_raw_y_ge_160 = 0;
};

enum class OamAttr0WriterKind : std::uint8_t {
    Unseen = 0,
    Cpu = 1,
    Dma = 2,
};

// Latest ATTR0 provenance for each OAM-shadow slot. This is metadata only;
// no attribute/tile words are retained here.
struct OamAttr0Provenance {
    OamAttr0WriterKind kind = OamAttr0WriterKind::Unseen;
    std::uint32_t writer_pc = 0;
    std::uint64_t generation = 0;
    std::uint64_t cycle = 0;
    std::uint8_t touched_bytes = 0;
};

struct OamAttr0CandidateGroup {
    std::uint8_t raw_y = 0;
    OamAttr0WriterKind kind = OamAttr0WriterKind::Unseen;
    std::uint32_t writer_pc = 0;
    std::uint32_t slot_first = 0;
    std::uint32_t slot_last = 0;
    std::uint32_t slot_count = 0;
    std::uint64_t generation = 0;
    std::uint64_t cycle = 0;
};

struct OamAttr0TraceStats {
    std::uint64_t post_copies = 0;
    std::uint64_t candidate_slots = 0;
    std::uint64_t exact_192_slots = 0;
    std::uint64_t other_slots = 0;
    std::uint64_t unseen_slots = 0;
    std::uint64_t groups_dropped = 0;
    std::uint32_t group_count = 0;
    OamAttr0CandidateGroup groups[64]{};
};

void reset_oam_trace_window();
void get_oam_shadow_trace_stats(OamShadowTraceStats* out);
void get_oam_dma_trace_stats(OamDmaTraceStats* out);
void get_oam_attr0_trace_stats(OamAttr0TraceStats* out);
bool get_oam_attr0_provenance(std::size_t slot,
                              OamAttr0Provenance* out);

// Called after a DMA block has committed. It is a no-op unless the opt-in OAM
// trace is enabled and the destination intersects 0x07000000..0x070003FF.
void trace_oam_dma(armv4t::Bus* bus, int channel, std::uint32_t pc,
                   std::uint32_t source, std::uint32_t destination,
                   std::uint32_t bytes, std::uint16_t control,
                   int start_mode);

// Equivalent post-transfer summary for the measured IWRAM OAM shadow range.
// Decrement/fixed DMA destination modes are reduced to the touched contiguous
// range before slot accounting; transfer payload is never retained.
void trace_oam_shadow_dma(armv4t::Bus* bus, int channel, std::uint32_t pc,
                          std::uint32_t source, std::uint32_t destination,
                          std::uint32_t bytes, std::uint16_t control,
                          int start_mode);

// DMA writes go through GbaBus too. Bracket them so they are reported once as
// a descriptor rather than once per copied unit as CPU stores.
void begin_dma();

// Independent bounded probe: identify the writer(s) of the field-object OAM
// shadow buffer (IWRAM 0x0300347C..0x0300387C), which the ROM 0x080036A4
// DMA later copies into real OAM 0x07000000. Opt-in only via the launcher's
// OAM shadow writer trace toggle (GSR_OAM_SHADOW_TRACE); off by default and
// bounded independently of the VRAM map probe above.
void trace_oam_shadow_write(std::uint32_t pc, std::uint32_t address,
                            std::uint32_t size);
// A registered range predicate is applied before the committed observer, so
// unrelated IWRAM writes never reach a game-owned callback.
void trace_oam_shadow_write_committed(std::uint32_t pc,
                                      std::uint32_t address,
                                      std::uint32_t size);
void end_dma();
bool dma_active();

}  // namespace gba::vram_trace
