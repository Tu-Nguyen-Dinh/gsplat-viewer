#include "core/registration.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace gsv {

namespace {

// Cyclic Jacobi for a symmetric 4x4 matrix; returns the eigenvector of the largest eigenvalue.
glm::dvec4 largestEigenvector4(double a[4][4]) {
  double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  for (int sweep = 0; sweep < 60; ++sweep) {
    double off = 0.0;
    for (int p = 0; p < 4; ++p)
      for (int q = p + 1; q < 4; ++q) off += a[p][q] * a[p][q];
    if (off < 1e-30) break;
    for (int p = 0; p < 3; ++p)
      for (int q = p + 1; q < 4; ++q) {
        if (std::fabs(a[p][q]) < 1e-300) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        for (int k = 0; k < 4; ++k) {
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 4; ++k) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 4; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
  }
  int best = 0;
  for (int i = 1; i < 4; ++i)
    if (a[i][i] > a[best][best]) best = i;
  return {v[0][best], v[1][best], v[2][best], v[3][best]};
}

// Solves the symmetric positive (semi-)definite 6x6 system A x = b (Gaussian elimination with
// partial pivoting). Returns false when singular (degenerate geometry, e.g. a single plane).
bool solve6(double A[6][6], double b[6], double x[6]) {
  for (int c = 0; c < 6; ++c) {
    int piv = c;
    for (int r = c + 1; r < 6; ++r)
      if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) piv = r;
    if (std::fabs(A[piv][c]) < 1e-12) return false;
    if (piv != c) {
      for (int k = 0; k < 6; ++k) std::swap(A[c][k], A[piv][k]);
      std::swap(b[c], b[piv]);
    }
    for (int r = c + 1; r < 6; ++r) {
      const double f = A[r][c] / A[c][c];
      for (int k = c; k < 6; ++k) A[r][k] -= f * A[c][k];
      b[r] -= f * b[c];
    }
  }
  for (int r = 5; r >= 0; --r) {
    double s = b[r];
    for (int k = r + 1; k < 6; ++k) s -= A[r][k] * x[k];
    x[r] = s / A[r][r];
  }
  return true;
}

uint32_t nextRandom(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

}  // namespace

bool estimateSimilarity(const std::vector<glm::dvec3>& src, const std::vector<glm::dvec3>& dst, bool withScale,
                        GeoFrame& out) {
  const size_t n = src.size();
  if (n < 3 || dst.size() != n) return false;
  glm::dvec3 ca(0.0), cb(0.0);
  for (size_t i = 0; i < n; ++i) {
    ca += src[i];
    cb += dst[i];
  }
  ca /= static_cast<double>(n);
  cb /= static_cast<double>(n);
  // Cross-covariance S = sum (a_i - ca)(b_i - cb)^T, and spread of the source.
  double S[3][3] = {};
  double varA = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const glm::dvec3 a = src[i] - ca, b = dst[i] - cb;
    varA += glm::dot(a, a);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) S[r][c] += a[r] * b[c];
  }
  if (varA < 1e-18) return false;
  // Horn 1987: the rotation quaternion is the eigenvector of N with the largest eigenvalue.
  const double Sxx = S[0][0], Sxy = S[0][1], Sxz = S[0][2], Syx = S[1][0], Syy = S[1][1], Syz = S[1][2],
               Szx = S[2][0], Szy = S[2][1], Szz = S[2][2];
  double N[4][4] = {{Sxx + Syy + Szz, Syz - Szy, Szx - Sxz, Sxy - Syx},
                    {Syz - Szy, Sxx - Syy - Szz, Sxy + Syx, Szx + Sxz},
                    {Szx - Sxz, Sxy + Syx, -Sxx + Syy - Szz, Syz + Szy},
                    {Sxy - Syx, Szx + Sxz, Syz + Szy, -Sxx - Syy + Szz}};
  const glm::dvec4 q = largestEigenvector4(N);  // (w, x, y, z)
  const glm::dquat rot = glm::normalize(glm::dquat(q.x, q.y, q.z, q.w));
  const glm::dmat3 R = glm::mat3_cast(rot);
  // Rank check: collinear points leave the rotation about their line undetermined.
  glm::dvec3 values;
  glm::dmat3 vectors;
  glm::dmat3 cov(0.0);
  for (size_t i = 0; i < n; ++i) cov += glm::outerProduct(src[i] - ca, src[i] - ca);
  symmetricEigen3(cov, values, vectors);
  if (values[1] < 1e-12 * values[2]) return false;
  double s = 1.0;
  if (withScale) {
    double num = 0.0;
    for (size_t i = 0; i < n; ++i) num += glm::dot(dst[i] - cb, R * (src[i] - ca));
    s = num / varA;
    if (!(s > 0.0)) return false;
  }
  out.scale = s;
  out.rotation = R;
  out.translation = cb - s * (R * ca);
  return true;
}

RansacResult estimateSimilarityRansac(const std::vector<glm::dvec3>& src, const std::vector<glm::dvec3>& dst,
                                      bool withScale, double threshold, int iterations, uint32_t seed) {
  RansacResult best;
  const size_t n = src.size();
  if (n < 3 || dst.size() != n) return best;
  uint32_t state = seed ? seed : 1u;
  const double t2 = threshold * threshold;
  std::vector<uint32_t> inliers;
  for (int it = 0; it < iterations; ++it) {
    const uint32_t i0 = nextRandom(state) % n, i1 = nextRandom(state) % n, i2 = nextRandom(state) % n;
    if (i0 == i1 || i1 == i2 || i0 == i2) continue;
    GeoFrame f;
    if (!estimateSimilarity({src[i0], src[i1], src[i2]}, {dst[i0], dst[i1], dst[i2]}, withScale, f)) continue;
    inliers.clear();
    for (uint32_t i = 0; i < n; ++i) {
      const glm::dvec3 e = f.apply(src[i]) - dst[i];
      if (glm::dot(e, e) < t2) inliers.push_back(i);
    }
    if (inliers.size() > best.inliers.size()) {
      best.inliers = inliers;
      best.transform = f;
      best.ok = true;
    }
  }
  if (!best.ok || best.inliers.size() < 3) {
    best.ok = false;
    return best;
  }
  // Refit on all inliers, then recount once.
  std::vector<glm::dvec3> a, b;
  for (uint32_t i : best.inliers) {
    a.push_back(src[i]);
    b.push_back(dst[i]);
  }
  GeoFrame refined;
  if (estimateSimilarity(a, b, withScale, refined)) best.transform = refined;
  best.inliers.clear();
  double sum = 0.0;
  for (uint32_t i = 0; i < n; ++i) {
    const glm::dvec3 e = best.transform.apply(src[i]) - dst[i];
    if (glm::dot(e, e) < t2) {
      best.inliers.push_back(i);
      sum += glm::dot(e, e);
    }
  }
  best.rms = best.inliers.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(best.inliers.size()));
  return best;
}

IcpResult icpPointToPlane(const std::vector<glm::vec3>& source, const std::vector<glm::vec3>& target,
                          const std::vector<glm::vec3>& normals, const KdTree& tree, const GeoFrame& initial,
                          const IcpOptions& o) {
  IcpResult res;
  res.transform = initial;
  if (source.empty() || target.empty() || normals.size() != target.size()) return res;
  // Linearize around the centroid of the moved source: better conditioning of the rotation part.
  double maxDist = o.maxCorrespondenceDistance;
  for (int it = 0; it < o.maxIterations; ++it) {
    res.iterations = it + 1;
    double A[6][6] = {}, b[6] = {};
    glm::dvec3 centroid(0.0);
    std::vector<std::pair<glm::dvec3, uint32_t>> pairs;
    std::vector<float> pointDist;  // point-to-point distances of the accepted pairs
    pairs.reserve(source.size());
    pointDist.reserve(source.size());
    const float maxD2 = static_cast<float>(maxDist * maxDist);
    for (const glm::vec3& s : source) {
      const glm::dvec3 p = res.transform.apply(glm::dvec3(s));
      uint32_t j;
      float d2;
      if (!tree.nearest(glm::vec3(p), j, d2) || d2 > maxD2) continue;
      pairs.emplace_back(p, j);
      pointDist.push_back(std::sqrt(d2));
      centroid += p;
    }
    if (pairs.size() < 6) break;
    centroid /= static_cast<double>(pairs.size());
    double sumSq = 0.0;
    for (const auto& [p, j] : pairs) {
      const glm::dvec3 q(target[j]), n(normals[j]);
      const glm::dvec3 pc = p - centroid;
      const double r = glm::dot(p - q, n);  // signed point-to-plane residual
      sumSq += r * r;
      // d(residual)/d(omega, t) for p' = p + omega x pc + t
      const glm::dvec3 c = glm::cross(pc, n);
      const double J[6] = {c.x, c.y, c.z, n.x, n.y, n.z};
      for (int r0 = 0; r0 < 6; ++r0) {
        b[r0] -= J[r0] * r;
        for (int c0 = 0; c0 < 6; ++c0) A[r0][c0] += J[r0] * J[c0];
      }
    }
    res.pairs = pairs.size();
    res.rms = std::sqrt(sumSq / static_cast<double>(pairs.size()));
    double x[6];
    if (!solve6(A, b, x)) break;
    const glm::dvec3 omega(x[0], x[1], x[2]), t(x[3], x[4], x[5]);
    const double angle = glm::length(omega);
    GeoFrame delta;
    if (angle > 0.0) delta.rotation = glm::mat3_cast(glm::angleAxis(angle, omega / angle));
    // rotation about the centroid, then translation
    delta.translation = centroid - delta.rotation * centroid + t;
    res.transform = delta * res.transform;
    res.transform.orthonormalize();
    // Shrink the gate with the *point-to-point* distances: two scans never sample the same spots, so
    // even perfectly aligned pairs are ~one point spacing apart (the plane residual is not).
    if (o.shrinkDistance) {
      std::nth_element(pointDist.begin(), pointDist.begin() + static_cast<long>(pointDist.size() / 2), pointDist.end());
      maxDist = std::clamp(3.0 * pointDist[pointDist.size() / 2], 1e-4, o.maxCorrespondenceDistance);
    }
    if (angle < o.convergenceRotation && glm::length(t) < o.convergenceTranslation) {
      res.converged = true;
      break;
    }
  }
  return res;
}

bool fitPlane(const std::vector<glm::vec3>& points, const std::vector<uint32_t>& subset, Plane& out) {
  if (subset.size() < 3) return false;
  glm::dvec3 mean(0.0);
  for (uint32_t i : subset) mean += glm::dvec3(points[i]);
  mean /= static_cast<double>(subset.size());
  glm::dmat3 cov(0.0);
  for (uint32_t i : subset) {
    const glm::dvec3 d = glm::dvec3(points[i]) - mean;
    cov += glm::outerProduct(d, d);
  }
  glm::dvec3 values;
  glm::dmat3 vectors;
  symmetricEigen3(cov, values, vectors);
  if (values[1] <= 1e-15) return false;  // collinear
  out.normal = glm::normalize(vectors[0]);
  out.d = -glm::dot(out.normal, mean);
  return true;
}

PlaneFit fitPlaneRansac(const std::vector<glm::vec3>& points, double threshold, int iterations, uint32_t seed) {
  PlaneFit best;
  const size_t n = points.size();
  if (n < 3) return best;
  uint32_t state = seed ? seed : 1u;
  std::vector<uint32_t> inliers;
  for (int it = 0; it < iterations; ++it) {
    const glm::dvec3 a(points[nextRandom(state) % n]), b(points[nextRandom(state) % n]), c(points[nextRandom(state) % n]);
    const glm::dvec3 nrm = glm::cross(b - a, c - a);
    const double len = glm::length(nrm);
    if (len < 1e-12) continue;
    Plane pl;
    pl.normal = nrm / len;
    pl.d = -glm::dot(pl.normal, a);
    inliers.clear();
    for (uint32_t i = 0; i < n; ++i)
      if (std::fabs(pl.distance(glm::dvec3(points[i]))) < threshold) inliers.push_back(i);
    if (inliers.size() > best.inliers.size()) {
      best.inliers = inliers;
      best.plane = pl;
      best.ok = true;
    }
  }
  if (!best.ok) return best;
  Plane refined;
  if (fitPlane(points, best.inliers, refined)) {
    best.plane = refined;
    best.inliers.clear();
    for (uint32_t i = 0; i < n; ++i)
      if (std::fabs(refined.distance(glm::dvec3(points[i]))) < threshold) best.inliers.push_back(i);
  }
  return best;
}

}  // namespace gsv
