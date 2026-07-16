#pragma once

#include <cstdint>

// Big-endian field loads for the ITCH 5.0 wire format. The byte-shift form is
// endian-agnostic and compilers lower it to a single load plus a byte-reverse.

namespace itch {

inline std::uint16_t load_be16(const unsigned char* p) noexcept {
    return static_cast<std::uint16_t>(std::uint16_t(p[0]) << 8 | p[1]);
}

inline std::uint32_t load_be32(const unsigned char* p) noexcept {
    return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 |
           std::uint32_t(p[2]) << 8 | std::uint32_t(p[3]);
}

inline std::uint64_t load_be64(const unsigned char* p) noexcept {
    return std::uint64_t(p[0]) << 56 | std::uint64_t(p[1]) << 48 |
           std::uint64_t(p[2]) << 40 | std::uint64_t(p[3]) << 32 |
           std::uint64_t(p[4]) << 24 | std::uint64_t(p[5]) << 16 |
           std::uint64_t(p[6]) << 8 | std::uint64_t(p[7]);
}

// ITCH timestamps are 48-bit nanoseconds since midnight.
inline std::uint64_t load_be48(const unsigned char* p) noexcept {
    return std::uint64_t(p[0]) << 40 | std::uint64_t(p[1]) << 32 |
           std::uint64_t(p[2]) << 24 | std::uint64_t(p[3]) << 16 |
           std::uint64_t(p[4]) << 8 | std::uint64_t(p[5]);
}

}  // namespace itch
