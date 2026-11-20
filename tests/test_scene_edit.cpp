// Tests for scene editing (src/scene/scene_edit.*) and saving the result.
#include "scene/demo_scene.h"
#include "scene/ply_io.h"
#include "scene/scene_edit.h"
#include "test_framework.h"

#include <filesystem>

using namespace gsv;

namespace {

// Splat i sits at (i, 0, 0) with opacity 0.5 and scale 0.1, SH degree 1 with distinct values.
GaussianScene lineScene(int n) {
  GaussianScene s;
  s.shDegree = 1;
  s.shRestPerChannel = 3;
  for (int i = 0; i < n; ++i) {
    s.position.push_back({float(i), 0.0f, 0.0f});
    s.logScale.push_back(glm::vec3(std::log(0.1f)));
    s.rotation.push_back({1, 0, 0, 0});
    s.opacityLogit.push_back(0.0f);
    s.shDC.push_back({float(i), 0.0f, 0.0f});
    for (int k = 0; k < 9; ++k) s.shRest.push_back(float(i * 100 + k));
  }
  computeRobustBounds(s);
  return s;
}

}  // namespace

TEST_CASE(edit_select_box_and_sphere) {
  const GaussianScene s = lineScene(10);
  SelectionShape box;
  box.center = {4.5f, 0.0f, 0.0f};
  box.halfExtent = {2.0f, 1.0f, 1.0f};  // x in [2.5, 6.5] -> 3, 4, 5, 6
  CHECK(countSelected(selectInside(s, box)) == 4);
  SelectionShape sphere;
  sphere.type = SelectionShape::Type::Sphere;
  sphere.center = {0.0f, 0.0f, 0.0f};
  sphere.radius = 2.0f;  // 0, 1, 2 (boundary included)
  const SplatMask m = selectInside(s, sphere);
  CHECK(countSelected(m) == 3);
  CHECK(m[0] && m[1] && m[2] && !m[3]);
}

TEST_CASE(edit_filter_by_opacity_and_size) {
  GaussianScene s = lineScene(6);
  s.opacityLogit[1] = logit(0.01f);             // nearly transparent
  s.logScale[3] = {std::log(5.0f), 0.0f, 0.0f};  // one huge axis
  const SplatMask m = selectByFilter(s, 0.05f, 2.0f);
  CHECK(countSelected(m) == 2);
  CHECK(m[1] && m[3]);
  CHECK(countSelected(selectByFilter(s, 0.05f, 0.0f)) == 1);  // size test disabled
}

TEST_CASE(edit_invert_and_unite) {
  SplatMask a{1, 0, 0, 1}, b{0, 1, 0, 0};
  unite(a, b);
  CHECK(countSelected(a) == 3);
  invert(a);
  CHECK(countSelected(a) == 1 && a[2] == 1);
}

// ER 2.3: removing splats keeps every remaining value (SH rows stay aligned), and the edited
// scene saves and reloads bit-exact (fp32).
TEST_CASE(edit_remove_then_save_roundtrip_is_exact) {
  const GaussianScene s = lineScene(8);
  SplatMask m(8, 0);
  m[0] = m[3] = m[7] = 1;
  const GaussianScene e = removeSelected(s, m);
  REQUIRE(e.size() == 5);
  const int kept[5] = {1, 2, 4, 5, 6};
  for (size_t j = 0; j < 5; ++j) {
    CHECK(e.position[j].x == float(kept[j]));
    CHECK(e.shDC[j].x == float(kept[j]));
    CHECK(e.shRest[j * 9 + 8] == float(kept[j] * 100 + 8));
  }
  CHECK(e.boundsMax.x <= 6.0f);

  const GaussianScene big = removeSelected(makeDemoScene(3000, 3), selectByFilter(makeDemoScene(3000, 3), 0.3f, 0.0f));
  const auto path = std::filesystem::temp_directory_path() / "gsv_edited.ply";
  std::string error;
  REQUIRE(savePly(path, big, error));
  GaussianScene back;
  REQUIRE(loadPly(path, back, error));
  REQUIRE(back.size() == big.size());
  bool same = back.shRest == big.shRest;
  for (size_t i = 0; i < big.size() && same; ++i)
    same = back.position[i] == big.position[i] && back.logScale[i] == big.logScale[i] &&
           back.rotation[i] == big.rotation[i] && back.opacityLogit[i] == big.opacityLogit[i] &&
           back.shDC[i] == big.shDC[i];
  CHECK(same);
  std::filesystem::remove(path);
}
