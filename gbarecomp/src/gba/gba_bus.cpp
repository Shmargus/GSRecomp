// gba_bus.cpp — see gba_bus.h.
//
// Region dispatch follows `gba_memory.h::classify()` and uses the
// canonical mirror behavior from `resolve_offset()`. The hot loop
// reads/writes directly into the backing array for each region;
// BIOS reads go through `GbaBios::read*()` so the loader can refuse
// to serve bytes before the hash is verified.

#include "gba_bus.h"
#include "gba_vram_trace.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

// Shared with the ARM fast-RAM accessors.  GbaBus is also the chokepoint for
// DMA and other non-generated writes, so those stores must invalidate the
// runtime's cached RAM-overlay identities too.
extern "C" unsigned long long g_ram_write_epoch;
extern "C" unsigned long long g_ram_code_page_mask_ewram_lo;
extern "C" unsigned long long g_ram_code_page_mask_ewram_hi;
extern "C" unsigned int g_ram_code_page_mask_iwram;
extern "C" unsigned int g_ram_code_page_epoch_ewram[64];
extern "C" unsigned int g_ram_code_page_epoch_iwram[8];
extern "C" void runtime_note_ram_code_write(uint32_t addr, uint32_t width);
extern "C" void runtime_note_ram_image_bus_write(uint32_t addr,
                                                   uint32_t width);
extern "C" void runtime_ram_code_dirty_reset(void);
extern "C" uint32_t runtime_current_pc(void);

// Protected BIOS-read inventory (BIOS removal plan, step 1). When
// GBARECOMP_BIOS_READ_LOG names a file, every data read of 0x0000..0x3FFF
// made while BIOS access is locked (the PC is outside the BIOS) is counted
// by reading PC, address and width, and saved at exit. Addresses and counts
// only: the value such a read returns is BIOS-derived and is not recorded.
namespace {
struct BiosReadKey {
    uint32_t pc, addr, width;
    bool operator<(const BiosReadKey& o) const {
        return std::tie(pc, addr, width) < std::tie(o.pc, o.addr, o.width);
    }
};
constexpr std::size_t kBiosReadLogMaxKeys = 100000;
std::map<BiosReadKey, unsigned long long> g_bios_read_counts;
unsigned long long g_bios_read_dropped = 0;

bool bios_read_log_armed() {
    static const bool armed = [] {
        const char* p = std::getenv("GBARECOMP_BIOS_READ_LOG");
        return p != nullptr && p[0] != '\0' && p[0] != '0';
    }();
    return armed;
}

void note_protected_bios_read(uint32_t addr, uint32_t width) {
    if (!bios_read_log_armed()) return;
    const BiosReadKey key{runtime_current_pc(), addr, width};
    auto it = g_bios_read_counts.find(key);
    if (it != g_bios_read_counts.end()) {
        ++it->second;
    } else if (g_bios_read_counts.size() < kBiosReadLogMaxKeys) {
        g_bios_read_counts.emplace(key, 1ull);
    } else {
        ++g_bios_read_dropped;
    }
}
}  // namespace

namespace gba {
uint32_t bios_read_log_save_file(const char* path) {
    if (!path || !bios_read_log_armed()) return 0;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return 0;
    std::fprintf(f, "pc,addr,width,count\n");
    uint32_t rows = 0;
    for (const auto& [key, count] : g_bios_read_counts) {
        std::fprintf(f, "0x%08x,0x%08x,%u,%llu\n", key.pc, key.addr,
                     key.width, count);
        ++rows;
    }
    if (g_bios_read_dropped != 0)
        std::fprintf(f, "# dropped %llu reads past the %zu-key cap\n",
                     g_bios_read_dropped, kBiosReadLogMaxKeys);
    std::fclose(f);
    return rows;
}
}  // namespace gba

static bool ram_code_write_affects_bus(uint32_t addr, uint32_t width) {
    const uint32_t region = addr >> 24;
    if (region == 0x03u) {
        const uint32_t off = addr & 0x00007FFFu;
        const uint32_t first = off >> 12;
        const uint32_t last = (off + width - 1u) >> 12;
        const unsigned int span = (1u << (last - first + 1u)) - 1u;
        return (g_ram_code_page_mask_iwram & (span << first)) != 0u;
    }
    if (region == 0x02u) {
        const uint32_t off = addr & 0x0003FFFFu;
        const uint32_t first = off >> 12;
        const uint32_t last = (off + width - 1u) >> 12;
        bool hit = false;
        if (first < 32u) {
            const uint32_t low_last = last < 31u ? last : 31u;
            const unsigned long long span =
                (1ull << (low_last - first + 1u)) - 1ull;
            hit = (g_ram_code_page_mask_ewram_lo & (span << first)) != 0ull;
        }
        if (last >= 32u) {
            const uint32_t high_first = first > 32u ? first - 32u : 0u;
            const uint32_t high_last = last - 32u;
            const unsigned long long span =
                (1ull << (high_last - high_first + 1u)) - 1ull;
            hit = hit || ((g_ram_code_page_mask_ewram_hi &
                           (span << high_first)) != 0ull);
        }
        return hit;
    }
    return false;
}

static inline void note_ram_code_write_bus(uint32_t addr, uint32_t width) {
    if (!ram_code_write_affects_bus(addr, width)) return;
    runtime_note_ram_code_write(addr, width);
    ++g_ram_write_epoch;
    const uint32_t region = addr >> 24;
    if (region == 0x03u) {
        const uint32_t off = addr & 0x00007FFFu;
        const uint32_t first = off >> 12;
        const uint32_t last = (off + width - 1u) >> 12;
        for (uint32_t page = first; page <= last && page < 8u; ++page)
            ++g_ram_code_page_epoch_iwram[page];
    } else if (region == 0x02u) {
        const uint32_t off = addr & 0x0003FFFFu;
        const uint32_t first = off >> 12;
        const uint32_t last = (off + width - 1u) >> 12;
        for (uint32_t page = first; page <= last && page < 64u; ++page)
            ++g_ram_code_page_epoch_ewram[page];
    }
}

// The lifecycle probe is deliberately called after the RAM bytes commit. It
// covers interpreter/host bus writes; generated stores are observed by their
// existing runtime_trace_event hook, with the generated slow path suppressing
// this callback around its GbaBus call.

#include "snapshot.h"

namespace gba {

extern "C" int (*g_rom_read32_override)(std::uint32_t, std::uint32_t,
                                         std::uint32_t*) = nullptr;
extern "C" void (*g_ws_ewram_write_observer)(std::uint32_t,
                                             std::uint32_t) = nullptr;

GbaBus::GbaBus() {
    io_dispatch_.set_audio(&audio_);
}
GbaBus::~GbaBus() = default;

void GbaBus::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    w.bytes(ewram_.data(), ewram_.size());
    w.bytes(iwram_.data(), iwram_.size());
    w.bytes(pal_.data(),   pal_.size());
    w.bytes(vram_.data(),  vram_.size());
    w.bytes(oam_.data(),   oam_.size());
    w.u32(last_fetched_);
    w.boolean(bios_access_enabled_);
    w.u64(unmapped_count_);
}

void GbaBus::deserialize(gbarecomp::debug::SnapshotReader& r) {
    r.bytes(ewram_.data(), ewram_.size());
    r.bytes(iwram_.data(), iwram_.size());
    r.bytes(pal_.data(),   pal_.size());
    r.bytes(vram_.data(),  vram_.size());
    r.bytes(oam_.data(),   oam_.size());
    last_fetched_        = r.u32();
    bios_access_enabled_ = r.boolean();
    unmapped_count_      = static_cast<std::size_t>(r.u64());
    // A savestate replaces RAM without going through write8/16/32.  Advance
    // the same generation so any cached transient-image identity is rechecked
    // before guest execution resumes.
    ++g_ram_write_epoch;
    for (unsigned int& epoch : g_ram_code_page_epoch_ewram) ++epoch;
    for (unsigned int& epoch : g_ram_code_page_epoch_iwram) ++epoch;
    runtime_ram_code_dirty_reset();
}

namespace {

// Helper: read a little-endian halfword/word from a byte buffer at
// the given offset. Caller has already bounds-checked.
uint16_t load_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t load_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) |
                                 (p[2] << 16) | (p[3] << 24));
}
void store_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
void store_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

bool is_eeprom_addr(uint32_t addr, const GbaSave& save) {
    return save.eeprom_enabled() && ((addr >> 24) == 0x0Du);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Reads
// ─────────────────────────────────────────────────────────────────────

// GBA open-bus: the value a protected-BIOS / unmapped read returns is the
// recently pre-fetched opcode. We read it straight from the BIOS/ROM image at
// the prefetch slot (ARM: word at PC+8; THUMB: the PC+4 halfword mirrored into
// both halves, per GBATEK). The prefetch source is the same image the CPU is
// fetching from — never recurse through the bus accessors.
uint32_t GbaBus::prefetch_word(uint32_t pc, bool thumb) const {
    auto code32 = [&](uint32_t a) -> uint32_t {
        if (a < 0x00004000u) return bios_ ? bios_->read32(a & 0x3FFFu) : 0u;
        if (rom_ && a >= 0x08000000u) {
            uint32_t o = a - 0x08000000u;
            if (o + 3u < rom_size_) return load_u32(&rom_[o]);
        }
        return 0u;
    };
    auto code16 = [&](uint32_t a) -> uint16_t {
        if (a < 0x00004000u) return bios_ ? bios_->read16(a & 0x3FFFu) : uint16_t{0};
        if (rom_ && a >= 0x08000000u) {
            uint32_t o = a - 0x08000000u;
            if (o + 1u < rom_size_) return load_u16(&rom_[o]);
        }
        return 0u;
    };
    if (thumb) {
        uint32_t h = code16(pc + 4u);
        return h | (h << 16);
    }
    return code32(pc + 8u);
}

uint8_t GbaBus::read8(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Bios:
            if (bios_ && bios_access_enabled_)
                return bios_->read8(static_cast<uint32_t>(off));
            note_protected_bios_read(addr, 1u);
            return static_cast<uint8_t>(bios_prefetch_ >> (8u * (addr & 3u)));
        case Region::Ewram: return ewram_[off];
        case Region::Iwram: return iwram_[off];
        case Region::Pal:   return pal_[off];
        case Region::Vram:  if (off < vram_.size()) return vram_[off]; break;
        case Region::Oam:   return oam_[off];
        case Region::Rom: {
            // Cartridge GPIO (RTC) at 0x080000C4..0xC9 when readable; else
            // the bus returns ordinary ROM (write-only GPIO mode).
            if (rtc_.active() && rtc_.read_enabled() && off >= 0xC4u && off <= 0xC9u)
                return rtc_.read(static_cast<uint32_t>(off));
            if (is_eeprom_addr(addr, save_)) {
                return static_cast<uint8_t>(save_.eeprom_read_bit());
            }
            if (rom_ && off < rom_size_) return rom_[off];
            // No-cart open-bus: ROM reads return the cart-address-bus
            // value (per GBATEK § "GBA Cartridge ROM" — when no cart
            // asserts data, the 16-bit address drives the data lines).
            // read16(0x08000000 + 2N) = N. Byte reads pick the right
            // half. Native must match mGBA here for BIOS-only diff.
            uint32_t halfword_index = static_cast<uint32_t>(off >> 1);
            uint16_t hw = static_cast<uint16_t>(halfword_index & 0xFFFFu);
            return (off & 1) ? static_cast<uint8_t>(hw >> 8)
                             : static_cast<uint8_t>(hw & 0xFFu);
        }
        case Region::Io:
            if (write_observer_) write_observer_->on_bus_read(addr);
            return io_dispatch_.read8(static_cast<uint32_t>(off));
        case Region::Save:
            if (save_.sram_enabled())
                return save_.sram_read(static_cast<uint32_t>(off));
            if (save_.flash_enabled())
                return save_.flash_read(static_cast<uint32_t>(off));
            log_unmapped(addr, 0, false, 1);
            return 0;
        case Region::OpenBus:
        case Region::Unknown: {
            uint32_t ob = prefetch_word(open_bus_pc_, open_bus_thumb_);
            log_unmapped(addr, ob, false, 1);
            return static_cast<uint8_t>(ob >> (8u * (addr & 3u)));
        }
    }
    return 0;
}

uint16_t GbaBus::read16(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    switch (region) {
        case Region::Bios:
            if (bios_ && bios_access_enabled_)
                return bios_->read16(static_cast<uint32_t>(off));
            note_protected_bios_read(addr, 2u);
            return static_cast<uint16_t>(bios_prefetch_ >> (8u * (addr & 2u)));
        case Region::Ewram: return load_u16(&ewram_[off]);
        case Region::Iwram: return load_u16(&iwram_[off]);
        case Region::Pal:   return load_u16(&pal_[off]);
        case Region::Vram:
            if (off + 1 < vram_.size()) return load_u16(&vram_[off]);
            break;
        case Region::Oam:   return load_u16(&oam_[off]);
        case Region::Rom: {
            if (rtc_.active() && rtc_.read_enabled() && off >= 0xC4u && off <= 0xC8u) {
                uint32_t o = static_cast<uint32_t>(off);
                return static_cast<uint16_t>(rtc_.read(o) | (rtc_.read(o + 1) << 8));
            }
            if (is_eeprom_addr(addr, save_)) {
                return save_.eeprom_read_bit();
            }
            if (rom_ && off + 1 < rom_size_) return load_u16(&rom_[off]);
            // No-cart open-bus: read16 returns the halfword index.
            return static_cast<uint16_t>((off >> 1) & 0xFFFFu);
        }
        case Region::Io:
            if (write_observer_) write_observer_->on_bus_read(addr);
            return io_dispatch_.read16(static_cast<uint32_t>(off));
        case Region::Save:
            if (save_.sram_enabled()) {
                uint8_t b = save_.sram_read(static_cast<uint32_t>(off));
                return static_cast<uint16_t>(b | (b << 8));  // 8-bit bus mirror
            }
            if (save_.flash_enabled()) {
                uint8_t b = save_.flash_read(static_cast<uint32_t>(off));
                return static_cast<uint16_t>(b | (b << 8));  // 8-bit bus mirror
            }
            log_unmapped(addr, 0, false, 2);
            return 0;
        case Region::OpenBus:
        case Region::Unknown: {
            uint32_t ob = prefetch_word(open_bus_pc_, open_bus_thumb_);
            log_unmapped(addr, ob, false, 2);
            return static_cast<uint16_t>(ob >> (8u * (addr & 2u)));
        }
    }
    return 0;
}

uint32_t GbaBus::read32(uint32_t addr) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    uint32_t v = 0;
    switch (region) {
        case Region::Bios:
            if (bios_ && bios_access_enabled_) {
                v = bios_->read32(static_cast<uint32_t>(off));
                last_fetched_ = v;
                return v;
            }
            note_protected_bios_read(addr, 4u);
            return bios_prefetch_;
        case Region::Ewram: return load_u32(&ewram_[off]);
        case Region::Iwram: return load_u32(&iwram_[off]);
        case Region::Pal:   return load_u32(&pal_[off]);
        case Region::Vram:
            if (off + 3 < vram_.size()) return load_u32(&vram_[off]);
            break;
        case Region::Oam:   return load_u32(&oam_[off]);
        case Region::Rom: {
            if (rtc_.active() && rtc_.read_enabled() && off >= 0xC4u && off <= 0xC6u) {
                uint32_t o = static_cast<uint32_t>(off);
                return rtc_.read(o) | (rtc_.read(o + 1) << 8) |
                       (rtc_.read(o + 2) << 16) | (rtc_.read(o + 3) << 24);
            }
            if (is_eeprom_addr(addr, save_)) {
                return save_.eeprom_read_bit();
            }
            if (rom_ && off + 3 < rom_size_) {
                const uint32_t original = load_u32(&rom_[off]);
                uint32_t overridden = original;
                if (g_rom_read32_override &&
                    g_rom_read32_override(addr, original, &overridden)) {
                    return overridden;
                }
                return original;
            }
            // No-cart open-bus: two consecutive halfwords.
            uint32_t hw_lo = (off >> 1) & 0xFFFFu;
            uint32_t hw_hi = ((off >> 1) + 1) & 0xFFFFu;
            return hw_lo | (hw_hi << 16);
        }
        case Region::Io:
            if (write_observer_) write_observer_->on_bus_read(addr);
            return io_dispatch_.read32(static_cast<uint32_t>(off));
        case Region::Save:
            if (save_.sram_enabled()) {
                uint8_t b = save_.sram_read(static_cast<uint32_t>(off));
                return static_cast<uint32_t>(b) * 0x01010101u;  // 8-bit bus mirror
            }
            if (save_.flash_enabled()) {
                uint8_t b = save_.flash_read(static_cast<uint32_t>(off));
                return static_cast<uint32_t>(b) * 0x01010101u;  // 8-bit bus mirror
            }
            log_unmapped(addr, 0, false, 4);
            return 0;
        case Region::OpenBus:
        case Region::Unknown: {
            uint32_t ob = prefetch_word(open_bus_pc_, open_bus_thumb_);
            log_unmapped(addr, ob, false, 4);
            return ob;
        }
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────
// Writes
// ─────────────────────────────────────────────────────────────────────

bool GbaBus::observe_write(Region region, std::size_t off, uint32_t addr,
                           uint8_t width, uint32_t new_value) {
    BusWriteRegion br;
    const uint8_t* p = nullptr;
    std::size_t cap = 0;
    switch (region) {
        case Region::Ewram: br = BusWriteRegion::Ewram; p = ewram_.data(); cap = ewram_.size(); break;
        case Region::Iwram: br = BusWriteRegion::Iwram; p = iwram_.data(); cap = iwram_.size(); break;
        case Region::Pal:   br = BusWriteRegion::Pal;   p = pal_.data();   cap = pal_.size();   break;
        case Region::Vram:  br = BusWriteRegion::Vram;  p = vram_.data();  cap = vram_.size();  break;
        case Region::Oam:   br = BusWriteRegion::Oam;   p = oam_.data();   cap = oam_.size();   break;
        default:            br = BusWriteRegion::Device; break;
    }
    uint32_t old = 0;
    if (p && off + width <= cap) {
        if (width == 1) old = p[off];
        else if (width == 2) old = uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8);
        else old = uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8) |
                   (uint32_t(p[off + 2]) << 16) | (uint32_t(p[off + 3]) << 24);
    }
    return write_observer_->on_bus_write(br, static_cast<uint32_t>(off), addr,
                                         width, old, new_value);
}

void GbaBus::write8(uint32_t addr, uint8_t v) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    if (write_observer_ && observe_write(region, off, addr, 1, v)) return;
    switch (region) {
        case Region::Ewram:
            if (g_ws_ewram_write_observer) g_ws_ewram_write_observer(addr, 1u);
            ewram_[off] = v;
            note_ram_code_write_bus(addr, 1);
            runtime_note_ram_image_bus_write(addr, 1);
            return;
        case Region::Iwram:
            vram_trace::trace_oam_shadow_write(runtime_current_pc(), addr,
                                               1u);
            iwram_[off] = v;
            vram_trace::trace_oam_shadow_write_committed(runtime_current_pc(),
                                                         addr, 1u);
            note_ram_code_write_bus(addr, 1);
            runtime_note_ram_image_bus_write(addr, 1);
            return;
        case Region::Pal:
            vram_trace::note_palette_cpu_write(runtime_current_pc(), addr, 1u);
            pal_[off] = v;
            return;
        case Region::Vram:
            vram_trace::trace_cpu_write(io_dispatch_.raw(),
                                        runtime_current_pc(), addr, 1u);
            if (off < vram_.size()) vram_[off] = v;
            return;
        case Region::Oam:   oam_[off] = v; return;
        case Region::Bios:
            // BIOS is read-only; hardware ignores writes to this window.
            return;
        case Region::Rom:
            if (rtc_.active() && off >= 0xC4u && off <= 0xC9u) {
                rtc_.write(static_cast<uint32_t>(off), v);
                return;
            }
            if (is_eeprom_addr(addr, save_)) {
                save_.eeprom_write_bit(v);
                return;
            }
            // Cartridge ROM is read-only at write time (writes to ROM
            // are used by some save-chip protocols, but that's the
            // SAVE region, not the ROM region itself).
            log_unmapped(addr, v, true, 1);
            return;
        case Region::Io:
            io_dispatch_.write8(static_cast<uint32_t>(off), v);
            return;
        case Region::Save:
            if (save_.sram_enabled()) {
                save_.sram_write(static_cast<uint32_t>(off), v);
                return;
            }
            if (save_.flash_enabled()) {
                save_.flash_write(static_cast<uint32_t>(off), v);
                return;
            }
            log_unmapped(addr, v, true, 1);
            return;
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, v, true, 1);
            return;
    }
}

void GbaBus::write16(uint32_t addr, uint16_t v) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    if (write_observer_ && observe_write(region, off, addr, 2, v)) return;
    switch (region) {
        case Region::Ewram:
            if (g_ws_ewram_write_observer) g_ws_ewram_write_observer(addr, 2u);
            store_u16(&ewram_[off], v);
            note_ram_code_write_bus(addr, 2);
            runtime_note_ram_image_bus_write(addr, 2);
            return;
        case Region::Iwram:
            vram_trace::trace_oam_shadow_write(runtime_current_pc(), addr,
                                               2u);
            store_u16(&iwram_[off], v);
            vram_trace::trace_oam_shadow_write_committed(runtime_current_pc(),
                                                         addr, 2u);
            note_ram_code_write_bus(addr, 2);
            runtime_note_ram_image_bus_write(addr, 2);
            return;
        case Region::Pal:
            vram_trace::note_palette_cpu_write(runtime_current_pc(), addr, 2u);
            store_u16(&pal_[off], v);
            return;
        case Region::Vram:
            vram_trace::trace_cpu_write(io_dispatch_.raw(),
                                        runtime_current_pc(), addr, 2u);
            if (off + 1 < vram_.size()) store_u16(&vram_[off], v);
            return;
        case Region::Oam:   store_u16(&oam_[off], v); return;
        case Region::Bios:
            // BIOS is read-only; hardware ignores writes to this window.
            return;
        case Region::Rom:
            if (region == Region::Rom && rtc_.active() && off >= 0xC4u && off <= 0xC8u) {
                rtc_.write(static_cast<uint32_t>(off), static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (region == Region::Rom && is_eeprom_addr(addr, save_)) {
                save_.eeprom_write_bit(v);
                return;
            }
            log_unmapped(addr, v, true, 2);
            return;
        case Region::Io:
            io_dispatch_.write16(static_cast<uint32_t>(off), v);
            return;
        case Region::Save:
            if (save_.sram_enabled()) {
                save_.sram_write(static_cast<uint32_t>(off),
                                 static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (save_.flash_enabled()) {
                save_.flash_write(static_cast<uint32_t>(off),
                                  static_cast<uint8_t>(v & 0xFF));
                return;
            }
            log_unmapped(addr, v, true, 2);
            return;
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, v, true, 2);
            return;
    }
}

void GbaBus::write32(uint32_t addr, uint32_t v) {
    auto region = classify(addr);
    auto off    = resolve_offset(addr, region);
    if (write_observer_ && observe_write(region, off, addr, 4, v)) return;
    switch (region) {
        case Region::Ewram:
            if (g_ws_ewram_write_observer) g_ws_ewram_write_observer(addr, 4u);
            store_u32(&ewram_[off], v);
            note_ram_code_write_bus(addr, 4);
            runtime_note_ram_image_bus_write(addr, 4);
            return;
        case Region::Iwram:
            vram_trace::trace_oam_shadow_write(runtime_current_pc(), addr,
                                               4u);
            store_u32(&iwram_[off], v);
            vram_trace::trace_oam_shadow_write_committed(runtime_current_pc(),
                                                         addr, 4u);
            note_ram_code_write_bus(addr, 4);
            runtime_note_ram_image_bus_write(addr, 4);
            return;
        case Region::Pal:
            vram_trace::note_palette_cpu_write(runtime_current_pc(), addr, 4u);
            store_u32(&pal_[off], v);
            return;
        case Region::Vram:
            vram_trace::trace_cpu_write(io_dispatch_.raw(),
                                        runtime_current_pc(), addr, 4u);
            if (off + 3 < vram_.size()) store_u32(&vram_[off], v);
            return;
        case Region::Oam:   store_u32(&oam_[off], v); return;
        case Region::Bios:
            // BIOS is read-only; hardware ignores writes to this window.
            return;
        case Region::Rom:
            if (region == Region::Rom && rtc_.active() && off >= 0xC4u && off <= 0xC6u) {
                rtc_.write(static_cast<uint32_t>(off), static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (region == Region::Rom && is_eeprom_addr(addr, save_)) {
                save_.eeprom_write_bit(static_cast<uint16_t>(v));
                return;
            }
            log_unmapped(addr, v, true, 4);
            return;
        case Region::Io:
            io_dispatch_.write32(static_cast<uint32_t>(off), v);
            return;
        case Region::Save:
            if (save_.sram_enabled()) {
                save_.sram_write(static_cast<uint32_t>(off),
                                 static_cast<uint8_t>(v & 0xFF));
                return;
            }
            if (save_.flash_enabled()) {
                save_.flash_write(static_cast<uint32_t>(off),
                                  static_cast<uint8_t>(v & 0xFF));
                return;
            }
            log_unmapped(addr, v, true, 4);
            return;
        case Region::OpenBus:
        case Region::Unknown:
            log_unmapped(addr, v, true, 4);
            return;
    }
}

// Per-region access-cycle table. Sources:
//   * GBATEK § "GBA Memory Map"
//   * GBATEK § "GBA Cycle Times"
//   * ARM7TDMI TRM § "Memory Access Cycles"
//
// Game Pak cycle counts derive from the live WAITCNT value. Bus width and
// waitstate combine: every 16-bit-bus region costs 2 cycles per
// 32-bit access (two consecutive halfword bus cycles); EWRAM adds
// 2 waitstates on top of that.
//
// `sequential` is the ARM7TDMI "S" cycle (an access immediately
// following another in the same area). For ARM/THUMB on the GBA
// most regions have S=N=1 for 16-bit and 1S=1N=1 / 2S=2N=2 for
// 32-bit, but ROM with WAITCNT > 0 gets a faster S than N. We use
// the same value for S/N in regions where they match and split
// only where they don't (ROM).
uint32_t GbaBus::access_cycles(uint32_t addr, uint8_t width,
                               bool sequential) const {
    // 8-bit accesses use the same cycle count as 16-bit on hardware
    // (regions with 16-bit data buses still complete a single
    // halfword cycle for either width).
    uint8_t w = (width == 4) ? 4 : 2;
    uint32_t region = (addr >> 24) & 0xFu;
    const uint16_t waitcnt = io_dispatch_.waitcnt();
    switch (region) {
        case 0x0:  // BIOS  (32-bit bus, 0 wait)
        case 0x3:  // IWRAM (32-bit bus, 0 wait)
        case 0x7:  // OAM   (32-bit bus, 0 wait)
            return 1;
        case 0x4:  // IO (32-bit bus, 0 wait)
            return 1;
        case 0x5:  // PAL   (16-bit bus, 0 wait)
        case 0x6:  // VRAM  (16-bit bus, 0 wait)
            return (w == 4) ? 2u : 1u;
        case 0x2:  // EWRAM (16-bit bus, 2 wait states)
            return (w == 4) ? 6u : 3u;
        case 0x8: case 0x9:  // ROM WS0
        case 0xA: case 0xB:  // ROM WS1
        case 0xC: case 0xD:  // ROM WS2
        {
            // mGBA's tables store the external N16/S16 components;
            // the returned cost includes the one base bus cycle.
            static constexpr uint8_t kNonseq16[4] = {4, 3, 2, 8};
            static constexpr uint8_t kSeq16[3][2] = {
                {2, 1}, {4, 1}, {8, 1}
            };
            const unsigned bank = (region - 0x8u) / 2u;
            const unsigned nonseq_shift = 2u + bank * 3u;
            const unsigned seq_shift = 4u + bank * 3u;
            const uint32_t n16 =
                kNonseq16[(waitcnt >> nonseq_shift) & 3u];
            const uint32_t s16 =
                kSeq16[bank][(waitcnt >> seq_shift) & 1u];
            if (w == 4) {
                return sequential ? (2u * s16 + 2u)
                                  : (n16 + s16 + 2u);
            }
            return (sequential ? s16 : n16) + 1u;
        }
        case 0xE:  // SRAM / Flash region — 8-bit bus, ~5 cycles
        {
            static constexpr uint8_t kSramWait[4] = {4, 3, 2, 8};
            return static_cast<uint32_t>(kSramWait[waitcnt & 3u]) + 1u;
        }
        default:
            return 1;
    }
}

int32_t GbaBus::data_access_cycles(uint32_t addr, uint8_t width,
                                   bool sequential,
                                   uint32_t executing_pc) {
    int32_t wait = static_cast<int32_t>(
        access_cycles(addr, width, sequential));

    // mGBA GBAMemoryStall only applies to below-cartridge data accesses while
    // code executes from a cartridge region with WAITCNT prefetch enabled.
    // The algorithm and its signed intermediate values mirror the pinned
    // oracle; in particular, the WAITCNT store that first enables prefetch can
    // produce a negative data-access adjustment for that same instruction.
    const uint32_t active_region = (executing_pc >> 24) & 0xFu;
    if (addr >= 0x08000000u || active_region < 0x8u ||
        (io_dispatch_.waitcnt() & 0x4000u) == 0u) {
        return wait;
    }

    int32_t previous_loads = 0;
    const uint32_t distance = last_prefetched_pc_ - executing_pc;
    int32_t max_loads = 8;
    if (distance < 16u) {
        previous_loads = static_cast<int32_t>(distance >> 1);
        max_loads -= previous_loads;
    }

    const int32_t seq16 = static_cast<int32_t>(
        access_cycles(executing_pc, 2u, true)) - 1;
    const int32_t nonseq16 = static_cast<int32_t>(
        access_cycles(executing_pc, 2u, false)) - 1;
    const int32_t nonseq_to_seq = nonseq16 - seq16 + 1;

    int32_t stall = seq16 + 1;
    int32_t loads = 1;
    while (stall < wait && loads < max_loads) {
        stall += seq16;
        ++loads;
    }
    last_prefetched_pc_ = executing_pc +
        2u * static_cast<uint32_t>(loads + previous_loads - 1);

    if (stall > wait) wait = stall;
    wait -= nonseq_to_seq;
    wait -= stall - 1;
    return wait;
}

void GbaBus::log_unmapped(uint32_t addr, uint32_t value, bool is_write, uint8_t width) {
    ++unmapped_count_;
    // Keep the counter hot-path cheap by default. Full stderr logging is
    // useful when chasing a specific bus issue, but gameplay can produce
    // millions of open-bus-style reads and that makes TCP replays unusable.
    static const bool log_unmapped_enabled =
        std::getenv("GBARECOMP_LOG_UNMAPPED") != nullptr;
    if (!log_unmapped_enabled) {
        return;
    }
    std::fprintf(stderr,
                 "[gba:bus] UNMAPPED %s%u @ 0x%08x = 0x%x\n",
                 is_write ? "W" : "R", width, addr, value);
}

}  // namespace gba
