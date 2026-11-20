// Tests for the .spz reader (src/scene/spz_io.*).
// Fixtures: `gsplat_viewer --gen demo.ply 2000` converted by PlayCanvas splat-transform 3.11.2
// with --spz-version 3 (gzip, supported) and 4 (zstd, must be rejected with a clear message).
#include "scene/demo_scene.h"
#include "scene/scene_io.h"
#include "scene/spz_io.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace gsv;

namespace {

std::filesystem::path spzFixture(const char* name) { return std::filesystem::path(GSV_TEST_DATA_DIR) / "spz" / name; }

void put24(std::vector<uint8_t>& d, int32_t v) {
  for (int b = 0; b < 3; ++b) d.push_back(uint8_t((v >> (8 * b)) & 0xFF));
}

}  // namespace

// Every attribute of the file matches the scene it was made from, within the format's
// quantization: 24-bit fixed point (12 fractional bits), 8-bit alpha / color / log scale,
// 10-bit quaternion fields, SH in 4-5 bits.
static void checkMatchesSource(const char* file) {
  const GaussianScene src = makeDemoScene(2000);  // what --gen wrote before the conversion
  GaussianScene s;
  std::string error;
  REQUIRE(loadSpz(spzFixture(file), s, error));
  REQUIRE(s.size() == src.size());
  CHECK(s.shDegree == 3 && s.shRestPerChannel == 15);
  double pos = 0, op = 0, dc = 0, scale = 0, rot = 0, sh = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    pos = std::max(pos, double(glm::length(s.position[i] - src.position[i])));
    op = std::max(op, double(std::abs(sigmoid(s.opacityLogit[i]) - sigmoid(src.opacityLogit[i]))));
    dc = std::max(dc, double(glm::length(s.shDC[i] - src.shDC[i])));
    scale = std::max(scale, double(glm::length(s.logScale[i] - src.logScale[i])));
    rot = std::max(rot, 1.0 - std::abs(double(glm::dot(glm::normalize(s.rotation[i]), glm::normalize(src.rotation[i])))));
    for (int k = 0; k < 45; ++k)
      sh = std::max(sh, double(std::abs(s.shRest[i * 45 + k] - std::clamp(src.shRest[i * 45 + k], -1.0f, 1.0f))));
  }
  std::printf("    max errors: position %.2e, opacity %.2e, dc %.2e, log scale %.2e, 1-|q.q'| %.2e, sh %.2e\n", pos, op,
              dc, scale, rot, sh);
  CHECK(pos < 2.2e-4);  // sqrt(3) * half of 1/4096 = 2.11e-4
  CHECK(op < 2.5e-3);   // half of 1/255 (+ float noise)
  CHECK(dc < 0.03);     // half of 1/(255 * 0.15), three channels
  CHECK(scale < 0.06);  // half of 1/16, three axes
  CHECK(rot < 1e-4);
  CHECK(sh < 0.07);     // 4-bit SH buckets: half of 16/128
}

TEST_CASE(spz_v3_matches_source_scene) { checkMatchesSource("demo2000_v3.spz"); }  // gzip
TEST_CASE(spz_v4_matches_source_scene) { checkMatchesSource("demo2000_v4.spz"); }  // zstd streams

// Both versions of the same scene decode to identical values (same quantized data, different
// compression).
TEST_CASE(spz_v3_and_v4_decode_identically) {
  GaussianScene a, b;
  std::string error;
  REQUIRE(loadSpz(spzFixture("demo2000_v3.spz"), a, error));
  REQUIRE(loadSpz(spzFixture("demo2000_v4.spz"), b, error));
  CHECK(a.position == b.position && a.logScale == b.logScale && a.rotation == b.rotation);
  CHECK(a.opacityLogit == b.opacityLogit && a.shDC == b.shDC && a.shRest == b.shRest);
}

TEST_CASE(spz_loads_through_scene_io) {
  const LoadResult r = loadScene(spzFixture("demo2000_v3.spz"));
  REQUIRE(r.ok);
  CHECK(r.scene.size() == 2000);
  CHECK(r.scene.boundsMax.x > r.scene.boundsMin.x);
}

// ER 2.5: a damaged file is reported, never a crash (v3 gzip and v4 zstd).
static void checkDamagedFile(const char* file, size_t flipAt) {
  std::ifstream f(spzFixture(file), std::ios::binary);
  const std::vector<uint8_t> good{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  REQUIRE(good.size() > 1000);
  const auto path = std::filesystem::temp_directory_path() / "gsv_bad.spz";
  auto tryLoad = [&](const std::vector<uint8_t>& bytes) {
    {
      std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    GaussianScene s;
    std::string error;
    const bool ok = loadSpz(path, s, error);
    return !ok && !error.empty();
  };
  CHECK(tryLoad(std::vector<uint8_t>(good.begin(), good.begin() + good.size() / 2)));  // truncated
  std::vector<uint8_t> flipped = good;
  flipped[flipAt] ^= 0xFF;
  CHECK(tryLoad(flipped));
  CHECK(tryLoad({1, 2, 3}));
  std::filesystem::remove(path);

  // Payload level: wrong magic, unknown version, fewer bytes than the header announces.
  GaussianScene s;
  std::string error;
  std::vector<uint8_t> p(16, 0);
  CHECK(!decodeSpzPayload(p, s, error));
  const uint32_t header[3] = {0x5053474E, 3, 1000};
  std::memcpy(p.data(), header, 12);
  CHECK(!decodeSpzPayload(p, s, error));  // 1000 points announced, no data
  CHECK(error.find("truncated") != std::string::npos);
  p[4] = 9;
  CHECK(!decodeSpzPayload(p, s, error));
}

TEST_CASE(spz_damaged_v3_is_an_error) { checkDamagedFile("demo2000_v3.spz", 10000); }  // inside the gzip data
TEST_CASE(spz_damaged_v4_is_an_error) { checkDamagedFile("demo2000_v4.spz", 33); }     // in the stream table

// Version 2 (3-byte rotations) built by hand: two points, degree 0, 12 fractional bits.
TEST_CASE(spz_v2_payload_decodes) {
  std::vector<uint8_t> d;
  const uint32_t header[3] = {0x5053474E, 2, 2};
  d.resize(12);
  std::memcpy(d.data(), header, 12);
  d.insert(d.end(), {0, 12, 0, 0});            // degree 0, 12 fractional bits, flags, reserved
  put24(d, 4096), put24(d, -2048), put24(d, 0);  // (1, -0.5, 0)
  put24(d, -4096 * 3), put24(d, 1), put24(d, 8191);
  d.insert(d.end(), {255, 0});                 // alphas
  d.insert(d.end(), {128, 128, 128, 255, 0, 64});
  d.insert(d.end(), {160, 160, 160, 0, 16, 255});  // log scale = s / 16 - 10
  d.insert(d.end(), {128, 128, 128, 255, 128, 128});  // ~identity; ~90 deg about x
  GaussianScene s;
  std::string error;
  REQUIRE(decodeSpzPayload(d, s, error));
  REQUIRE(s.size() == 2);
  CHECK_NEAR(s.position[0].x, 1.0, 1e-6);
  CHECK_NEAR(s.position[0].y, -0.5, 1e-6);
  CHECK_NEAR(s.position[1].x, -3.0, 1e-6);
  CHECK_NEAR(s.position[1].z, 8191.0 / 4096.0, 1e-6);
  CHECK(sigmoid(s.opacityLogit[0]) > 0.99f && sigmoid(s.opacityLogit[1]) < 0.01f);  // clamped, finite
  CHECK_NEAR(s.logScale[0].x, 0.0, 1e-6);
  CHECK_NEAR(s.logScale[1].x, -10.0, 1e-6);
  CHECK_NEAR(s.rotation[0].x, 1.0, 0.02);  // w of the identity
  CHECK_NEAR(s.rotation[1].y, 1.0, 0.01);  // x = 255 / 127.5 - 1 = 1, so w = 0
}
