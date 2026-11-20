#pragma once
// Compressed GPU format for splats: 24 bytes per splat instead of 64 (SH bands 1..3 excluded).
//
//   word 0 : position x (u16) | position y (u16)     quantized inside the splat's chunk bounds
//   word 1 : position z (u16) | opacity (f16)
//   word 2 : log scale x (f16) | log scale y (f16)
//   word 3 : log scale z (f16) | SH DC red (f16)
//   word 4 : SH DC green (f16) | SH DC blue (f16)
//   word 5 : rotation, "smallest three" quaternion (2-bit index + 3 x 10 bits)
//
// Positions are quantized to 16 bits per axis relative to the bounds of a chunk of 256 splats.
// Splats are first sorted along a Morton (Z-order) curve, so a chunk covers a small region of
// space and the 16 bits are spent where the splats are. The covariance is no longer stored: the
// preprocess shader rebuilds it from scale and rotation (a few dozen flops, while reading 40
// fewer bytes per splat). Same idea as the compressed formats of Spark / PlayCanvas.
// shaders/include/splat_codec.glsl decodes exactly what this file encodes.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace gsv {

struct GaussianScene;

inline constexpr uint32_t kCodecChunk = 256;

struct PackedSplat {
  uint32_t w[6];
};
static_assert(sizeof(PackedSplat) == 24);

struct ChunkBounds {  // std430-friendly: two vec4
  glm::vec4 min;      // w unused
  glm::vec4 max;
};

// Quaternion (w, x, y, z), any length -> 32 bits. Max error per component ~0.0007.
uint32_t packQuaternion(const glm::vec4& wxyz);
glm::vec4 unpackQuaternion(uint32_t v);

// 16-bit quantization of x in [lo, hi].
uint16_t quantize16(float x, float lo, float hi);
float dequantize16(uint16_t q, float lo, float hi);

// Permutation that orders the positions along a 3D Morton curve (21 bits per axis).
std::vector<uint32_t> mortonOrder(const std::vector<glm::vec3>& positions);

// Packs scene splats in the given order (order[k] = scene index of packed splat k).
void packSplats(const GaussianScene& scene, const std::vector<uint32_t>& order, std::vector<PackedSplat>& out,
                std::vector<ChunkBounds>& chunks);

// Decoded values of packed splat k (tests and the C++ reference).
struct DecodedSplat {
  glm::vec3 position;
  float opacity;
  glm::vec3 logScale;
  glm::vec3 dc;
  glm::vec4 rotation;  // w, x, y, z (unit length)
};
DecodedSplat unpackSplat(const PackedSplat& p, const ChunkBounds& chunk);

}  // namespace gsv
