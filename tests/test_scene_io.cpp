// Tests for scene file reading / writing.
#include "scene/demo_scene.h"
#include "scene/ply_io.h"
#include "test_framework.h"

#include <cstring>
#include <filesystem>
#include <fstream>

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
  CHECK(loadPly(path, b, error));
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
  CHECK(loadPly(path, s, error));
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
