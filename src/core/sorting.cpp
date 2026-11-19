#include "core/sorting.h"

#include <algorithm>
#include <array>
#include <numeric>

namespace gsv {

void computeDepthKeys(const std::vector<glm::vec3>& positions, const glm::mat4& mv, std::vector<uint32_t>& keys) {
  keys.resize(positions.size());
  // glm is column-major: mv[col][row]. Row 2 = (mv[0][2], mv[1][2], mv[2][2]), translation mv[3][2].
  const glm::vec3 row2(mv[0][2], mv[1][2], mv[2][2]);
  const float t = mv[3][2];
  for (size_t i = 0; i < positions.size(); ++i) keys[i] = depthKey32(-(glm::dot(row2, positions[i]) + t));
}

void sortByKeyStd(const std::vector<uint32_t>& keys, std::vector<uint32_t>& indicesOut) {
  const size_t n = keys.size();
  std::vector<uint64_t> packed(n);
  for (size_t i = 0; i < n; ++i) packed[i] = (static_cast<uint64_t>(keys[i]) << 32) | static_cast<uint32_t>(i);
  std::sort(packed.begin(), packed.end());
  indicesOut.resize(n);
  for (size_t i = 0; i < n; ++i) indicesOut[i] = static_cast<uint32_t>(packed[i]);
}

void sortByKeyRadix(const std::vector<uint32_t>& keys, std::vector<uint32_t>& indicesOut, RadixScratch& s) {
  const size_t n = keys.size();
  s.keysA = keys;
  s.keysB.resize(n);
  s.valuesB.resize(n);
  indicesOut.resize(n);
  std::iota(indicesOut.begin(), indicesOut.end(), 0u);

  std::vector<uint32_t>* srcK = &s.keysA;
  std::vector<uint32_t>* srcV = &indicesOut;
  std::vector<uint32_t>* dstK = &s.keysB;
  std::vector<uint32_t>* dstV = &s.valuesB;

  for (int pass = 0; pass < 4; ++pass) {
    const int shift = 8 * pass;
    // 1. Histogram of the current 8-bit digit.
    std::array<size_t, 256> count{};
    for (size_t i = 0; i < n; ++i) ++count[((*srcK)[i] >> shift) & 0xFFu];
    // Skip passes where every key has the same digit (common for the high bits of depth keys).
    if (n == 0 || count[((*srcK)[0] >> shift) & 0xFFu] == n) continue;
    // 2. Exclusive prefix sum: where each digit's bucket starts in the output.
    size_t sum = 0;
    for (size_t& c : count) {
      const size_t c0 = c;
      c = sum;
      sum += c0;
    }
    // 3. Scatter. Walking the input in order keeps equal digits in order: the sort is stable,
    //    which is what makes LSD (least significant digit first) radix sort correct.
    for (size_t i = 0; i < n; ++i) {
      const uint32_t k = (*srcK)[i];
      const size_t dst = count[(k >> shift) & 0xFFu]++;
      (*dstK)[dst] = k;
      (*dstV)[dst] = (*srcV)[i];
    }
    std::swap(srcK, dstK);
    std::swap(srcV, dstV);
  }
  if (srcV != &indicesOut) indicesOut.swap(*srcV);  // O(1): swaps the buffers, not the elements
}

}  // namespace gsv
