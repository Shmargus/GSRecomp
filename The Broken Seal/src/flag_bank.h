// Both games' story flags: bit n of the bank at 0x02000040, n taken modulo
// 0x1000. The Broken Seal: getter 08079338 (bank literal 08079354); all 4096
// read/set/clear cases checked against original ROM execution (FACTS.md,
// Adventure flag mapping). The Lost Age: getter 0x08016CE4 (its FACTS.md,
// Custom Battle).
#pragma once

namespace gsr {
constexpr unsigned kFlagBank = 0x40u;  // EWRAM offset
inline bool read_flag(const unsigned char* ewram, unsigned flag) {
    flag &= 0xfff;
    return (ewram[kFlagBank + (flag >> 3)] & (1u << (flag & 7))) != 0;
}
} // namespace gsr
