// Tests for the compressed GPU splat format (src/scene/splat_codec.*).
#include "core/gaussian_math.h"
#include "scene/demo_scene.h"
#include "scene/splat_codec.h"
#include "test_framework.h"

#include <random>

using namespace gsv;

TEST_CASE(codec_quaternion_roundtrip) {
  std::mt19937 rng(5);
  std::normal_distribution<float> n(0.0f, 1.0f);
  double maxMatrixError = 0;
  for (int i = 0; i < 20000; ++i) {
    const glm::vec4 q(n(rng), n(rng), n(rng), n(rng));  // random, unnormalized, any sign
    const glm::mat3 a = rotationFromQuaternion(q), b = rotationFromQuaternion(unpackQuaternion(packQuaternion(q)));
    for (int c = 0; c < 3; ++c)
      for (int r = 0; r < 3; ++r) maxMatrixError = std::max(maxMatrixError, double(std::abs(a[c][r] - b[c][r])));
  }
  std::printf("    max rotation matrix error %.2e\n", maxMatrixError);
  CHECK(maxMatrixError < 4e-3);  // 10 bits per component
  // q and -q encode the same rotation; identity stays exact.
  const glm::vec4 id = unpackQuaternion(packQuaternion({-1, 0, 0, 0}));
  CHECK_NEAR(id.x, 1.0, 1e-6);
}

TEST_CASE(codec_quantize16_error_bound) {
  const float lo = -3.0f, hi = 5.0f;
  double maxError = 0;
  for (int i = 0; i <= 10000; ++i) {
    const float x = lo + (hi - lo) * i / 10000.0f;
    maxError = std::max(maxError, double(std::abs(dequantize16(quantize16(x, lo, hi), lo, hi) - x)));
  }
  CHECK(maxError <= (hi - lo) / 65535.0 * 0.5 + 1e-6);  // half a step
  CHECK(quantize16(-10.0f, lo, hi) == 0);
  CHECK(quantize16(10.0f, lo, hi) == 65535);
}

// Sorting along the Morton curve makes 256-splat chunks small, which is what makes 16-bit
// positions precise: on a random cloud a chunk must be far smaller than the whole scene.
TEST_CASE(codec_morton_chunks_are_compact) {
  std::mt19937 rng(6);
  std::uniform_real_distribution<float> u(-50.0f, 50.0f);
  std::vector<glm::vec3> p(65536);
  for (glm::vec3& v : p) v = {u(rng), u(rng), u(rng)};
  const std::vector<uint32_t> order = mortonOrder(p);
  REQUIRE(order.size() == p.size());
  double meanDiagonal = 0;
  for (size_t c = 0; c < p.size() / kCodecChunk; ++c) {
    glm::vec3 lo(INFINITY), hi(-INFINITY);
    for (size_t k = c * kCodecChunk; k < (c + 1) * kCodecChunk; ++k) lo = glm::min(lo, p[order[k]]), hi = glm::max(hi, p[order[k]]);
    meanDiagonal += glm::length(hi - lo);
  }
  meanDiagonal /= double(p.size() / kCodecChunk);
  std::printf("    mean chunk diagonal %.2f (scene diagonal %.1f)\n", meanDiagonal, 100.0 * std::sqrt(3.0));
  CHECK(meanDiagonal < 0.25 * 100.0 * std::sqrt(3.0));
}

TEST_CASE(codec_pack_unpack_scene) {
  const GaussianScene s = makeDemoScene(5000, 9);
  const std::vector<uint32_t> order = mortonOrder(s.position);
  std::vector<PackedSplat> packed;
  std::vector<ChunkBounds> chunks;
  packSplats(s, order, packed, chunks);
  REQUIRE(packed.size() == s.size());
  REQUIRE(chunks.size() == (s.size() + kCodecChunk - 1) / kCodecChunk);
  double posErr = 0, opErr = 0, scaleErr = 0, dcErr = 0;
  for (size_t k = 0; k < packed.size(); ++k) {
    const size_t i = order[k];
    const DecodedSplat d = unpackSplat(packed[k], chunks[k / kCodecChunk]);
    const glm::vec3 extent = glm::vec3(chunks[k / kCodecChunk].max - chunks[k / kCodecChunk].min);
    posErr = std::max(posErr, double(glm::length((d.position - s.position[i]) / glm::max(extent, glm::vec3(1e-9f)))));
    opErr = std::max(opErr, double(std::abs(d.opacity - sigmoid(s.opacityLogit[i]))));
    scaleErr = std::max(scaleErr, double(glm::length(d.logScale - s.logScale[i])));
    dcErr = std::max(dcErr, double(glm::length(d.dc - s.shDC[i])));
  }
  std::printf("    max errors: position %.2e of chunk, opacity %.2e, log scale %.2e, dc %.2e\n", posErr, opErr,
              scaleErr, dcErr);
  CHECK(posErr < 2e-5);   // half a 16-bit step per axis: sqrt(3) * 0.5 / 65535
  CHECK(opErr < 5e-4);    // fp16
  CHECK(scaleErr < 1e-2);
  CHECK(dcErr < 5e-3);
}
