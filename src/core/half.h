#pragma once
// IEEE-754 binary16 ("half float") conversion. C++20 has no standard half type;
// GLSL unpacks pairs of halves with unpackHalf2x16().
//
//   half  : 1 sign bit | 5 exponent bits (bias 15)  | 10 mantissa bits
//   float : 1 sign bit | 8 exponent bits (bias 127) | 23 mantissa bits
// Precision: ~3 significant decimal digits (relative step 2^-10 ~ 0.001).

#include <bit>
#include <cstdint>

namespace gsv {

// Round-to-nearest-even conversion; overflow -> infinity, tiny values -> subnormal / zero.
inline uint16_t floatToHalf(float value) {
  const uint32_t f = std::bit_cast<uint32_t>(value);
  const uint32_t sign = (f >> 16) & 0x8000u;
  const int32_t exponent = static_cast<int32_t>((f >> 23) & 0xFFu) - 127 + 15;
  uint32_t mantissa = f & 0x7FFFFFu;

  if (((f >> 23) & 0xFFu) == 0xFFu)  // Inf or NaN
    return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
  if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7C00u);  // too large: infinity
  if (exponent <= 0) {                                                 // subnormal half or zero
    if (exponent < -10) return static_cast<uint16_t>(sign);
    mantissa |= 0x800000u;  // make the implicit leading 1 explicit
    const int shift = 14 - exponent;
    uint32_t half = mantissa >> shift;
    const uint32_t rest = mantissa & ((1u << shift) - 1u), halfway = 1u << (shift - 1);
    if (rest > halfway || (rest == halfway && (half & 1u))) ++half;
    return static_cast<uint16_t>(sign | half);
  }
  uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
  const uint32_t rest = mantissa & 0x1FFFu;  // the 13 dropped bits
  if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) ++half;  // may carry into the exponent: still correct
  return static_cast<uint16_t>(sign | half);
}

inline float halfToFloat(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1Fu;
  const uint32_t mantissa = h & 0x3FFu;
  if (exponent == 0) {  // zero or subnormal: value = mantissa * 2^-24
    const float v = static_cast<float>(mantissa) * (1.0f / 16777216.0f);
    return sign ? -v : v;
  }
  if (exponent == 31) return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13));
  return std::bit_cast<float>(sign | ((exponent - 15 + 127) << 23) | (mantissa << 13));
}

}  // namespace gsv
