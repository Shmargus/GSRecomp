// Pure policy for identifying the measured Func_1dc8 object-writer entries.
// The runtime supplies the verified image identity and generation; an address
// by itself is never sufficient because the working area is reused.
#pragma once

#include <cstdint>

namespace gsr {

enum class Func1dc8WriterRoute : std::uint8_t {
    D4 = 0,
    EC,
    F0,
    Count,
    Unknown = Count,
};

// These offsets are derived from the generated Func_1dc8 dispatch table at
// origin 0x03002400: D4=+0x58, EC=+0x70, F0=+0x74. Keep them explicit so a
// future image cannot accidentally be recognized by a coincidental address.
inline constexpr std::uint32_t kFunc1dc8ImageOrigin = 0x03002400u;
inline constexpr std::uint32_t kFunc1dc8ImageSize = 0x000000E0u;
inline constexpr std::uint32_t kFunc1dc8ImageKey = 0x1DC8u;
inline constexpr std::uint32_t kFunc1dc8KnownFixedBase = 0x0300387Cu;
inline constexpr std::uint32_t kFunc1dc8D4Offset = 0x58u;
inline constexpr std::uint32_t kFunc1dc8EcOffset = 0x70u;
inline constexpr std::uint32_t kFunc1dc8F0Offset = 0x74u;

inline constexpr std::uint32_t writer_offset(Func1dc8WriterRoute route) {
    switch (route) {
        case Func1dc8WriterRoute::D4: return kFunc1dc8D4Offset;
        case Func1dc8WriterRoute::EC: return kFunc1dc8EcOffset;
        case Func1dc8WriterRoute::F0: return kFunc1dc8F0Offset;
        case Func1dc8WriterRoute::Count: break;
    }
    return 0;
}

inline constexpr Func1dc8WriterRoute writer_route_for_offset(
    std::uint32_t offset) {
    return offset == kFunc1dc8D4Offset ? Func1dc8WriterRoute::D4
         : offset == kFunc1dc8EcOffset ? Func1dc8WriterRoute::EC
         : offset == kFunc1dc8F0Offset ? Func1dc8WriterRoute::F0
                                       : Func1dc8WriterRoute::Unknown;
}

struct Func1dc8WriterIdentity {
    bool image_verified = false;
    std::uint32_t image_key = 0;
    std::uint32_t base = 0;
    std::uint64_t generation = 0;
};

struct Func1dc8WriterResolution {
    bool recognized = false;
    bool identity_mismatch = false;
    bool unknown_variant = false;
    std::uint32_t base = 0;
    std::uint32_t offset = 0;
    Func1dc8WriterRoute route = Func1dc8WriterRoute::Unknown;
};

// Resolve a PC against a known image key and generation. `expected_image_key`
// is game metadata, not a RAM address. This is intentionally pure so tests
// can cover collisions and overlay transitions without protected data.
inline Func1dc8WriterResolution resolve_func1dc8_writer(
    std::uint32_t pc, Func1dc8WriterRoute expected_route,
    std::uint32_t expected_image_key,
    std::uint64_t expected_generation,
    const Func1dc8WriterIdentity& identity) {
    Func1dc8WriterResolution result{};
    if (identity.base < 0x02000000u ||
        identity.base + kFunc1dc8ImageSize > 0x04000000u ||
        pc < identity.base) {
        result.unknown_variant = true;
        return result;
    }
    // A PC can only be interpreted relative to the origin after its route is
    // selected; the base is derived from the measured entry offset.
    const std::uint32_t route_offset = writer_offset(expected_route);
    if (route_offset == 0 || pc < route_offset) {
        result.unknown_variant = true;
        return result;
    }
    result.offset = route_offset;
    result.route = writer_route_for_offset(route_offset);
    result.base = identity.base;
    if (result.route == Func1dc8WriterRoute::Unknown ||
        result.offset >= kFunc1dc8ImageSize ||
        pc != result.base + result.offset) {
        result.unknown_variant = true;
        return result;
    }
    if (!identity.image_verified || identity.image_key != expected_image_key ||
        identity.generation != expected_generation) {
        result.identity_mismatch = true;
        return result;
    }
    result.recognized = true;
    return result;
}

}  // namespace gsr
