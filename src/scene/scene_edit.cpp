#include "scene/scene_edit.h"

#include <algorithm>
#include <cmath>

namespace gsv {

bool SelectionShape::contains(const glm::vec3& p) const {
  if (type == Type::Sphere) return glm::dot(p - center, p - center) <= radius * radius;
  const glm::vec3 d = glm::abs(p - center);
  return d.x <= halfExtent.x && d.y <= halfExtent.y && d.z <= halfExtent.z;
}

SplatMask selectInside(const GaussianScene& scene, const SelectionShape& shape, const glm::mat4& model) {
  SplatMask mask(scene.size(), 0);
  for (size_t i = 0; i < scene.size(); ++i)
    mask[i] = shape.contains(glm::vec3(model * glm::vec4(scene.position[i], 1.0f))) ? 1 : 0;
  return mask;
}

SplatMask selectByFilter(const GaussianScene& scene, float minOpacity, float maxScale) {
  SplatMask mask(scene.size(), 0);
  // Compare in the stored (log / logit) domain: one log per call instead of exp per splat.
  const float minLogit = minOpacity <= 0.0f ? -INFINITY : minOpacity >= 1.0f ? INFINITY : logit(minOpacity);
  const float maxLogScale = maxScale > 0.0f ? std::log(maxScale) : INFINITY;
  for (size_t i = 0; i < scene.size(); ++i) {
    const glm::vec3& s = scene.logScale[i];
    const float largest = std::max(s.x, std::max(s.y, s.z));
    mask[i] = (scene.opacityLogit[i] < minLogit || largest > maxLogScale) ? 1 : 0;
  }
  return mask;
}

void invert(SplatMask& mask) {
  for (uint8_t& m : mask) m = m ? 0 : 1;
}

void unite(SplatMask& mask, const SplatMask& other) {
  for (size_t i = 0; i < mask.size() && i < other.size(); ++i) mask[i] = (mask[i] || other[i]) ? 1 : 0;
}

size_t countSelected(const SplatMask& mask) {
  return static_cast<size_t>(std::count(mask.begin(), mask.end(), uint8_t(1)));
}

GaussianScene removeSelected(const GaussianScene& scene, const SplatMask& mask) {
  GaussianScene out;
  out.shDegree = scene.shDegree;
  out.shRestPerChannel = scene.shRestPerChannel;
  out.sourceName = scene.sourceName;
  const size_t keep = scene.size() - countSelected(mask);
  const size_t rest = static_cast<size_t>(scene.shRestPerChannel) * 3;
  out.position.reserve(keep);
  out.logScale.reserve(keep);
  out.rotation.reserve(keep);
  out.opacityLogit.reserve(keep);
  out.shDC.reserve(keep);
  out.shRest.reserve(keep * rest);
  for (size_t i = 0; i < scene.size(); ++i) {
    if (i < mask.size() && mask[i]) continue;
    out.position.push_back(scene.position[i]);
    out.logScale.push_back(scene.logScale[i]);
    out.rotation.push_back(scene.rotation[i]);
    out.opacityLogit.push_back(scene.opacityLogit[i]);
    out.shDC.push_back(scene.shDC[i]);
    if (rest) out.shRest.insert(out.shRest.end(), scene.shRest.begin() + static_cast<std::ptrdiff_t>(i * rest),
                                scene.shRest.begin() + static_cast<std::ptrdiff_t>((i + 1) * rest));
  }
  if (!out.empty()) computeRobustBounds(out);
  return out;
}

}  // namespace gsv
