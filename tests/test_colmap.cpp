// Tests for reading COLMAP sparse models (scene/colmap_io).
// tests/data/colmap_tiny/txt was written by hand with known poses; bin/ was produced from it by
// `colmap model_converter --output_type BIN` (COLMAP 4.2.1), so both formats must agree.
#include "scene/colmap_io.h"
#include "test_framework.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace gsv;

namespace {

const std::filesystem::path kTiny = std::filesystem::path(GSV_TEST_DATA_DIR) / "colmap_tiny";

bool near(const glm::dvec3& a, const glm::dvec3& b, double tol) { return glm::length(a - b) <= tol; }

void checkTinyModel(const ColmapModel& m) {
  REQUIRE(m.cameras.size() == 2);
  REQUIRE(m.images.size() == 3);

  const ColmapCamera* c1 = m.camera(1);
  REQUIRE(c1 != nullptr);
  CHECK(c1->model == 1 && c1->width == 640 && c1->height == 480);  // PINHOLE
  CHECK(c1->pinhole() == glm::dvec4(500, 510, 320, 240));
  const ColmapCamera* c2 = m.camera(2);
  REQUIRE(c2 != nullptr);
  CHECK(c2->model == 2);                                      // SIMPLE_RADIAL: f, cx, cy, k
  CHECK(c2->pinhole() == glm::dvec4(700, 700, 400, 300));     // distortion term ignored

  // Sorted by name; centers C = -R^T t and viewing directions as designed in images.txt.
  const ColmapImage& a = m.images[0];
  const ColmapImage& b = m.images[1];
  const ColmapImage& c = m.images[2];
  CHECK(a.name == "a.jpg" && b.name == "b.jpg" && c.name == "c.jpg");
  CHECK(near(a.center(), {0, 0, -5}, 1e-9));
  CHECK(near(b.center(), {-5, 0, 0}, 1e-9));
  CHECK(near(c.center(), {1, 2, -5}, 1e-9));
  CHECK(near(a.forwardInWorld(), {0, 0, 1}, 1e-9));
  CHECK(near(b.forwardInWorld(), {1, 0, 0}, 1e-9));
  CHECK(near(a.upInWorld(), {0, -1, 0}, 1e-9));  // COLMAP: image +Y points down
  CHECK(c.cameraId == 2);
}

}  // namespace

TEST_CASE(colmap_text_model) {
  ColmapModel m;
  std::string error;
  REQUIRE(loadColmapModel(kTiny / "txt", m, error));
  checkTinyModel(m);
}

TEST_CASE(colmap_binary_model_matches_text) {
  ColmapModel bin, txt;
  std::string error;
  REQUIRE(loadColmapModel(kTiny / "bin", bin, error));
  checkTinyModel(bin);
  REQUIRE(loadColmapModel(kTiny / "txt", txt, error));
  for (size_t i = 0; i < 3; ++i) {
    // Quaternion -> matrix -> quaternion round trip and text/binary agreement (PLAN 1.6: < 1e-6).
    const glm::dquat q = glm::quat_cast(bin.images[i].worldToCameraRotation());
    CHECK(std::abs(std::abs(glm::dot(q, bin.images[i].rotation)) - 1.0) < 1e-6);
    CHECK(std::abs(std::abs(glm::dot(bin.images[i].rotation, txt.images[i].rotation)) - 1.0) < 1e-12);
    CHECK(bin.images[i].translation == txt.images[i].translation);
  }
}

TEST_CASE(colmap_gl_view_matrix) {
  ColmapModel m;
  std::string error;
  REQUIRE(loadColmapModel(kTiny / "txt", m, error));
  // Camera b sits at (-5,0,0) looking at the origin: in OpenGL view space the origin is straight
  // ahead (-Z) at distance 5, and the camera itself maps to the view-space origin.
  const glm::mat4 v = m.images[1].viewMatrixGl();
  const glm::vec4 origin = v * glm::vec4(0, 0, 0, 1);
  CHECK(glm::length(glm::vec3(origin) - glm::vec3(0, 0, -5)) < 1e-5f);
  const glm::vec4 self = v * glm::vec4(-5, 0, 0, 1);
  CHECK(glm::length(glm::vec3(self)) < 1e-5f);
  // A point above the scene (world -Y is "up" for these COLMAP cameras) appears above the center.
  CHECK((v * glm::vec4(0, -1, 0, 1)).y > 0.0f);
}

TEST_CASE(colmap_average_up_and_model_lookup) {
  ColmapModel m;
  std::string error;
  REQUIRE(loadColmapModel(kTiny / "txt", m, error));
  CHECK(glm::length(averageCameraUp(m) - glm::vec3(0, -1, 0)) < 1e-6f);
  CHECK(findColmapModelFor(kTiny / "scene.ply").empty());  // colmap_tiny has no sparse/ folder
  CHECK(!loadColmapModel(kTiny / "missing", m, error));
}
