#pragma once
// A save record in either game's flash: "CAMELOT", +7 the block or slot it
// is named by, +8 u16 sum of the first `summed` payload bytes, +0xA
// generation, payload from +0x10 (save_files_picker.h answer_record). No
// engine dependencies, so the launcher reads emulator saves with it too.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gsr::save_files {

constexpr std::size_t kRecordHeader = 0x10;  // record bytes before the payload
inline bool present(const std::uint8_t* record) {
    return std::memcmp(record, "CAMELOT", 7) == 0;
}
// A record the game's own load would accept: present, and its +8 sum of the
// first `summed` payload bytes matches (the rule answer_record writes). A
// damaged file reads as absent, as the cartridge's damaged groups do; its
// bytes stay in the extension until the player saves over it.
inline bool valid(const std::uint8_t* record, std::size_t summed) {
    if (!present(record)) return false;
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < summed; ++i) sum += record[kRecordHeader + i];
    return (sum & 0xFFFFu) == static_cast<std::uint32_t>(record[8] | (record[9] << 8));
}

}  // namespace gsr::save_files
