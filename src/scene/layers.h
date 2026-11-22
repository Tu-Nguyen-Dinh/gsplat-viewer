#pragma once
// A scene made of several layers sharing one world frame: Gaussian splats, point clouds (and
// meshes later). Each layer has a GeoFrame (layer -> world, double precision). Renderers draw every
// layer relative to a common `renderOrigin` (see geo_frame.h) chosen near the data.
//
// The stack only owns data and transforms; GPU state lives in the renderers (render/).

#include "core/geo_frame.h"
#include "scene/gaussian_scene.h"
#include "scene/point_cloud.h"

#include <memory>
#include <string>
#include <vector>

namespace gsv {

enum class LayerKind : int { Splats = 0, Points = 1 };

struct Layer {
  int id = 0;                                   // unique within the stack, never reused
  std::string name;
  LayerKind kind = LayerKind::Splats;
  GeoFrame frame;                               // layer coordinates -> world
  bool visible = true;
  std::shared_ptr<GaussianScene> splats;        // kind == Splats
  std::shared_ptr<PointCloud> points;           // kind == Points
};

// Axis-aligned box in world coordinates (double).
struct WorldBounds {
  glm::dvec3 min{0.0}, max{0.0};
  bool valid = false;
  void extend(const glm::dvec3& p);
  glm::dvec3 center() const { return 0.5 * (min + max); }
  double radius() const { return 0.5 * glm::length(max - min); }
};

class LayerStack {
 public:
  int addSplats(std::shared_ptr<GaussianScene> scene, const std::string& name, const GeoFrame& frame = {});
  int addPoints(std::shared_ptr<PointCloud> cloud, const std::string& name, const GeoFrame& frame = {});
  bool remove(int id);
  Layer* find(int id);
  const Layer* find(int id) const;
  const std::vector<Layer>& layers() const { return layers_; }
  std::vector<Layer>& layers() { return layers_; }
  size_t size() const { return layers_.size(); }
  bool empty() const { return layers_.empty(); }

  // World bounds of the visible layers. Splat layers use their robust bounds (floaters ignored),
  // point layers their full extent; each box's 8 corners go through the layer frame.
  WorldBounds worldBounds(bool visibleOnly = true) const;
  // Origin used for float rendering: the centre of the world bounds, rounded to whole metres so it
  // stays stable when a small layer is added. Recomputed by the caller when layers change.
  glm::dvec3 suggestRenderOrigin() const;

 private:
  std::vector<Layer> layers_;
  int nextId_ = 1;
};

}  // namespace gsv
