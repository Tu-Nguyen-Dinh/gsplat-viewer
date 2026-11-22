// Core 1: double-precision frames and relative-to-origin rendering.
#include "core/geo_frame.h"
#include "scene/layers.h"
#include "test_framework.h"

#include <cmath>
#include <memory>

using namespace gsv;

namespace {
GeoFrame someFrame() {
  return GeoFrame::fromQuaternion(glm::angleAxis(0.7, glm::normalize(glm::dvec3(0.3, -1.0, 0.4))),
                                  glm::dvec3(612345.678, 1234567.891, 12.5), 1.37);
}
}  // namespace

TEST_CASE(geo_frame_compose_and_inverse) {
  const GeoFrame a = someFrame();
  const GeoFrame b = GeoFrame::fromQuaternion(glm::angleAxis(-1.9, glm::dvec3(0, 0, 1)), {3, -4, 5}, 0.5);
  const glm::dvec3 p(1.25, -7.5, 3.0);
  const glm::dvec3 viaCompose = (a * b).apply(p), direct = a.apply(b.apply(p));
  CHECK(glm::length(viaCompose - direct) < 1e-9 * glm::length(direct));
  const glm::dvec3 back = a.inverse().apply(a.apply(p));
  CHECK(glm::length(back - p) < 1e-9);
  const GeoFrame id = a * a.inverse();
  CHECK_NEAR(id.scale, 1.0, 1e-12);
  CHECK(glm::length(id.translation) < 1e-7);  // translation ~1e6: relative 1e-13
  CHECK(rotationAngleBetween(id.rotation, glm::dmat3(1.0)) < 1e-7);
  // 4x4 matrix agrees with apply()
  const glm::dvec4 h = a.matrix() * glm::dvec4(p, 1.0);
  CHECK(glm::length(glm::dvec3(h) - a.apply(p)) < 1e-6);
}

TEST_CASE(geo_frame_local_float_keeps_millimetres) {
  // A survey point at UTM/VN-2000-like magnitudes.
  const glm::dvec3 world(600000.1234, 1200000.5678, 10.0009);
  const glm::dvec3 origin(600000.0, 1200000.0, 0.0);
  // Naive: store the absolute coordinate in a float.
  const glm::dvec3 naive = glm::dvec3(glm::vec3(world));
  // Relative to a nearby origin: only the offset is a float.
  const glm::dvec3 rel = fromLocal(toLocal(world, origin), origin);
  const double naiveErr = glm::length(naive - world), relErr = glm::length(rel - world);
  std::printf("    float absolute error %.4f m, relative-to-origin error %.2e m\n", naiveErr, relErr);
  CHECK(naiveErr >= 0.03);   // float spacing at 1.2e6 is 0.125 m
  CHECK(relErr <= 1e-3);

  // The float model matrix built in double keeps the same precision for a transformed layer.
  const GeoFrame f = someFrame();
  const glm::dvec3 local(10.25, -3.5, 2.0);
  const glm::dvec3 exact = f.apply(local);
  const glm::dvec3 renderOrigin = glm::round(exact);
  const glm::vec4 viaFloat = f.localMatrix(renderOrigin) * glm::vec4(glm::vec3(local), 1.0f);
  CHECK(glm::length(glm::dvec3(viaFloat) + renderOrigin - exact) < 1e-4);
}

TEST_CASE(point_cloud_rebase_is_exact) {
  PointCloud c;
  c.origin = {500000.0, 1100000.0, 0.0};
  c.position = {{1.5f, 2.25f, 3.0f}, {-4.0f, 0.5f, 10.0f}};
  const glm::dvec3 w0 = c.world(0), w1 = c.world(1);
  c.rebase({500010.0, 1099990.0, 5.0});
  CHECK(glm::length(c.world(0) - w0) < 1e-5);
  CHECK(glm::length(c.world(1) - w1) < 1e-5);
  CHECK_NEAR(c.localMin.x, -14.0, 1e-5);
}

TEST_CASE(layer_stack_bounds_and_origin) {
  LayerStack stack;
  auto splats = std::make_shared<GaussianScene>();
  splats->position = {glm::vec3(0.0f)};
  splats->boundsMin = glm::vec3(-1.0f);
  splats->boundsMax = glm::vec3(1.0f);
  auto cloud = std::make_shared<PointCloud>();
  cloud->origin = {1000.0, 2000.0, 30.0};
  cloud->position = {{0, 0, 0}, {10, 20, 5}};
  cloud->computeBounds();
  const int a = stack.addSplats(splats, "splats", GeoFrame::fromTranslation({1000.0, 2000.0, 30.0}));
  const int b = stack.addPoints(cloud, "lidar");
  CHECK(a != b);
  CHECK(stack.size() == 2);
  const WorldBounds wb = stack.worldBounds();
  REQUIRE(wb.valid);
  CHECK(glm::length(wb.min - glm::dvec3(999, 1999, 29)) < 1e-9);
  CHECK(glm::length(wb.max - glm::dvec3(1010, 2020, 35)) < 1e-9);
  CHECK(glm::length(stack.suggestRenderOrigin() - glm::dvec3(1005, 2010, 32)) < 1e-9);
  stack.find(b)->visible = false;
  CHECK(glm::length(stack.worldBounds().max - glm::dvec3(1001, 2001, 31)) < 1e-9);
  CHECK(stack.remove(a));
  CHECK(!stack.remove(a));
  CHECK(stack.find(a) == nullptr);
  CHECK(stack.find(b) != nullptr);
}
