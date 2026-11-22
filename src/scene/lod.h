#pragma once
// Core 2: level of detail for Gaussian-splat scenes. See docs/core/CORE2-lod.md.
//
// Build (offline / at load):
//   1. splats are ordered along a Morton curve; the octree is the Morton-prefix hierarchy, so every
//      node is a contiguous range of that order and children are spatially compact;
//   2. leaves keep the original splats (<= leafCapacity each);
//   3. an inner node holds at most `nodeCapacity` proxy splats: its children's splats, Morton-
//      contiguous groups merged into one Gaussian each by moment matching (weights = opacity x area),
//      preserving the group's mean and covariance exactly.
// Use (every time the camera moved enough):
//   selectCut() walks the tree from the root, refining the node with the largest projected size first
//   until every node is below `pixelError` or the splat budget is reached; gatherCut() concatenates the
//   chosen nodes' splats into a GaussianScene for SplatRenderer::upload().

#include "scene/gaussian_scene.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gsv {

struct LodNode {
  glm::vec3 center{0.0f};   // bounding sphere of every splat below the node (3-sigma extents)
  float radius = 0.0f;
  uint32_t firstChild = 0;  // index in LodTree::nodes, children are consecutive
  uint32_t childCount = 0;  // 0 = leaf
  uint32_t splatBegin = 0;  // range in LodTree::splats
  uint32_t splatCount = 0;
  uint32_t level = 0;       // 0 = root
};

struct LodTree {
  GaussianScene splats;           // leaf splats (original) followed by inner-node proxies
  std::vector<LodNode> nodes;     // nodes[0] = root
  uint32_t leafSplatCount = 0;    // = original scene size
  int depth = 0;                  // number of levels
  bool empty() const { return nodes.empty(); }
};

struct LodBuildOptions {
  uint32_t leafCapacity = 4096;   // max splats per leaf
  uint32_t nodeCapacity = 4096;   // max proxy splats per inner node
};

LodTree buildLod(const GaussianScene& scene, const LodBuildOptions& options = LodBuildOptions());

struct LodCutParams {
  glm::vec3 cameraPosition{0.0f};  // in scene coordinates
  float focalPixels = 1000.0f;
  float pixelError = 2.0f;         // refine nodes whose projected radius exceeds this (pixels)
  uint64_t splatBudget = 2000000;  // never exceeded once the root fits
};
struct LodCut {
  std::vector<uint32_t> nodes;
  uint64_t splatCount = 0;
  bool budgetLimited = false;      // refinement stopped by the budget, not by the pixel error
};
LodCut selectCut(const LodTree& tree, const LodCutParams& params);
GaussianScene gatherCut(const LodTree& tree, const LodCut& cut);

// Moment-matching merge of splats `indices` of `scene` into one Gaussian appended to `out`
// (exposed for tests). Weights: opacity * (det Sigma)^(1/3) (area-like).
void mergeSplats(const GaussianScene& scene, const std::vector<uint32_t>& indices, GaussianScene& out);
// Full 3x3 covariance (double) of splat i, for tests and merging.
glm::dmat3 splatCovariance(const GaussianScene& scene, size_t i);

// Binary file ".gsvlod": the whole tree (version 1). Returns false with `error` on failure; never
// crashes on malformed input.
bool saveLod(const std::string& path, const LodTree& tree, std::string& error);
bool loadLod(const std::string& path, LodTree& tree, std::string& error);

}  // namespace gsv
