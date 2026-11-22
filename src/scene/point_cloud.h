#pragma once
// A plain (non-Gaussian) point cloud: LiDAR scans, SfM points, depth-sensor fusion.
//
// Coordinates are split into a double `origin` and float offsets, so a cloud in survey
// coordinates (10^6 m) keeps millimetre resolution: the float only holds offsets of at most a few
// kilometres. Attributes are optional (empty vector = absent), in structure-of-arrays form.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace gsv {

struct PointCloud {
  glm::dvec3 origin{0.0};                  // world position of local (0, 0, 0)
  std::vector<glm::vec3> position;         // offsets from origin (metres)
  std::vector<glm::u8vec3> color;          // sRGB 8-bit, optional
  std::vector<uint16_t> intensity;         // LiDAR return strength, optional
  std::vector<uint8_t> classification;     // ASPRS class (2 = ground, 6 = building...), optional
  std::vector<glm::vec3> normal;           // unit normals, optional (estimateNormals)
  std::string sourceName;

  size_t size() const { return position.size(); }
  bool empty() const { return position.empty(); }
  bool hasColor() const { return color.size() == position.size() && !position.empty(); }
  glm::dvec3 world(size_t i) const { return origin + glm::dvec3(position[i]); }

  // Local-space bounds of the offsets (computeBounds fills them).
  glm::vec3 localMin{0.0f}, localMax{0.0f};
  void computeBounds();

  // Moves the origin (exactly, in double) and rewrites the offsets around it.
  void rebase(const glm::dvec3& newOrigin);
  void reserve(size_t n, bool withColor, bool withIntensity, bool withClass);
};

}  // namespace gsv
