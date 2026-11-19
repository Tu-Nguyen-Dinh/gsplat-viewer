// GPU radix sort must produce exactly the same (stable) order as std::stable_sort.
#include "gl/buffer.h"
#include "render/gpu_radix_sort.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <random>

using namespace gsv;

namespace {

struct SortResult {
  std::vector<uint32_t> keys, values;
  double gpuMs = 0.0;
};

// Uploads keys/values, runs the GPU sort on the first `count` elements and reads back.
SortResult gpuSort(GpuRadixSort& sorter, const std::vector<uint32_t>& keys, uint32_t count, int keyBits) {
  const uint32_t capacity = static_cast<uint32_t>(keys.size());
  std::vector<uint32_t> values(capacity);
  std::iota(values.begin(), values.end(), 0u);
  GlBuffer k, v, c;
  k.allocate(capacity * sizeof(uint32_t), keys.data());
  v.allocate(capacity * sizeof(uint32_t), values.data());
  const uint32_t countData[2] = {0xDEADu, count};  // count stored at index 1, like the draw command
  c.allocate(sizeof(countData), countData);

  GLuint query;
  glGenQueries(1, &query);
  glBeginQuery(GL_TIME_ELAPSED, query);
  sorter.sort(k, v, c, 1, capacity, keyBits);
  glEndQuery(GL_TIME_ELAPSED);
  GLuint64 ns = 0;
  glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);  // blocking read: fine in a test
  glDeleteQueries(1, &query);

  SortResult r;
  r.gpuMs = static_cast<double>(ns) * 1e-6;
  r.keys.resize(count);
  r.values.resize(count);
  k.download(0, count * sizeof(uint32_t), r.keys.data());
  v.download(0, count * sizeof(uint32_t), r.values.data());
  return r;
}

std::vector<uint32_t> stableOrder(const std::vector<uint32_t>& keys, uint32_t count) {
  std::vector<uint32_t> idx(count);
  std::iota(idx.begin(), idx.end(), 0u);
  std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
  return idx;
}

std::vector<uint32_t> randomKeys(size_t n, uint32_t seed, uint32_t mask) {
  std::mt19937 rng(seed);
  std::vector<uint32_t> k(n);
  for (auto& x : k) x = rng() & mask;
  return k;
}

}  // namespace

GPU_TEST_CASE(gpu_radix_sort_matches_stable_sort) {
  GpuRadixSort sorter;
  std::string log;
  CHECK(sorter.init(log));
  if (!log.empty()) std::printf("%s\n", log.c_str());
  // Sizes around the 1024-element block and 512-element scan boundaries.
  for (uint32_t n : {1u, 2u, 1000u, 1024u, 1025u, 4096u, 100000u, 600000u}) {
    for (uint32_t mask : {0xFFFFFFFFu, 0x0000003Fu}) {  // 0x3F: many duplicates -> checks stability
      const std::vector<uint32_t> keys = randomKeys(n, n ^ mask, mask);
      const SortResult r = gpuSort(sorter, keys, n, 32);
      const std::vector<uint32_t> expected = stableOrder(keys, n);
      bool ok = r.values == expected;
      for (uint32_t i = 0; ok && i < n; ++i) ok = r.keys[i] == keys[expected[i]];
      if (!ok) std::printf("    mismatch for n=%u mask=%08x\n", n, mask);
      CHECK(ok);
    }
  }
}

GPU_TEST_CASE(gpu_radix_sort_respects_gpu_count) {
  // Only the first `count` elements (count read from a GPU buffer) are sorted.
  GpuRadixSort sorter;
  std::string log;
  CHECK(sorter.init(log));
  const std::vector<uint32_t> keys = randomKeys(50000, 9, 0xFFFFFFFFu);
  const uint32_t count = 31337;
  const SortResult r = gpuSort(sorter, keys, count, 32);
  CHECK(r.values == stableOrder(keys, count));
}

GPU_TEST_CASE(gpu_radix_sort_16bit_keys) {
  GpuRadixSort sorter;
  std::string log;
  CHECK(sorter.init(log));
  const std::vector<uint32_t> keys = randomKeys(200000, 3, 0xFFFFu);
  const SortResult r = gpuSort(sorter, keys, 200000, 16);
  CHECK(r.values == stableOrder(keys, 200000));
}

GPU_TEST_CASE(gpu_radix_sort_timing) {
  GpuRadixSort sorter;
  std::string log;
  CHECK(sorter.init(log));
  for (uint32_t n : {300000u, 1000000u, 3000000u}) {
    const std::vector<uint32_t> keys32 = randomKeys(n, 1, 0xFFFFFFFFu), keys16 = randomKeys(n, 2, 0xFFFFu);
    gpuSort(sorter, keys32, n, 32);  // warm-up (allocations, shader caches)
    const double t32 = gpuSort(sorter, keys32, n, 32).gpuMs;
    const double t16 = gpuSort(sorter, keys16, n, 16).gpuMs;
    std::printf("    %7u keys: GPU radix 32-bit %.2f ms, 16-bit %.2f ms\n", n, t32, t16);
  }
}
