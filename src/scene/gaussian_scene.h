#pragma once

#include "core/gaussian_math.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace gsv {

// A 3D Gaussian Splatting scene, stored exactly as trained (structure of arrays).
// Activation functions are applied when the scene is uploaded to the GPU:
//   scale    = exp(logScale)
//   opacity  = sigmoid(opacityLogit)
//   rotation = normalize(quaternion)          (stored as w, x, y, z = rot_0..rot_3)
//   color    = 0.5 + SH(view direction)       (band 0 = shDC, bands 1..3 = shRest)
struct GaussianScene {
  std::vector<glm::vec3> position;
  std::vector<glm::vec3> logScale;
  std::vector<glm::vec4> rotation;    // (w, x, y, z)
  std::vector<float> opacityLogit;
  std::vector<glm::vec3> shDC;        // f_dc_0..2
  std::vector<float> shRest;          // f_rest_*, per splat, channel-major: R[n], G[n], B[n]
  int shDegree = 0;                   // 0..3
  int shRestPerChannel = 0;           // 0, 3, 8 or 15  (= (degree+1)^2 - 1)

  // Robust bounds (2nd..98th percentile per axis) so a few far "floaters"
  // do not break camera framing.
  glm::vec3 boundsMin{-1.0f}, boundsMax{1.0f};
  // The dense subject of the scene (computeFocus): what the camera frames on load.
  glm::vec3 focusCenter{0.0f};
  float focusRadius = 1.0f;

  std::string sourceName;

  size_t size() const { return position.size(); }
  bool empty() const { return position.empty(); }
  const float* restOf(size_t i) const {
    return shRestPerChannel ? shRest.data() + i * static_cast<size_t>(shRestPerChannel) * 3 : nullptr;
  }
};

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }
inline float logit(float p) { return std::log(p / (1.0f - p)); }

// Base (view independent, SH band 0) color of splat i, in [0, 1].
inline glm::vec3 baseColor(const GaussianScene& s, size_t i) {
  return glm::clamp(glm::vec3(0.5f) + kShC0 * s.shDC[i], glm::vec3(0.0f), glm::vec3(1.0f));
}

// Fills boundsMin/Max (2nd..98th percentile per axis) and then the focus (computeFocus).
void computeRobustBounds(GaussianScene& scene);
// Center and radius of the densest region around the median position (see scene_bounds.cpp).
void computeFocus(GaussianScene& scene);

}  // namespace gsv
