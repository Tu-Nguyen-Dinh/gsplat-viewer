#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

// A 3D Gaussian Splatting scene, stored exactly as trained (structure of arrays).
// Activation functions are applied at render time, like the reference implementation:
//   scale    = exp(logScale)
//   opacity  = sigmoid(opacityLogit)
//   rotation = normalize(quaternion)          (stored as w, x, y, z = rot_0..rot_3)
//   color    = 0.5 + SH_C0 * shDC + higher SH bands (view dependent)
struct GaussianScene {
  std::vector<glm::vec3> position;
  std::vector<glm::vec3> logScale;
  std::vector<glm::vec4> rotation;    // (w, x, y, z)
  std::vector<float> opacityLogit;
  std::vector<glm::vec3> shDC;        // f_dc_0..2
  std::vector<float> shRest;          // f_rest_*, per splat, channel-major: R[n], G[n], B[n]
  int shDegree = 0;                   // 0..3
  int shRestPerChannel = 0;           // 0, 3, 8 or 15

  // Robust bounds (2nd..98th percentile per axis) so a few far "floaters"
  // do not break camera framing.
  glm::vec3 boundsMin{-1.0f}, boundsMax{1.0f};

  std::string sourceName;

  size_t size() const { return position.size(); }
  bool empty() const { return position.empty(); }
};

inline constexpr float kShC0 = 0.28209479177387814f;  // 1 / (2 * sqrt(pi))

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }
inline float logit(float p) { return std::log(p / (1.0f - p)); }

// Base (view independent) color of splat i, in [0, 1].
inline glm::vec3 baseColor(const GaussianScene& s, size_t i) {
  return glm::clamp(glm::vec3(0.5f) + kShC0 * s.shDC[i], glm::vec3(0.0f), glm::vec3(1.0f));
}

void computeRobustBounds(GaussianScene& scene);
