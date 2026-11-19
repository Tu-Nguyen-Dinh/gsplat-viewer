#include "core/gaussian_math.h"

#include <algorithm>
#include <cmath>

namespace gsv {

glm::mat3 Cov3::toMat3() const {
  // glm is column-major, but the matrix is symmetric so the order does not matter.
  return glm::mat3(xx, xy, xz,  //
                   xy, yy, yz,  //
                   xz, yz, zz);
}

glm::mat3 rotationFromQuaternion(const glm::vec4& q) {
  const float len = glm::length(q);
  const glm::vec4 n = len > 0.0f ? q / len : glm::vec4(1, 0, 0, 0);
  const float w = n.x, x = n.y, y = n.z, z = n.w;
  // Standard unit-quaternion -> rotation matrix. glm::mat3(c0, c1, c2) takes COLUMNS.
  return glm::mat3(1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + w * z), 2.0f * (x * z - w * y),   // column 0
                   2.0f * (x * y - w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + w * x),   // column 1
                   2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y));  // column 2
}

Cov3 covarianceFromScaleRotation(const glm::vec3& scale, const glm::vec4& wxyz) {
  const glm::mat3 R = rotationFromQuaternion(wxyz);
  // M = R * S: scaling each column of R by the corresponding axis length.
  const glm::mat3 M(R[0] * scale.x, R[1] * scale.y, R[2] * scale.z);
  const glm::mat3 S = M * glm::transpose(M);  // = R S S^T R^T
  return {S[0][0], S[1][0], S[2][0], S[1][1], S[2][1], S[2][2]};
}

ProjectedGaussian projectGaussian(const glm::vec3& mean, const Cov3& cov, const ProjectionParams& p) {
  ProjectedGaussian out;

  // 1. Center in camera space. The camera looks down -Z, so depth = -t.z.
  const glm::vec3 t = glm::vec3(p.view * glm::vec4(mean, 1.0f));
  const float z = -t.z;
  if (z < p.nearPlane) return out;

  // 2. Jacobian of the perspective projection, evaluated at the center. Far outside
  //    the view cone the linear approximation explodes, so clamp x/z and y/z.
  const float limX = 1.3f * p.tanHalfFov.x, limY = 1.3f * p.tanHalfFov.y;
  const float tx = std::clamp(t.x / z, -limX, limX) * z;
  const float ty = std::clamp(t.y / z, -limY, limY) * z;
  const float j00 = p.focal.x / z, j02 = p.focal.x * tx / (z * z);
  const float j11 = p.focal.y / z, j12 = p.focal.y * ty / (z * z);

  // 3. Covariance in camera space: Sigma_view = W Sigma W^T (W = rotation part of the view matrix).
  const glm::mat3 W(p.view);
  const float s2 = p.splatScale * p.splatScale;
  const glm::mat3 S = W * (cov.toMat3() * s2) * glm::transpose(W);
  const float s00 = S[0][0], s01 = S[1][0], s02 = S[2][0], s11 = S[1][1], s12 = S[2][1], s22 = S[2][2];

  // 4. Screen-space covariance Sigma' = J Sigma_view J^T, written out for J = [[j00 0 j02][0 j11 j12]].
  float a = j00 * j00 * s00 + 2.0f * j00 * j02 * s02 + j02 * j02 * s22;
  const float b = j00 * j11 * s01 + j00 * j12 * s02 + j02 * j11 * s12 + j02 * j12 * s22;
  float c = j11 * j11 * s11 + 2.0f * j11 * j12 * s12 + j12 * j12 * s22;
  a += kScreenDilation;
  c += kScreenDilation;

  const float det = a * c - b * b;
  if (det <= 0.0f) return out;

  // 5. Eigen-decomposition of the 2x2 covariance: lengths and directions of the ellipse axes.
  const float mid = 0.5f * (a + c);
  const float disc = std::sqrt(std::max(0.1f, mid * mid - det));
  const float lambda1 = mid + disc;
  const float lambda2 = std::max(mid - disc, 0.1f);
  glm::vec2 v1 = std::abs(b) > 1e-6f ? glm::normalize(glm::vec2(b, lambda1 - a))
                                     : (a >= c ? glm::vec2(1, 0) : glm::vec2(0, 1));
  const glm::vec2 v2(-v1.y, v1.x);

  out.center = glm::vec2(p.focal.x * t.x / z, p.focal.y * t.y / z) + 0.5f * p.viewport;
  out.depth = z;
  out.conic = glm::vec3(c, -b, a) / det;
  out.radius = std::ceil(kSigmaExtent * std::sqrt(lambda1));
  out.axisMajor = v1 * (kSigmaExtent * std::sqrt(lambda1));
  out.axisMinor = v2 * (kSigmaExtent * std::sqrt(lambda2));

  // 6. Cull splats whose 3-sigma box does not touch the viewport.
  if (p.cullOutsideViewport) {
    const float r = out.radius;
    if (out.center.x + r < 0.0f || out.center.x - r > p.viewport.x || out.center.y + r < 0.0f ||
        out.center.y - r > p.viewport.y)
      return out;
  }
  out.visible = true;
  return out;
}

float gaussianFalloff(const glm::vec3& conic, const glm::vec2& d) {
  const float power = -0.5f * (conic.x * d.x * d.x + 2.0f * conic.y * d.x * d.y + conic.z * d.y * d.y);
  return power > 0.0f ? 0.0f : std::exp(power);
}

glm::vec3 evalSphericalHarmonics(int degree, const glm::vec3& dir, const glm::vec3& dc, const float* rest,
                                 int restPerChannel) {
  // sh(k) = coefficient k (k >= 1) for all three channels.
  auto sh = [&](int k) {
    return glm::vec3(rest[0 * restPerChannel + k - 1], rest[1 * restPerChannel + k - 1],
                     rest[2 * restPerChannel + k - 1]);
  };
  glm::vec3 result = kShC0 * dc;
  if (degree > 0 && rest && restPerChannel >= 3) {
    const float x = dir.x, y = dir.y, z = dir.z;
    result += -kShC1 * y * sh(1) + kShC1 * z * sh(2) - kShC1 * x * sh(3);
    if (degree > 1 && restPerChannel >= 8) {
      const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, yz = y * z, xz = x * z;
      result += kShC2[0] * xy * sh(4) + kShC2[1] * yz * sh(5) + kShC2[2] * (2.0f * zz - xx - yy) * sh(6) +
                kShC2[3] * xz * sh(7) + kShC2[4] * (xx - yy) * sh(8);
      if (degree > 2 && restPerChannel >= 15) {
        result += kShC3[0] * y * (3.0f * xx - yy) * sh(9) + kShC3[1] * xy * z * sh(10) +
                  kShC3[2] * y * (4.0f * zz - xx - yy) * sh(11) +
                  kShC3[3] * z * (2.0f * zz - 3.0f * xx - 3.0f * yy) * sh(12) +
                  kShC3[4] * x * (4.0f * zz - xx - yy) * sh(13) + kShC3[5] * z * (xx - yy) * sh(14) +
                  kShC3[6] * x * (xx - 3.0f * yy) * sh(15);
      }
    }
  }
  return glm::max(result + 0.5f, glm::vec3(0.0f));
}

}  // namespace gsv
