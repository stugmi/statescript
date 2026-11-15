// Cleaned helper adjacent to DataFlow code. RVA 0xEE8360; caller role unresolved.
#pragma once
#include <cstdint>
#include <cstring>
#include <limits>

namespace statescript {
// Bit-exact beta conversion, NOT a general IEEE binary16 encoder.
// Drops 13 mantissa bits without rounding; flushes exponent <=112 to +0;
// rebiases every other exponent without overflow/NaN/infinity handling.
constexpr std::uint16_t EncodeCompactFloatBits(std::uint32_t bits) noexcept {
    const auto exponent = (bits >> 23) & 0xFFu;
    if (exponent <= 112u)
        return 0;
    return static_cast<std::uint16_t>(
        ((exponent - 112u) << 10) |
        ((bits >> 16) & 0x8000u) |
        ((bits >> 13) & 0x03FFu));
}

inline std::uint16_t EncodeCompactFloat(float value) noexcept {
    static_assert(sizeof(float) == sizeof(std::uint32_t) &&
                  std::numeric_limits<float>::is_iec559,
                  "The beta helper consumes IEEE binary32 bits");
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return EncodeCompactFloatBits(bits);
}
} // namespace statescript
