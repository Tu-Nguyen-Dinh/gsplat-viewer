// Tests for scene file reading / writing.
#include "scene/demo_scene.h"
#include "scene/ply_io.h"
#include "scene/scene_io.h"
#include "test_framework.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace gsv;

namespace {
std::filesystem::path tempFile(const char* name) { return std::filesystem::temp_directory_path() / name; }
}  // namespace

TEST_CASE(ply_roundtrip_is_exact) {
  const GaussianScene a = makeDemoScene(2000, 7);
  std::string error;
  const auto path = tempFile("gsv_roundtrip.ply");
  CHECK(savePly(path, a, error));
  GaussianScene b;
  REQUIRE(loadPly(path, b, error));
  CHECK(b.size() == a.size());
  CHECK(b.shDegree == a.shDegree);
  bool same = true;
  for (size_t i = 0; i < a.size() && same; ++i)
    same = a.position[i] == b.position[i] && a.logScale[i] == b.logScale[i] && a.rotation[i] == b.rotation[i] &&
           a.opacityLogit[i] == b.opacityLogit[i] && a.shDC[i] == b.shDC[i];
  CHECK(same);
  CHECK(a.shRest == b.shRest);
  std::filesystem::remove(path);
}

TEST_CASE(ply_point_cloud_with_uchar_colors) {
  // A plain point cloud (x y z float + red green blue uchar) must load as small splats.
  const auto path = tempFile("gsv_points.ply");
  {
    std::ofstream f(path, std::ios::binary);
    f << "ply\nformat binary_little_endian 1.0\nelement vertex 2\n"
         "property float x\nproperty float y\nproperty float z\n"
         "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
    const float p0[3] = {1, 2, 3}, p1[3] = {-1, 0, 0.5f};
    const unsigned char c0[3] = {255, 0, 0}, c1[3] = {0, 0, 255};
    f.write(reinterpret_cast<const char*>(p0), 12);
    f.write(reinterpret_cast<const char*>(c0), 3);  // stride 15: floats are NOT 4-byte aligned after this
    f.write(reinterpret_cast<const char*>(p1), 12);
    f.write(reinterpret_cast<const char*>(c1), 3);
  }
  GaussianScene s;
  std::string error;
  REQUIRE(loadPly(path, s, error));
  CHECK(s.size() == 2);
  CHECK(s.position[1] == glm::vec3(-1, 0, 0.5f));
  CHECK_NEAR(baseColor(s, 0).r, 1.0, 1e-5);
  CHECK_NEAR(baseColor(s, 1).b, 1.0, 1e-5);
  std::filesystem::remove(path);
}

TEST_CASE(ply_rejects_truncated_file) {
  const GaussianScene a = makeDemoScene(100, 3);
  std::string error;
  const auto path = tempFile("gsv_truncated.ply");
  CHECK(savePly(path, a, error));
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 10);
  GaussianScene b;
  CHECK(!loadPly(path, b, error));
  CHECK(error.find("truncated") != std::string::npos);
  std::filesystem::remove(path);
}

TEST_CASE(ply_rejects_ascii) {
  const auto path = tempFile("gsv_ascii.ply");
  {
    std::ofstream f(path);
    f << "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nend_header\n1\n";
  }
  GaussianScene s;
  std::string error;
  CHECK(!loadPly(path, s, error));
  std::filesystem::remove(path);
}

TEST_CASE(splat_roundtrip_within_quantization) {
  // .splat stores colors, opacity and rotation in 8 bits: values come back rounded.
  const GaussianScene a = makeDemoScene(1000, 13, false);
  std::string error;
  const auto path = tempFile("gsv_roundtrip.splat");
  CHECK(saveSplat(path, a, error));
  CHECK(std::filesystem::file_size(path) == a.size() * 32);
  GaussianScene b;
  REQUIRE(loadSplat(path, b, error));
  CHECK(b.size() == a.size());
  float maxPos = 0, maxScale = 0, maxColor = 0, maxOpacity = 0, maxRotDot = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    maxPos = std::max(maxPos, glm::length(a.position[i] - b.position[i]));
    maxScale = std::max(maxScale, glm::length(a.logScale[i] - b.logScale[i]));
    maxColor = std::max(maxColor, glm::length(baseColor(a, i) - baseColor(b, i)));
    maxOpacity = std::max(maxOpacity, std::abs(sigmoid(a.opacityLogit[i]) - sigmoid(b.opacityLogit[i])));
    // q and -q are the same rotation: compare |dot| of the normalized quaternions.
    const float d = std::abs(glm::dot(glm::normalize(a.rotation[i]), glm::normalize(b.rotation[i])));
    maxRotDot = std::max(maxRotDot, 1.0f - d);
  }
  CHECK(maxPos == 0.0f);          // float32: exact
  CHECK(maxScale < 1e-5f);        // float32 linear -> log: tiny rounding
  CHECK(maxColor < 2.0f / 255.0f);
  CHECK(maxOpacity < 1.0f / 255.0f);
  CHECK(maxRotDot < 1e-3f);
  std::filesystem::remove(path);
}

TEST_CASE(splat_fixture_from_splat_transform) {
  // tests/data/minimal.splat comes from PlayCanvas splat-transform's test fixtures (MIT).
  const LoadResult r = loadScene(std::filesystem::path(GSV_TEST_DATA_DIR) / "minimal.splat");
  REQUIRE(r.ok);
  REQUIRE(r.scene.size() == 4);
  CHECK(r.removedInvalid == 0);
  CHECK(r.scene.position[0] == glm::vec3(-1, 0, -1));
  CHECK_NEAR(std::exp(r.scene.logScale[0].x), 0.1, 1e-6);
  // First record bytes 24..31 are 85 85 128 230 | 128 128 128 255: rgba then quaternion.
  CHECK_NEAR(sigmoid(r.scene.opacityLogit[0]), 230.0 / 255.0, 1e-4);
  CHECK_NEAR(baseColor(r.scene, 0).r, 85.0 / 255.0, 1e-5);
  // Rotation bytes 128 128 128 255 decode to (w, x, y, z) ~ (0, 0, 0, 1): 180 degrees about Z.
  // Note: the vec4 stores the quaternion as (w, x, y, z) in components [0..3].
  const glm::vec4 q = glm::normalize(r.scene.rotation[0]);
  CHECK_NEAR(q[0], 0.0, 1e-2);  // w
  CHECK_NEAR(q[3], 1.0, 1e-3);  // z
}

TEST_CASE(load_scene_dispatches_by_extension) {
  const GaussianScene a = makeDemoScene(200, 2);
  std::string error;
  const auto ply = tempFile("gsv_dispatch.PLY");  // extension check is case-insensitive
  CHECK(savePly(ply, a, error));
  const LoadResult r = loadScene(ply);
  REQUIRE(r.ok);
  CHECK(r.scene.size() == 200);
  CHECK(r.scene.shDegree == 3);
  CHECK(r.scene.sourceName == "gsv_dispatch.PLY");
  const LoadResult bad = loadScene(tempFile("scene.obj"));
  CHECK(!bad.ok);
  CHECK(bad.error.find("Unsupported") != std::string::npos);
  std::filesystem::remove(ply);
}

TEST_CASE(remove_invalid_splats_keeps_order) {
  GaussianScene s = makeDemoScene(10, 1);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const glm::vec3 keep7 = s.position[7];
  const float rest7 = s.shRest[7 * 45 + 3];
  s.position[2].y = nan;
  s.logScale[5].z = std::numeric_limits<float>::infinity();
  s.rotation[6] = glm::vec4(0.0f);  // zero quaternion
  CHECK(removeInvalidSplats(s) == 3);
  CHECK(s.size() == 7);
  CHECK(s.shRest.size() == 7 * 45);
  CHECK(s.position[4] == keep7);  // 0,1,3,4,7 -> index 4
  CHECK(s.shRest[4 * 45 + 3] == rest7);
}

TEST_CASE(load_scene_async) {
  const GaussianScene a = makeDemoScene(5000, 6);
  std::string error;
  const auto path = tempFile("gsv_async.ply");
  CHECK(savePly(path, a, error));
  std::future<LoadResult> f = loadSceneAsync(path);  // returns immediately
  const LoadResult r = f.get();                      // waits for the worker thread
  REQUIRE(r.ok);
  CHECK(r.scene.size() == 5000);
  CHECK(r.milliseconds > 0.0);
  std::filesystem::remove(path);
}
