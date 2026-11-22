#include "core/ray_pick.h"

#include "core/gaussian_math.h"
#include "core/parallel.h"

#include <algorithm>
#include <cmath>

namespace gsv {

void SplatRayCache::build(const GaussianScene& s) {
  scene = &s;
  inverseCovariance.resize(s.size());
  opacity.resize(s.size());
  parallelFor(s.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const glm::dmat3 R(rotationFromQuaternion(s.rotation[i]));
      const glm::dvec3 sc = glm::exp(glm::dvec3(s.logScale[i]));
      const glm::dvec3 inv2 = 1.0 / glm::max(sc * sc, glm::dvec3(1e-24));
      // Sigma = R S^2 R^T  =>  Sigma^-1 = R S^-2 R^T (no matrix inversion needed)
      inverseCovariance[i] = R * glm::dmat3(inv2.x, 0, 0, 0, inv2.y, 0, 0, 0, inv2.z) * glm::transpose(R);
      opacity[i] = sigmoid(s.opacityLogit[i]);
    }
  });
}

std::vector<std::pair<float, float>> rayProfile(const SplatRayCache& c, const glm::mat4& model, const glm::vec3& originW,
                                                const glm::vec3& directionW, const RayPickOptions& o) {
  std::vector<std::pair<float, float>> hits;
  if (!c.scene) return hits;
  const GaussianScene& s = *c.scene;
  // Ray in scene coordinates. The model matrix may scale uniformly: distances are converted back.
  const glm::dmat4 inv = glm::inverse(glm::dmat4(model));
  const glm::dvec3 o3 = glm::dvec3(inv * glm::dvec4(glm::dvec3(originW), 1.0));
  const glm::dvec3 dRaw = glm::dvec3(inv * glm::dvec4(glm::dvec3(directionW), 0.0));
  const double sceneUnitsPerWorld = glm::length(dRaw);
  const glm::dvec3 d = dRaw / sceneUnitsPerWorld;
  const double maxM2 = static_cast<double>(o.maxSigma) * o.maxSigma;
  const double minAlpha = 1.0 / 255.0;
  // Each splat writes its own slot (no shared state between threads), then a serial compaction.
  std::vector<float> tOf(s.size(), -1.0f), alphaOf(s.size(), 0.0f);
  parallelFor(s.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      double opacity;
      glm::dmat3 A;
      if (c.inverseCovariance.empty()) {  // no cache: Sigma^-1 = R S^-2 R^T on the fly (no extra memory)
        opacity = sigmoid(s.opacityLogit[i]);
        if (opacity < minAlpha) continue;
        const glm::dmat3 R(rotationFromQuaternion(s.rotation[i]));
        const glm::dvec3 sc = glm::exp(glm::dvec3(s.logScale[i]));
        const glm::dvec3 inv2 = 1.0 / glm::max(sc * sc, glm::dvec3(1e-24));
        A = R * glm::dmat3(inv2.x, 0, 0, 0, inv2.y, 0, 0, 0, inv2.z) * glm::transpose(R);
      } else {
        opacity = c.opacity[i];
        if (opacity < minAlpha) continue;
        A = c.inverseCovariance[i];
      }
      const glm::dvec3 u = glm::dvec3(s.position[i]) - o3;
      const glm::dvec3 Ad = A * d;
      const double a = glm::dot(d, Ad);
      if (!(a > 0.0)) continue;
      const double b = glm::dot(u, Ad);
      const double t = b / a;                             // density maximum along the ray
      const double m2 = glm::dot(u, A * u) - b * b / a;   // squared Mahalanobis distance there
      if (m2 > maxM2) continue;
      const double tWorld = t / sceneUnitsPerWorld;
      if (tWorld < o.nearDistance) continue;
      const double alpha = std::min(0.99, opacity * std::exp(-0.5 * std::max(m2, 0.0)));
      if (alpha < minAlpha) continue;
      tOf[i] = static_cast<float>(tWorld);
      alphaOf[i] = static_cast<float>(alpha);
    }
  });
  for (size_t i = 0; i < s.size(); ++i)
    if (tOf[i] >= 0.0f) hits.emplace_back(tOf[i], alphaOf[i]);
  std::sort(hits.begin(), hits.end());
  return hits;
}

std::optional<RayHit> raycastSplats(const SplatRayCache& c, const glm::mat4& model, const glm::vec3& origin,
                                    const glm::vec3& direction, const RayPickOptions& o) {
  const auto profile = rayProfile(c, model, origin, direction, o);
  double T = 1.0;
  for (const auto& [t, alpha] : profile) {
    T *= 1.0 - alpha;
    if (1.0 - T >= o.coverageThreshold) {
      RayHit h;
      h.distance = t;
      h.point = origin + direction * t;
      h.coverage = static_cast<float>(1.0 - T);
      return h;
    }
  }
  return std::nullopt;
}

std::optional<RayHit> raycastSplats(const GaussianScene& scene, const glm::mat4& model, const glm::vec3& origin,
                                    const glm::vec3& direction, const RayPickOptions& o) {
  SplatRayCache c;  // empty cache: matrices computed on the fly
  c.scene = &scene;
  return raycastSplats(c, model, origin, direction, o);
}

}  // namespace gsv
