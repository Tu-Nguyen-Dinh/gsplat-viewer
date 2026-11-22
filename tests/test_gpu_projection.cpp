// GPU tests: the preprocess compute shader must produce the same projection as the
// CPU implementation in gaussian_math.cpp.
#include "core/camera.h"
#include "core/gaussian_math.h"
#include "core/half.h"
#include "render/splat_renderer.h"
#include "scene/demo_scene.h"
#include "scene/edit_ops.h"
#include <glm/gtc/matrix_transform.hpp>
#include "test_framework.h"

#include <algorithm>
#include <bit>
#include <vector>

using namespace gsv;

namespace {

struct ProjectedSplatCpu {  // mirror of ProjectedSplat in splat_buffers.glsl (32 bytes)
  glm::vec4 centerConic;    // center xy, conic xx, xy
  glm::uvec4 bits;          // conic yy, depth (float bits), half2(r, g), half2(b, opacity)
  bool visible() const { return bits.w != 0u; }
  glm::vec3 conic() const { return {centerConic.z, centerConic.w, std::bit_cast<float>(bits.x)}; }
  glm::vec3 color() const {
    return {halfToFloat(uint16_t(bits.z & 0xFFFFu)), halfToFloat(uint16_t(bits.z >> 16)), halfToFloat(uint16_t(bits.w & 0xFFFFu))};
  }
};
static_assert(sizeof(ProjectedSplatCpu) == 32);

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
  p.principal = v.principalPixels();
  p.nearPlane = v.nearPlane;
  return p;
}

}  // namespace

// Runs the GPU preprocess pass and compares every splat with the CPU implementation.
// SH coefficients are uploaded as float32 or float16; the color tolerance follows:
// float16 has a relative step of 2^-10, and up to 15 coefficients add up.
static void checkPreprocessAgainstCpu(bool halfSh, float colorTolerance, bool colmapIntrinsics = false) {
  const GaussianScene scene = makeDemoScene(20000, 11);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  if (!log.empty()) std::printf("%s\n", log.c_str());
  renderer.setHalfPrecisionSh(halfSh);
  renderer.setCompressed(false);  // full precision, file order: GPU splat i == scene splat i
  renderer.upload(scene);

  RenderView view = testView(scene, 0.7f);
  if (colmapIntrinsics) {  // non-square pixels and an off-center principal point, like a real camera
    view.focalOverride = view.focalPixels() * glm::vec2(1.0f, 1.03f);
    view.principal = 0.5f * glm::vec2(view.viewport) + glm::vec2(7.5f, -4.25f);
  }
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
    const bool gpuVis = gpu[i].visible();
    if (c.visible) ++cpuVisible;
    if (c.visible != gpuVis) {
      ++mismatches;  // can differ only for splats exactly on the culling boundary
      continue;
    }
    if (!c.visible) continue;
    maxCenterError = std::max(maxCenterError, glm::length(c.center - glm::vec2(gpu[i].centerConic)));
    const glm::vec3 gc = gpu[i].conic();
    maxConicError = std::max(maxConicError, glm::length(c.conic - gc) / glm::length(c.conic));
    // View-dependent color (spherical harmonics, degree 3).
    const glm::vec3 dir = glm::normalize(scene.position[i] - view.cameraPosition);
    const glm::vec3 color = evalSphericalHarmonics(3, dir, scene.shDC[i], scene.restOf(i), scene.shRestPerChannel);
    maxColorError = std::max(maxColorError, glm::length(color - gpu[i].color()));
  }
  std::printf("    SH %s: visible cpu %u gpu %u, boundary mismatches %u, max center err %.2e px,"
              " max conic rel err %.2e, max SH color err %.2e\n",
              halfSh ? "fp16" : "fp32", cpuVisible, gpuVisible, mismatches, maxCenterError, maxConicError,
              maxColorError);
  CHECK(scene.shDegree == 3);
  CHECK(maxColorError < colorTolerance);
  CHECK(cpuVisible > 1000);
  CHECK(cpuVisible < scene.size());  // the close-up view must cull something
  CHECK(mismatches <= scene.size() / 1000);
  CHECK(gpuVisible + mismatches >= cpuVisible && gpuVisible <= cpuVisible + mismatches);
  CHECK(maxCenterError < 1e-2f);
  CHECK(maxConicError < 1e-3f);
}

// Colors are stored as fp16 in the projected buffer: error up to ~2^-11 relative.
GPU_TEST_CASE(gpu_preprocess_matches_cpu_projection) { checkPreprocessAgainstCpu(false, 2e-3f); }
GPU_TEST_CASE(gpu_preprocess_with_half_sh) { checkPreprocessAgainstCpu(true, 4e-3f); }
GPU_TEST_CASE(gpu_preprocess_with_pinhole_intrinsics) { checkPreprocessAgainstCpu(false, 2e-3f, true); }

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

// Core 5 (ER 5.11): edit flags. Selected splats get the highlight colour, hidden splats are culled,
// in both formats (the compressed one re-orders splats: flags must follow the Morton order).
static void checkSelectionFlags(bool compressed) {
  const GaussianScene scene = makeDemoScene(4000, 3);
  SplatRenderer renderer;
  std::string log;
  REQUIRE(renderer.init(log));
  renderer.setCompressed(compressed);
  renderer.upload(scene);
  const RenderView view = testView(scene, 0.4f);
  SplatSettings settings;
  settings.cullOutsideViewport = false;
  auto run = [&](std::vector<ProjectedSplatCpu>& out, uint32_t& visible) {
    renderer.preprocess(view, settings);
    out.resize(scene.size());
    renderer.projectedBuffer().download(0, out.size() * sizeof(ProjectedSplatCpu), out.data());
    renderer.drawCommandBuffer().download(4, sizeof(uint32_t), &visible);
  };
  std::vector<ProjectedSplatCpu> base, flagged;
  uint32_t visibleBase = 0, visibleFlagged = 0;
  run(base, visibleBase);
  std::vector<uint8_t> flags(scene.size(), 0);
  size_t hiddenVisible = 0;
  const std::vector<uint32_t>& g2s = renderer.gpuToScene();
  REQUIRE(g2s.size() == scene.size());
  for (size_t i = 0; i < scene.size(); ++i) flags[i] = i % 3 == 0 ? 1 : (i % 7 == 0 ? 2 : 0);
  for (size_t k = 0; k < scene.size(); ++k)
    if (flags[g2s[k]] == 2 && base[k].visible()) ++hiddenVisible;
  renderer.setSelectionFlags(flags);
  CHECK(renderer.hasSelectionFlags());
  run(flagged, visibleFlagged);
  CHECK(visibleFlagged + hiddenVisible == visibleBase);
  float maxErr = 0.0f;
  size_t wrong = 0;
  for (size_t k = 0; k < scene.size(); ++k) {
    const uint8_t f = flags[g2s[k]];
    if (f == 2) {
      wrong += flagged[k].visible();
      continue;
    }
    if (!base[k].visible()) continue;
    const glm::vec3 expect = f == 1 ? glm::mix(base[k].color(), glm::vec3(settings.highlight), settings.highlight.a)
                                    : base[k].color();
    maxErr = std::max(maxErr, glm::length(flagged[k].color() - expect));
  }
  CHECK(wrong == 0);
  CHECK(maxErr < 4e-3f);  // fp16 colour storage
  // Clearing restores the original output.
  renderer.clearSelectionFlags();
  run(flagged, visibleFlagged);
  CHECK(visibleFlagged == visibleBase);
  // Wrong size: ignored.
  renderer.setSelectionFlags(std::vector<uint8_t>(3, 1));
  CHECK(!renderer.hasSelectionFlags());
}
GPU_TEST_CASE(gpu_selection_flags_fp32) { checkSelectionFlags(false); }
GPU_TEST_CASE(gpu_selection_flags_compressed) { checkSelectionFlags(true); }

// Phase 6 (ER 6.14): the live gizmo preview on the GPU draws exactly what transforming the data on
// the CPU would draw: same centres, ellipses and view-dependent colours, for the selected splats only.
static void checkTransformPreview(bool compressed) {
  const GaussianScene scene = makeDemoScene(3000, 3);
  const RenderView view = testView(scene, 0.7f);
  SplatSettings settings;
  settings.cullOutsideViewport = false;
  std::vector<uint8_t> flags(scene.size(), 0);
  SplatMask mask(scene.size(), 0);
  for (size_t i = 0; i < scene.size(); i += 2) flags[i] = mask[i] = 1;
  const glm::vec3 c = 0.5f * (scene.boundsMin + scene.boundsMax);
  const glm::mat4 T = glm::translate(glm::mat4(1.0f), c + glm::vec3(0.3f, 0.1f, -0.2f)) *
                      glm::rotate(glm::mat4(1.0f), 0.6f, glm::normalize(glm::vec3(0.2f, 1.0f, 0.1f))) *
                      glm::scale(glm::mat4(1.0f), glm::vec3(1.3f)) * glm::translate(glm::mat4(1.0f), -c);
  auto project = [&](const GaussianScene& s, bool preview) -> std::vector<ProjectedSplatCpu> {
    SplatRenderer r;
    std::string log;
    if (!r.init(log)) return {};
    r.setCompressed(compressed);
    r.upload(s);
    r.setSelectionFlags(flags);
    SplatSettings st = settings;
    st.highlight.a = 0.0f;  // compare true colours
    st.previewActive = preview;
    st.previewAffine = T;
    r.preprocess(view, st);
    std::vector<ProjectedSplatCpu> gpu(s.size()), byScene(s.size());
    r.projectedBuffer().download(0, gpu.size() * sizeof(ProjectedSplatCpu), gpu.data());
    for (size_t k = 0; k < gpu.size(); ++k) byScene[r.gpuToScene()[k]] = gpu[k];
    return byScene;
  };
  GaussianScene moved = scene;
  transformSplats(moved, mask, T);
  const auto a = project(scene, true), b = project(moved, false);
  REQUIRE(a.size() == scene.size() && b.size() == scene.size());
  float centerErr = 0.0f, conicErr = 0.0f, colorErr = 0.0f;
  size_t compared = 0, visibilityMismatch = 0;
  for (size_t i = 0; i < scene.size(); ++i) {
    if (a[i].visible() != b[i].visible()) {
      ++visibilityMismatch;
      continue;
    }
    if (!a[i].visible()) continue;
    ++compared;
    centerErr = std::max(centerErr, glm::length(glm::vec2(a[i].centerConic) - glm::vec2(b[i].centerConic)));
    const glm::vec3 ca = a[i].conic(), cb = b[i].conic();
    conicErr = std::max(conicErr, glm::length(ca - cb) / std::max(glm::length(cb), 1e-6f));
    colorErr = std::max(colorErr, glm::length(a[i].color() - b[i].color()));
  }
  std::printf("    preview vs CPU (%s): %zu splats, centre %.3g px, conic %.3g rel, colour %.3g, visibility diff %zu\n",
              compressed ? "compressed" : "fp32", compared, static_cast<double>(centerErr), static_cast<double>(conicErr),
              static_cast<double>(colorErr), visibilityMismatch);
  CHECK(compared > scene.size() / 2);
  if (!compressed) {
    CHECK(visibilityMismatch == 0);
    CHECK(centerErr < 1e-2f);
    CHECK(conicErr < 1e-3f);
    CHECK(colorErr < 4e-3f);  // fp16 colour storage
  } else {  // the CPU path re-quantises the moved splats (24-byte codec): compare within codec error
    CHECK(visibilityMismatch <= scene.size() / 500);
    CHECK(centerErr < 0.5f);
    CHECK(colorErr < 0.03f);
  }
}
GPU_TEST_CASE(gpu_transform_preview_fp32) { checkTransformPreview(false); }
GPU_TEST_CASE(gpu_transform_preview_compressed) { checkTransformPreview(true); }
