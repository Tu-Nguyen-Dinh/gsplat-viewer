#include "core/geo_frame.h"

#include <algorithm>
#include <cmath>

namespace gsv {

GeoFrame GeoFrame::inverse() const {
  // p = s R q + t  =>  q = (1/s) R^T (p - t)
  GeoFrame r;
  r.scale = 1.0 / scale;
  r.rotation = glm::transpose(rotation);
  r.translation = -(r.scale * (r.rotation * translation));
  return r;
}

glm::dmat4 GeoFrame::matrix() const {
  glm::dmat4 m(1.0);
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) m[c][r] = scale * rotation[c][r];
  m[3] = glm::dvec4(translation, 1.0);
  return m;
}

glm::mat4 GeoFrame::localMatrix(const glm::dvec3& origin) const {
  glm::dmat4 m = matrix();
  m[3] = glm::dvec4(translation - origin, 1.0);
  return glm::mat4(m);
}

void GeoFrame::orthonormalize() {
  glm::dvec3 x = glm::normalize(rotation[0]);
  glm::dvec3 y = rotation[1] - glm::dot(rotation[1], x) * x;
  y = glm::normalize(y);
  const glm::dvec3 z = glm::cross(x, y);
  rotation = glm::dmat3(x, y, z);
}

double rotationAngleBetween(const glm::dmat3& a, const glm::dmat3& b) {
  const glm::dmat3 r = glm::transpose(a) * b;
  const double c = std::clamp((r[0][0] + r[1][1] + r[2][2] - 1.0) * 0.5, -1.0, 1.0);
  return std::acos(c);
}

}  // namespace gsv
