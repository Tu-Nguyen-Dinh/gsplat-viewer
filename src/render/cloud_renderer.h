#pragma once
// Core 1: draws point-cloud layers (scene/point_cloud.h) as opaque points with depth writes.
// See docs/core/CORE1-scene.md.

#include "core/geo_frame.h"
#include "gl/shader.h"
#include "render/render_view.h"
#include "scene/point_cloud.h"

#include <glm/glm.hpp>

#include <string>

namespace gsv {

enum class CloudColorMode : int { Rgb = 0, Intensity = 1, Classification = 2, Height = 3 };

struct CloudSettings {
  float pointSize = 2.0f;        // pixels, used when worldSize <= 0
  float worldSize = 0.0f;        // metres: perspective-correct point size (0 = fixed pixel size)
  bool roundPoints = true;
  CloudColorMode colorMode = CloudColorMode::Rgb;  // falls back to Height when the cloud has no color
  int heightAxis = 2;            // 2 = z up (LiDAR), 1 = y up
};

class CloudRenderer {
 public:
  CloudRenderer() = default;
  ~CloudRenderer();
  CloudRenderer(const CloudRenderer&) = delete;
  CloudRenderer& operator=(const CloudRenderer&) = delete;

  bool init(std::string& log);
  bool reloadShaders(std::string& log) { return program_.reload(log); }
  void upload(const PointCloud& cloud);
  // `layerToWorld` maps the cloud's local offsets to world (layer frame * translate(origin)); the
  // view matrix in `view` maps (world - renderOrigin) to camera. Enables depth test + depth write.
  void draw(const RenderView& view, const GeoFrame& layerToWorld, const glm::dvec3& renderOrigin,
            const CloudSettings& settings);
  size_t pointCount() const { return static_cast<size_t>(count_); }
  size_t gpuMemoryBytes() const { return static_cast<size_t>(count_) * 20u; }

 private:
  ShaderProgram program_;
  GLuint vao_ = 0, vbo_ = 0;
  GLsizei count_ = 0;
  bool hasColor_ = false;
  glm::vec3 localMin_{0.0f}, localMax_{1.0f};
};

}  // namespace gsv
