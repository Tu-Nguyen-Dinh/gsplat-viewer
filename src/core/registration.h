#pragma once
// Core 3: aligning data sets. See docs/core/CORE3-geometry.md.
//
//   estimateSimilarity   closed-form least squares s, R, t from point pairs (Horn's quaternion method,
//                        same optimum as Umeyama 1991); used for COLMAP <-> ARCore trajectories,
//                        marker corners, hand-picked point pairs.
//   estimateSimilarityRansac   the same with outlier pairs.
//   icpPointToPlane      fine alignment of two overlapping clouds (scan <-> scan, LiDAR <-> splat centres).
//   fitPlaneRansac       dominant plane (floor, wall) with outliers.
// All maths in double; inputs are float offsets around a common origin (see geo_frame.h).

#include "core/geo_frame.h"
#include "core/kdtree.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace gsv {

// Least-squares dst ~ s R src + t. withScale = false forces s = 1 (rigid). Needs >= 3 non-collinear
// pairs; returns false otherwise.
bool estimateSimilarity(const std::vector<glm::dvec3>& src, const std::vector<glm::dvec3>& dst, bool withScale,
                        GeoFrame& out);

struct RansacResult {
  GeoFrame transform;
  std::vector<uint32_t> inliers;
  double rms = 0.0;   // over inliers, after refitting
  bool ok = false;
};
RansacResult estimateSimilarityRansac(const std::vector<glm::dvec3>& src, const std::vector<glm::dvec3>& dst,
                                      bool withScale, double inlierThreshold, int iterations = 500,
                                      uint32_t seed = 1);

struct IcpOptions {
  int maxIterations = 50;
  double maxCorrespondenceDistance = 0.5;  // metres; pairs farther apart are ignored
  double convergenceRotation = 1e-6;       // radians per iteration
  double convergenceTranslation = 1e-6;    // metres per iteration
  bool shrinkDistance = true;              // gate -> 3 x median pair distance (robust to partial overlap)
};
struct IcpResult {
  GeoFrame transform;      // maps source coordinates onto the target
  double rms = 0.0;        // point-to-plane RMS of the last iteration's pairs
  size_t pairs = 0;
  int iterations = 0;
  bool converged = false;
};
// `target` with its normals (estimateNormals) and kd-tree; `source` points in the same coordinate
// system as `initial` expects (initial maps source -> target). Rigid (6 DoF).
IcpResult icpPointToPlane(const std::vector<glm::vec3>& source, const std::vector<glm::vec3>& target,
                          const std::vector<glm::vec3>& targetNormals, const KdTree& targetTree,
                          const GeoFrame& initial, const IcpOptions& options = IcpOptions());

struct Plane {
  glm::dvec3 normal{0, 0, 1};  // unit
  double d = 0.0;              // plane: dot(normal, p) + d = 0
  double distance(const glm::dvec3& p) const { return glm::dot(normal, p) + d; }
};
struct PlaneFit {
  Plane plane;
  std::vector<uint32_t> inliers;
  bool ok = false;
};
PlaneFit fitPlaneRansac(const std::vector<glm::vec3>& points, double threshold, int iterations = 300, uint32_t seed = 1);
// Least-squares plane through points (PCA).
bool fitPlane(const std::vector<glm::vec3>& points, const std::vector<uint32_t>& subset, Plane& out);

}  // namespace gsv
