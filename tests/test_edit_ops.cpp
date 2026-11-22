// Core 5: selection, editing operations, edit history and project file (ER 5.1-5.10).
#include "scene/edit_history.h"
#include "scene/edit_ops.h"
#include "scene/project_file.h"
#include "core/image.h"
#include "core/kdtree.h"
#include "test_framework.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <filesystem>
#include <random>

using namespace gsv;

namespace {

void addSplat(GaussianScene& s, glm::vec3 p, glm::vec3 color, float opacity = 0.9f) {
  s.position.push_back(p);
  s.logScale.push_back(glm::vec3(std::log(0.01f)));
  s.rotation.push_back({1, 0, 0, 0});
  s.opacityLogit.push_back(logit(opacity));
  s.shDC.push_back((color - glm::vec3(0.5f)) / kShC0);
  for (int k = 0; k < s.shRestPerChannel * 3; ++k) s.shRest.push_back(0.0f);
}

// Grid-sampled box surface (spacing h) so neighbours are always within ~h.
void addBox(GaussianScene& s, glm::vec3 lo, glm::vec3 hi, float h, glm::vec3 color) {
  for (float x = lo.x; x <= hi.x + 1e-4f; x += h)
    for (float y = lo.y; y <= hi.y + 1e-4f; y += h)
      for (float z = lo.z; z <= hi.z + 1e-4f; z += h) {
        const bool onFace = std::fabs(x - lo.x) < 1e-4f || std::fabs(x - hi.x) < h * 0.5f || std::fabs(y - lo.y) < 1e-4f ||
                            std::fabs(y - hi.y) < h * 0.5f || std::fabs(z - lo.z) < 1e-4f || std::fabs(z - hi.z) < h * 0.5f;
        if (onFace) addSplat(s, {x, y, z}, color);
      }
}

void addFloor(GaussianScene& s, float size, float h, glm::vec3 color) {
  for (float x = -size; x <= size; x += h)
    for (float z = -size; z <= size; z += h) addSplat(s, {x, 0.0f, z}, color);
}

bool sameScene(const GaussianScene& a, const GaussianScene& b) {
  auto eq = [](const auto& x, const auto& y) {
    return x.size() == y.size() && (x.empty() || std::memcmp(x.data(), y.data(), x.size() * sizeof(x[0])) == 0);
  };
  return eq(a.position, b.position) && eq(a.logScale, b.logScale) && eq(a.rotation, b.rotation) &&
         eq(a.opacityLogit, b.opacityLogit) && eq(a.shDC, b.shDC) && eq(a.shRest, b.shRest);
}

GaussianScene randomScene(size_t n, int degree, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  GaussianScene s;
  s.shDegree = degree;
  s.shRestPerChannel = shCoeffCount(degree) - 1;
  for (size_t i = 0; i < n; ++i) {
    s.position.push_back({u(rng) * 3, u(rng) * 3, u(rng) * 3});
    s.logScale.push_back({u(rng) - 3, u(rng) - 3, u(rng) - 3});
    s.rotation.push_back(glm::normalize(glm::vec4(u(rng), u(rng), u(rng), u(rng))));
    s.opacityLogit.push_back(u(rng) * 3);
    s.shDC.push_back({u(rng), u(rng), u(rng)});
    for (int k = 0; k < s.shRestPerChannel * 3; ++k) s.shRest.push_back(u(rng) * 0.2f);
  }
  computeRobustBounds(s);
  return s;
}

}  // namespace

// ER 5.1: screen selection is exact for rect, polygon (incl. concave) and circle.
TEST_CASE(edit5_screen_selection) {
  const std::vector<glm::vec2> concave = {{0, 0}, {10, 0}, {10, 10}, {5, 4}, {0, 10}};  // a "V" notch at the top
  CHECK(pointInPolygon({2, 2}, concave));
  CHECK(pointInPolygon({9, 8}, concave));
  CHECK(!pointInPolygon({5, 8}, concave));  // inside the notch
  CHECK(!pointInPolygon({-1, 5}, concave));

  // 21 x 21 grid of splats on the plane z = -5, seen by an identity-view 90-degree camera.
  GaussianScene s;
  for (int y = -10; y <= 10; ++y)
    for (int x = -10; x <= 10; ++x) addSplat(s, {x * 0.2f, y * 0.2f, -5.0f}, glm::vec3(0.5f));
  addSplat(s, {0, 0, 5}, glm::vec3(0.5f));  // behind the camera: never selected
  const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
  const glm::vec2 vp(1000, 1000);
  // x = 0.2*i at z=-5 -> ndc 0.04*i -> pixel 500 + 20*i ; y flipped.
  const SplatMask rect = selectScreenRect(s, proj, vp, {490, 490}, {550, 530});  // i in 0..2, j in -1..0
  CHECK(countSelected(rect) == 3 * 2);
  const SplatMask circle = selectScreenCircle(s, proj, vp, {500, 500}, 20.5f);  // centre + 4 neighbours
  CHECK(countSelected(circle) == 5);
  const std::vector<glm::vec2> tri = {{489, 489}, {561, 489}, {489, 561}};  // hypotenuse x + y = 1050
  // grid pixels (500+20i, 500+20j), i, j >= 0, inside x + y < 1050  ->  i + j <= 2: 6 points
  CHECK(countSelected(selectScreenPolygon(s, proj, vp, tri)) == 6);
}

// ER 5.2: selection set operations follow their truth tables.
TEST_CASE(edit5_select_ops_truth_table) {
  const SplatMask a = {0, 0, 1, 1}, b = {0, 1, 0, 1};
  SplatMask m = a;
  combineSelection(m, b, SelectOp::Replace);
  CHECK((m == SplatMask{0, 1, 0, 1}));
  m = a;
  combineSelection(m, b, SelectOp::Add);
  CHECK((m == SplatMask{0, 1, 1, 1}));
  m = a;
  combineSelection(m, b, SelectOp::Subtract);
  CHECK((m == SplatMask{0, 0, 1, 0}));
  m = a;
  combineSelection(m, b, SelectOp::Intersect);
  CHECK((m == SplatMask{0, 0, 0, 1}));
}

// ER 5.3: sphere brush and connected (flood) selection separate objects.
TEST_CASE(edit5_brush_and_connected) {
  GaussianScene s;
  addFloor(s, 2.0f, 0.04f, {0.5f, 0.5f, 0.5f});
  const size_t floorN = s.size();
  addBox(s, {0.0f, 0.02f, 0.0f}, {0.4f, 0.42f, 0.4f}, 0.02f, {0.9f, 0.1f, 0.1f});
  const size_t box1End = s.size();
  addBox(s, {1.0f, 0.02f, 1.0f}, {1.4f, 0.42f, 1.4f}, 0.02f, {0.85f, 0.12f, 0.1f});
  computeRobustBounds(s);

  // Sphere brush around box 1 above the floor.
  const SplatMask brush = selectSphere(s, glm::mat4(1.0f), {0.2f, 0.25f, 0.2f}, 0.2f);
  size_t floorHit = 0;
  for (size_t i = 0; i < floorN; ++i) floorHit += brush[i];
  CHECK(floorHit == 0);
  CHECK(countSelected(brush) > 0);

  // Connected selection from a point on box 1.
  const uint32_t seed = nearestSplat(s, {0.2f, 0.42f, 0.2f});
  REQUIRE(seed >= floorN && seed < box1End);
  const SplatMask c = selectConnected(s, seed, 0.05f, 0.15f);
  size_t inBox1 = 0, other = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!c[i]) continue;
    if (i >= floorN && i < box1End) ++inBox1;
    else ++other;
  }
  CHECK(inBox1 >= static_cast<size_t>(0.99 * static_cast<double>(box1End - floorN)));
  CHECK(other == 0);
  std::printf("    connected: %zu / %zu of box 1, %zu others\n", inBox1, box1End - floorN, other);
}

// ER 5.4: SH rotation keeps the view-dependent colour: new(d) == old(R^T d), every band, <= 1e-5.
TEST_CASE(edit5_sh_rotation_exact) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  float maxErr = 0.0f;
  for (int trial = 0; trial < 5; ++trial) {
    const glm::quat q = glm::normalize(glm::quat(u(rng), u(rng), u(rng), u(rng)));
    const glm::mat3 R = glm::mat3_cast(q);
    const ShRotation shr = shRotation(R);
    float rest[45], rotated[45];
    for (float& v : rest) v = u(rng) * 0.1f;
    std::memcpy(rotated, rest, sizeof rest);
    rotateShRest(rotated, 15, shr);
    const glm::vec3 dc(4.0f);  // keeps colour > 0 so the clamp never hides an error
    for (int k = 0; k < 200; ++k) {
      const glm::vec3 d = glm::normalize(glm::vec3(u(rng), u(rng), u(rng)));
      for (int degree = 1; degree <= 3; ++degree) {
        const glm::vec3 a = evalSphericalHarmonics(degree, d, dc, rotated, 15);
        const glm::vec3 b = evalSphericalHarmonics(degree, glm::transpose(R) * d, dc, rest, 15);
        maxErr = std::max(maxErr, glm::length(a - b));
      }
    }
  }
  std::printf("    SH rotation max colour error %.3g\n", static_cast<double>(maxErr));
  CHECK(maxErr <= 1e-5f);
}

// ER 5.4 (geometry): a similarity transform moves positions and covariances exactly.
TEST_CASE(edit5_transform_splats) {
  GaussianScene s = randomScene(50, 3, 3);
  const GaussianScene before = s;
  const glm::mat4 T = glm::translate(glm::mat4(1.0f), {1, 2, 3}) *
                      glm::rotate(glm::mat4(1.0f), 0.7f, glm::normalize(glm::vec3(1, 2, 0.5f))) *
                      glm::scale(glm::mat4(1.0f), glm::vec3(2.5f));
  SplatMask mask(s.size(), 1);
  mask[0] = 0;
  transformSplats(s, mask, T);
  CHECK(sameScene(GaussianScene{}, GaussianScene{}));
  CHECK(s.position[0] == before.position[0]);
  float posErr = 0.0f, covErr = 0.0f;
  const glm::mat3 M(T);
  for (size_t i = 1; i < s.size(); ++i) {
    posErr = std::max(posErr, glm::length(s.position[i] - glm::vec3(T * glm::vec4(before.position[i], 1.0f))));
    const Cov3 c0 = covarianceFromScaleRotation(glm::exp(before.logScale[i]), before.rotation[i]);
    const Cov3 c1 = covarianceFromScaleRotation(glm::exp(s.logScale[i]), s.rotation[i]);
    const glm::mat3 m0(c0.xx, c0.xy, c0.xz, c0.xy, c0.yy, c0.yz, c0.xz, c0.yz, c0.zz);
    const glm::mat3 m1(c1.xx, c1.xy, c1.xz, c1.xy, c1.yy, c1.yz, c1.xz, c1.yz, c1.zz);
    const glm::mat3 expect = M * m0 * glm::transpose(M);
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b)
        covErr = std::max(covErr, std::fabs(m1[a][b] - expect[a][b]) / (std::fabs(expect[a][a]) + 1e-12f));
  }
  CHECK(posErr < 1e-5f);
  CHECK(covErr < 1e-4f);
}

// ER 5.5: undo / redo over 6 mixed operations is bit-exact.
TEST_CASE(edit5_history_bit_exact) {
  GaussianScene s = randomScene(2000, 3, 11);
  const GaussianScene original = s;
  EditHistory h;
  std::vector<GaussianScene> states{s};
  auto every = [&](int m) {
    SplatMask k(s.size(), 0);
    for (size_t i = 0; i < s.size(); i += static_cast<size_t>(m)) k[i] = 1;
    return k;
  };
  h.modify(s, every(3), "move", [](GaussianScene& g, const SplatMask& m) {
    transformSplats(g, m, glm::rotate(glm::translate(glm::mat4(1.0f), {0.5f, 0, 0}), 0.3f, glm::vec3(0, 1, 0)));
  });
  states.push_back(s);
  h.remove(s, every(5), "delete");
  states.push_back(s);
  h.modify(s, every(2), "colour", [](GaussianScene& g, const SplatMask& m) {
    ColorAdjust a;
    a.saturation = 0.3f;
    a.brightness = 0.1f;
    adjustColor(g, m, a);
  });
  states.push_back(s);
  h.remove(s, every(7), "delete 2");
  states.push_back(s);
  h.modify(s, every(4), "scale", [](GaussianScene& g, const SplatMask& m) {
    transformSplats(g, m, glm::scale(glm::mat4(1.0f), glm::vec3(1.7f)));
  });
  states.push_back(s);
  h.remove(s, every(11), "delete 3");
  states.push_back(s);
  REQUIRE(h.size() == 6);
  for (int k = 6; k > 0; --k) {
    CHECK(h.undo(s));
    CHECK(sameScene(s, states[static_cast<size_t>(k - 1)]));
  }
  CHECK(!h.undo(s));
  CHECK(sameScene(s, original));
  for (int k = 1; k <= 6; ++k) {
    CHECK(h.redo(s));
    CHECK(sameScene(s, states[static_cast<size_t>(k)]));
  }
  // A new command after undo drops the redo branch.
  h.undo(s);
  h.remove(s, every(13), "branch");
  CHECK(!h.canRedo());
  CHECK(h.size() == 6);
  // Memory budget: a tiny budget keeps only the newest command.
  EditHistory small(1024);
  GaussianScene t = randomScene(500, 3, 5);
  small.remove(t, SplatMask(t.size() / 2, 1), "half");  // > 1 KB: over budget, still kept
  small.remove(t, SplatMask(t.size(), 0), "none");  // empty: not recorded
  CHECK(small.size() == 1);
}

// ER 5.6: floaters in free space are found (>= 90 %) with <= 1 % false positives.
TEST_CASE(edit5_floater_detection) {
  GaussianScene s;
  addFloor(s, 2.0f, 0.03f, glm::vec3(0.5f));
  addBox(s, {-0.5f, 0.0f, -0.5f}, {0.5f, 0.8f, 0.5f}, 0.025f, glm::vec3(0.6f));
  const size_t surfaceN = s.size();
  std::mt19937 rng(9);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  int floaters = 0;
  while (floaters < 200) {  // free space: at least 0.3 from the floor and the box
    const glm::vec3 p(-1.8f + 3.6f * u(rng), 0.3f + 1.5f * u(rng), -1.8f + 3.6f * u(rng));
    const glm::vec3 q = glm::clamp(p, glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 0.8f, 0.5f));
    if (glm::length(p - q) < 0.3f) continue;
    addSplat(s, p, glm::vec3(0.8f), 0.5f);
    ++floaters;
  }
  const SplatMask m = selectOutliers(s);
  size_t tp = 0, fp = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!m[i]) continue;
    if (i >= surfaceN) ++tp;
    else ++fp;
  }
  std::printf("    floaters found %zu / %d, false positives %zu / %zu\n", tp, floaters, fp, surfaceN);
  CHECK(tp >= static_cast<size_t>(0.9 * floaters));
  CHECK(fp <= surfaceN / 100);

  // Camera visibility: a splat no training camera saw is selected.
  GaussianScene v;
  addSplat(v, {0, 0, -5}, glm::vec3(0.5f));  // in front
  addSplat(v, {0, 0, 5}, glm::vec3(0.5f));   // behind
  addSplat(v, {50, 0, -5}, glm::vec3(0.5f)); // outside the image
  CameraView cam;
  cam.focal = {500, 500};
  cam.principal = {500, 500};
  cam.size = {1000, 1000};
  const SplatMask unseen = selectUnseen(v, {cam}, 1);
  CHECK((unseen == SplatMask{0, 1, 1}));
}

// ER 5.7: colour adjustment: neutral is bit-exact, saturation 0 gives luma grey.
TEST_CASE(edit5_color_adjust) {
  GaussianScene s = randomScene(100, 3, 21);
  const GaussianScene before = s;
  const SplatMask all(s.size(), 1);
  adjustColor(s, all, ColorAdjust{});
  CHECK(sameScene(s, before));
  ColorAdjust grey;
  grey.saturation = 0.0f;
  adjustColor(s, all, grey);
  float err = 0.0f, viewErr = 0.0f;
  for (size_t i = 0; i < s.size(); ++i) {
    const glm::vec3 c0 = glm::vec3(0.5f) + kShC0 * before.shDC[i];
    const float luma = 0.2126f * c0.r + 0.7152f * c0.g + 0.0722f * c0.b;
    const glm::vec3 c1 = glm::vec3(0.5f) + kShC0 * s.shDC[i];
    err = std::max(err, glm::length(c1 - glm::vec3(luma)));
    const glm::vec3 col = evalSphericalHarmonics(3, glm::normalize(glm::vec3(1, 2, 3)), s.shDC[i], s.restOf(i), 15);
    viewErr = std::max(viewErr, std::max(std::fabs(col.r - col.g), std::fabs(col.g - col.b)));
  }
  CHECK(err < 1e-5f);
  CHECK(viewErr < 1e-5f);  // view-dependent colour is grey too
}

// ER 5.8: scale from a known length and level from a floor normal.
TEST_CASE(edit5_scale_and_level) {
  CHECK_NEAR(scaleForKnownLength({0, 0, 0}, {0, 0.5, 0}, 1.2), 2.4, 1e-12);
  const glm::vec3 tilted = glm::normalize(glm::vec3(0.1f, 1.0f, -0.05f));
  const glm::mat4 R = rotationBetween(tilted, {0, 1, 0});
  const glm::vec3 up = glm::normalize(glm::vec3(R * glm::vec4(tilted, 0.0f)));
  const double angle = std::acos(std::min(1.0, static_cast<double>(up.y))) * 180.0 / 3.14159265358979;
  CHECK(angle <= 0.01);
  const glm::mat4 flip = rotationBetween({0, -1, 0}, {0, 1, 0});
  CHECK(glm::vec3(flip * glm::vec4(0, -1, 0, 0)).y > 0.9999f);
}

// ER 5.9: polyline length and polygon area (non-axis-aligned plane).
TEST_CASE(edit5_measurements) {
  CHECK_NEAR(polylineLength({{0, 0, 0}, {3, 0, 0}, {3, 4, 0}}), 7.0, 1e-12);
  // 3 x 4 rectangle, tilted and moved far from the origin.
  const glm::dmat3 R = glm::dmat3(glm::rotate(glm::dmat4(1.0), 0.6, glm::normalize(glm::dvec3(1, 1, 0.3))));
  const glm::dvec3 o(5e5, 1.2e6, 30.0);
  std::vector<glm::dvec3> rect = {o, o + R * glm::dvec3(3, 0, 0), o + R * glm::dvec3(3, 4, 0), o + R * glm::dvec3(0, 4, 0)};
  CHECK_NEAR(polygonArea(rect), 12.0, 1e-9);
}

// ER 5.10: project file round trip; malformed input is rejected without crashing.
TEST_CASE(edit5_project_file) {
  Project p;
  p.scenePath = "scenes/phòng/scene.ply";  // UTF-8
  p.lasPath = "C:\\data\\scan \"A\".las";
  p.hasLasTransform = true;
  for (int k = 0; k < 8; ++k) p.lasTransform[k] = 0.1 * k + 1e-9;
  p.unit = "m";
  p.unitScale = 0.5;
  p.level[0] = 0.8;
  p.level[2] = 0.6;
  p.bookmarks.push_back({"door", {1, 2, 3}, {0, 0, 0}, 0.9f});
  p.annotations.push_back({"crack", "line 1\nline 2", {0.5f, 1, -2}, 0});
  MeasurementRecord m;
  m.kind = MeasurementRecord::Kind::Area;
  m.name = "floor, room 1";
  m.points = {{0, 0, 0}, {3, 0, 0}, {3, 0, 4}, {0, 0, 4}};
  p.measurements.push_back(m);
  Project q;
  std::string err;
  REQUIRE(projectFromJson(projectToJson(p), q, err));
  CHECK(q.scenePath == p.scenePath && q.lasPath == p.lasPath && q.unit == "m");
  CHECK(q.hasLasTransform && q.lasTransform[7] == p.lasTransform[7]);
  CHECK(q.bookmarks.size() == 1 && q.bookmarks[0].name == "door" && q.bookmarks[0].eye == p.bookmarks[0].eye);
  CHECK(q.annotations.size() == 1 && q.annotations[0].text == "line 1\nline 2" && q.annotations[0].bookmark == 0);
  REQUIRE(q.measurements.size() == 1);
  CHECK(q.measurements[0].kind == MeasurementRecord::Kind::Area);
  CHECK(q.measurements[0].points == m.points);
  CHECK_NEAR(measurementValue(q.measurements[0]), 12.0, 1e-12);
  CHECK(q.unitScale == 0.5 && q.level[0] == 0.8 && q.level[2] == 0.6);
  CHECK_NEAR(measurementRealValue(q.measurements[0], q.unitScale), 3.0, 1e-12);  // area scales with s^2
  CHECK(measurementsToCsv(q.measurements, "m", q.unitScale).find("floor  room 1,area,3,m^2,") != std::string::npos);

  const char* bad[] = {"",
                       "{",
                       "[]",
                       "{\"format\": \"other\"}",
                       "{\"format\": \"gsplat-viewer project\", \"version\": 99}",
                       "{\"format\": \"gsplat-viewer project\", \"bookmarks\": [{\"eye\": [1,2]}]}",
                       "{\"format\": \"gsplat-viewer project\", \"measurements\": 5}",
                       "{\"format\": \"gsplat-viewer project\", \"lasTransform\": [1,2,3]}",
                       "{\"format\": \"gsplat-viewer project\"} extra",
                       "{\"format\": \"gsplat-viewer project\", \"unit\": \"m\\q\"}",
                       "{\"format\": \"gsplat-viewer project\", \"unitScale\": -1}",
                       "{\"format\": \"gsplat-viewer project\", \"level\": [0, 0, 0, 0]}"};
  for (const char* b : bad) CHECK(!projectFromJson(b, q, err) && !err.empty());
  std::string deep = "{\"format\": \"gsplat-viewer project\", \"x\": ";
  for (int k = 0; k < 10000; ++k) deep += "[";
  CHECK(!projectFromJson(deep, q, err));
  // Unknown keys are ignored.
  CHECK(projectFromJson("{\"format\": \"gsplat-viewer project\", \"future\": {\"a\": [1, true, null]}}", q, err));
}

// ER 5.5 (live edit): many previews then commit = one command; undo restores bit-exact.
TEST_CASE(edit5_history_live_edit) {
  GaussianScene s = randomScene(300, 3, 4);
  const GaussianScene original = s;
  SplatMask m(s.size(), 0);
  for (size_t i = 0; i < s.size(); i += 2) m[i] = 1;
  EditHistory h;
  h.begin(s, m, "drag");
  for (int k = 1; k <= 20; ++k) {
    h.restore(s);
    transformSplats(s, m, glm::translate(glm::mat4(1.0f), {0.01f * k, 0, 0}));
  }
  const GaussianScene dragged = s;
  GaussianScene direct = original;
  transformSplats(direct, m, glm::translate(glm::mat4(1.0f), {0.01f * 20, 0, 0}));
  CHECK(sameScene(dragged, direct));  // total change re-applied: no drift
  CHECK(h.commit(s));
  CHECK(h.size() == 1);
  CHECK(h.undo(s) && sameScene(s, original));
  CHECK(h.redo(s) && sameScene(s, dragged));
  h.begin(s, m, "colour");
  ColorAdjust a;
  a.brightness = 0.2f;
  adjustColor(s, m, a);
  h.cancel(s);
  CHECK(sameScene(s, dragged) && h.size() == 1 && !h.liveActive());
}

// ER 5.5 (duplicate): appends copies; undo removes them, redo restores them, bit-exact.
TEST_CASE(edit5_history_duplicate) {
  GaussianScene s = randomScene(100, 2, 8);
  const GaussianScene original = s;
  SplatMask m(s.size(), 0);
  m[3] = m[50] = m[99] = 1;
  EditHistory h;
  REQUIRE(h.duplicate(s, m, "dup") == 3);
  REQUIRE(s.size() == 103);
  CHECK(s.position[100] == s.position[3] && s.position[102] == s.position[99]);
  CHECK(s.shRest.size() == 103 * 24u);
  const GaussianScene dup = s;
  CHECK(h.undo(s) && sameScene(s, original));
  CHECK(h.redo(s) && sameScene(s, dup));
}

// ER 5.3 (real-scene behaviour): splat colours are noisy in 3DGS; the region mean keeps the object
// whole. Neighbour-to-neighbour comparison selected 9 splats of a printer in the phong scene.
TEST_CASE(edit5_connected_noisy_colours) {
  GaussianScene s;
  addFloor(s, 1.5f, 0.03f, {0.75f, 0.75f, 0.72f});
  const size_t floorN = s.size();
  addBox(s, {0.0f, 0.02f, 0.0f}, {0.5f, 0.32f, 0.4f}, 0.02f, {0.25f, 0.25f, 0.27f});  // dark printer
  const size_t boxEnd = s.size();
  std::mt19937 rng(3);
  std::normal_distribution<float> noise(0.0f, 0.08f);
  for (size_t i = 0; i < s.size(); ++i) s.shDC[i] += glm::vec3(noise(rng), noise(rng), noise(rng)) / kShC0;
  // every 4th box splat nearly transparent (they join but do not spread)
  for (size_t i = floorN; i < boxEnd; i += 4) s.opacityLogit[i] = logit(0.03f);
  computeRobustBounds(s);
  uint32_t seed = floorN;
  while (s.opacityLogit[seed] < 0.0f) ++seed;
  const SplatMask c = selectConnected(s, seed, 0.05f, 0.3f);
  size_t inBox = 0, floor = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!c[i]) continue;
    if (i >= floorN && i < boxEnd) ++inBox;
    else ++floor;
  }
  std::printf("    noisy connected: %zu / %zu of the box, %zu floor\n", inBox, boxEnd - floorN, floor);
  CHECK(inBox >= static_cast<size_t>(0.99 * static_cast<double>(boxEnd - floorN)));
  CHECK(floor <= (boxEnd - floorN) / 100);
}

// ER 6.13: "visible only" keeps the splats in front, drops the ones hidden behind a nearer surface.
TEST_CASE(ux_keep_visible) {
  GaussianScene s;
  // Near plane z = -3 on the left half of the view (x < 0), far plane z = -6 everywhere.
  for (int y = -10; y <= 10; ++y)
    for (int x = -10; x <= 10; ++x) {
      if (x < 0) addSplat(s, {x * 0.1f, y * 0.1f, -3.0f}, glm::vec3(0.8f));
      addSplat(s, {x * 0.2f, y * 0.2f, -6.0f}, glm::vec3(0.3f));
    }
  const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
  // The depth image a renderer would give: 3 where the near plane covers (x < 0, |y| <= 1), else 6.
  DepthImage d;
  d.width = d.height = 200;
  d.depth.assign(200 * 200, 6.0f);
  d.coverage.assign(200 * 200, 1.0f);
  for (int py = 0; py < 200; ++py)
    for (int px = 0; px < 200; ++px) {
      const float ndcX = (px + 0.5f) / 100.0f - 1.0f, ndcY = 1.0f - (py + 0.5f) / 100.0f;
      const float wx = ndcX * 3.0f, wy = ndcY * 3.0f;  // on the near plane (tan 45 = 1)
      if (wx < -0.05f && wx >= -1.05f && std::fabs(wy) <= 1.05f) d.depth[static_cast<size_t>(py) * 200 + px] = 3.0f;
    }
  SplatMask m(s.size(), 1);
  keepVisible(s, glm::mat4(1.0f), proj, d, m, 0.03f);
  size_t nearKept = 0, farKept = 0, farHidden = 0, farTotal = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const bool isNear = s.position[i].z > -4.0f;
    if (isNear) {
      nearKept += m[i];
      continue;
    }
    ++farTotal;
    // Far splat projected onto the near plane: x/2, y/2 -> hidden when that lands inside the near patch.
    const float hx = s.position[i].x * 0.5f, hy = s.position[i].y * 0.5f;
    const bool behind = hx < -0.15f && hx > -0.95f && std::fabs(hy) < 0.95f;  // clear of the edges
    if (behind) farHidden += !m[i];
    else if (hx > 0.05f || std::fabs(hy) > 1.15f) farKept += m[i];
  }
  std::printf("    near kept %zu / 210, far behind removed %zu, far elsewhere kept %zu\n", nearKept, farHidden, farKept);
  CHECK(nearKept == 210);
  size_t behindTotal = 0, elsewhereTotal = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s.position[i].z > -4.0f) continue;
    const float hx = s.position[i].x * 0.5f, hy = s.position[i].y * 0.5f;
    if (hx < -0.15f && hx > -0.95f && std::fabs(hy) < 0.95f) ++behindTotal;
    else if (hx > 0.05f || std::fabs(hy) > 1.15f) ++elsewhereTotal;
  }
  CHECK(farHidden == behindTotal);
  CHECK(farKept == elsewhereTotal);
  CHECK(behindTotal > 20 && elsewhereTotal > 200);
}

// ER 6.15: grow / shrink one layer, flood limited to a radius.
TEST_CASE(ux_grow_shrink_radius) {
  GaussianScene s;
  for (int x = 0; x < 40; ++x) addSplat(s, {x * 0.1f, 0.0f, 0.0f}, glm::vec3(0.5f));  // a line, spacing 0.1
  computeRobustBounds(s);
  KdTree tree;
  tree.build(s.position);
  SplatMask m(s.size(), 0);
  m[20] = 1;
  growSelection(s, tree, m, 0.15f);
  CHECK(countSelected(m) == 3 && m[19] && m[21]);
  growSelection(s, tree, m, 0.15f);
  CHECK(countSelected(m) == 5);
  shrinkSelection(s, tree, m, 0.15f);
  CHECK(countSelected(m) == 3 && m[20] && !m[18]);
  const SplatMask all = selectConnected(s, 20, 0.15f, 1.0f);
  CHECK(countSelected(all) == 40);
  const SplatMask lim = selectConnectedWithin(s, 20, 0.15f, 1.0f, 0.55f);  // |x - 2.0| <= 0.55
  CHECK(countSelected(lim) == 11);
  for (size_t i = 0; i < s.size(); ++i)
    if (lim[i]) CHECK(std::fabs(s.position[i].x - 2.0f) <= 0.55f);
}

// ER 6.16: large undo steps keep only indices in memory; their data lives in a file. Bit-exact.
TEST_CASE(ux_history_spill_to_disk) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "gsv_history_spill_test";
  std::filesystem::remove_all(dir);
  GaussianScene s = randomScene(20000, 3, 17);
  const GaussianScene original = s;
  std::vector<GaussianScene> states{s};
  size_t memWithout = 0;
  {
    EditHistory h;
    SplatMask half(s.size(), 0);
    for (size_t i = 0; i < s.size(); i += 2) half[i] = 1;
    GaussianScene t = s;
    h.remove(t, half, "delete");
    memWithout = h.bytes();
  }
  EditHistory h;
  h.setSpill(dir.string(), 64 * 1024);  // anything above 64 KB goes to disk
  auto every = [&](size_t m) {
    SplatMask k(s.size(), 0);
    for (size_t i = 0; i < s.size(); i += m) k[i] = 1;
    return k;
  };
  h.remove(s, every(2), "delete");
  states.push_back(s);
  const size_t memWith = h.bytes();
  h.modify(s, every(3), "move", [](GaussianScene& g, const SplatMask& m) {
    transformSplats(g, m, glm::rotate(glm::mat4(1.0f), 0.4f, glm::vec3(0, 1, 0)));
  });
  states.push_back(s);
  h.duplicate(s, every(5), "dup");
  states.push_back(s);
  h.remove(s, SplatMask(s.size(), 0), "none");  // empty: not recorded
  size_t files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) files += e.is_regular_file();
  std::printf("    memory %zu B instead of %zu B, %zu files, %.1f MB on disk\n", memWith, memWithout, files,
              static_cast<double>(h.spilledBytes()) / 1e6);
  CHECK(files == 3);
  CHECK(memWith * 20 < memWithout);  // indices only: 4 B instead of ~240 B per splat
  for (int k = 3; k > 0; --k) {
    REQUIRE(h.undo(s));
    CHECK(sameScene(s, states[static_cast<size_t>(k - 1)]));
  }
  CHECK(sameScene(s, original));
  for (int k = 1; k <= 3; ++k) {
    REQUIRE(h.redo(s));
    CHECK(sameScene(s, states[static_cast<size_t>(k)]));
  }
  // A missing file: undo refuses and leaves the scene unchanged (no crash, no half-undo).
  for (const auto& e : std::filesystem::directory_iterator(dir)) std::filesystem::remove(e.path());
  const GaussianScene before = s;
  CHECK(!h.undo(s));
  CHECK(sameScene(s, before));
  h.clear();
  CHECK(h.spilledBytes() == 0);
  std::filesystem::remove_all(dir);
}
