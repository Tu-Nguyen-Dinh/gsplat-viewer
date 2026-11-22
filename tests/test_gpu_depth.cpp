// Core 1: expected-depth output and occlusion of splats by opaque layers.
#include "core/camera.h"
#include "gl/framebuffer.h"
#include "core/picking.h"
#include "core/ray_pick.h"
#include "render/cloud_renderer.h"
#include "render/reference_renderer.h"
#include "render/splat_renderer.h"
#include "scene/demo_scene.h"
#include "scene/lod.h"
#include "test_framework.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace gsv;

namespace {
constexpr int kW = 256, kH = 192;

RenderView viewOf(const GaussianScene& scene, float yaw) {
  OrbitCamera cam;
  cam.frameBounds(scene.boundsMin, scene.boundsMax);
  cam.yaw = yaw;
  cam.distance *= 0.8f;
  RenderView v;
  v.view = cam.view();
  v.viewport = {kW, kH};
  v.projection = cam.projection(v.aspect());
  v.cameraPosition = cam.position();
  v.fovY = cam.fovY;
  v.nearPlane = cam.nearPlane;
  return v;
}
}  // namespace

GPU_TEST_CASE(gpu_expected_depth_matches_reference) {
  const GaussianScene scene = makeDemoScene(20000, 5);
  SplatRenderer r;
  std::string log;
  REQUIRE(r.init(log));
  r.upload(scene);
  const RenderView view = viewOf(scene, 1.1f);
  SplatSettings s;
  s.sort = SortMode::CpuRadix;
  Framebuffer fb;
  REQUIRE(fb.create(kW, kH, Framebuffer::Format::RGBA16F, {true, false}));
  fb.clear(glm::vec3(0.0f));
  r.render(view, s);
  const DepthImage gpu = readDepthOutput(fb);
  ReferenceOptions ro;
  ReferenceDepth ref;
  renderReference(scene, view, ro, &ref);
  std::vector<double> rel;
  double covErr = 0.0;
  for (size_t i = 0; i < ref.depth.size(); ++i) {
    covErr = std::max(covErr, static_cast<double>(std::fabs(gpu.coverage[i] - ref.coverage[i])));
    if (ref.coverage[i] > 0.5f && gpu.coverage[i] > 0.5f)
      rel.push_back(std::fabs(gpu.depth[i] - ref.depth[i]) / ref.depth[i]);
  }
  REQUIRE(rel.size() > 1000);
  std::sort(rel.begin(), rel.end());
  const double median = rel[rel.size() / 2], p99 = rel[rel.size() * 99 / 100];
  std::printf("    %zu covered pixels: depth rel. error median %.2e, p99 %.2e; max coverage error %.3f\n", rel.size(),
              median, p99, covErr);
  CHECK(median < 5e-3);
  CHECK(p99 < 5e-2);
}

GPU_TEST_CASE(gpu_opaque_points_occlude_splats) {
  // A dense wall of red points between the camera and the scene hides the splats behind it when
  // splats test depth, and is hidden by them when the wall is placed behind the scene.
  const GaussianScene scene = makeDemoScene(8000, 3);
  SplatRenderer r;
  CloudRenderer cr;
  std::string log;
  REQUIRE(r.init(log));
  REQUIRE(cr.init(log));
  r.upload(scene);
  const RenderView view = viewOf(scene, 0.0f);
  const glm::vec3 center = 0.5f * (scene.boundsMin + scene.boundsMax);
  const glm::vec3 toCam = glm::normalize(view.cameraPosition - center);
  const float radius = 0.5f * glm::length(scene.boundsMax - scene.boundsMin);

  auto wallAt = [&](float offset) {
    PointCloud wall;
    const glm::vec3 right = glm::normalize(glm::cross(toCam, glm::vec3(0, 1, 0)));
    const glm::vec3 up = glm::cross(right, toCam);
    for (int y = -60; y <= 60; ++y)
      for (int x = -60; x <= 60; ++x)
        wall.position.push_back(center + toCam * offset + (right * float(x) + up * float(y)) * (radius / 30.0f));
    wall.color.assign(wall.position.size(), glm::u8vec3(255, 0, 0));
    wall.computeBounds();
    return wall;
  };
  auto redFraction = [&](float offset, bool depthTest) {
    cr.upload(wallAt(offset));
    Framebuffer fb;
    fb.create(kW, kH, Framebuffer::Format::RGBA8, {true, true});
    fb.clear(glm::vec3(0.0f));
    CloudSettings cs;
    cs.pointSize = 16.0f;  // wall points ~11 px apart at this distance: no gaps
    cs.roundPoints = false;
    cr.draw(view, GeoFrame::identity(), glm::dvec3(0.0), cs);
    SplatSettings ss;
    ss.depthTest = depthTest;
    r.render(view, ss);
    const Image img = readFramebuffer(fb.id(), kW, kH);
    // centre region, where the scene is
    int red = 0, total = 0;
    for (int y = kH / 3; y < 2 * kH / 3; ++y)
      for (int x = kW / 3; x < 2 * kW / 3; ++x) {
        const uint8_t* p = img.pixel(x, y);
        ++total;
        if (p[0] > 200 && p[1] < 40 && p[2] < 40) ++red;
      }
    return static_cast<double>(red) / total;
  };
  const double front = redFraction(radius * 1.2f, true);
  const double behindTested = redFraction(-radius * 1.5f, true);
  const double frontUntested = redFraction(radius * 1.2f, false);
  std::printf("    pure-red share of the centre: wall in front %.2f, wall behind %.2f, in front without depth test %.2f\n",
              front, behindTested, frontUntested);
  CHECK(front > 0.95);          // splats behind the wall are rejected by the depth test
  CHECK(behindTested < 0.5);    // splats in front cover the wall
  CHECK(frontUntested < front - 0.2);  // without the test the splats are blended over the wall
}

GPU_TEST_CASE(gpu_pick_on_splat_plane) {
  // A floor made of flat, opaque splats (y = 0). Picking through the depth output must land on it.
  GaussianScene floor;
  for (int z = -40; z <= 40; ++z)
    for (int x = -40; x <= 40; ++x) {
      floor.position.push_back({x * 0.05f, 0.0f, z * 0.05f});
      floor.logScale.push_back(glm::log(glm::vec3(0.04f, 0.002f, 0.04f)));
      floor.rotation.push_back({1, 0, 0, 0});
      floor.opacityLogit.push_back(logit(0.95f));
      floor.shDC.push_back({0.5f, 0.2f, 0.1f});
    }
  floor.boundsMin = {-2, -0.1f, -2};
  floor.boundsMax = {2, 0.1f, 2};
  SplatRenderer r;
  std::string log;
  REQUIRE(r.init(log));
  r.upload(floor);
  OrbitCamera cam;
  cam.target = {0, 0, 0};
  cam.distance = 2.5f;
  cam.pitch = 0.6f;
  cam.yaw = 0.4f;
  RenderView v;
  v.view = cam.view();
  v.viewport = {kW, kH};
  v.projection = cam.projection(v.aspect());
  v.cameraPosition = cam.position();
  v.fovY = cam.fovY;
  v.nearPlane = cam.nearPlane;
  Framebuffer fb;
  REQUIRE(fb.create(kW, kH, Framebuffer::Format::RGBA16F, {true, false}));
  fb.clear(glm::vec3(0.0f));
  SplatSettings s;
  s.depthOutput = true;
  r.render(v, s);
  const DepthImage depth = readDepthOutput(fb);
  PickCamera pc{v.view, v.focalPixels(), v.principalPixels()};
  double worst = 0.0;
  int picked = 0;
  for (int y = kH / 4; y < 3 * kH / 4; y += 16)
    for (int x = kW / 4; x < 3 * kW / 4; x += 16) {
      const auto p = pickPoint(depth, x, y, pc);
      if (!p) continue;
      ++picked;
      // exact intersection of the pixel ray with y = 0
      const glm::vec2 px(x + 0.5f, (kH - 1 - y) + 0.5f);
      const glm::vec3 dirCam((px.x - pc.principal.x) / pc.focal.x, (px.y - pc.principal.y) / pc.focal.y, -1.0f);
      const glm::mat4 inv = glm::inverse(v.view);
      const glm::vec3 o = glm::vec3(inv[3]), d = glm::vec3(inv * glm::vec4(dirCam, 0.0f));
      const glm::vec3 hit = o + d * (-o.y / d.y);
      worst = std::max(worst, static_cast<double>(glm::length(*p - hit) / glm::length(hit - o)));
    }
  std::printf("    %d picks on the splat floor: worst error %.3f %% of the distance\n", picked, worst * 100.0);
  CHECK(picked >= 40);
  CHECK(worst < 0.005);
}

GPU_TEST_CASE(gpu_lod_cut_image_close_to_full) {
  // A third of the splats, chosen by the LOD cut for this camera, must look like the full scene.
  const GaussianScene scene = makeDemoScene(120000, 9);
  LodBuildOptions o;
  o.leafCapacity = 1500;
  o.nodeCapacity = 1500;
  const LodTree tree = buildLod(scene, o);
  OrbitCamera cam;
  cam.frameBounds(scene.boundsMin, scene.boundsMax);
  cam.distance *= 2.5f;  // a distant view: where LOD matters
  RenderView v;
  v.view = cam.view();
  v.viewport = {kW, kH};
  v.projection = cam.projection(v.aspect());
  v.cameraPosition = cam.position();
  v.fovY = cam.fovY;
  v.nearPlane = cam.nearPlane;
  LodCutParams p;
  p.cameraPosition = v.cameraPosition;
  p.focalPixels = v.focalPixels().y;
  p.pixelError = 1.0f;
  p.splatBudget = scene.size() / 3;
  const LodCut cut = selectCut(tree, p);
  const GaussianScene lod = gatherCut(tree, cut);
  auto render = [&](const GaussianScene& s) {
    SplatRenderer r;
    std::string log;
    r.init(log);
    r.upload(s);
    Framebuffer fb;
    fb.create(kW, kH, Framebuffer::Format::RGBA16F);
    fb.clear(glm::vec3(0.07f, 0.08f, 0.1f));
    r.render(v, SplatSettings());
    return readFramebuffer(fb.id(), kW, kH);
  };
  const Image full = render(scene), reduced = render(lod);
  // Control: the same number of splats picked uniformly at random (every 3rd) instead of by the cut.
  GaussianScene every3rd;
  every3rd.shDegree = scene.shDegree;
  every3rd.shRestPerChannel = scene.shRestPerChannel;
  every3rd.boundsMin = scene.boundsMin;
  every3rd.boundsMax = scene.boundsMax;
  const size_t rest = static_cast<size_t>(scene.shRestPerChannel) * 3;
  for (size_t i = 0; i < scene.size(); i += 3) {
    every3rd.position.push_back(scene.position[i]);
    every3rd.logScale.push_back(scene.logScale[i]);
    every3rd.rotation.push_back(scene.rotation[i]);
    every3rd.opacityLogit.push_back(scene.opacityLogit[i]);
    every3rd.shDC.push_back(scene.shDC[i]);
    every3rd.shRest.insert(every3rd.shRest.end(), scene.shRest.begin() + static_cast<long>(i * rest),
                           scene.shRest.begin() + static_cast<long>((i + 1) * rest));
  }
  const double pLod = psnr(full, reduced), pDrop = psnr(full, render(every3rd));
  std::printf("    %zu -> %zu splats (%zu nodes): LOD %.2f dB vs full, dropping 2 of 3 splats %.2f dB\n", scene.size(),
              lod.size(), cut.nodes.size(), pLod, pDrop);
  CHECK(lod.size() <= p.splatBudget);
  CHECK(pLod >= 25.0);
  CHECK(pLod > pDrop + 3.0);  // merging beats discarding
}

namespace {
// A table top (y = 0.75, 1.6 x 0.8) above a floor (y = 0), plus faint floaters between the camera and
// the table: the situation in which measuring the table's length failed in the viewer (22/11/2026).
GaussianScene tableScene() {
  GaussianScene s;
  auto add = [&](glm::vec3 p, glm::vec3 scale, float opacity, glm::vec3 dc) {
    s.position.push_back(p);
    s.logScale.push_back(glm::log(scale));
    s.rotation.push_back({1, 0, 0, 0});
    s.opacityLogit.push_back(logit(opacity));
    s.shDC.push_back(dc);
  };
  for (float z = -0.4f; z <= 0.4f + 1e-4f; z += 0.04f)
    for (float x = -0.8f; x <= 0.8f + 1e-4f; x += 0.04f) add({x, 0.75f, z}, {0.025f, 0.002f, 0.025f}, 0.95f, {0.4f, 0.1f, -0.2f});
  for (float z = -2.0f; z <= 2.0f + 1e-4f; z += 0.08f)
    for (float x = -2.0f; x <= 2.0f + 1e-4f; x += 0.08f) add({x, 0.0f, z}, {0.05f, 0.003f, 0.05f}, 0.95f, {-0.3f, -0.3f, -0.3f});
  uint32_t r = 7;
  for (int i = 0; i < 60; ++i) {  // faint floaters between the camera and the table (a light haze, not fog)
    auto u = [&] { r = r * 1664525u + 1013904223u; return (r >> 8) / 16777216.0f; };
    add({u() * 2.4f - 1.2f, 0.8f + u() * 0.6f, u() * 1.6f - 0.8f + 1.0f}, glm::vec3(0.05f + 0.05f * u()), 0.15f, {0.0f, 0.0f, 0.0f});
  }
  s.boundsMin = {-2, -0.1f, -2};
  s.boundsMax = {2, 1.5f, 2};
  return s;
}
}  // namespace

GPU_TEST_CASE(gpu_pick_table_edge_median_vs_expected) {
  const GaussianScene scene = tableScene();
  SplatRenderer r;
  std::string log;
  REQUIRE(r.init(log));
  r.upload(scene);
  OrbitCamera cam;
  cam.target = {0, 0.6f, 0};
  cam.distance = 2.6f;
  cam.pitch = 0.45f;
  cam.yaw = 0.25f;
  RenderView v;
  v.view = cam.view();
  v.viewport = {kW, kH};
  v.projection = cam.projection(v.aspect());
  v.cameraPosition = cam.position();
  v.fovY = cam.fovY;
  v.nearPlane = cam.nearPlane;
  Framebuffer fb;
  REQUIRE(fb.create(kW, kH, Framebuffer::Format::RGBA16F, {true, false}));
  fb.clear(glm::vec3(0.0f));
  SplatSettings s;
  s.depthOutput = true;
  r.render(v, s);
  const DepthImage depth = readDepthOutput(fb);
  const PickCamera pc{v.view, v.focalPixels(), v.principalPixels()};
  SplatRayCache cache;
  cache.build(scene);
  const glm::mat4 inv = glm::inverse(v.view);
  const glm::vec3 eye = glm::vec3(inv[3]);
  // Every pixel whose exact ray hits the table top within 4 cm of its edge (inside the edge): what
  // a user clicks when measuring the table's length.
  double worstExpected = 0.0, worstMedian = 0.0;
  std::vector<double> errE, errM;
  int n = 0;
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x) {
      const glm::vec2 px(x + 0.5f, (kH - 1 - y) + 0.5f);
      const glm::vec3 dir = glm::normalize(glm::vec3(inv * glm::vec4((px.x - pc.principal.x) / pc.focal.x,
                                                                      (px.y - pc.principal.y) / pc.focal.y, -1.0f, 0.0f)));
      const float t = (0.75f - eye.y) / dir.y;
      if (!(t > 0.0f)) continue;
      const glm::vec3 hit = eye + dir * t;
      const float inside = std::min(0.8f - std::fabs(hit.x), 0.4f - std::fabs(hit.z));
      if (inside < 0.0f || inside > 0.04f) continue;
      ++n;
      const auto p = pickPoint(depth, x, y, pc, 0.5f, 0);
      errE.push_back(p ? glm::length(*p - hit) : 1.0);
      const auto h = raycastSplats(cache, glm::mat4(1.0f), eye, dir);
      errM.push_back(h ? glm::length(h->point - hit) : 1.0);
    }
  REQUIRE(n > 50);
  std::sort(errE.begin(), errE.end());
  std::sort(errM.begin(), errM.end());
  auto pct = [](const std::vector<double>& e, int q) { return e[e.size() * q / 100] * 100.0; };
  worstExpected = errE.back();
  worstMedian = errM.back();
  std::printf("    %d table-edge pixels, distance to the true surface point (cm): expected depth median %.1f / p95 %.1f / "
              "worst %.1f; 3D ray median-depth median %.2f / p95 %.2f / worst %.2f\n", n, pct(errE, 50), pct(errE, 95),
              worstExpected * 100.0, pct(errM, 50), pct(errM, 95), worstMedian * 100.0);
  CHECK(worstMedian < 0.01);          // within 1 cm on a 1.6 m table seen from 2.6 m
  CHECK(worstExpected > worstMedian);  // documents the bug of the expected-depth pick
}

TEST_CASE(ray_median_ignores_floaters_and_background) {
  // Along one ray: a faint floater (alpha 0.3) at 1 m, a surface (0.8) at 2 m, a wall (0.95) at 6 m.
  // Expected depth = weighted mean = somewhere between; median = the surface.
  GaussianScene s;
  auto add = [&](float z, float opacity) {
    s.position.push_back({0.0f, 0.0f, -z});
    s.logScale.push_back(glm::log(glm::vec3(0.2f, 0.2f, 0.001f)));
    s.rotation.push_back({1, 0, 0, 0});
    s.opacityLogit.push_back(logit(opacity));
    s.shDC.push_back(glm::vec3(0.0f));
  };
  add(1.0f, 0.3f);
  add(2.0f, 0.8f);
  add(6.0f, 0.95f);
  SplatRayCache cache;
  cache.build(s);
  const auto h = raycastSplats(cache, glm::mat4(1.0f), glm::vec3(0.0f), glm::vec3(0, 0, -1));
  REQUIRE(h.has_value());
  // expected depth with the same alphas (front to back weights w = a * T)
  const double w1 = 0.3, w2 = 0.7 * 0.8, w3 = 0.7 * 0.2 * 0.95;
  const double expected = (w1 * 1.0 + w2 * 2.0 + w3 * 6.0) / (w1 + w2 + w3);
  std::printf("    median pick %.3f m, expected depth %.3f m (surface at 2 m)\n", h->distance, expected);
  CHECK_NEAR(h->distance, 2.0, 1e-3);
  CHECK(std::fabs(expected - 2.0) > 0.2);  // 23 cm: the floater-pulled mean
  // A scaled model matrix: distances stay in world units.
  const auto h2 = raycastSplats(cache, glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)), glm::vec3(0.0f), glm::vec3(0, 0, -1));
  REQUIRE(h2.has_value());
  CHECK_NEAR(h2->distance, 4.0, 2e-3);
}
