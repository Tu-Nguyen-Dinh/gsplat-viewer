#include "core/picking.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gsv {

std::optional<glm::vec3> pickPoint(const DepthImage& d, int x, int y, const PickCamera& cam, float minCoverage,
                                   int r) {
  if (d.empty() || x < 0 || y < 0 || x >= d.width || y >= d.height) return std::nullopt;
  std::vector<float> zs;
  for (int yy = std::max(0, y - r); yy <= std::min(d.height - 1, y + r); ++yy)
    for (int xx = std::max(0, x - r); xx <= std::min(d.width - 1, x + r); ++xx)
      if (d.coverageAt(xx, yy) >= minCoverage && d.depthAt(xx, yy) > 0.0f) zs.push_back(d.depthAt(xx, yy));
  if (d.coverageAt(x, y) < minCoverage || zs.empty()) return std::nullopt;
  std::nth_element(zs.begin(), zs.begin() + static_cast<long>(zs.size() / 2), zs.end());
  const float z = zs[zs.size() / 2];
  // Pixel centre in OpenGL convention (origin bottom-left), then pinhole back-projection.
  const glm::vec2 px(static_cast<float>(x) + 0.5f, static_cast<float>(d.height - 1 - y) + 0.5f);
  const glm::vec3 camPoint((px.x - cam.principal.x) / cam.focal.x * z, (px.y - cam.principal.y) / cam.focal.y * z, -z);
  return glm::vec3(glm::inverse(cam.view) * glm::vec4(camPoint, 1.0f));
}

Measurement measure(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& up) {
  Measurement m;
  const glm::dvec3 d = b - a;
  const glm::dvec3 u = glm::normalize(up);
  m.distance = glm::length(d);
  m.vertical = glm::dot(d, u);
  m.horizontal = glm::length(d - m.vertical * u);
  return m;
}

}  // namespace gsv
