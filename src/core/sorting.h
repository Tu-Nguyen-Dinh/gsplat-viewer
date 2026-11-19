#pragma once
// CPU sorting of splats by depth (back-to-front).

#include <glm/glm.hpp>

#include <bit>
#include <cstdint>
#include <vector>

namespace gsv {

// Sort key such that ASCENDING key order = FAR to NEAR. For positive floats the
// IEEE-754 bit pattern grows with the value, so inverting it reverses the order.
// Same formula as depthKey() in splat_preprocess.comp.
inline uint32_t depthKey32(float depth) {
  const float d = depth > 0.0f ? depth : 0.0f;
  return 0xFFFFFFFFu - std::bit_cast<uint32_t>(d);  // C++20: reinterpret the bits, no UB
}

// keys[i] = depthKey32(depth of positions[i]) for the given model-view matrix.
// Only the third row of the matrix is needed: depth = -(row2 . p + t).
void computeDepthKeys(const std::vector<glm::vec3>& positions, const glm::mat4& modelView,
                      std::vector<uint32_t>& keys);

// Returns the indices 0..n-1 ordered by ascending key (stable). Uses std::sort on
// packed 64-bit (key << 32 | index) values: the index breaks ties, so it is stable.
void sortByKeyStd(const std::vector<uint32_t>& keys, std::vector<uint32_t>& indicesOut);

// Same result with an LSD radix sort (4 passes of 8 bits). O(n) instead of O(n log n).
// Scratch vectors are kept by the caller to avoid reallocating every frame.
struct RadixScratch {
  std::vector<uint32_t> keysA, keysB, valuesB;
};
void sortByKeyRadix(const std::vector<uint32_t>& keys, std::vector<uint32_t>& indicesOut, RadixScratch& scratch);

}  // namespace gsv
