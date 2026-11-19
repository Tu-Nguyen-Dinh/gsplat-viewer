// Tests for CPU depth sorting (src/core/sorting.*).
#include "core/sorting.h"
#include "test_framework.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <random>

using namespace gsv;

namespace {

std::vector<uint32_t> randomKeys(size_t n, uint32_t seed, uint32_t mask = 0xFFFFFFFFu) {
  std::mt19937 rng(seed);
  std::vector<uint32_t> k(n);
  for (auto& v : k) v = rng() & mask;
  return k;
}

// Ground truth: stable sort of indices by key.
std::vector<uint32_t> expectedOrder(const std::vector<uint32_t>& keys) {
  std::vector<uint32_t> idx(keys.size());
  std::iota(idx.begin(), idx.end(), 0u);
  std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
  return idx;
}

}  // namespace

TEST_CASE(depth_key_orders_far_to_near) {
  CHECK(depthKey32(10.0f) < depthKey32(5.0f));
  CHECK(depthKey32(5.0f) < depthKey32(0.001f));
  CHECK(depthKey32(1.0f) == depthKey32(1.0f));
  CHECK(depthKey32(-3.0f) == depthKey32(0.0f));  // behind the camera clamps to 0
}

TEST_CASE(compute_depth_keys_uses_view_depth) {
  const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0), glm::vec3(0, 1, 0));
  std::vector<uint32_t> keys;
  computeDepthKeys({{0, 0, 0}, {0, 0, 2}, {3, 1, -1}}, view, keys);
  CHECK(keys[0] == depthKey32(5.0f));
  CHECK(keys[1] == depthKey32(3.0f));
  CHECK(keys[2] == depthKey32(6.0f));
}

TEST_CASE(sort_std_and_radix_match_stable_sort) {
  RadixScratch scratch;
  for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(1000), size_t(200000)}) {
    // A small mask creates many duplicate keys, which checks stability.
    for (uint32_t mask : {0xFFFFFFFFu, 0x000003FFu, 0x00FF0000u}) {
      const std::vector<uint32_t> keys = randomKeys(n, static_cast<uint32_t>(n) + mask, mask);
      const std::vector<uint32_t> expected = expectedOrder(keys);
      std::vector<uint32_t> a, b;
      sortByKeyStd(keys, a);
      sortByKeyRadix(keys, b, scratch);
      CHECK(a == expected);
      CHECK(b == expected);
    }
  }
}

TEST_CASE(sort_timing_1m_keys) {
  // Not a pass/fail test: prints the cost of each method for the documentation.
  const std::vector<uint32_t> keys = randomKeys(1000000, 42);
  std::vector<uint32_t> out;
  RadixScratch scratch;
  auto time = [&](auto&& fn) {
    double best = 1e9;
    for (int r = 0; r < 5; ++r) {
      const auto t0 = std::chrono::steady_clock::now();
      fn();
      best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return best;
  };
  const double tStd = time([&] { sortByKeyStd(keys, out); });
  const double tRadix = time([&] { sortByKeyRadix(keys, out, scratch); });
  std::printf("    1M keys: std::sort %.2f ms, radix %.2f ms (%.1fx)\n", tStd, tRadix, tStd / tRadix);
  CHECK(tRadix > 0.0);
}
