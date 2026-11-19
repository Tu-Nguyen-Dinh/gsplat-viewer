#pragma once
// CPU implementation of the per-Gaussian math used by the renderer.
//
// The GLSL shaders (shaders/include/gaussian.glsl) implement exactly the same
// formulas. Keeping a C++ copy lets us unit-test the math, build a CPU
// reference renderer and compare the GPU output against it.

#include <glm/glm.hpp>

#include <array>

namespace gsv {

// Symmetric 3x3 covariance, stored as its 6 unique entries.
struct Cov3 {
  float xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
  glm::mat3 toMat3() const;
};

// Rotation matrix from a quaternion stored as (w, x, y, z). Normalizes first.
glm::mat3 rotationFromQuaternion(const glm::vec4& wxyz);

// Sigma = R * S * S^T * R^T with S = diag(scale). `scale` is already activated (exp).
Cov3 covarianceFromScaleRotation(const glm::vec3& scale, const glm::vec4& wxyz);

// Everything the projection needs to know about the camera and the viewport.
struct ProjectionParams {
  glm::mat4 view{1.0f};          // world (model already applied) -> camera space, camera looks down -Z
  glm::vec2 viewport{1.0f};      // pixels
  glm::vec2 focal{1.0f};         // focal length in pixels (x, y)
  glm::vec2 tanHalfFov{1.0f};    // tan(fov/2) horizontally and vertically
  float nearPlane = 0.01f;
  float splatScale = 1.0f;       // user multiplier on Gaussian size (debug)
  bool cullOutsideViewport = true;
};

// Low-pass filter added to the 2D covariance (in pixels^2), as in the reference
// implementation: every splat covers at least ~one pixel, which avoids aliasing.
inline constexpr float kScreenDilation = 0.3f;
// Splats are drawn out to 3 standard deviations (99.7% of the mass).
inline constexpr float kSigmaExtent = 3.0f;

struct ProjectedGaussian {
  bool visible = false;
  glm::vec2 center{0.0f};   // pixels, origin bottom-left (same as gl_FragCoord)
  float depth = 0.0f;       // distance along the view direction (> 0)
  glm::vec3 conic{0.0f};    // inverse 2D covariance (a, b, c) for [[a b][b c]]
  glm::vec2 axisMajor{0.0f}, axisMinor{0.0f};  // half-axes of the 3-sigma ellipse, pixels
  float radius = 0.0f;      // 3 * sqrt(largest eigenvalue), pixels
};

// EWA splatting: project a 3D Gaussian (mean, covariance) to a 2D screen-space Gaussian.
ProjectedGaussian projectGaussian(const glm::vec3& mean, const Cov3& cov, const ProjectionParams& p);

// Gaussian weight of a pixel at offset `d` (pixels) from the center: exp(-0.5 d^T conic d).
float gaussianFalloff(const glm::vec3& conic, const glm::vec2& d);

// --- Spherical harmonics -------------------------------------------------------
inline constexpr float kShC0 = 0.28209479177387814f;
inline constexpr float kShC1 = 0.4886025119029199f;
inline constexpr std::array<float, 5> kShC2 = {1.0925484305920792f, -1.0925484305920792f, 0.31539156525252005f,
                                               -1.0925484305920792f, 0.5462742152960396f};
inline constexpr std::array<float, 7> kShC3 = {-0.5900435899266435f, 2.890611442640554f, -0.4570457994644658f,
                                               0.3731763325901154f,  -0.4570457994644658f, 1.445305721320277f,
                                               -0.5900435899266435f};

// Number of coefficients per color channel for a given degree: (degree + 1)^2.
inline constexpr int shCoeffCount(int degree) { return (degree + 1) * (degree + 1); }

// RGB color seen from direction `dir` (unit vector from camera to the Gaussian).
// `dc`: band-0 coefficients. `rest`: higher bands in PLY order, channel-major
// (R[restPerChannel], G[...], B[...]). Returns max(0, 0.5 + SH(dir)).
glm::vec3 evalSphericalHarmonics(int degree, const glm::vec3& dir, const glm::vec3& dc, const float* rest,
                                 int restPerChannel);

}  // namespace gsv
