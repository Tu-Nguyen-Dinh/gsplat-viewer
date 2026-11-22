#pragma once
// Core 3: static 3D kd-tree for nearest-neighbour queries (ICP correspondences, normal estimation,
// picking snaps). Balanced by median splits on the widest axis; leaves hold up to 16 points.
// Points are referenced, not copied: the vector must outlive the tree and not change.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace gsv {

class KdTree {
 public:
  void build(const std::vector<glm::vec3>& points);
  bool empty() const { return points_ == nullptr || points_->empty(); }

  // Index of the nearest point and its squared distance; returns false for an empty tree.
  bool nearest(const glm::vec3& q, uint32_t& index, float& dist2) const;
  // k nearest points sorted by increasing distance (fewer if the tree is smaller).
  void knn(const glm::vec3& q, int k, std::vector<uint32_t>& indices, std::vector<float>& dist2) const;
  // Every point within `radius` (unsorted).
  void radius(const glm::vec3& q, float radius, std::vector<uint32_t>& indices) const;

 private:
  struct Node {
    glm::vec3 lo, hi;      // bounding box of the node's points
    uint32_t begin, end;   // range in index_
    uint32_t left, right;  // child nodes (0 = leaf)
  };
  uint32_t buildNode(uint32_t begin, uint32_t end);
  const std::vector<glm::vec3>* points_ = nullptr;
  std::vector<uint32_t> index_;
  std::vector<Node> nodes_;
};

// Unit normals by PCA of the k nearest neighbours (smallest-eigenvalue eigenvector). The sign is
// oriented towards `viewpoint` when given (pass NaN to leave it arbitrary).
std::vector<glm::vec3> estimateNormals(const std::vector<glm::vec3>& points, const KdTree& tree, int k,
                                       const glm::vec3& viewpoint);

// Eigen-decomposition of a symmetric 3x3 matrix (cyclic Jacobi). Eigenvalues ascending; column i of
// `vectors` is the unit eigenvector of values[i].
void symmetricEigen3(const glm::dmat3& m, glm::dvec3& values, glm::dmat3& vectors);

}  // namespace gsv
