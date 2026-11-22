#include "scene/layers.h"

#include <algorithm>
#include <cmath>

namespace gsv {

// ---------------------------------------------------------------- PointCloud
void PointCloud::computeBounds() {
  if (position.empty()) {
    localMin = localMax = glm::vec3(0.0f);
    return;
  }
  localMin = localMax = position[0];
  for (const glm::vec3& p : position) {
    localMin = glm::min(localMin, p);
    localMax = glm::max(localMax, p);
  }
}

void PointCloud::rebase(const glm::dvec3& newOrigin) {
  const glm::dvec3 delta = origin - newOrigin;
  for (glm::vec3& p : position) p = glm::vec3(glm::dvec3(p) + delta);
  origin = newOrigin;
  computeBounds();
}

void PointCloud::reserve(size_t n, bool withColor, bool withIntensity, bool withClass) {
  position.reserve(n);
  if (withColor) color.reserve(n);
  if (withIntensity) intensity.reserve(n);
  if (withClass) classification.reserve(n);
}

// ---------------------------------------------------------------- WorldBounds
void WorldBounds::extend(const glm::dvec3& p) {
  if (!valid) {
    min = max = p;
    valid = true;
    return;
  }
  min = glm::min(min, p);
  max = glm::max(max, p);
}

// ---------------------------------------------------------------- LayerStack
int LayerStack::addSplats(std::shared_ptr<GaussianScene> scene, const std::string& name, const GeoFrame& frame) {
  Layer l;
  l.id = nextId_++;
  l.name = name;
  l.kind = LayerKind::Splats;
  l.frame = frame;
  l.splats = std::move(scene);
  layers_.push_back(std::move(l));
  return layers_.back().id;
}

int LayerStack::addPoints(std::shared_ptr<PointCloud> cloud, const std::string& name, const GeoFrame& frame) {
  Layer l;
  l.id = nextId_++;
  l.name = name;
  l.kind = LayerKind::Points;
  l.frame = frame;
  l.points = std::move(cloud);
  layers_.push_back(std::move(l));
  return layers_.back().id;
}

bool LayerStack::remove(int id) {
  const auto it = std::find_if(layers_.begin(), layers_.end(), [id](const Layer& l) { return l.id == id; });
  if (it == layers_.end()) return false;
  layers_.erase(it);
  return true;
}

Layer* LayerStack::find(int id) {
  for (Layer& l : layers_)
    if (l.id == id) return &l;
  return nullptr;
}

const Layer* LayerStack::find(int id) const {
  for (const Layer& l : layers_)
    if (l.id == id) return &l;
  return nullptr;
}

WorldBounds LayerStack::worldBounds(bool visibleOnly) const {
  WorldBounds b;
  for (const Layer& l : layers_) {
    if (visibleOnly && !l.visible) continue;
    glm::dvec3 lo, hi;
    if (l.kind == LayerKind::Splats && l.splats && !l.splats->empty()) {
      lo = glm::dvec3(l.splats->boundsMin);
      hi = glm::dvec3(l.splats->boundsMax);
    } else if (l.kind == LayerKind::Points && l.points && !l.points->empty()) {
      lo = l.points->origin + glm::dvec3(l.points->localMin);
      hi = l.points->origin + glm::dvec3(l.points->localMax);
    } else {
      continue;
    }
    for (int c = 0; c < 8; ++c) {
      const glm::dvec3 corner((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
      b.extend(l.frame.apply(corner));
    }
  }
  return b;
}

glm::dvec3 LayerStack::suggestRenderOrigin() const {
  const WorldBounds b = worldBounds(false);
  if (!b.valid) return glm::dvec3(0.0);
  const glm::dvec3 c = b.center();
  return {std::round(c.x), std::round(c.y), std::round(c.z)};
}

}  // namespace gsv
