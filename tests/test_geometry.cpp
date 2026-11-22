// Core 3: kd-tree, normals, similarity estimation, ICP, plane fitting, measurement.
#include "core/kdtree.h"
#include "core/picking.h"
#include "core/registration.h"
#include "test_framework.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>

using namespace gsv;

namespace {
std::vector<glm::vec3> randomPoints(size_t n, uint32_t seed, float extent = 10.0f) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-extent, extent);
  std::vector<glm::vec3> p(n);
  for (auto& v : p) v = {u(rng), u(rng), u(rng)};
  return p;
}

// A room-like shape: floor, two walls, a box. Point-to-plane ICP needs non-parallel planes.
std::vector<glm::vec3> roomPoints(uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  std::vector<glm::vec3> p;
  for (int i = 0; i < 6000; ++i) p.push_back({4.0f * u(rng), 0.0f, 3.0f * u(rng)});          // floor
  for (int i = 0; i < 4000; ++i) p.push_back({4.0f * u(rng), 2.5f * u(rng), 0.0f});          // wall
  for (int i = 0; i < 3000; ++i) p.push_back({0.0f, 2.5f * u(rng), 3.0f * u(rng)});          // wall
  for (int i = 0; i < 2000; ++i) {                                                           // box
    const float a = u(rng), b = u(rng);
    const int face = static_cast<int>(u(rng) * 3.0f);
    const glm::vec3 base(2.0f, 0.0f, 1.0f);
    p.push_back(base + (face == 0 ? glm::vec3(a, 0.6f, b) : face == 1 ? glm::vec3(0.0f, 0.6f * a, b) : glm::vec3(a, 0.6f * b, 0.0f)));
  }
  return p;
}
}  // namespace

TEST_CASE(kdtree_matches_brute_force) {
  const auto pts = randomPoints(20000, 11);
  KdTree tree;
  tree.build(pts);
  const auto queries = randomPoints(200, 12, 12.0f);
  std::vector<uint32_t> idx, rad;
  std::vector<float> d2;
  bool ok = true;
  for (const glm::vec3& q : queries) {
    tree.knn(q, 8, idx, d2);
    std::vector<std::pair<float, uint32_t>> all;
    for (uint32_t i = 0; i < pts.size(); ++i) all.emplace_back(glm::dot(pts[i] - q, pts[i] - q), i);
    std::partial_sort(all.begin(), all.begin() + 8, all.end());
    for (int k = 0; k < 8; ++k) ok = ok && idx[k] == all[k].second;
    tree.radius(q, 1.5f, rad);
    size_t expected = 0;
    for (const auto& e : all) expected += e.first <= 2.25f;
    ok = ok && rad.size() == expected;
  }
  CHECK(ok);
  const auto big = randomPoints(1000000, 13, 100.0f);
  const auto t0 = std::chrono::steady_clock::now();
  KdTree bigTree;
  bigTree.build(big);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("    kd-tree build: 1M points in %.0f ms\n", ms);
}

TEST_CASE(normals_of_noisy_plane) {
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::normal_distribution<float> noise(0.0f, 0.002f);
  const glm::vec3 n0 = glm::normalize(glm::vec3(0.3f, 1.0f, -0.2f));
  const glm::vec3 t1 = glm::normalize(glm::cross(n0, glm::vec3(1, 0, 0))), t2 = glm::cross(n0, t1);
  std::vector<glm::vec3> pts;
  for (int i = 0; i < 5000; ++i) pts.push_back(t1 * u(rng) + t2 * u(rng) + n0 * noise(rng));
  KdTree tree;
  tree.build(pts);
  const auto normals = estimateNormals(pts, tree, 30, n0 * 10.0f);
  std::vector<double> err;
  bool oriented = true;
  for (const glm::vec3& n : normals) {
    err.push_back(glm::degrees(std::acos(std::min(1.0, static_cast<double>(glm::dot(n, n0))))));
    oriented = oriented && glm::dot(n, n0) > 0.0f;
  }
  std::sort(err.begin(), err.end());
  const double median = err[err.size() / 2], p99 = err[err.size() * 99 / 100];
  // Noise 2 mm, 30 neighbours (~8 cm across at ~2.8 cm spacing); with 16 neighbours the median is 1.15 deg.
  std::printf("    normal error: median %.2f deg, p99 %.2f deg, worst %.2f deg\n", median, p99, err.back());
  CHECK(median < 1.0);
  CHECK(p99 < 4.0);
  CHECK(oriented);
}

TEST_CASE(similarity_closed_form_and_ransac) {
  const GeoFrame truth = GeoFrame::fromQuaternion(glm::angleAxis(1.1, glm::normalize(glm::dvec3(1, 2, -0.5))),
                                                  {12.5, -3.0, 40.0}, 2.75);
  std::mt19937 rng(9);
  std::uniform_real_distribution<double> u(-5.0, 5.0);
  std::vector<glm::dvec3> src, dst;
  for (int i = 0; i < 100; ++i) {
    src.push_back({u(rng), u(rng), u(rng)});
    dst.push_back(truth.apply(src.back()));
  }
  GeoFrame est;
  REQUIRE(estimateSimilarity(src, dst, true, est));
  CHECK_NEAR(est.scale, truth.scale, 1e-9);
  CHECK(rotationAngleBetween(est.rotation, truth.rotation) < 1e-7);
  CHECK(glm::length(est.translation - truth.translation) < 1e-8);
  // rigid fit of scaled data recovers the rotation but not the scale
  GeoFrame rigid;
  REQUIRE(estimateSimilarity(src, dst, false, rigid));
  CHECK_NEAR(rigid.scale, 1.0, 0.0);
  CHECK(rotationAngleBetween(rigid.rotation, truth.rotation) < 1e-7);
  // degenerate: collinear
  std::vector<glm::dvec3> line = {{0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3}};
  CHECK(!estimateSimilarity(line, line, true, est));
  // 30 % outliers + noise
  std::normal_distribution<double> noise(0.0, 0.01);
  for (size_t i = 0; i < dst.size(); ++i) {
    dst[i] += glm::dvec3(noise(rng), noise(rng), noise(rng));
    if (i % 10 < 3) dst[i] += glm::dvec3(u(rng), u(rng), u(rng)) * 4.0;
  }
  const RansacResult r = estimateSimilarityRansac(src, dst, true, 0.05, 500, 4);
  REQUIRE(r.ok);
  std::printf("    RANSAC: %zu inliers of 100, scale error %.2e, rotation error %.4f deg, rms %.4f\n", r.inliers.size(),
              std::fabs(r.transform.scale - truth.scale) / truth.scale,
              glm::degrees(rotationAngleBetween(r.transform.rotation, truth.rotation)), r.rms);
  CHECK(r.inliers.size() >= 68);
  CHECK(std::fabs(r.transform.scale - truth.scale) / truth.scale < 0.01);
  CHECK(glm::degrees(rotationAngleBetween(r.transform.rotation, truth.rotation)) < 0.5);
}

TEST_CASE(icp_point_to_plane_recovers_offset) {
  const auto target = roomPoints(21);
  KdTree tree;
  tree.build(target);
  const auto normals = estimateNormals(target, tree, 12, glm::vec3(2.0f, 1.2f, 1.5f));
  // Source = a second noisy scan of the same room, moved by 5 deg / 10 cm.
  auto scan = roomPoints(22);
  std::mt19937 rng(23);
  std::normal_distribution<float> noise(0.0f, 0.001f);
  for (auto& p : scan) p += glm::vec3(noise(rng), noise(rng), noise(rng));
  const GeoFrame offset = GeoFrame::fromQuaternion(glm::angleAxis(glm::radians(5.0), glm::normalize(glm::dvec3(0.2, 1, 0.1))),
                                                   {0.07, -0.05, 0.06});
  std::vector<glm::vec3> source;
  for (const auto& p : scan) source.push_back(glm::vec3(offset.inverse().apply(glm::dvec3(p))));
  IcpOptions o;
  o.maxCorrespondenceDistance = 0.3;
  const IcpResult r = icpPointToPlane(source, target, normals, tree, GeoFrame::identity(), o);
  const double angErr = glm::degrees(rotationAngleBetween(r.transform.rotation, offset.rotation));
  const double transErr = glm::length(r.transform.translation - offset.translation);
  std::printf("    ICP: %d iterations, %zu pairs, rms %.4f m, rotation error %.4f deg, translation error %.2f mm%s\n",
              r.iterations, r.pairs, r.rms, angErr, transErr * 1000.0, r.converged ? "" : " (not converged)");
  CHECK(angErr < 0.05);
  CHECK(transErr < 1e-3);
}

TEST_CASE(plane_ransac_with_outliers) {
  std::mt19937 rng(31);
  std::uniform_real_distribution<float> u(-3.0f, 3.0f);
  std::normal_distribution<float> noise(0.0f, 0.003f);
  const glm::dvec3 n0 = glm::normalize(glm::dvec3(0.1, 1.0, 0.05));
  const glm::vec3 t1 = glm::normalize(glm::cross(glm::vec3(n0), glm::vec3(1, 0, 0))), t2 = glm::cross(glm::vec3(n0), t1);
  std::vector<glm::vec3> pts;
  for (int i = 0; i < 3000; ++i) pts.push_back(t1 * u(rng) + t2 * u(rng) + glm::vec3(n0) * (1.5f + noise(rng)));
  for (int i = 0; i < 3000; ++i) pts.push_back({u(rng), u(rng), u(rng)});  // 50 % outliers
  const PlaneFit f = fitPlaneRansac(pts, 0.01, 300, 2);
  REQUIRE(f.ok);
  const double err = glm::degrees(std::acos(std::min(1.0, std::fabs(glm::dot(f.plane.normal, n0)))));
  std::printf("    plane: %zu inliers, normal error %.3f deg, offset %.4f\n", f.inliers.size(), err, std::fabs(f.plane.d));
  CHECK(err < 0.5);
  CHECK(std::fabs(std::fabs(f.plane.d) - 1.5) < 0.01);
  CHECK(f.inliers.size() >= 2900);
}

TEST_CASE(measure_distance_and_height) {
  const Measurement m = measure({0, 0, 0}, {3, 4, 12}, {0, 0, 1});
  CHECK_NEAR(m.distance, 13.0, 1e-12);
  CHECK_NEAR(m.horizontal, 5.0, 1e-12);
  CHECK_NEAR(m.vertical, 12.0, 1e-12);
}
