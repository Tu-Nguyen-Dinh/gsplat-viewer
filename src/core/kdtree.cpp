#include "core/kdtree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace gsv {

namespace {
constexpr uint32_t kLeafSize = 16;

float boxDist2(const glm::vec3& q, const glm::vec3& lo, const glm::vec3& hi) {
  const glm::vec3 d = glm::max(glm::max(lo - q, q - hi), glm::vec3(0.0f));
  return glm::dot(d, d);
}
}  // namespace

void KdTree::build(const std::vector<glm::vec3>& points) {
  points_ = &points;
  index_.resize(points.size());
  for (uint32_t i = 0; i < index_.size(); ++i) index_[i] = i;
  nodes_.clear();
  nodes_.reserve(2 * (points.size() / kLeafSize + 1));
  if (!points.empty()) buildNode(0, static_cast<uint32_t>(points.size()));
}

uint32_t KdTree::buildNode(uint32_t begin, uint32_t end) {
  const std::vector<glm::vec3>& P = *points_;
  Node n{P[index_[begin]], P[index_[begin]], begin, end, 0, 0};
  for (uint32_t i = begin + 1; i < end; ++i) {
    n.lo = glm::min(n.lo, P[index_[i]]);
    n.hi = glm::max(n.hi, P[index_[i]]);
  }
  const uint32_t id = static_cast<uint32_t>(nodes_.size());
  nodes_.push_back(n);
  if (end - begin <= kLeafSize) return id;
  const glm::vec3 ext = n.hi - n.lo;
  const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
  const uint32_t mid = begin + (end - begin) / 2;
  std::nth_element(index_.begin() + begin, index_.begin() + mid, index_.begin() + end,
                   [&](uint32_t a, uint32_t b) { return P[a][axis] < P[b][axis]; });
  const uint32_t left = buildNode(begin, mid);
  const uint32_t right = buildNode(mid, end);
  nodes_[id].left = left;
  nodes_[id].right = right;
  return id;
}

bool KdTree::nearest(const glm::vec3& q, uint32_t& index, float& dist2) const {
  std::vector<uint32_t> idx;
  std::vector<float> d2;
  knn(q, 1, idx, d2);
  if (idx.empty()) return false;
  index = idx[0];
  dist2 = d2[0];
  return true;
}

void KdTree::knn(const glm::vec3& q, int k, std::vector<uint32_t>& indices, std::vector<float>& dist2) const {
  indices.clear();
  dist2.clear();
  if (empty() || k <= 0) return;
  const std::vector<glm::vec3>& P = *points_;
  // Max-heap of the best k candidates (distance, index).
  std::priority_queue<std::pair<float, uint32_t>> best;
  auto worst = [&]() { return static_cast<int>(best.size()) < k ? std::numeric_limits<float>::infinity() : best.top().first; };
  // Depth-first traversal, nearer child first, pruning boxes farther than the current k-th.
  uint32_t stack[128];
  int top = 0;
  stack[top++] = 0;
  while (top > 0) {
    const Node& n = nodes_[stack[--top]];
    if (boxDist2(q, n.lo, n.hi) >= worst()) continue;
    if (n.left == 0) {
      for (uint32_t i = n.begin; i < n.end; ++i) {
        const glm::vec3 d = P[index_[i]] - q;
        const float dd = glm::dot(d, d);
        if (dd < worst()) {
          best.emplace(dd, index_[i]);
          if (static_cast<int>(best.size()) > k) best.pop();
        }
      }
      continue;
    }
    const float dl = boxDist2(q, nodes_[n.left].lo, nodes_[n.left].hi);
    const float dr = boxDist2(q, nodes_[n.right].lo, nodes_[n.right].hi);
    if (top + 2 > 128) continue;  // cannot happen for balanced trees (depth ~log2(n/16))
    if (dl < dr) {
      stack[top++] = n.right;
      stack[top++] = n.left;
    } else {
      stack[top++] = n.left;
      stack[top++] = n.right;
    }
  }
  indices.resize(best.size());
  dist2.resize(best.size());
  for (size_t i = best.size(); i-- > 0;) {
    dist2[i] = best.top().first;
    indices[i] = best.top().second;
    best.pop();
  }
}

void KdTree::radius(const glm::vec3& q, float r, std::vector<uint32_t>& indices) const {
  indices.clear();
  if (empty()) return;
  const std::vector<glm::vec3>& P = *points_;
  const float r2 = r * r;
  uint32_t stack[128];
  int top = 0;
  stack[top++] = 0;
  while (top > 0) {
    const Node& n = nodes_[stack[--top]];
    if (boxDist2(q, n.lo, n.hi) > r2) continue;
    if (n.left == 0) {
      for (uint32_t i = n.begin; i < n.end; ++i) {
        const glm::vec3 d = P[index_[i]] - q;
        if (glm::dot(d, d) <= r2) indices.push_back(index_[i]);
      }
    } else if (top + 2 <= 128) {
      stack[top++] = n.left;
      stack[top++] = n.right;
    }
  }
}

void symmetricEigen3(const glm::dmat3& m, glm::dvec3& values, glm::dmat3& vectors) {
  double a[3][3];
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) a[r][c] = m[c][r];
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 50; ++sweep) {
    const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    if (off < 1e-30) break;
    for (int p = 0; p < 2; ++p)
      for (int q = p + 1; q < 3; ++q) {
        if (std::fabs(a[p][q]) < 1e-300) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (int k = 0; k < 3; ++k) {  // A = J^T A J
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
  }
  int order[3] = {0, 1, 2};
  std::sort(order, order + 3, [&](int i, int j) { return a[i][i] < a[j][j]; });
  for (int i = 0; i < 3; ++i) {
    values[i] = a[order[i]][order[i]];
    vectors[i] = glm::dvec3(v[0][order[i]], v[1][order[i]], v[2][order[i]]);
  }
}

std::vector<glm::vec3> estimateNormals(const std::vector<glm::vec3>& points, const KdTree& tree, int k,
                                       const glm::vec3& viewpoint) {
  std::vector<glm::vec3> normals(points.size(), glm::vec3(0, 0, 1));
  std::vector<uint32_t> idx;
  std::vector<float> d2;
  const bool orient = std::isfinite(viewpoint.x);
  for (size_t i = 0; i < points.size(); ++i) {
    tree.knn(points[i], k, idx, d2);
    if (idx.size() < 3) continue;
    glm::dvec3 mean(0.0);
    for (uint32_t j : idx) mean += glm::dvec3(points[j]);
    mean /= static_cast<double>(idx.size());
    glm::dmat3 cov(0.0);
    for (uint32_t j : idx) {
      const glm::dvec3 d = glm::dvec3(points[j]) - mean;
      cov += glm::outerProduct(d, d);
    }
    glm::dvec3 values;
    glm::dmat3 vectors;
    symmetricEigen3(cov, values, vectors);
    glm::vec3 n = glm::normalize(glm::vec3(vectors[0]));
    if (orient && glm::dot(n, viewpoint - points[i]) < 0.0f) n = -n;
    normals[i] = n;
  }
  return normals;
}

}  // namespace gsv
