// Unit tests for the per-Gaussian math (src/core/gaussian_math.*) and image metrics.
#include "core/gaussian_math.h"
#include "core/image.h"
#include "test_framework.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>

using namespace gsv;

namespace {

// A camera at the origin looking down -Z, 800x600 pixels, 60 degree vertical FOV.
ProjectionParams testCamera() {
  ProjectionParams p;
  p.view = glm::mat4(1.0f);
  p.viewport = {800.0f, 600.0f};
  p.tanHalfFov.y = std::tan(glm::radians(30.0f));
  p.tanHalfFov.x = p.tanHalfFov.y * 800.0f / 600.0f;
  p.focal = 0.5f * p.viewport / p.tanHalfFov;
  p.nearPlane = 0.01f;
  return p;
}

}  // namespace

TEST_CASE(rotation_identity_quaternion) {
  const glm::mat3 R = rotationFromQuaternion({1, 0, 0, 0});
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) CHECK_NEAR(R[c][r], c == r ? 1.0 : 0.0, 1e-6);
}

TEST_CASE(rotation_90_degrees_about_y) {
  // q = (cos 45, 0, sin 45, 0): right-handed rotation of +90 deg about +Y sends +X to -Z.
  const float h = std::sqrt(0.5f);
  const glm::vec3 v = rotationFromQuaternion({h, 0, h, 0}) * glm::vec3(1, 0, 0);
  CHECK_NEAR(v.x, 0.0, 1e-6);
  CHECK_NEAR(v.y, 0.0, 1e-6);
  CHECK_NEAR(v.z, -1.0, 1e-6);
}

TEST_CASE(rotation_normalizes_quaternion) {
  const glm::mat3 a = rotationFromQuaternion({0.3f, 0.5f, -0.2f, 0.7f});
  const glm::mat3 b = rotationFromQuaternion(2.5f * glm::vec4(0.3f, 0.5f, -0.2f, 0.7f));
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) CHECK_NEAR(a[c][r], b[c][r], 1e-6);
}

TEST_CASE(covariance_isotropic_ignores_rotation) {
  const Cov3 c = covarianceFromScaleRotation(glm::vec3(0.5f), {0.3f, 0.5f, -0.2f, 0.7f});
  CHECK_NEAR(c.xx, 0.25, 1e-6);
  CHECK_NEAR(c.yy, 0.25, 1e-6);
  CHECK_NEAR(c.zz, 0.25, 1e-6);
  CHECK_NEAR(c.xy, 0.0, 1e-6);
  CHECK_NEAR(c.xz, 0.0, 1e-6);
  CHECK_NEAR(c.yz, 0.0, 1e-6);
}

TEST_CASE(covariance_rotation_preserves_eigenvalues) {
  // Rotation changes the axes but not their lengths: trace and determinant are invariant.
  const glm::vec3 s(0.1f, 0.4f, 2.0f);
  const glm::mat3 m = covarianceFromScaleRotation(s, {0.8f, 0.1f, -0.5f, 0.3f}).toMat3();
  CHECK_NEAR(m[0][0] + m[1][1] + m[2][2], s.x * s.x + s.y * s.y + s.z * s.z, 1e-5);
  CHECK_NEAR(glm::determinant(m), std::pow(s.x * s.y * s.z, 2.0), 1e-6);
}

TEST_CASE(projection_isotropic_at_center) {
  // A sphere of std-dev s at distance d projects to a circle of std-dev f*s/d pixels.
  const ProjectionParams cam = testCamera();
  const float s = 0.05f, d = 4.0f;
  const Cov3 cov = covarianceFromScaleRotation(glm::vec3(s), {1, 0, 0, 0});
  const ProjectedGaussian g = projectGaussian({0, 0, -d}, cov, cam);
  CHECK(g.visible);
  CHECK_NEAR(g.center.x, 400.0, 1e-3);
  CHECK_NEAR(g.center.y, 300.0, 1e-3);
  CHECK_NEAR(g.depth, d, 1e-5);
  const double sigmaPx = cam.focal.y * s / d;
  const double var = sigmaPx * sigmaPx + kScreenDilation;
  CHECK_NEAR(g.conic.x, 1.0 / var, 1e-4);
  CHECK_NEAR(g.conic.y, 0.0, 1e-6);
  CHECK_NEAR(g.conic.z, 1.0 / var, 1e-4);
  CHECK_NEAR(g.radius, std::ceil(3.0 * std::sqrt(var)), 1e-6);
}

TEST_CASE(projection_center_follows_pinhole_model) {
  const ProjectionParams cam = testCamera();
  const Cov3 cov = covarianceFromScaleRotation(glm::vec3(0.01f), {1, 0, 0, 0});
  const ProjectedGaussian g = projectGaussian({0.5f, -0.25f, -2.0f}, cov, cam);
  CHECK(g.visible);
  CHECK_NEAR(g.center.x, 400.0 + cam.focal.x * 0.5 / 2.0, 1e-3);
  CHECK_NEAR(g.center.y, 300.0 + cam.focal.y * -0.25 / 2.0, 1e-3);
}

TEST_CASE(projection_matches_numerical_jacobian) {
  // Check Sigma' = J Sigma_view J^T against a Jacobian computed by finite differences
  // of the pinhole projection, for an anisotropic, rotated Gaussian seen off-center.
  ProjectionParams cam = testCamera();
  cam.view = glm::lookAt(glm::vec3(1.0f, 0.5f, 3.0f), glm::vec3(0.2f, 0.1f, 0.0f), glm::vec3(0, 1, 0));
  const glm::vec3 mean(0.4f, 0.3f, -0.2f);
  const Cov3 cov = covarianceFromScaleRotation({0.05f, 0.2f, 0.01f}, {0.9f, 0.2f, 0.3f, -0.1f});
  const ProjectedGaussian g = projectGaussian(mean, cov, cam);
  CHECK(g.visible);

  auto toPixels = [&](const glm::vec3& t) { return glm::vec2(cam.focal.x * t.x / -t.z, cam.focal.y * t.y / -t.z); };
  const glm::vec3 t = glm::vec3(cam.view * glm::vec4(mean, 1.0f));
  glm::mat3x2 J;  // 3 columns, 2 rows
  const float h = 1e-3f;
  for (int k = 0; k < 3; ++k) {
    glm::vec3 e(0.0f);
    e[k] = h;
    J[k] = (toPixels(t + e) - toPixels(t - e)) / (2.0f * h);
  }
  const glm::mat3 W(cam.view);
  const glm::mat3 S = W * cov.toMat3() * glm::transpose(W);
  const glm::mat2 sigma2 = J * S * glm::transpose(J) + glm::mat2(kScreenDilation);
  const glm::mat2 conic = glm::inverse(sigma2);
  CHECK_NEAR(g.conic.x, conic[0][0], 1e-3 * std::abs(conic[0][0]) + 1e-6);
  CHECK_NEAR(g.conic.y, conic[1][0], 1e-3 * std::abs(conic[0][0]) + 1e-6);
  CHECK_NEAR(g.conic.z, conic[1][1], 1e-3 * std::abs(conic[1][1]) + 1e-6);
}

TEST_CASE(projection_axes_are_eigenvectors) {
  // The ellipse half-axes must satisfy d^T conic d = 9 (the 3-sigma contour) and be orthogonal.
  ProjectionParams cam = testCamera();
  const Cov3 cov = covarianceFromScaleRotation({0.3f, 0.02f, 0.05f}, {0.7f, 0.1f, 0.6f, 0.2f});
  const ProjectedGaussian g = projectGaussian({0.1f, 0.0f, -3.0f}, cov, cam);
  CHECK(g.visible);
  for (const glm::vec2& d : {g.axisMajor, g.axisMinor}) {
    const float q = g.conic.x * d.x * d.x + 2.0f * g.conic.y * d.x * d.y + g.conic.z * d.y * d.y;
    CHECK_NEAR(q, 9.0, 1e-2);
  }
  CHECK_NEAR(glm::dot(g.axisMajor, g.axisMinor), 0.0, 1e-3);
  CHECK(glm::length(g.axisMajor) >= glm::length(g.axisMinor));
}

TEST_CASE(projection_culls_behind_and_outside) {
  const ProjectionParams cam = testCamera();
  const Cov3 cov = covarianceFromScaleRotation(glm::vec3(0.01f), {1, 0, 0, 0});
  CHECK(!projectGaussian({0, 0, 1.0f}, cov, cam).visible);   // behind the camera
  CHECK(!projectGaussian({50, 0, -1.0f}, cov, cam).visible);  // far to the right
  ProjectionParams noCull = cam;
  noCull.cullOutsideViewport = false;
  CHECK(projectGaussian({50, 0, -1.0f}, cov, noCull).visible);
}

TEST_CASE(gaussian_falloff_values) {
  const glm::vec3 conic(1.0f / 4.0f, 0.0f, 1.0f);  // sigma_x = 2, sigma_y = 1
  CHECK_NEAR(gaussianFalloff(conic, {0, 0}), 1.0, 1e-7);
  CHECK_NEAR(gaussianFalloff(conic, {2, 0}), std::exp(-0.5), 1e-6);
  CHECK_NEAR(gaussianFalloff(conic, {0, 1}), std::exp(-0.5), 1e-6);
}

TEST_CASE(spherical_harmonics_degree0_and_1) {
  const glm::vec3 dc(1.0f, -1.0f, 0.0f);
  const glm::vec3 c0 = evalSphericalHarmonics(0, {0, 0, 1}, dc, nullptr, 0);
  CHECK_NEAR(c0.r, 0.5 + kShC0, 1e-6);
  CHECK_NEAR(c0.g, 0.5 - kShC0, 1e-6);
  // Degree 1 with only coefficient 2 (the "z" basis function) set to 1 for red.
  float rest[9] = {0, 1, 0, /*G*/ 0, 0, 0, /*B*/ 0, 0, 0};
  const glm::vec3 up = evalSphericalHarmonics(1, {0, 0, 1}, glm::vec3(0), rest, 3);
  const glm::vec3 down = evalSphericalHarmonics(1, {0, 0, -1}, glm::vec3(0), rest, 3);
  CHECK_NEAR(up.r, 0.5 + kShC1, 1e-6);
  CHECK_NEAR(down.r, 0.5 - kShC1, 1e-6);  // band 1 is odd: opposite direction, opposite sign
  CHECK_NEAR(up.g, 0.5, 1e-6);
}

TEST_CASE(spherical_harmonics_clamps_negative) {
  const glm::vec3 c = evalSphericalHarmonics(0, {1, 0, 0}, glm::vec3(-10.0f), nullptr, 0);
  CHECK_NEAR(c.r, 0.0, 1e-7);
}

TEST_CASE(psnr_values) {
  Image a(4, 4), b(4, 4);
  CHECK(psnr(a, b) == std::numeric_limits<double>::infinity());
  b.rgb[0] = 255;  // one channel of one pixel fully wrong: MSE = 255^2 / 48
  CHECK_NEAR(psnr(a, b), 10.0 * std::log10(48.0), 1e-9);
  CHECK(maxAbsDifference(a, b) == 255);
}
