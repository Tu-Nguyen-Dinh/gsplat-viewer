// Scene extents: the full robust bounds (used by the renderer for depth-key ranges) and the
// "focus" region, the dense part of the scene a camera should frame when the file is opened.
#include "scene/gaussian_scene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace gsv {

namespace {

// Per-axis median: robust center that ignores far floaters.
glm::vec3 medianPosition(const std::vector<glm::vec3>& p) {
  const size_t n = p.size();
  std::vector<float> axis(n);
  glm::vec3 m(0.0f);
  for (int a = 0; a < 3; ++a) {
    for (size_t i = 0; i < n; ++i) axis[i] = p[i][a];
    std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(n / 2), axis.end());
    m[a] = axis[n / 2];
  }
  return m;
}

}  // namespace

void computeRobustBounds(GaussianScene& scene) {
  const size_t n = scene.size();
  if (n == 0) return;
  std::vector<float> axis(n);
  for (int a = 0; a < 3; ++a) {
    for (size_t i = 0; i < n; ++i) axis[i] = scene.position[i][a];
    const size_t lo = n / 50, hi = n - 1 - n / 50;
    std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(lo), axis.end());
    scene.boundsMin[a] = axis[lo];
    std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(hi), axis.end());
    scene.boundsMax[a] = axis[hi];
  }
  computeFocus(scene);
}

// Captured scenes are a dense subject (the truck, the desk) surrounded by sparse background and
// floaters that can reach 10x further out. Per-axis percentiles still include the background, so
// the focus is found from the radial density profile around the median instead:
//   1. splats are weighted by opacity (near-transparent floaters barely count);
//   2. the density (weight / shell volume) is measured in geometric shells r_k = r_0 * 1.25^k;
//   3. the core ends at the first shell after the densest one whose density is < 10 % of the peak;
//   4. the radius is the 98th weighted percentile of distance inside the core, and the center is
//      refined to the weighted mean of the core.
void computeFocus(GaussianScene& scene) {
  if (scene.empty()) return;
  // Framing does not need every splat: a fixed-stride sample of at most 200k gives the same
  // percentiles to well under 1 % and keeps this at a few milliseconds for 2.5 M splats.
  const size_t stride = std::max<size_t>(1, scene.size() / 200000);
  std::vector<glm::vec3> position;
  std::vector<float> weight;
  for (size_t i = 0; i < scene.size(); i += stride) {
    position.push_back(scene.position[i]);
    weight.push_back(scene.opacityLogit.empty() ? 1.0f : sigmoid(scene.opacityLogit[i]));
  }
  const size_t n = position.size();
  glm::vec3 center = medianPosition(position);

  // O(n) with histograms (no sort).
  std::vector<float> distance(n);

  for (int pass = 0; pass < 2; ++pass) {  // second pass re-centers on the core found by the first
    float rMax = 0.0f;
    for (size_t i = 0; i < n; ++i) rMax = std::max(rMax, distance[i] = glm::length(position[i] - center));

    // Shells start at the 1 % distance: the innermost ball holds too few splats to be meaningful.
    std::vector<float> tmp(distance);
    std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n / 100), tmp.end());
    const float r0 = std::max(tmp[n / 100], 1e-6f);
    constexpr float kGrowth = 1.25f;
    const float invLogGrowth = 1.0f / std::log(kGrowth);
    const size_t shells = 2 + static_cast<size_t>(std::max(0.0f, std::log(std::max(rMax, r0) / r0) * invLogGrowth));
    auto shellOf = [&](float d) {
      return d <= r0 ? size_t(0) : std::min(shells - 1, size_t(1) + static_cast<size_t>(std::log(d / r0) * invLogGrowth));
    };
    auto outerEdge = [&](size_t k) { return r0 * std::pow(kGrowth, static_cast<float>(k)); };  // shell k: (edge k-1, edge k]

    std::vector<double> shellWeight(shells, 0.0);
    for (size_t i = 0; i < n; ++i) shellWeight[shellOf(distance[i])] += weight[i];
    std::vector<double> density(shells);
    for (size_t k = 0; k < shells; ++k) {
      const double r2 = outerEdge(k), r1 = k == 0 ? 0.0 : outerEdge(k - 1);
      density[k] = shellWeight[k] / (r2 * r2 * r2 - r1 * r1 * r1);  // the 4/3 pi cancels in ratios
    }
    const size_t peak = static_cast<size_t>(std::max_element(density.begin(), density.end()) - density.begin());
    float coreRadius = rMax;  // no clear drop: the whole scene is the subject
    for (size_t k = peak + 1; k < shells; ++k)
      if (density[k] < 0.1 * density[peak]) {
        coreRadius = outerEdge(k - 1);
        break;
      }

    // 98th weighted percentile of the distance inside the core, from a fine histogram.
    constexpr size_t kBins = 1024;
    std::vector<double> bins(kBins, 0.0);
    double total = 0;
    for (size_t i = 0; i < n; ++i)
      if (distance[i] <= coreRadius) {
        bins[std::min(kBins - 1, static_cast<size_t>(distance[i] / coreRadius * kBins))] += weight[i];
        total += weight[i];
      }
    double acc = 0;
    size_t b = 0;
    while (b + 1 < kBins && (acc += bins[b]) < 0.98 * total) ++b;
    scene.focusRadius = std::max(coreRadius * static_cast<float>(b + 1) / kBins, 1e-4f);
    if (pass == 1) break;
    glm::dvec3 sum(0.0);
    double coreWeight = 0;
    for (size_t j = 0; j < n; ++j) {
      if (distance[j] > scene.focusRadius) continue;
      sum += glm::dvec3(position[j]) * static_cast<double>(weight[j]);
      coreWeight += weight[j];
    }
    if (coreWeight > 0) center = glm::vec3(sum / coreWeight);
  }
  scene.focusCenter = center;
}

}  // namespace gsv
