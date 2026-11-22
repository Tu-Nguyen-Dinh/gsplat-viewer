#include "scene/lod.h"

#include "core/kdtree.h"
#include "core/utf8_path.h"
#include "scene/splat_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <queue>

#include <glm/gtc/quaternion.hpp>

namespace gsv {

namespace {

constexpr float kSigmaExtent = 3.0f;

// Copies splat i of `src` to the end of `dst`. `src` and `dst` may be the same scene (inner nodes
// copy their children's splats), so every value is read into a local before the vectors grow.
void appendSplat(const GaussianScene& src, size_t i, GaussianScene& dst) {
  const glm::vec3 position = src.position[i], logScale = src.logScale[i], dc = src.shDC[i];
  const glm::vec4 rotation = src.rotation[i];
  const float opacity = src.opacityLogit[i];
  const size_t rest = static_cast<size_t>(src.shRestPerChannel) * 3;
  float restCopy[45];
  for (size_t k = 0; k < rest; ++k) restCopy[k] = src.shRest[i * rest + k];
  dst.position.push_back(position);
  dst.logScale.push_back(logScale);
  dst.rotation.push_back(rotation);
  dst.opacityLogit.push_back(opacity);
  dst.shDC.push_back(dc);
  dst.shRest.insert(dst.shRest.end(), restCopy, restCopy + rest);
}

float splatExtent(const GaussianScene& s, size_t i) {
  const glm::vec3 sc = glm::exp(s.logScale[i]);
  return kSigmaExtent * std::max({sc.x, sc.y, sc.z});
}

// Bounding sphere (approximate: centre of the box, max distance + extent) of splats [b, e).
void boundRange(const GaussianScene& s, uint32_t b, uint32_t e, glm::vec3& center, float& radius) {
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (uint32_t i = b; i < e; ++i) {
    lo = glm::min(lo, s.position[i]);
    hi = glm::max(hi, s.position[i]);
  }
  center = 0.5f * (lo + hi);
  radius = 0.0f;
  for (uint32_t i = b; i < e; ++i) radius = std::max(radius, glm::length(s.position[i] - center) + splatExtent(s, i));
}

struct Builder {
  const LodBuildOptions& opt;
  LodTree& tree;
  std::vector<uint64_t> codes;  // Morton codes of the (sorted) leaf splats

  // Fills node `slot` with the subtree of leaf-splat range [b, e) whose Morton codes agree above bit
  // `shift`. Children of a node are allocated as one consecutive block before recursing into them.
  void build(uint32_t b, uint32_t e, int shift, uint32_t level, uint32_t slot) {
    tree.depth = std::max(tree.depth, static_cast<int>(level) + 1);
    std::vector<std::pair<uint32_t, uint32_t>> parts;
    while (e - b > opt.leafCapacity && shift >= 0) {
      parts.clear();
      uint32_t start = b;
      while (start < e) {
        const uint64_t key = codes[start] >> shift;
        uint32_t end = start + 1;
        while (end < e && (codes[end] >> shift) == key) ++end;
        parts.emplace_back(start, end);
        start = end;
      }
      if (parts.size() > 1) break;
      parts.clear();
      shift -= 3;  // everything in one octant: look at the next 3 bits without adding a level
    }
    if (parts.empty()) {  // leaf: the original splats themselves
      LodNode n;
      n.splatBegin = b;
      n.splatCount = e - b;
      n.level = level;
      boundRange(tree.splats, b, e, n.center, n.radius);
      tree.nodes[slot] = n;
      return;
    }
    const uint32_t first = static_cast<uint32_t>(tree.nodes.size());
    tree.nodes.resize(tree.nodes.size() + parts.size());
    for (size_t c = 0; c < parts.size(); ++c)
      build(parts[c].first, parts[c].second, shift - 3, level + 1, first + static_cast<uint32_t>(c));
    LodNode n;
    n.firstChild = first;
    n.childCount = static_cast<uint32_t>(parts.size());
    n.level = level;
    // Proxies: concatenate the children's splats, merge Morton-contiguous groups.
    std::vector<uint32_t> pool;
    for (uint32_t c = 0; c < n.childCount; ++c) {
      const LodNode& child = tree.nodes[first + c];
      for (uint32_t k = 0; k < child.splatCount; ++k) pool.push_back(child.splatBegin + k);
    }
    n.splatBegin = static_cast<uint32_t>(tree.splats.size());
    if (pool.size() <= opt.nodeCapacity) {
      for (uint32_t i : pool) appendSplat(tree.splats, i, tree.splats);
    } else {
      const size_t group = (pool.size() + opt.nodeCapacity - 1) / opt.nodeCapacity;
      std::vector<uint32_t> members;
      for (size_t g = 0; g < pool.size(); g += group) {
        members.assign(pool.begin() + static_cast<long>(g), pool.begin() + static_cast<long>(std::min(pool.size(), g + group)));
        mergeSplats(tree.splats, members, tree.splats);
      }
    }
    n.splatCount = static_cast<uint32_t>(tree.splats.size()) - n.splatBegin;
    // Sphere enclosing the children's spheres.
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (uint32_t c = 0; c < n.childCount; ++c) {
      const LodNode& ch = tree.nodes[first + c];
      lo = glm::min(lo, ch.center - ch.radius);
      hi = glm::max(hi, ch.center + ch.radius);
    }
    n.center = 0.5f * (lo + hi);
    for (uint32_t c = 0; c < n.childCount; ++c) {
      const LodNode& ch = tree.nodes[first + c];
      n.radius = std::max(n.radius, glm::length(ch.center - n.center) + ch.radius);
    }
    tree.nodes[slot] = n;
  }
};

uint32_t nextPow2Shift(int bitsPerAxis) { return static_cast<uint32_t>(3 * bitsPerAxis - 3); }

}  // namespace

glm::dmat3 splatCovariance(const GaussianScene& s, size_t i) {
  return glm::dmat3(covarianceFromScaleRotation(glm::exp(s.logScale[i]), s.rotation[i]).toMat3());
}

void mergeSplats(const GaussianScene& s, const std::vector<uint32_t>& idx, GaussianScene& out) {
  double W = 0.0;
  glm::dvec3 mean(0.0);
  std::vector<double> w(idx.size());
  std::vector<glm::dmat3> cov(idx.size());
  double alphaArea = 0.0;
  for (size_t k = 0; k < idx.size(); ++k) {
    cov[k] = splatCovariance(s, idx[k]);
    const double area = std::cbrt(std::max(glm::determinant(cov[k]), 1e-30));  // ~ sigma^2
    const double alpha = sigmoid(s.opacityLogit[idx[k]]);
    w[k] = std::max(alpha * area, 1e-30);
    alphaArea += alpha * area;
    W += w[k];
    mean += w[k] * glm::dvec3(s.position[idx[k]]);
  }
  mean /= W;
  glm::dmat3 C(0.0);
  for (size_t k = 0; k < idx.size(); ++k) {
    const glm::dvec3 d = glm::dvec3(s.position[idx[k]]) - mean;
    C += w[k] * (cov[k] + glm::outerProduct(d, d));
  }
  C /= W;
  glm::dvec3 values;
  glm::dmat3 vectors;
  symmetricEigen3(C, values, vectors);
  if (glm::determinant(vectors) < 0.0) vectors[0] = -vectors[0];
  const glm::dquat q = glm::normalize(glm::quat_cast(vectors));
  const glm::vec3 scale(std::sqrt(std::max(values[0], 1e-20)), std::sqrt(std::max(values[1], 1e-20)),
                        std::sqrt(std::max(values[2], 1e-20)));
  // Opacity: same "coverage" (alpha x area) spread over the merged area, capped below 1.
  const double mergedArea = std::cbrt(std::max(glm::determinant(C), 1e-30));
  const float alpha = static_cast<float>(std::clamp(alphaArea / mergedArea, 1.0 / 255.0, 0.99));
  out.position.push_back(glm::vec3(mean));
  out.logScale.push_back(glm::log(scale));
  out.rotation.push_back(glm::vec4(static_cast<float>(q.w), static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z)));
  out.opacityLogit.push_back(logit(alpha));
  glm::dvec3 dc(0.0);
  const size_t rest = static_cast<size_t>(s.shRestPerChannel) * 3;
  std::vector<double> restAcc(rest, 0.0);
  for (size_t k = 0; k < idx.size(); ++k) {
    dc += w[k] * glm::dvec3(s.shDC[idx[k]]);
    const float* r = s.restOf(idx[k]);
    for (size_t j = 0; j < rest; ++j) restAcc[j] += w[k] * r[j];
  }
  out.shDC.push_back(glm::vec3(dc / W));
  for (size_t j = 0; j < rest; ++j) out.shRest.push_back(static_cast<float>(restAcc[j] / W));
}

LodTree buildLod(const GaussianScene& scene, const LodBuildOptions& options) {
  LodTree tree;
  if (scene.empty()) return tree;
  // 1. Morton order of the original splats.
  const std::vector<uint32_t> order = mortonOrder(scene.position);
  GaussianScene& S = tree.splats;
  S.shDegree = scene.shDegree;
  S.shRestPerChannel = scene.shRestPerChannel;
  S.sourceName = scene.sourceName;
  for (uint32_t i : order) appendSplat(scene, i, S);
  tree.leafSplatCount = static_cast<uint32_t>(scene.size());
  // Morton codes again for the sorted positions (same quantization as mortonOrder: 21 bits / axis).
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (const glm::vec3& p : S.position) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  const glm::vec3 inv = 2097151.0f / glm::max(hi - lo, glm::vec3(1e-12f));
  auto spread = [](uint64_t v) {
    v &= 0x1FFFFF;
    v = (v | v << 32) & 0x1F00000000FFFFull;
    v = (v | v << 16) & 0x1F0000FF0000FFull;
    v = (v | v << 8) & 0x100F00F00F00F00Full;
    v = (v | v << 4) & 0x10C30C30C30C30C3ull;
    v = (v | v << 2) & 0x1249249249249249ull;
    return v;
  };
  Builder b{options, tree, {}};
  b.codes.resize(S.size());
  for (size_t i = 0; i < S.size(); ++i) {
    const glm::uvec3 q = glm::uvec3(glm::clamp((S.position[i] - lo) * inv, glm::vec3(0.0f), glm::vec3(2097151.0f)));
    b.codes[i] = spread(q.x) | spread(q.y) << 1 | spread(q.z) << 2;
  }
  // mortonOrder may quantize differently: make the codes non-decreasing in our order.
  std::vector<uint32_t> perm(S.size());
  for (uint32_t i = 0; i < perm.size(); ++i) perm[i] = i;
  std::stable_sort(perm.begin(), perm.end(), [&](uint32_t x, uint32_t y) { return b.codes[x] < b.codes[y]; });
  bool sorted = true;
  for (size_t i = 0; i < perm.size(); ++i) sorted = sorted && perm[i] == i;
  if (!sorted) {
    GaussianScene R;
    R.shDegree = S.shDegree;
    R.shRestPerChannel = S.shRestPerChannel;
    R.sourceName = S.sourceName;
    std::vector<uint64_t> codes(perm.size());
    for (size_t i = 0; i < perm.size(); ++i) {
      appendSplat(S, perm[i], R);
      codes[i] = b.codes[perm[i]];
    }
    S = std::move(R);
    b.codes = std::move(codes);
  }
  tree.nodes.resize(1);
  b.build(0, static_cast<uint32_t>(S.size()), static_cast<int>(nextPow2Shift(21)), 0, 0);
  S.boundsMin = scene.boundsMin;
  S.boundsMax = scene.boundsMax;
  S.focusCenter = scene.focusCenter;
  S.focusRadius = scene.focusRadius;
  return tree;
}

LodCut selectCut(const LodTree& tree, const LodCutParams& p) {
  LodCut cut;
  if (tree.empty()) return cut;
  auto projected = [&](const LodNode& n) {
    const float d = glm::length(n.center - p.cameraPosition) - n.radius;
    if (d <= 1e-6f) return 1e30f;  // camera inside the node: always worth refining
    return n.radius * p.focalPixels / d;
  };
  // Max-heap on projected size: refine the coarsest-looking node first.
  std::priority_queue<std::pair<float, uint32_t>> open;
  open.emplace(projected(tree.nodes[0]), 0u);
  uint64_t count = tree.nodes[0].splatCount;
  std::vector<uint32_t> done;
  while (!open.empty()) {
    const auto [size, id] = open.top();
    open.pop();
    const LodNode& n = tree.nodes[id];
    if (n.childCount == 0 || size <= p.pixelError) {
      done.push_back(id);
      continue;
    }
    uint64_t childSplats = 0;
    for (uint32_t c = 0; c < n.childCount; ++c) childSplats += tree.nodes[n.firstChild + c].splatCount;
    if (count - n.splatCount + childSplats > p.splatBudget) {
      cut.budgetLimited = true;
      done.push_back(id);
      continue;
    }
    count = count - n.splatCount + childSplats;
    for (uint32_t c = 0; c < n.childCount; ++c) open.emplace(projected(tree.nodes[n.firstChild + c]), n.firstChild + c);
  }
  cut.nodes = std::move(done);
  std::sort(cut.nodes.begin(), cut.nodes.end());
  cut.splatCount = count;
  return cut;
}

GaussianScene gatherCut(const LodTree& tree, const LodCut& cut) {
  GaussianScene out;
  out.shDegree = tree.splats.shDegree;
  out.shRestPerChannel = tree.splats.shRestPerChannel;
  out.sourceName = tree.splats.sourceName;
  const size_t n = cut.splatCount;
  out.position.reserve(n);
  out.logScale.reserve(n);
  out.rotation.reserve(n);
  out.opacityLogit.reserve(n);
  out.shDC.reserve(n);
  out.shRest.reserve(n * static_cast<size_t>(out.shRestPerChannel) * 3);
  for (uint32_t id : cut.nodes) {
    const LodNode& node = tree.nodes[id];
    for (uint32_t k = 0; k < node.splatCount; ++k) appendSplat(tree.splats, node.splatBegin + k, out);
  }
  out.boundsMin = tree.splats.boundsMin;
  out.boundsMax = tree.splats.boundsMax;
  out.focusCenter = tree.splats.focusCenter;
  out.focusRadius = tree.splats.focusRadius;
  return out;
}

// ---------------------------------------------------------------------------- file format
namespace {
constexpr char kMagic[8] = {'G', 'S', 'V', 'L', 'O', 'D', '1', '\0'};

template <typename T>
void put(std::ofstream& f, const std::vector<T>& v) {
  const uint64_t n = v.size();
  f.write(reinterpret_cast<const char*>(&n), sizeof(n));
  if (n) f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(n * sizeof(T)));
}
template <typename T>
bool get(std::ifstream& f, std::vector<T>& v, uint64_t maxCount) {
  uint64_t n = 0;
  if (!f.read(reinterpret_cast<char*>(&n), sizeof(n)) || n > maxCount) return false;
  v.resize(static_cast<size_t>(n));
  return n == 0 || static_cast<bool>(f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * sizeof(T))));
}
}  // namespace

bool saveLod(const std::string& path, const LodTree& t, std::string& error) {
  std::ofstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) {
    error = "cannot write " + path;
    return false;
  }
  f.write(kMagic, sizeof(kMagic));
  const int32_t header[4] = {t.splats.shDegree, t.splats.shRestPerChannel, t.depth, static_cast<int32_t>(t.leafSplatCount)};
  f.write(reinterpret_cast<const char*>(header), sizeof(header));
  const glm::vec3 b[3] = {t.splats.boundsMin, t.splats.boundsMax, t.splats.focusCenter};
  f.write(reinterpret_cast<const char*>(b), sizeof(b));
  f.write(reinterpret_cast<const char*>(&t.splats.focusRadius), sizeof(float));
  put(f, t.nodes);
  put(f, t.splats.position);
  put(f, t.splats.logScale);
  put(f, t.splats.rotation);
  put(f, t.splats.opacityLogit);
  put(f, t.splats.shDC);
  put(f, t.splats.shRest);
  if (!f) {
    error = "write error on " + path;
    return false;
  }
  return true;
}

bool loadLod(const std::string& path, LodTree& t, std::string& error) {
  t = LodTree{};
  std::ifstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  f.seekg(0, std::ios::end);
  const uint64_t size = static_cast<uint64_t>(f.tellg());
  f.seekg(0);
  char magic[8] = {};
  int32_t header[4] = {};
  glm::vec3 b[3];
  float focusRadius = 0.0f;
  if (!f.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0) {
    error = "not a .gsvlod file";
    return false;
  }
  if (!f.read(reinterpret_cast<char*>(header), sizeof(header)) || !f.read(reinterpret_cast<char*>(b), sizeof(b)) ||
      !f.read(reinterpret_cast<char*>(&focusRadius), sizeof(float))) {
    error = "truncated .gsvlod header";
    return false;
  }
  if (header[0] < 0 || header[0] > 3 || header[1] != (header[0] + 1) * (header[0] + 1) - 1 || header[3] < 0) {
    error = "invalid .gsvlod header";
    return false;
  }
  t.splats.shDegree = header[0];
  t.splats.shRestPerChannel = header[1];
  t.depth = header[2];
  t.leafSplatCount = static_cast<uint32_t>(header[3]);
  t.splats.boundsMin = b[0];
  t.splats.boundsMax = b[1];
  t.splats.focusCenter = b[2];
  t.splats.focusRadius = focusRadius;
  const uint64_t cap = size;  // no array can have more elements than the file has bytes
  GaussianScene& s = t.splats;
  if (!get(f, t.nodes, cap) || !get(f, s.position, cap) || !get(f, s.logScale, cap) || !get(f, s.rotation, cap) ||
      !get(f, s.opacityLogit, cap) || !get(f, s.shDC, cap) || !get(f, s.shRest, cap)) {
    error = "truncated or corrupt .gsvlod data";
    t = LodTree{};
    return false;
  }
  const size_t n = s.position.size();
  bool ok = !t.nodes.empty() && s.logScale.size() == n && s.rotation.size() == n && s.opacityLogit.size() == n &&
            s.shDC.size() == n && s.shRest.size() == n * static_cast<size_t>(s.shRestPerChannel) * 3 && t.leafSplatCount <= n;
  for (const LodNode& node : t.nodes) {
    ok = ok && static_cast<uint64_t>(node.splatBegin) + node.splatCount <= n &&
         (node.childCount == 0 || static_cast<uint64_t>(node.firstChild) + node.childCount <= t.nodes.size());
    if (!ok) break;
  }
  if (!ok) {
    error = "inconsistent .gsvlod data";
    t = LodTree{};
    return false;
  }
  return true;
}

}  // namespace gsv
