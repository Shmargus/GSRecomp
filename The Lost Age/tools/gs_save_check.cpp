// Standalone GS1 import checks using synthetic cartridge-save sectors.
// Optional argument: inspect a user's save read-only, printing slot metadata.
#include "../src/gs_save.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kSectorBytes = 0x1000;
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void halfword(std::uint8_t* p, unsigned value) {
    p[0] = std::uint8_t(value);
    p[1] = std::uint8_t(value >> 8);
}

std::uint8_t* sector(Bytes& bytes, unsigned physical) {
    return bytes.data() + physical * kSectorBytes;
}

void checksum(std::uint8_t* p) {
    unsigned sum = 0;
    for (std::size_t i = 0x10; i < 0xFF8; ++i) sum += p[i];
    halfword(p + 8, sum);
}

void write_sector(Bytes& bytes, unsigned physical, unsigned logical,
                  unsigned generation, bool clear = false) {
    auto* p = sector(bytes, physical);
    std::fill_n(p, kSectorBytes, 0);
    std::memcpy(p, "CAMELOT", 7);
    p[7] = std::uint8_t(logical);
    halfword(p + 10, generation);
    if (logical < 3) {
        // A nonuniform payload makes offset and length errors observable.
        for (std::size_t i = 0x10; i < 0xFF8; ++i)
            p[i] = std::uint8_t((i * 37 + (i >> 8) * 13) & 0xFF);
        std::fill_n(p + 0x10, 12, 0);
        std::memcpy(p + 0x10, "Leader", 6);
        p[0x1C] = 42;
        p[0x31] = clear ? 1 : 0;
        const char* names[] = {"Leader", "Mage", "Warrior", "Scout"};
        for (unsigned c = 0; c < 4; ++c) {
            auto* name = p + 0x510 + c * 0x14C;
            std::fill_n(name, 15, 0);
            std::memcpy(name, names[c], std::strlen(names[c]));
        }
    }
    checksum(p);
}

Bytes fixture() {
    Bytes bytes(gs_save::kFileBytes, 0xFF);
    write_sector(bytes, 0, 0, 1, true);
    write_sector(bytes, 1, 3, 1);
    write_sector(bytes, 2, 1, 1, false);
    write_sector(bytes, 3, 4, 1);
    return bytes;
}

void expect_slots(const Bytes& bytes, std::initializer_list<unsigned> expected,
                  const char* message) {
    const auto slots = gs_save::cleared_slots(bytes);
    std::vector<unsigned> actual;
    for (const auto& slot : slots) actual.push_back(slot.slot);
    check(actual == std::vector<unsigned>(expected), message);
}

void synthetic_checks() {
    const Bytes original = fixture();
    expect_slots(original, {1}, "unfinished slot 2 must be excluded");
    const auto slots = gs_save::cleared_slots(original);
    if (slots.size() == 1) {
        check(slots[0].name == "Leader" && slots[0].level == 42,
              "slot summary must preserve the leader name and level");
        const auto* body = original.data() + 0x10;
        const auto& transfer = slots[0].transfer;
        check(std::equal(transfer.begin(), transfer.begin() + 0x254, body),
              "transfer must retain the first 0x254 payload bytes");
        check(std::all_of(transfer.begin() + 0x254, transfer.begin() + 0x260,
                          [](std::uint8_t value) { return value == 0; }),
              "transfer padding must be zero");
        check(std::equal(transfer.begin() + 0x260, transfer.end(), body + 0x240),
              "transfer must shift payload 0x240..0xF5F to 0x260..0xF7F");
    }

    auto bytes = original;
    // Physical order deliberately differs from logical slot order.
    write_sector(bytes, 4, 2, 1, true);
    write_sector(bytes, 5, 5, 1);
    write_sector(bytes, 6, 1, 2, true);
    expect_slots(bytes, {1, 2, 3}, "all cleared slots must appear in slot order");

    bytes = original;
    sector(bytes, 0)[0x600] ^= 1;
    expect_slots(bytes, {}, "corrupt main checksum must be excluded");
    bytes = original;
    sector(bytes, 0)[0] = 'X';
    expect_slots(bytes, {}, "incorrect signature must be excluded");
    bytes = original;
    sector(bytes, 0)[7] = 16;
    expect_slots(bytes, {}, "invalid logical sector ID must be excluded");

    for (const std::size_t size : {std::size_t(0), std::size_t(32),
                                  gs_save::kFileBytes - 1, gs_save::kFileBytes + 1}) {
        bytes = original;
        bytes.resize(size);
        expect_slots(bytes, {}, "malformed or truncated save size must be rejected");
    }

    bytes = original;
    write_sector(bytes, 4, 0, 2, false);
    expect_slots(bytes, {}, "newer unfinished main must hide older cleared main");
    bytes = original;
    write_sector(bytes, 4, 0, 1, false);
    expect_slots(bytes, {1}, "equal generation must retain the first valid sector");
    bytes = original;
    write_sector(bytes, 4, 0, 2, false);
    sector(bytes, 4)[0x600] ^= 1;
    expect_slots(bytes, {1}, "corrupt newest main must fall back to older valid main");

    bytes = original;
    std::fill_n(sector(bytes, 1), kSectorBytes, 0xFF);
    expect_slots(bytes, {}, "missing companion block must reject a cleared slot");
    bytes = original;
    sector(bytes, 1)[0x600] ^= 1;
    expect_slots(bytes, {}, "corrupt companion block must reject a cleared slot");

    bytes = original;
    auto* p = sector(bytes, 0);
    for (unsigned c = 0; c < 4; ++c) {
        std::fill_n(p + 0x510 + c * 0x14C, 15, 0);
        std::fill_n(p + 0x530 + c * 0x14C, 15, 0);
        std::memcpy(p + 0x530 + c * 0x14C, "TLA name", 8);
    }
    checksum(p);
    expect_slots(bytes, {}, "TLA character-name offsets must not pass as a GS1 save");

    bytes = original;
    std::fill_n(sector(bytes, 0) + 0xFF8, 8, 0xA5);
    expect_slots(bytes, {1}, "the final eight sector bytes are outside the checksum");
}

bool inspect_save(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Could not open the supplied save.\n";
        return false;
    }
    const Bytes bytes{std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>()};
    const auto slots = gs_save::cleared_slots(bytes);
    std::cout << "Cleared slots: " << slots.size() << '\n';
    for (const auto& slot : slots)
        std::cout << "Slot " << slot.slot << ": " << slot.name
                  << ", level " << slot.level << '\n';
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    synthetic_checks();
    if (argc > 2) {
        std::cerr << "Usage: gs_save_check [save.sav]\n";
        return 1;
    }
    if (argc == 2 && !inspect_save(argv[1])) return 1;
    if (failures) return 1;
    std::cout << "GS1 save import checks passed.\n";
    return 0;
}
