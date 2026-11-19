// GPU tests: the preprocess compute shader must produce the same projection as the
// CPU implementation in gaussian_math.cpp.
#include "core/camera.h"
#include "core/gaussian_math.h"
#include "render/splat_renderer.h"
#include "scene/demo_scene.h"
#include "test_framework.h"

#include <algorithm>
#include <vector>

using namespace gsv;

namespace {

struct ProjectedSplatCpu {  // mirror of ProjectedSplat in splat_buffers.glsl
  glm::vec4 centerDepthRadius, conicOpacity, axes, color;
};

RenderView testView(const GaussianScene& scene, float yaw, int w = 640, int h = 480) {
  OrbitCamera cam;
  cam.frameBounds(scene.boundsMin, scene.boundsMax);
  cam.yaw = yaw;
  cam.distance *= 0.6f;  // close enough that part of the scene is outside the viewport
  RenderView v;
  v.view = cam.view();
  v.viewport = {w, h};
  v.projection = cam.projection(v.aspect());
  v.cameraPosition = cam.position();
  v.fovY = cam.fovY;
  v.nearPlane = cam.nearPlane;
  return v;
}

ProjectionParams cpuParams(const RenderView& v) {
  ProjectionParams p;
  p.view = v.view * v.model;
  p.viewport = glm::vec2(v.viewport);
  p.focal = v.focalPixels();
  p.tanHalfFov = v.tanHalfFov();
  p.nearPlane = v.nearPlane;
  return p;
}

}  // namespace

GPU_TEST_CASE(gpu_preprocess_matches_cpu_projection) {
  const GaussianScene scene = makeDemoScene(20000, 11);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  if (!log.empty()) std::printf("%s\n", log.c_str());
  renderer.upload(scene);

  const RenderView view = testView(scene, 0.7f);
  renderer.preprocess(view, SplatSettings{});
  std::vector<ProjectedSplatCpu> gpu(scene.size());
  renderer.projectedBuffer().download(0, gpu.size() * sizeof(ProjectedSplatCpu), gpu.data());
  uint32_t gpuVisible = 0;
  renderer.drawCommandBuffer().download(4, sizeof(uint32_t), &gpuVisible);

  const ProjectionParams params = cpuParams(view);
  uint32_t cpuVisible = 0, mismatches = 0;
  float maxCenterError = 0.0f, maxConicError = 0.0f, maxColorError = 0.0f;
  for (size_t i = 0; i < scene.size(); ++i) {
    const Cov3 cov = covarianceFromScaleRotation(glm::exp(scene.logScale[i]), scene.rotation[i]);
    const ProjectedGaussian c = projectGaussian(scene.position[i], cov, params);
    const bool gpuVis = gpu[i].centerDepthRadius.w > 0.0f;
    if (c.visible) ++cpuVisible;
    if (c.visible != gpuVis) {
      ++mismatches;  // can differ only for splats exactly on the culling boundary
      continue;
    }
    if (!c.visible) continue;
    maxCenterError = std::max(maxCenterError, glm::length(c.center - glm::vec2(gpu[i].centerDepthRadius)));
    const glm::vec3 gc(gpu[i].conicOpacity);
    maxConicError = std::max(maxConicError, glm::length(c.conic - gc) / glm::length(c.conic));
    // View-dependent color (spherical harmonics, degree 3).
    const glm::vec3 dir = glm::normalize(scene.position[i] - view.cameraPosition);
    const glm::vec3 color = evalSphericalHarmonics(3, dir, scene.shDC[i], scene.restOf(i), scene.shRestPerChannel);
    maxColorError = std::max(maxColorError, glm::length(color - glm::vec3(gpu[i].color)));
  }
  std::printf("    visible cpu %u gpu %u, boundary mismatches %u, max center err %.2e px, max conic rel err %.2e,"
              " max SH color err %.2e\n",
              cpuVisible, gpuVisible, mismatches, maxCenterError, maxConicError, maxColorError);
  CHECK(scene.shDegree == 3);
  CHECK(maxColorError < 1e-4f);
  CHECK(cpuVisible > 1000);
  CHECK(cpuVisible < scene.size());  // the close-up view must cull something
  CHECK(mismatches <= scene.size() / 1000);
  CHECK(gpuVisible + mismatches >= cpuVisible && gpuVisible <= cpuVisible + mismatches);
  CHECK(maxCenterError < 1e-2f);
  CHECK(maxConicError < 1e-3f);
}

GPU_TEST_CASE(gpu_viewport_culling_reduces_work) {
  // Close-up view: many splats are in front of the camera but outside the image.
  const GaussianScene scene = makeDemoScene(50000, 12);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  renderer.upload(scene);
  RenderView view = testView(scene, 0.9f);
  OrbitCamera cam;
  cam.frameBounds(scene.boundsMin, scene.boundsMax);
  cam.yaw = 0.9f;
  cam.distance *= 0.25f;
  view.view = cam.view();
  view.cameraPosition = cam.position();
  view.nearPlane = cam.nearPlane;

  auto visibleCount = [&](bool cull) {
    SplatSettings s;
    s.cullOutsideViewport = cull;
    renderer.preprocess(view, s);
    uint32_t v = 0;
    renderer.drawCommandBuffer().download(4, sizeof(v), &v);
    return v;
  };
  const uint32_t withCull = visibleCount(true), withoutCull = visibleCount(false);
  // Without viewport culling, only the near-plane test removes splats.
  uint32_t inFront = 0;
  for (const glm::vec3& p : scene.position)
    if (-(view.view * glm::vec4(p, 1.0f)).z >= view.nearPlane) ++inFront;
  std::printf("    close-up: %u in front of the camera, %u inside the viewport (%.0f%%)\n", withoutCull, withCull,
              100.0 * withCull / withoutCull);
  CHECK(withoutCull == inFront);
  CHECK(withCull < withoutCull);
}

GPU_TEST_CASE(gpu_visible_list_is_a_permutation) {
  // Every visible splat index must appear exactly once in the draw list.
  const GaussianScene scene = makeDemoScene(5000, 5);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  renderer.upload(scene);
  renderer.preprocess(testView(scene, 2.0f), SplatSettings{});
  uint32_t visible = 0;
  renderer.drawCommandBuffer().download(4, sizeof(visible), &visible);
  std::vector<uint32_t> order(visible);
  renderer.orderBuffer().download(0, visible * sizeof(uint32_t), order.data());
  std::sort(order.begin(), order.end());
  CHECK(std::adjacent_find(order.begin(), order.end()) == order.end());  // no duplicates
  CHECK(visible == 0 || order.back() < scene.size());
}
