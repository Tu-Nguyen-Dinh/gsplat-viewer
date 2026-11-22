#include "scene/edit_ops.h"

#include "core/gaussian_math.h"
#include "core/image.h"
#include "core/kdtree.h"
#include "core/parallel.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace gsv {

// ------------------------------------------------------------------ selection
void combineSelection(SplatMask& mask, const SplatMask& next, SelectOp op) {
  if (mask.size() != next.size()) mask.assign(next.size(), 0);
  for (size_t i = 0; i < next.size(); ++i) {
    switch (op) {
      case SelectOp::Replace: mask[i] = next[i]; break;
      case SelectOp::Add: mask[i] = mask[i] | next[i]; break;
      case SelectOp::Subtract: mask[i] = mask[i] & static_cast<uint8_t>(!next[i]); break;
      case SelectOp::Intersect: mask[i] = mask[i] & next[i]; break;
    }
  }
}

SplatMask selectAll(const GaussianScene& scene) { return SplatMask(scene.size(), 1); }

glm::vec3 selectionCentroid(const GaussianScene& scene, const SplatMask& mask) {
  glm::dvec3 sum(0.0);
  size_t n = 0;
  for (size_t i = 0; i < scene.size() && i < mask.size(); ++i)
    if (mask[i]) {
      sum += glm::dvec3(scene.position[i]);
      ++n;
    }
  return n ? glm::vec3(sum / static_cast<double>(n)) : scene.focusCenter;
}

namespace {
// Projects every splat centre to top-left pixel coordinates; false behind the camera.
template <typename Inside>
SplatMask selectProjected(const GaussianScene& scene, const glm::mat4& mvp, glm::vec2 viewport, Inside&& inside) {
  SplatMask mask(scene.size(), 0);
  parallelFor(scene.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      const glm::vec4 c = mvp * glm::vec4(scene.position[i], 1.0f);
      if (c.w <= 1e-6f) continue;
      const glm::vec2 ndc(c.x / c.w, c.y / c.w);
      const glm::vec2 px((ndc.x * 0.5f + 0.5f) * viewport.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * viewport.y);
      if (inside(px)) mask[i] = 1;
    }
  });
  return mask;
}
}  // namespace

SplatMask selectScreenRect(const GaussianScene& scene, const glm::mat4& mvp, glm::vec2 viewport, glm::vec2 a,
                           glm::vec2 b) {
  const glm::vec2 lo = glm::min(a, b), hi = glm::max(a, b);
  return selectProjected(scene, mvp, viewport,
                         [&](glm::vec2 p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; });
}

bool pointInPolygon(glm::vec2 p, const std::vector<glm::vec2>& poly) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const glm::vec2 a = poly[i], b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
  }
  return in;
}

SplatMask selectScreenPolygon(const GaussianScene& scene, const glm::mat4& mvp, glm::vec2 viewport,
                              const std::vector<glm::vec2>& polygon) {
  if (polygon.size() < 3) return SplatMask(scene.size(), 0);
  glm::vec2 lo = polygon[0], hi = polygon[0];
  for (const glm::vec2& p : polygon) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  return selectProjected(scene, mvp, viewport, [&](glm::vec2 p) {
    return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && pointInPolygon(p, polygon);
  });
}

SplatMask selectScreenCircle(const GaussianScene& scene, const glm::mat4& mvp, glm::vec2 viewport, glm::vec2 center,
                             float r) {
  const float r2 = r * r;
  return selectProjected(scene, mvp, viewport, [&](glm::vec2 p) {
    const glm::vec2 d = p - center;
    return glm::dot(d, d) <= r2;
  });
}

SplatMask selectSphere(const GaussianScene& scene, const glm::mat4& model, const glm::vec3& center, float radius) {
  SelectionShape s;
  s.type = SelectionShape::Type::Sphere;
  s.center = center;
  s.radius = radius;
  return selectInside(scene, s, model);
}

uint32_t nearestSplat(const GaussianScene& scene, const glm::vec3& point, float minOpacity) {
  uint32_t best = static_cast<uint32_t>(scene.size());
  float bestD = 1e30f;
  const float minLogit = logit(std::clamp(minOpacity, 1e-4f, 0.9999f));
  for (size_t i = 0; i < scene.size(); ++i) {
    if (scene.opacityLogit[i] < minLogit) continue;
    const glm::vec3 d = scene.position[i] - point;
    const float dd = glm::dot(d, d);
    if (dd < bestD) {
      bestD = dd;
      best = static_cast<uint32_t>(i);
    }
  }
  return best;
}

SplatMask selectConnected(const GaussianScene& scene, uint32_t seed, float maxGap, float colorTolerance,
                          float minOpacity, size_t maxCount, const KdTree* tree) {
  SplatMask mask(scene.size(), 0);
  if (seed >= scene.size()) return mask;
  KdTree own;
  if (!tree) {
    own.build(scene.position);
    tree = &own;
  }
  const float minLogit = logit(std::clamp(minOpacity, 1e-4f, 0.9999f));
  std::vector<uint32_t> frontier{seed}, neighbours;  // breadth first: the mean settles near the seed
  size_t head = 0;
  mask[seed] = 1;
  size_t count = 1;
  glm::dvec3 colorSum(baseColor(scene, seed));
  size_t colorN = 1;
  while (head < frontier.size() && count < maxCount) {
    const uint32_t i = frontier[head++];
    tree->radius(scene.position[i], maxGap, neighbours);
    const glm::vec3 mean = glm::vec3(colorSum / static_cast<double>(colorN));
    for (uint32_t j : neighbours) {
      if (mask[j]) continue;
      const glm::vec3 cj = baseColor(scene, j);
      if (glm::length(cj - mean) > colorTolerance) continue;
      mask[j] = 1;
      ++count;
      if (scene.opacityLogit[j] >= minLogit) {  // only visible splats carry the region on
        frontier.push_back(j);
        colorSum += glm::dvec3(cj);
        ++colorN;
      }
      if (count >= maxCount) break;
    }
  }
  return mask;
}

SplatMask selectConnectedWithin(const GaussianScene& scene, uint32_t seed, float maxGap, float colorTolerance,
                                float maxRadius, const KdTree* tree) {
  if (maxRadius <= 0.0f || seed >= scene.size()) return selectConnected(scene, seed, maxGap, colorTolerance, 0.1f, 5000000, tree);
  // Same growth, but only through splats inside the ball (others are treated as absent).
  GaussianScene ball;
  ball.shDegree = scene.shDegree;
  ball.shRestPerChannel = scene.shRestPerChannel;
  std::vector<uint32_t> ids;
  const float r2 = maxRadius * maxRadius;
  const glm::vec3 c = scene.position[seed];
  uint32_t localSeed = 0;
  for (uint32_t i = 0; i < scene.size(); ++i) {
    const glm::vec3 d = scene.position[i] - c;
    if (glm::dot(d, d) > r2) continue;
    if (i == seed) localSeed = static_cast<uint32_t>(ids.size());
    ids.push_back(i);
    ball.position.push_back(scene.position[i]);
    ball.opacityLogit.push_back(scene.opacityLogit[i]);
    ball.shDC.push_back(scene.shDC[i]);
  }
  ball.logScale.resize(ids.size());
  ball.rotation.resize(ids.size());
  ball.shRestPerChannel = 0;  // colours are compared on the base colour only
  const SplatMask local = selectConnected(ball, localSeed, maxGap, colorTolerance, 0.1f, 5000000, nullptr);
  SplatMask out(scene.size(), 0);
  for (size_t k = 0; k < ids.size(); ++k) out[ids[k]] = local[k];
  return out;
}

void growSelection(const GaussianScene& scene, const KdTree& tree, SplatMask& mask, float gap, float colorTolerance) {
  if (mask.size() != scene.size()) return;
  glm::dvec3 sum(0.0);
  size_t n = 0;
  for (size_t i = 0; i < scene.size(); ++i)
    if (mask[i]) sum += glm::dvec3(baseColor(scene, i)), ++n;
  if (n == 0) return;
  const glm::vec3 mean = glm::vec3(sum / static_cast<double>(n));
  SplatMask add(scene.size(), 0);
  std::vector<uint32_t> nb;
  for (size_t i = 0; i < scene.size(); ++i) {
    if (!mask[i]) continue;
    tree.radius(scene.position[i], gap, nb);
    for (uint32_t j : nb)
      if (!mask[j] && (colorTolerance >= 1.0f || glm::length(baseColor(scene, j) - mean) <= colorTolerance)) add[j] = 1;
  }
  for (size_t i = 0; i < scene.size(); ++i) mask[i] |= add[i];
}

void shrinkSelection(const GaussianScene& scene, const KdTree& tree, SplatMask& mask, float gap) {
  if (mask.size() != scene.size()) return;
  const float minLogit = logit(0.1f);
  SplatMask remove(scene.size(), 0);
  std::vector<uint32_t> nb;
  for (size_t i = 0; i < scene.size(); ++i) {
    if (!mask[i]) continue;
    tree.radius(scene.position[i], gap, nb);
    for (uint32_t j : nb)
      if (!mask[j] && scene.opacityLogit[j] >= minLogit) {
        remove[i] = 1;
        break;
      }
  }
  for (size_t i = 0; i < scene.size(); ++i)
    if (remove[i]) mask[i] = 0;
}

void keepVisible(const GaussianScene& scene, const glm::mat4& viewModel, const glm::mat4& projection, const DepthImage& depth,
                 SplatMask& mask, float relTolerance) {
  if (depth.empty() || mask.size() != scene.size()) return;
  const int w = depth.width, h = depth.height;
  parallelFor(scene.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      if (!mask[i]) continue;
      const glm::vec4 v = viewModel * glm::vec4(scene.position[i], 1.0f);
      const float z = -v.z;
      const glm::vec4 c = projection * v;
      if (c.w <= 1e-6f) {
        mask[i] = 0;
        continue;
      }
      const int px = static_cast<int>((c.x / c.w * 0.5f + 0.5f) * static_cast<float>(w));
      const int py = static_cast<int>((0.5f - c.y / c.w * 0.5f) * static_cast<float>(h));
      if (px < 0 || py < 0 || px >= w || py >= h) {
        mask[i] = 0;
        continue;
      }
      float farthest = 0.0f;
      bool empty = false;
      for (int dy = -1; dy <= 1 && !empty; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          const int x = std::clamp(px + dx, 0, w - 1), y = std::clamp(py + dy, 0, h - 1);
          if (depth.coverageAt(x, y) < 0.5f) {
            empty = true;  // see-through next to it: the centre may well be visible
            break;
          }
          farthest = std::max(farthest, depth.depthAt(x, y));
        }
      if (!empty && z > farthest * (1.0f + relTolerance)) mask[i] = 0;
    }
  });
}

float typicalSpacing(const GaussianScene& scene, const KdTree& tree, int samples) {
  if (scene.size() < 2) return 0.0f;
  std::vector<float> d;
  std::vector<uint32_t> idx;
  std::vector<float> d2;
  const size_t step = std::max<size_t>(1, scene.size() / static_cast<size_t>(std::max(samples, 1)));
  const float minLogit = logit(0.1f);
  for (size_t i = 0; i < scene.size(); i += step) {
    if (scene.opacityLogit[i] < minLogit) continue;
    tree.knn(scene.position[i], 2, idx, d2);
    if (d2.size() == 2) d.push_back(std::sqrt(d2[1]));
  }
  if (d.empty()) return 0.0f;
  std::nth_element(d.begin(), d.begin() + static_cast<long>(d.size() / 2), d.end());
  return d[d.size() / 2];
}

SplatMask selectOutliers(const GaussianScene& scene, int k, float stdRatio) {
  SplatMask mask(scene.size(), 0);
  if (scene.size() < static_cast<size_t>(k) + 2) return mask;
  KdTree tree;
  tree.build(scene.position);
  std::vector<float> meanDist(scene.size());
  parallelFor(scene.size(), [&](size_t begin, size_t end) {
    std::vector<uint32_t> idx;
    std::vector<float> d2;
    for (size_t i = begin; i < end; ++i) {
      tree.knn(scene.position[i], k + 1, idx, d2);  // includes the point itself
      double s = 0.0;
      for (size_t j = 1; j < d2.size(); ++j) s += std::sqrt(d2[j]);
      meanDist[i] = static_cast<float>(s / std::max<size_t>(1, d2.size() - 1));
    }
  });
  double mean = 0.0, sq = 0.0;
  for (float d : meanDist) mean += d;
  mean /= static_cast<double>(meanDist.size());
  for (float d : meanDist) sq += (d - mean) * (d - mean);
  const double sigma = std::sqrt(sq / static_cast<double>(meanDist.size()));
  const double threshold = mean + stdRatio * sigma;
  for (size_t i = 0; i < scene.size(); ++i) mask[i] = meanDist[i] > threshold;
  return mask;
}

SplatMask selectUnseen(const GaussianScene& scene, const std::vector<CameraView>& cams, int minViews) {
  SplatMask mask(scene.size(), 0);
  if (cams.empty()) return mask;
  parallelFor(scene.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      int views = 0;
      for (const CameraView& c : cams) {
        const glm::vec3 t = glm::vec3(c.view * glm::vec4(scene.position[i], 1.0f));
        const float z = -t.z;
        if (z <= 1e-4f || z > c.maxDistance) continue;
        // Image pixels: x right, y DOWN (camera space y is up).
        const glm::vec2 px(c.principal.x + c.focal.x * t.x / z, c.principal.y - c.focal.y * t.y / z);
        if (px.x < 0 || px.y < 0 || px.x >= c.size.x || px.y >= c.size.y) continue;
        if (++views >= minViews) break;
      }
      mask[i] = views < minViews;
    }
  });
  return mask;
}

// ------------------------------------------------------------------ SH rotation
namespace {
constexpr int kBandStart[4] = {0, 0, 3, 8};  // offset of band l in the rest coefficients
constexpr int kBandSize[4] = {1, 3, 5, 7};

// Real SH basis values Y_k(dir), k = 1..15, with the same constants and signs as
// evalSphericalHarmonics (which adds 0.5 and clamps, so it cannot be used to sample the basis).
std::array<double, 15> basis(const glm::vec3& d) {
  const double x = d.x, y = d.y, z = d.z, xx = x * x, yy = y * y, zz = z * z;
  return {-kShC1 * y,
          kShC1 * z,
          -kShC1 * x,
          kShC2[0] * x * y,
          kShC2[1] * y * z,
          kShC2[2] * (2 * zz - xx - yy),
          kShC2[3] * x * z,
          kShC2[4] * (xx - yy),
          kShC3[0] * y * (3 * xx - yy),
          kShC3[1] * x * y * z,
          kShC3[2] * y * (4 * zz - xx - yy),
          kShC3[3] * z * (2 * zz - 3 * xx - 3 * yy),
          kShC3[4] * x * (4 * zz - xx - yy),
          kShC3[5] * z * (xx - yy),
          kShC3[6] * x * (xx - 3 * yy)};
}

// Solves (A^T A) X = A^T B for a band (n x n), Gaussian elimination with partial pivoting.
template <size_t N2>
void solveBand(int band, const std::vector<std::array<double, 15>>& ya, const std::vector<std::array<double, 15>>& yb,
               std::array<double, N2>& out) {
  const int n = kBandSize[band], o = kBandStart[band];
  std::vector<double> ata(static_cast<size_t>(n * n), 0.0), atb(static_cast<size_t>(n * n), 0.0);
  for (size_t s = 0; s < ya.size(); ++s)
    for (int r = 0; r < n; ++r)
      for (int c = 0; c < n; ++c) {
        ata[static_cast<size_t>(r * n + c)] += ya[s][o + r] * ya[s][o + c];
        atb[static_cast<size_t>(r * n + c)] += ya[s][o + r] * yb[s][o + c];
      }
  // Gauss-Jordan on [ata | atb]
  for (int c = 0; c < n; ++c) {
    int piv = c;
    for (int r = c + 1; r < n; ++r)
      if (std::fabs(ata[static_cast<size_t>(r * n + c)]) > std::fabs(ata[static_cast<size_t>(piv * n + c)])) piv = r;
    for (int k = 0; k < n; ++k) {
      std::swap(ata[static_cast<size_t>(c * n + k)], ata[static_cast<size_t>(piv * n + k)]);
      std::swap(atb[static_cast<size_t>(c * n + k)], atb[static_cast<size_t>(piv * n + k)]);
    }
    const double p = ata[static_cast<size_t>(c * n + c)];
    for (int k = 0; k < n; ++k) {
      ata[static_cast<size_t>(c * n + k)] /= p;
      atb[static_cast<size_t>(c * n + k)] /= p;
    }
    for (int r = 0; r < n; ++r) {
      if (r == c) continue;
      const double f = ata[static_cast<size_t>(r * n + c)];
      for (int k = 0; k < n; ++k) {
        ata[static_cast<size_t>(r * n + k)] -= f * ata[static_cast<size_t>(c * n + k)];
        atb[static_cast<size_t>(r * n + k)] -= f * atb[static_cast<size_t>(c * n + k)];
      }
    }
  }
  // X maps old coefficients to new ones: c_new = X^T-wise -> store M with c'_r = sum_c M[r][c] c_c.
  // Least squares gave X (n x n) such that Ya * X ~= Yb, i.e. Ya * (X c) = Yb * c: M = X.
  for (int r = 0; r < n; ++r)
    for (int c = 0; c < n; ++c) out[static_cast<size_t>(r * n + c)] = atb[static_cast<size_t>(r * n + c)];
}
}  // namespace

ShRotation shRotation(const glm::mat3& R) {
  // New colour at direction d must equal the old colour at R^T d:
  //   sum_k c'_k Y_k(d) = sum_k c_k Y_k(R^T d)  for all d   =>  Ya c' = Yb c  (least squares, exact).
  constexpr int kSamples = 64;
  std::vector<std::array<double, 15>> ya, yb;
  const glm::mat3 Rt = glm::transpose(R);
  const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
  for (int s = 0; s < kSamples; ++s) {
    const double z = 1.0 - (s + 0.5) * 2.0 / kSamples, r = std::sqrt(1.0 - z * z), phi = golden * s;
    const glm::vec3 d(static_cast<float>(r * std::cos(phi)), static_cast<float>(r * std::sin(phi)), static_cast<float>(z));
    ya.push_back(basis(d));
    yb.push_back(basis(glm::normalize(Rt * d)));
  }
  ShRotation out;
  solveBand(1, ya, yb, out.band1);
  solveBand(2, ya, yb, out.band2);
  solveBand(3, ya, yb, out.band3);
  return out;
}

void rotateShRest(float* rest, int restPerChannel, const ShRotation& r) {
  const double* m[4] = {nullptr, r.band1.data(), r.band2.data(), r.band3.data()};
  for (int ch = 0; ch < 3; ++ch) {
    float* c = rest + ch * restPerChannel;
    for (int band = 1; band <= 3; ++band) {
      const int n = kBandSize[band], o = kBandStart[band];
      if (o + n > restPerChannel) break;
      double in[7], outv[7];
      for (int k = 0; k < n; ++k) in[k] = c[o + k];
      // Ya * c' = Yb * c with c' = X c where X solves (Ya^T Ya) X = Ya^T Yb: X[r][k] stored row-major.
      for (int rr = 0; rr < n; ++rr) {
        double s = 0.0;
        for (int k = 0; k < n; ++k) s += m[band][rr * n + k] * in[k];
        outv[rr] = s;
      }
      for (int k = 0; k < n; ++k) c[o + k] = static_cast<float>(outv[k]);
    }
  }
}

// ------------------------------------------------------------------ modifications
void transformSplats(GaussianScene& scene, const SplatMask& mask, const glm::mat4& affine) {
  const glm::mat3 m3(affine);
  const float det = glm::determinant(m3);
  const float s = std::cbrt(std::max(det, 1e-30f));
  const glm::mat3 R = m3 / s;
  const glm::quat qr = glm::normalize(glm::quat_cast(R));
  const float logS = std::log(s);
  float offIdentity = 0.0f;
  for (int c = 0; c < 3; ++c)
    for (int r = 0; r < 3; ++r) offIdentity = std::max(offIdentity, std::fabs(R[c][r] - (c == r ? 1.0f : 0.0f)));
  const bool rotates = offIdentity > 1e-7f;
  ShRotation shr;
  if (rotates && scene.shRestPerChannel > 0) shr = shRotation(R);
  parallelFor(scene.size(), [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i) {
      if (i >= mask.size() || !mask[i]) continue;
      scene.position[i] = glm::vec3(affine * glm::vec4(scene.position[i], 1.0f));
      const glm::vec4& w = scene.rotation[i];  // (w, x, y, z)
      const glm::quat q = qr * glm::normalize(glm::quat(w.x, w.y, w.z, w.w));
      scene.rotation[i] = glm::vec4(q.w, q.x, q.y, q.z);
      scene.logScale[i] += glm::vec3(logS);
      if (rotates && scene.shRestPerChannel > 0)
        rotateShRest(scene.shRest.data() + i * static_cast<size_t>(scene.shRestPerChannel) * 3, scene.shRestPerChannel, shr);
    }
  });
}

void adjustColor(GaussianScene& scene, const SplatMask& mask, const ColorAdjust& a) {
  if (a.isNeutral()) return;
  const size_t restN = static_cast<size_t>(scene.shRestPerChannel) * 3;
  // View-dependent part: contrast and saturation scale its amplitude (grey mix done per channel below).
  for (size_t i = 0; i < scene.size(); ++i) {
    if (i >= mask.size() || !mask[i]) continue;
    glm::vec3 c = glm::vec3(0.5f) + kShC0 * scene.shDC[i];
    c = (c - glm::vec3(0.5f)) * a.contrast + glm::vec3(0.5f) + glm::vec3(a.brightness);
    const float luma = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    c = glm::vec3(luma) + (c - glm::vec3(luma)) * a.saturation;
    c.r += a.temperature;
    c.b -= a.temperature;
    scene.shDC[i] = (c - glm::vec3(0.5f)) / kShC0;
    if (restN) {
      float* r = scene.shRest.data() + i * restN;
      const size_t n = static_cast<size_t>(scene.shRestPerChannel);
      for (size_t k = 0; k < n; ++k) {
        glm::vec3 v(r[k], r[n + k], r[2 * n + k]);
        v *= a.contrast;
        const float l = 0.2126f * v.r + 0.7152f * v.g + 0.0722f * v.b;
        v = glm::vec3(l) + (v - glm::vec3(l)) * a.saturation;
        r[k] = v.r;
        r[n + k] = v.g;
        r[2 * n + k] = v.b;
      }
    }
  }
}

double scaleForKnownLength(const glm::dvec3& a, const glm::dvec3& b, double realLength) {
  const double d = glm::length(b - a);
  return d > 0.0 ? realLength / d : 1.0;
}

glm::mat4 rotationBetween(const glm::vec3& from, const glm::vec3& to) {
  const glm::vec3 f = glm::normalize(from), t = glm::normalize(to);
  const float c = glm::dot(f, t);
  if (c > 1.0f - 1e-7f) return glm::mat4(1.0f);
  if (c < -1.0f + 1e-7f) {  // opposite: any perpendicular axis
    glm::vec3 axis = glm::cross(f, glm::vec3(1, 0, 0));
    if (glm::length(axis) < 1e-3f) axis = glm::cross(f, glm::vec3(0, 1, 0));
    return glm::mat4_cast(glm::angleAxis(3.14159265f, glm::normalize(axis)));
  }
  return glm::mat4_cast(glm::rotation(f, t));
}

// ------------------------------------------------------------------ measurements
double polylineLength(const std::vector<glm::dvec3>& p) {
  double s = 0.0;
  for (size_t i = 1; i < p.size(); ++i) s += glm::length(p[i] - p[i - 1]);
  return s;
}

double polygonArea(const std::vector<glm::dvec3>& p) {
  if (p.size() < 3) return 0.0;
  glm::dvec3 n(0.0);
  for (size_t i = 0; i < p.size(); ++i) {
    // Relative to the first vertex: georeferenced coordinates (1e6) would cancel catastrophically.
    const glm::dvec3 a = p[i] - p[0];
    const glm::dvec3 b = p[(i + 1) % p.size()] - p[0];
    n += glm::cross(a, b);
  }
  return 0.5 * glm::length(n);
}

}  // namespace gsv
