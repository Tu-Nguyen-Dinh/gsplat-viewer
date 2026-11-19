#pragma once

#include "gl/buffer.h"
#include "gl/shader.h"

#include <cstdint>
#include <string>

namespace gsv {

// Stable LSD radix sort of (uint key, uint value) pairs, entirely on the GPU.
//
// Per pass (4-bit digit):
//   1. radix_histogram.comp : digit histogram of every 1024-element block
//   2. scan_local / scan_add: exclusive prefix sum over all (digit, block) counts
//   3. radix_scatter.comp   : stable local ranking + scatter to the global position
//
// The element count is read from a GPU buffer (e.g. the instance count written by
// the preprocess pass), so the CPU never waits for the GPU.
class GpuRadixSort {
 public:
  bool init(std::string& log);
  bool reloadShaders(std::string& log);

  // Sorts keys/values in place. `countBuffer[countIndex]` (uint) holds the number of
  // valid elements, which must not exceed `capacity` (the size of keys/values).
  // keyBits = 32 (8 passes) or 16 (4 passes, keys must be < 65536).
  void sort(GlBuffer& keys, GlBuffer& values, const GlBuffer& countBuffer, uint32_t countIndex, uint32_t capacity,
            int keyBits);

  // Largest number of elements supported (limited by the two-level scan).
  static constexpr uint32_t kMaxElements = 16u * 1024u * 1024u;

 private:
  void ensureCapacity(uint32_t capacity);
  void scan(uint32_t length);

  ShaderProgram histogram_, scatter_, scanLocal_, scanAdd_;
  GlBuffer keysTmp_, valuesTmp_, blockHist_, blockSums_, blockSumsTotal_;
  uint32_t capacity_ = 0;
};

}  // namespace gsv
