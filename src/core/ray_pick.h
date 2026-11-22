#pragma once
// Core 3: picking by casting a ray through the 3D Gaussians (CPU, exact, view independent).
// See docs/core/CORE3-geometry.md.
//
// Why not the expected depth (Core 1 depth output)? It is the weighted MEAN depth of everything the
// pixel sees: at an object's edge, or behind semi-transparent floaters, the mean lies between two
// surfaces, so the picked point floats in the air. It still projects onto the clicked pixel, so the
// error only shows when the view changes. Here every Gaussian is intersected in 3D along the ray at
// its density maximum, contributions are composited front to back, and the result is the MEDIAN depth:
// the first point where the accumulated opacity reaches 50 %.

#include "scene/gaussian_scene.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace gsv {

struct RayHit {
  glm::vec3 point{0.0f};   // world (display) coordinates
  float distance = 0.0f;   // along the ray (world units)
  float coverage = 0.0f;   // opacity accumulated up to the hit (>= threshold)
  uint32_t splat = 0;      // the splat that crossed the threshold
};

struct RayPickOptions {
  float coverageThreshold = 0.5f;  // 0.5 = median depth
  float maxSigma = 3.0f;           // same footprint as the rasterizer (3 sigma)
  float nearDistance = 1e-4f;
};

// `model` maps scene coordinates to world (the viewer's model matrix: rotation, optional uniform scale).
// `origin` and unit `direction` are in world coordinates. Empty when the ray never reaches the threshold.
std::optional<RayHit> raycastSplats(const GaussianScene& scene, const glm::mat4& model, const glm::vec3& origin,
                                    const glm::vec3& direction, const RayPickOptions& options = RayPickOptions());

// Precomputed inverse covariances (72 B/splat: 180 MB for 2.5 M splats), for many rays on the same
// scene (tests, batch tools). The scene overload computes them on the fly instead (no extra memory).
struct SplatRayCache {
  std::vector<glm::dmat3> inverseCovariance;  // Sigma^-1 = R S^-2 R^T, in scene coordinates
  std::vector<float> opacity;
  const GaussianScene* scene = nullptr;
  void build(const GaussianScene& scene);
};
std::optional<RayHit> raycastSplats(const SplatRayCache& cache, const glm::mat4& model, const glm::vec3& origin,
                                    const glm::vec3& direction, const RayPickOptions& options = RayPickOptions());

// Opacity accumulated front to back along a ray, as (distance, alpha) pairs sorted by distance:
// the raw profile the pick uses (exposed for tests and debugging).
std::vector<std::pair<float, float>> rayProfile(const SplatRayCache& cache, const glm::mat4& model,
                                                const glm::vec3& origin, const glm::vec3& direction,
                                                const RayPickOptions& options = RayPickOptions());

}  // namespace gsv
