// Core 2: LOD tree construction, moment-matching merge, cut selection, file round trip.
#include "scene/demo_scene.h"
#include "scene/lod.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <random>

using namespace gsv;

TEST_CASE(lod_merge_preserves_moments) {
  const GaussianScene s = makeDemoScene(64, 3);
  std::vector<uint32_t> idx;
  for (uint32_t i = 0; i < 16; ++i) idx.push_back(i);
  GaussianScene out;
  out.shDegree = s.shDegree;
  out.shRestPerChannel = s.shRestPerChannel;
  mergeSplats(s, idx, out);
  REQUIRE(out.size() == 1);
  // Reference moments with the same weights.
  double W = 0.0;
  glm::dvec3 mean(0.0);
  std::vector<double> w;
  for (uint32_t i : idx) {
    const glm::dmat3 c = splatCovariance(s, i);
    w.push_back(sigmoid(s.opacityLogit[i]) * std::cbrt(glm::determinant(c)));
    W += w.back();
    mean += w.back() * glm::dvec3(s.position[i]);
  }
  mean /= W;
  glm::dmat3 C(0.0);
  for (size_t k = 0; k < idx.size(); ++k) {
    const glm::dvec3 d = glm::dvec3(s.position[idx[k]]) - mean;
    C += w[k] * (splatCovariance(s, idx[k]) + glm::outerProduct(d, d));
  }
  C /= W;
  const glm::dmat3 merged = splatCovariance(out, 0);
  double err = 0.0, norm = 0.0;
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) {
      err = std::max(err, std::fabs(merged[c][r] - C[c][r]));
      norm = std::max(norm, std::fabs(C[c][r]));
    }
  std::printf("    merged mean error %.2e, covariance relative error %.2e\n", glm::length(glm::dvec3(out.position[0]) - mean), err / norm);
  CHECK(glm::length(glm::dvec3(out.position[0]) - mean) < 1e-5 * (1.0 + glm::length(mean)));
  CHECK(err / norm < 1e-5);
  CHECK(out.shRest.size() == static_cast<size_t>(out.shRestPerChannel) * 3);
}

TEST_CASE(lod_tree_structure) {
  const GaussianScene s = makeDemoScene(100000, 4);
  LodBuildOptions o;
  o.leafCapacity = 2000;
  o.nodeCapacity = 2000;
  const auto t0 = std::chrono::steady_clock::now();
  const LodTree t = buildLod(s, o);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REQUIRE(!t.empty());
  // every original splat in exactly one leaf; capacities respected; children consecutive & valid
  std::vector<int> seen(t.leafSplatCount, 0);
  size_t leaves = 0;
  bool ok = true;
  for (const LodNode& n : t.nodes) {
    if (n.childCount == 0) {
      ++leaves;
      ok = ok && n.splatCount <= o.leafCapacity && n.splatBegin + n.splatCount <= t.leafSplatCount;
      for (uint32_t k = 0; k < n.splatCount; ++k) seen[n.splatBegin + k]++;
    } else {
      ok = ok && n.splatCount <= o.nodeCapacity && n.firstChild + n.childCount <= t.nodes.size() && n.childCount <= 8;
      for (uint32_t c = 0; c < n.childCount; ++c) {
        const LodNode& ch = t.nodes[n.firstChild + c];
        ok = ok && ch.level == n.level + 1;
        ok = ok && glm::length(ch.center - n.center) + ch.radius <= n.radius * 1.0001f + 1e-5f;  // nested spheres
      }
    }
  }
  bool once = true;
  for (int v : seen) once = once && v == 1;
  std::printf("    100k splats: %zu nodes, %zu leaves, depth %d, %zu splats incl. proxies, built in %.0f ms\n", t.nodes.size(),
              leaves, t.depth, t.splats.size(), ms);
  CHECK(ok);
  CHECK(once);
  CHECK(t.depth >= 3);
  CHECK(t.nodes[0].splatCount <= o.nodeCapacity);
}

TEST_CASE(lod_cut_budget_and_distance) {
  const GaussianScene s = makeDemoScene(60000, 6);
  LodBuildOptions o;
  o.leafCapacity = 1000;
  o.nodeCapacity = 1000;
  const LodTree t = buildLod(s, o);
  const glm::vec3 c = 0.5f * (s.boundsMin + s.boundsMax);
  const float r = 0.5f * glm::length(s.boundsMax - s.boundsMin);
  LodCutParams p;
  p.focalPixels = 1000.0f;
  p.pixelError = 8.0f;
  p.splatBudget = 40000;
  uint64_t previous = 0;
  bool monotonic = true, withinBudget = true, farNotLimited = true;
  for (float dist : {200.0f, 40.0f, 12.0f, 4.0f, 1.5f}) {
    p.cameraPosition = c + glm::vec3(0, 0, dist * r);
    const LodCut cut = selectCut(t, p);
    withinBudget = withinBudget && cut.splatCount <= p.splatBudget;
    const GaussianScene g = gatherCut(t, cut);
    withinBudget = withinBudget && g.size() == cut.splatCount;
    std::printf("    camera at %4.1f radii: %6llu splats in %zu nodes%s\n", dist, static_cast<unsigned long long>(cut.splatCount),
                cut.nodes.size(), cut.budgetLimited ? " (budget)" : "");
    // A budget-limited cut is greedy, so it may stop a few hundred splats short: compare only
    // error-limited cuts strictly.
    if (!cut.budgetLimited) monotonic = monotonic && cut.splatCount >= previous;
    if (dist >= 40.0f) farNotLimited = farNotLimited && !cut.budgetLimited;
    previous = cut.splatCount;
  }
  CHECK(withinBudget);
  CHECK(monotonic);
  CHECK(farNotLimited);  // far away the pixel error, not the budget, decides
  // Unlimited budget and zero error: exactly the original splats.
  p.splatBudget = ~0ull;
  p.pixelError = 0.0f;
  const LodCut all = selectCut(t, p);
  CHECK(all.splatCount == s.size());
  const GaussianScene g = gatherCut(t, all);
  CHECK(g.size() == s.size());
}

TEST_CASE(lod_file_round_trip_and_corruption) {
  const GaussianScene s = makeDemoScene(5000, 8);
  LodBuildOptions o;
  o.leafCapacity = 500;
  o.nodeCapacity = 500;
  const LodTree t = buildLod(s, o);
  const std::string path = (std::filesystem::temp_directory_path() / "gsv_test.gsvlod").string();
  std::string err;
  REQUIRE(saveLod(path, t, err));
  LodTree back;
  REQUIRE(loadLod(path, back, err));
  CHECK(back.nodes.size() == t.nodes.size());
  CHECK(back.splats.size() == t.splats.size());
  CHECK(std::memcmp(back.splats.position.data(), t.splats.position.data(), t.splats.size() * sizeof(glm::vec3)) == 0);
  CHECK(std::memcmp(back.nodes.data(), t.nodes.data(), t.nodes.size() * sizeof(LodNode)) == 0);
  std::vector<char> bytes;
  {
    std::ifstream f(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(f), {});
  }
  const std::string bad = path + ".bad";
  auto tryBytes = [&](const std::vector<char>& b) {
    {
      std::ofstream f(bad, std::ios::binary);
      f.write(b.data(), static_cast<std::streamsize>(b.size()));
    }
    LodTree x;
    std::string e;
    return loadLod(bad, x, e);
  };
  CHECK(!tryBytes(std::vector<char>(bytes.begin(), bytes.begin() + static_cast<long>(bytes.size() / 3))));
  std::vector<char> b = bytes;
  b[0] = 'X';
  CHECK(!tryBytes(b));
  std::mt19937 rng(5);
  for (int k = 0; k < 40; ++k) {  // random corruption: must fail or load, never crash
    b = bytes;
    for (int j = 0; j < 8; ++j) b[8 + rng() % (b.size() - 8)] = static_cast<char>(rng());
    tryBytes(b);
  }
  std::filesystem::remove(path);
  std::filesystem::remove(bad);
}
