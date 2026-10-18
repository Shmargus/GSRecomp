#include "tla_save_update.h"
#include "../../player_builder/guest_call.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace tla_save_update {
namespace {
// TLA's own loader/checksum (0x08015E44/0x08015EC8), slot summaries
// (0x08016054), and Update picker mode 6 (0x08043E10..0x08043E5C).
// FACTS.md, "Updating a saved adventure", 2026-10-18.
constexpr std::size_t kGroupBytes = 0x3000;
constexpr std::size_t kHeaderBytes = 0x10;
constexpr std::size_t kPayloadBytes = kGroupBytes - kHeaderBytes;
unsigned halfword(const std::uint8_t* p) { return p[0] | unsigned(p[1]) << 8; }
unsigned checksum(const std::uint8_t* group) {
    unsigned sum = 0;
    for (std::size_t i = kHeaderBytes; i < kGroupBytes; ++i) sum += group[i];
    return sum & 0xFFFFu;
}
}

std::vector<Slot> slots(const std::vector<std::uint8_t>& flash) {
    if (flash.size() != gs_save::kFileBytes) return {};
    std::array<std::size_t, 3> latest;
    latest.fill(flash.size());
    for (std::size_t off = 0; off + kGroupBytes <= flash.size(); off += kGroupBytes) {
        const auto* p = flash.data() + off;
        if (std::memcmp(p, "CAMELOT", 7) || p[7] >= latest.size() ||
            checksum(p) != halfword(p + 8) || halfword(p + 10) == 0) continue;
        auto& previous = latest[p[7]];
        // Native 0x08015F0C keeps the first group on tied generations.
        if (previous == flash.size() ||
            halfword(p + 10) > halfword(flash.data() + previous + 10)) previous = off;
    }
    std::vector<Slot> out;
    for (unsigned i = 0; i < latest.size(); ++i) {
        if (latest[i] == flash.size()) continue;
        const auto* p = flash.data() + latest[i];
        if (p[0x1C] == 0) continue;  // Native picker: no saved adventure.
        Slot slot{i + 1, p[0x1C], {}, p[0x33] != 0, latest[i]};
        for (unsigned n = 0; n < 12 && p[0x10 + n]; ++n) {
            const auto c = p[0x10 + n];
            slot.name += c >= 0x20 && c < 0x7F ? char(c) : '?';
        }
        out.push_back(std::move(slot));
    }
    return out;
}

bool prepare(const std::vector<std::uint8_t>& rom,
             const std::vector<std::uint8_t>& ewram,
             const std::vector<std::uint8_t>& iwram,
             const std::vector<std::uint8_t>& flash, unsigned destination,
             const gs_save::ClearSlot& source,
             std::vector<std::uint8_t>& updated, std::string& error) {
    updated.clear();
    const auto destinations = slots(flash);
    const auto it = std::find_if(destinations.begin(), destinations.end(),
        [destination](const Slot& s) { return s.slot == destination; });
    if (it == destinations.end() || it->post_reunion) {
        error = "Choose a saved adventure from before the reunion.";
        return false;
    }
    if (ewram.size() != 0x40000 || iwram.size() != 0x8000) {
        error = "The game's memory is unavailable.";
        return false;
    }
    gsr::builder::GuestMachine machine(rom);
    machine.ewram() = ewram;
    machine.iwram() = iwram;
    machine.set_instant_dma3(true);
    // This call only applies the menu's colours/font, not transferred data.
    // The same UI-only exclusion is used by the measured importer oracle.
    machine.stub_returning(0x08038348u, 0);
    const auto* payload = flash.data() + it->offset + kHeaderBytes;
    std::copy_n(payload, kPayloadBytes, machine.ewram().begin());
    std::copy(source.transfer.begin(), source.transfer.end(), machine.ewram().begin() + 0x13000);
    // The original slot loader's globals (0x080437C6..0x080437E0).
    std::copy_n(payload + 0x244, 4, machine.iwram().begin() + 0x117C);
    machine.iwram()[0x1200] = payload[0x48A];
    machine.iwram()[0x1218] = machine.iwram()[0x1219] = 0;
    const std::uint32_t args[4] = {0x02013000u, 0, 1, 0};
    if (!machine.call(0x080AE411u, args, nullptr, &error)) {
        error = "The save update could not finish: " + error;
        return false;
    }
    // The original importer recalculates party data, including Felix's
    // records in some saves. Keep its result; only this private machine
    // has run, so the current game's party and unsaved progress are intact.
    updated = flash;
    auto* group = updated.data() + it->offset;
    std::copy_n(machine.ewram().begin(), kPayloadBytes, group + kHeaderBytes);
    const auto sum = checksum(group);
    group[8] = std::uint8_t(sum);
    group[9] = std::uint8_t(sum >> 8);
    return true;
}
}
