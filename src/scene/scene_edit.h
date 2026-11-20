#pragma once

// Basic scene editing: select splats with a box or a sphere, filter by opacity / size, and
// remove them. Everything works on a per-splat mask so the UI can preview counts first.

#include "scene/gaussian_scene.h"

#include <cstdint>
#include <vector>

namespace gsv {

// A selection volume in scene (file) coordinates.
struct SelectionShape {
  enum class Type { Box, Sphere };
  Type type = Type::Box;
  glm::vec3 center{0.0f};
  glm::vec3 halfExtent{1.0f};  // box
  float radius = 1.0f;         // sphere

  bool contains(const glm::vec3& p) const;
};

using SplatMask = std::vector<uint8_t>;  // 1 = selected

// Splats whose center lies inside the shape.
// `model` maps scene positions to the space the shape is defined in (the viewer defines it in
// display space, so a box stays level with the floor the user sees).
SplatMask selectInside(const GaussianScene& scene, const SelectionShape& shape,
                       const glm::mat4& model = glm::mat4(1.0f));
// Splats that are nearly transparent (opacity < minOpacity) or huge (largest axis > maxScale,
// in scene units; <= 0 disables the size test). Typical floaters are both.
SplatMask selectByFilter(const GaussianScene& scene, float minOpacity, float maxScale);

void invert(SplatMask& mask);
void unite(SplatMask& mask, const SplatMask& other);
size_t countSelected(const SplatMask& mask);

// Copy of the scene without the selected splats (bounds and focus recomputed).
GaussianScene removeSelected(const GaussianScene& scene, const SplatMask& mask);

}  // namespace gsv
