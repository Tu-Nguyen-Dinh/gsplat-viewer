#pragma once

#include "core/sorting.h"
#include "gl/buffer.h"
#include "gl/gpu_timer.h"
#include "gl/shader.h"
#include "render/gpu_radix_sort.h"
#include "render/render_view.h"
#include "scene/gaussian_scene.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gsv {

enum class DebugView : int { Color = 0, Depth = 1, Overdraw = 2, Ellipses = 3 };

// How splats are ordered back-to-front before blending.
enum class SortMode : int {
  None = 0,      // GPU visible-list order (random): wrong image, shows why sorting matters
  CpuStd = 1,    // std::sort on the CPU, upload the order every time the camera moves
  CpuRadix = 2,  // LSD radix sort on the CPU
  GpuRadix = 3,  // radix sort in compute shaders: no CPU work, no upload
};

struct SplatSettings {
  int shDegree = 3;              // clamped to what the scene has
  float splatScale = 1.0f;
  bool orientedQuads = true;     // quad aligned with the ellipse axes (fewer wasted fragments)
  bool cullOutsideViewport = true;
  SortMode sort = SortMode::GpuRadix;
  int depthKeyBits = 32;         // 32: exact float depth (8 passes); 16: quantized depth (4 passes)
  DebugView debugView = DebugView::Color;
  bool countCoverage = false;    // benchmark only: shade every rasterized fragment (no discard)
};

struct SplatStats {
  uint32_t total = 0;
  uint32_t visible = 0;          // read back asynchronously, a few frames old
  double preprocessMs = 0.0;     // GPU times
  double gpuSortMs = 0.0;
  double drawMs = 0.0;
  double cpuSortMs = 0.0;        // CPU time of the last sort (depth keys + sort)
  double uploadMs = 0.0;         // CPU time to upload the sorted order
  bool sortSkipped = false;      // camera did not move: previous order reused
  double uploadSceneMs = 0.0;    // CPU time of the last upload() (precompute + buffer creation)
  size_t gpuMemoryBytes = 0;     // size of the scene-dependent buffers
};

// GPU pipeline for 3D Gaussian Splatting:
//   1. preprocess (compute): project + color + cull every Gaussian, append visible ones
//   2. sort back-to-front (GPU radix sort, or CPU std::sort / radix for comparison)
//   3. draw: one instanced quad per visible splat, alpha blended
class SplatRenderer {
 public:
  bool init(std::string& log);
  bool reloadShaders(std::string& log);
  // Store SH bands 1..3 as float16 (default) or float32. Takes effect at the next upload().
  void setHalfPrecisionSh(bool half) { halfSh_ = half; }
  bool halfPrecisionSh() const { return halfSh_; }
  void upload(const GaussianScene& scene);
  void render(const RenderView& view, const SplatSettings& settings);

  const SplatStats& stats() const { return stats_; }

  // Benchmark support: plain averages of the GPU timers since the last reset.
  struct Averages {
    double preprocessMs = 0.0, gpuSortMs = 0.0, drawMs = 0.0;
  };
  void resetAverages();
  Averages averages() const;
  uint32_t splatCount() const { return count_; }
  // Exposed for tests: per-splat projection results and the visible list.
  const GlBuffer& projectedBuffer() const { return projected_; }
  const GlBuffer& orderBuffer() const { return order_; }
  const GlBuffer& drawCommandBuffer() const { return drawCommand_; }

  // Runs only the preprocess pass (used by render() and by tests).
  void preprocess(const RenderView& view, const SplatSettings& settings);

 private:
  void draw(const RenderView& view, const SplatSettings& settings);
  void sortOnCpu(const RenderView& view, SortMode mode);
  void harvestVisibleCount();

  // CPU sorting state.
  std::vector<glm::vec3> positions_;
  std::vector<uint32_t> cpuKeys_, cpuOrder_;
  RadixScratch radixScratch_;
  GlBuffer cpuOrderBuffer_;  // uint[count], all splats back-to-front
  glm::mat4 lastSortedModelView_{0.0f};
  SortMode lastCpuSortMode_ = SortMode::None;
  bool cpuOrderValid_ = false;

  ShaderProgram preprocess_;
  ShaderProgram draw_;
  GpuRadixSort gpuSort_;
  GpuTimer sortTimer_;
  GLuint vao_ = 0;

  GlBuffer splats_;       // GpuSplat[count]
  GlBuffer shRest_;       // float[count * 3 * restPerChannel]
  GlBuffer projected_;    // ProjectedSplat[count]
  GlBuffer order_;        // uint[count]  visible splat indices, in draw order
  GlBuffer sortKeys_;     // uint[count]  depth keys of the visible splats
  GlBuffer drawCommand_;  // DrawCommand (indirect draw arguments)

  uint32_t count_ = 0;
  int sceneShDegree_ = 0;
  int restPerChannel_ = 0;
  bool halfSh_ = true;
  uint32_t shStride_ = 0;  // uints per splat in shRest_
  glm::vec2 depthRange_{0.0f, 1.0f};  // (zMin, 1 / (zMax - zMin)) for this frame
  glm::vec3 boundsCenter_{0.0f};
  float boundsRadius_ = 1.0f;

  GpuTimer preprocessTimer_, drawTimer_;
  SplatStats stats_;

  // Asynchronous read-back of the visible count: copy to a staging buffer + fence,
  // read it a few frames later when the fence has signaled (never stalls).
  struct Readback {
    GlBuffer staging;
    GLsync fence = nullptr;
  };
  std::array<Readback, 3> readbacks_{};
  int readbackIndex_ = 0;
};

}  // namespace gsv
