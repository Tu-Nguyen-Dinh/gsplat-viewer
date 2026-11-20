#include "scene/splat_codec.h"

#include "core/half.h"
#include "core/parallel.h"
#include "core/sorting.h"
#include "scene/gaussian_scene.h"

#include <algorithm>
#include <cmath>

namespace gsv {

namespace {

constexpr float kInvSqrt2 = 0.70710678f;

// Spreads the low 21 bits of v so that there are two zero bits between each of them.
uint64_t spreadBits3(uint64_t v) {
  v &= 0x1FFFFFull;
  v = (v | v << 32) & 0x1F00000000FFFFull;
  v = (v | v << 16) & 0x1F0000FF0000FFull;
  v = (v | v << 8) & 0x100F00F00F00F00Full;
  v = (v | v << 4) & 0x10C30C30C30C30C3ull;
  v = (v | v << 2) & 0x1249249249249249ull;
  return v;
}

uint32_t half2(float lo, float hi) { return uint32_t(floatToHalf(lo)) | uint32_t(floatToHalf(hi)) << 16; }

}  // namespace

uint32_t packQuaternion(const glm::vec4& wxyz) {
  float len = glm::length(wxyz);
  glm::vec4 q = len > 0.0f ? wxyz / len : glm::vec4(1, 0, 0, 0);
  int largest = 0;
  for (int i = 1; i < 4; ++i)
    if (std::abs(q[i]) > std::abs(q[largest])) largest = i;
  if (q[largest] < 0.0f) q = -q;  // q and -q are the same rotation: make the dropped one positive
  uint32_t v = static_cast<uint32_t>(largest) << 30;
  int shift = 20;
  for (int i = 0; i < 4; ++i) {
    if (i == largest) continue;
    // The other three lie in [-1/sqrt2, 1/sqrt2] (they are smaller than the largest one).
    const float t = std::clamp(q[i] / kInvSqrt2 * 0.5f + 0.5f, 0.0f, 1.0f);
    v |= static_cast<uint32_t>(t * 1023.0f + 0.5f) << shift;
    shift -= 10;
  }
  return v;
}

glm::vec4 unpackQuaternion(uint32_t v) {
  const int largest = static_cast<int>(v >> 30);
  glm::vec4 q(0.0f);
  float sum = 0.0f;
  int shift = 20;
  for (int i = 0; i < 4; ++i) {
    if (i == largest) continue;
    const float c = (static_cast<float>((v >> shift) & 1023u) / 1023.0f * 2.0f - 1.0f) * kInvSqrt2;
    q[i] = c;
    sum += c * c;
    shift -= 10;
  }
  q[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
  return q;
}

uint16_t quantize16(float x, float lo, float hi) {
  if (!(hi > lo)) return 0;
  const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
  return static_cast<uint16_t>(t * 65535.0f + 0.5f);  // t >= 0: truncation after +0.5 rounds
}

float dequantize16(uint16_t q, float lo, float hi) { return lo + (hi - lo) * (static_cast<float>(q) / 65535.0f); }

std::vector<uint32_t> mortonOrder(const std::vector<glm::vec3>& positions) {
  const size_t n = positions.size();
  glm::vec3 lo(INFINITY), hi(-INFINITY);
  for (const glm::vec3& p : positions) lo = glm::min(lo, p), hi = glm::max(hi, p);
  const glm::vec3 extent = glm::max(hi - lo, glm::vec3(1e-20f));
  // The radix sort works on 32-bit keys: use the top 30 bits of the 63-bit Morton code
  // (10 bits per axis = 1024 cells per axis, plenty to make chunks compact).
  std::vector<uint32_t> keys(n);
  parallelFor(n, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const glm::vec3 t = glm::clamp((positions[i] - lo) / extent, 0.0f, 1.0f) * 2097151.0f;  // 21 bits
      const uint64_t code = spreadBits3(uint64_t(t.x)) | spreadBits3(uint64_t(t.y)) << 1 | spreadBits3(uint64_t(t.z)) << 2;
      keys[i] = static_cast<uint32_t>(code >> 33);
    }
  });
  std::vector<uint32_t> order;
  RadixScratch scratch;
  sortByKeyRadix(keys, order, scratch);
  return order;
}

void packSplats(const GaussianScene& scene, const std::vector<uint32_t>& order, std::vector<PackedSplat>& out,
                std::vector<ChunkBounds>& chunks) {
  const size_t n = order.size();
  out.assign(n, PackedSplat{});
  chunks.assign((n + kCodecChunk - 1) / kCodecChunk, ChunkBounds{});
  // One work item = one chunk of 256 splats, so the default grain (16384 items) would run a
  // 2.5 M splat scene (9927 chunks) on a single thread: use a grain of 16 chunks.
  parallelFor(chunks.size(), [&](size_t cBegin, size_t cEnd) {
    for (size_t c = cBegin; c < cEnd; ++c) {
      const size_t begin = c * kCodecChunk, end = std::min(n, begin + kCodecChunk);
      glm::vec3 lo(INFINITY), hi(-INFINITY);
      for (size_t k = begin; k < end; ++k) {
        lo = glm::min(lo, scene.position[order[k]]);
        hi = glm::max(hi, scene.position[order[k]]);
      }
      chunks[c] = {glm::vec4(lo, 0.0f), glm::vec4(hi, 0.0f)};
      for (size_t k = begin; k < end; ++k) {
        const size_t i = order[k];
        const glm::vec3& p = scene.position[i];
        const glm::vec3& s = scene.logScale[i];
        const glm::vec3& dc = scene.shDC[i];
        PackedSplat& o = out[k];
        o.w[0] = uint32_t(quantize16(p.x, lo.x, hi.x)) | uint32_t(quantize16(p.y, lo.y, hi.y)) << 16;
        o.w[1] = uint32_t(quantize16(p.z, lo.z, hi.z)) | uint32_t(floatToHalf(sigmoid(scene.opacityLogit[i]))) << 16;
        o.w[2] = half2(s.x, s.y);
        o.w[3] = half2(s.z, dc.r);
        o.w[4] = half2(dc.g, dc.b);
        o.w[5] = packQuaternion(scene.rotation[i]);
      }
    }
  }, 16);
}

DecodedSplat unpackSplat(const PackedSplat& p, const ChunkBounds& c) {
  DecodedSplat d;
  d.position = {dequantize16(uint16_t(p.w[0] & 0xFFFFu), c.min.x, c.max.x),
                dequantize16(uint16_t(p.w[0] >> 16), c.min.y, c.max.y),
                dequantize16(uint16_t(p.w[1] & 0xFFFFu), c.min.z, c.max.z)};
  d.opacity = halfToFloat(uint16_t(p.w[1] >> 16));
  d.logScale = {halfToFloat(uint16_t(p.w[2] & 0xFFFFu)), halfToFloat(uint16_t(p.w[2] >> 16)),
                halfToFloat(uint16_t(p.w[3] & 0xFFFFu))};
  d.dc = {halfToFloat(uint16_t(p.w[3] >> 16)), halfToFloat(uint16_t(p.w[4] & 0xFFFFu)),
          halfToFloat(uint16_t(p.w[4] >> 16))};
  d.rotation = unpackQuaternion(p.w[5]);
  return d;
}

}  // namespace gsv
