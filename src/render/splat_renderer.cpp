#include "render/splat_renderer.h"

#include "core/gaussian_math.h"
#include "core/sorting.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

namespace gsv {

namespace {

// Must match `GpuSplat` / `ProjectedSplat` / `DrawCommand` in shaders/include/splat_buffers.glsl.
struct GpuSplat {
  glm::vec4 positionOpacity;
  glm::vec4 cov0;
  glm::vec4 cov1;
  glm::vec4 dc;
};
static_assert(sizeof(GpuSplat) == 64);
constexpr size_t kProjectedSize = 64;  // 4 x vec4
struct DrawCommand {
  uint32_t count, instanceCount, first, baseInstance;
};

constexpr GLuint kBindSplats = 0, kBindShRest = 1, kBindProjected = 2, kBindOrder = 3, kBindDrawCmd = 4,
                 kBindSortKeys = 5;
constexpr unsigned kPreprocessGroup = 256;

}  // namespace

bool SplatRenderer::init(std::string& log) {
  if (!preprocess_.loadCompute("splat_preprocess.comp", log)) return false;
  if (!draw_.loadGraphics("splat.vert", "splat.frag", log)) return false;
  if (!gpuSort_.init(log)) return false;
  glGenVertexArrays(1, &vao_);  // no vertex attributes: everything comes from SSBOs
  const DrawCommand cmd{4, 0, 0, 0};
  drawCommand_.allocate(sizeof(cmd), &cmd);
  for (auto& r : readbacks_) r.staging.allocate(sizeof(uint32_t), nullptr, GL_STREAM_READ);
  return true;
}

bool SplatRenderer::reloadShaders(std::string& log) {
  std::string l1, l2, l3;
  const bool ok1 = preprocess_.reload(l1), ok2 = draw_.reload(l2), ok3 = gpuSort_.reloadShaders(l3);
  log = l1 + l2 + l3;
  return ok1 && ok2 && ok3;
}

void SplatRenderer::upload(const GaussianScene& scene) {
  count_ = static_cast<uint32_t>(scene.size());
  sceneShDegree_ = scene.shDegree;
  restPerChannel_ = scene.shRestPerChannel;

  // Activations and the 3D covariance depend only on the scene, not on the camera:
  // compute them once here instead of every frame on the GPU.
  std::vector<GpuSplat> gpu(scene.size());
  for (size_t i = 0; i < scene.size(); ++i) {
    const Cov3 c = covarianceFromScaleRotation(glm::exp(scene.logScale[i]), scene.rotation[i]);
    gpu[i].positionOpacity = glm::vec4(scene.position[i], sigmoid(scene.opacityLogit[i]));
    gpu[i].cov0 = {c.xx, c.xy, c.xz, c.yy};
    gpu[i].cov1 = {c.yz, c.zz, 0.0f, 0.0f};
    gpu[i].dc = glm::vec4(scene.shDC[i], 0.0f);
  }
  splats_.allocate(gpu.size() * sizeof(GpuSplat), gpu.data(), GL_STATIC_DRAW);
  if (!scene.shRest.empty())
    shRest_.allocate(scene.shRest.size() * sizeof(float), scene.shRest.data(), GL_STATIC_DRAW);
  else
    shRest_.allocate(sizeof(float), nullptr, GL_STATIC_DRAW);  // SSBOs may not be empty
  projected_.allocate(scene.size() * kProjectedSize);
  order_.allocate(scene.size() * sizeof(uint32_t));
  cpuOrderBuffer_.allocate(scene.size() * sizeof(uint32_t));
  positions_ = scene.position;  // CPU copy for depth sorting
  cpuOrderValid_ = false;
  sortKeys_.allocate(scene.size() * sizeof(uint32_t));

  boundsCenter_ = 0.5f * (scene.boundsMin + scene.boundsMax);
  boundsRadius_ = std::max(0.5f * glm::length(scene.boundsMax - scene.boundsMin), 1e-3f);
  stats_ = {};
  stats_.total = count_;
}

void SplatRenderer::preprocess(const RenderView& view, const SplatSettings& settings) {
  // Reset the visible counter (instanceCount) to zero.
  const uint32_t zero = 0;
  drawCommand_.upload(offsetof(DrawCommand, instanceCount), sizeof(zero), &zero);

  const glm::mat4 modelView = view.view * view.model;
  const glm::vec3 cameraInModel = glm::vec3(glm::inverse(view.model) * glm::vec4(view.cameraPosition, 1.0f));

  splats_.bindBase(GL_SHADER_STORAGE_BUFFER, kBindSplats);
  shRest_.bindBase(GL_SHADER_STORAGE_BUFFER, kBindShRest);
  projected_.bindBase(GL_SHADER_STORAGE_BUFFER, kBindProjected);
  order_.bindBase(GL_SHADER_STORAGE_BUFFER, kBindOrder);
  drawCommand_.bindBase(GL_SHADER_STORAGE_BUFFER, kBindDrawCmd);
  sortKeys_.bindBase(GL_SHADER_STORAGE_BUFFER, kBindSortKeys);

  preprocess_.use();
  preprocess_.set("uCount", count_);
  preprocess_.set("uView", modelView);
  preprocess_.set("uViewport", glm::vec2(view.viewport));
  preprocess_.set("uFocal", view.focalPixels());
  preprocess_.set("uTanHalfFov", view.tanHalfFov());
  preprocess_.set("uNear", view.nearPlane);
  preprocess_.set("uSplatScale", settings.splatScale);
  preprocess_.set("uCull", settings.cullOutsideViewport ? 1 : 0);
  preprocess_.set("uCameraPos", cameraInModel);
  preprocess_.set("uShDegree", std::min(settings.shDegree, sceneShDegree_));
  preprocess_.set("uRestPerChannel", static_cast<unsigned>(restPerChannel_));
  preprocess_.set("uDepthRange", depthRange_);
  preprocess_.set("uKeyBits", settings.depthKeyBits == 16 ? 16 : 32);
  preprocess_.dispatch1D(count_, kPreprocessGroup);

  // The draw reads the SSBOs in the vertex shader and the counter as indirect arguments.
  glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
}

void SplatRenderer::draw(const RenderView& view, const SplatSettings& settings) {
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  if (settings.debugView == DebugView::Overdraw)
    glBlendFunc(GL_ONE, GL_ONE);  // count layers
  else
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied "over", back to front

  // CPU sorting orders ALL splats (culled ones are skipped in the vertex shader);
  // otherwise the GPU-built visible list is drawn with an indirect call.
  const bool cpuOrder = settings.sort == SortMode::CpuStd || settings.sort == SortMode::CpuRadix;
  projected_.bindBase(GL_SHADER_STORAGE_BUFFER, kBindProjected);
  (cpuOrder ? cpuOrderBuffer_ : order_).bindBase(GL_SHADER_STORAGE_BUFFER, kBindOrder);
  draw_.use();
  draw_.set("uViewport", glm::vec2(view.viewport));
  draw_.set("uOrientedQuads", settings.orientedQuads ? 1 : 0);
  draw_.set("uDebugView", static_cast<int>(settings.debugView));
  draw_.set("uDepthRange", depthRange_);

  glBindVertexArray(vao_);
  if (cpuOrder) {
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count_));
  } else {
    drawCommand_.bind(GL_DRAW_INDIRECT_BUFFER);
    glDrawArraysIndirect(GL_TRIANGLE_STRIP, nullptr);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
  }
  glBindVertexArray(0);

  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
}

void SplatRenderer::render(const RenderView& view, const SplatSettings& settings) {
  if (count_ == 0) return;

  // Depth range covered by the scene's bounding sphere from this camera. Used to
  // normalize depth for the depth debug view and for 16-bit sort keys.
  const glm::vec3 center = glm::vec3(view.model * glm::vec4(boundsCenter_, 1.0f));
  const float d = glm::length(center - view.cameraPosition);
  const float zMin = std::max(view.nearPlane, d - boundsRadius_), zMax = std::max(zMin + 1e-3f, d + boundsRadius_);
  depthRange_ = {zMin, 1.0f / (zMax - zMin)};

  preprocessTimer_.begin();
  preprocess(view, settings);
  preprocessTimer_.end();

  if (settings.sort == SortMode::CpuStd || settings.sort == SortMode::CpuRadix) {
    sortOnCpu(view, settings.sort);
  } else if (settings.sort == SortMode::GpuRadix) {
    // Sorts the visible list in place: keys = depth keys, values = splat indices.
    // The count is the instanceCount field (uint #1) of the indirect draw command.
    sortTimer_.begin();
    gpuSort_.sort(sortKeys_, order_, drawCommand_, 1, count_, settings.depthKeyBits == 16 ? 16 : 32);
    sortTimer_.end();
    stats_.gpuSortMs = sortTimer_.milliseconds();
  }

  drawTimer_.begin();
  draw(view, settings);
  drawTimer_.end();

  harvestVisibleCount();
  stats_.preprocessMs = preprocessTimer_.milliseconds();
  stats_.drawMs = drawTimer_.milliseconds();
}

void SplatRenderer::sortOnCpu(const RenderView& view, SortMode mode) {
  // The order only depends on the camera: skip the sort while it does not move.
  const glm::mat4 modelView = view.view * view.model;
  if (cpuOrderValid_ && mode == lastCpuSortMode_ && modelView == lastSortedModelView_) {
    stats_.sortSkipped = true;
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  computeDepthKeys(positions_, modelView, cpuKeys_);
  if (mode == SortMode::CpuStd)
    sortByKeyStd(cpuKeys_, cpuOrder_);
  else
    sortByKeyRadix(cpuKeys_, cpuOrder_, radixScratch_);
  const auto t1 = std::chrono::steady_clock::now();
  cpuOrderBuffer_.upload(0, cpuOrder_.size() * sizeof(uint32_t), cpuOrder_.data());
  const auto t2 = std::chrono::steady_clock::now();

  stats_.cpuSortMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
  stats_.uploadMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
  stats_.sortSkipped = false;
  lastSortedModelView_ = modelView;
  lastCpuSortMode_ = mode;
  cpuOrderValid_ = true;
}

void SplatRenderer::harvestVisibleCount() {
  // 1. Collect every read-back whose fence has signaled (timeout 0 = never wait).
  for (auto& r : readbacks_) {
    if (!r.fence) continue;
    if (glClientWaitSync(r.fence, 0, 0) == GL_TIMEOUT_EXPIRED) continue;
    glDeleteSync(r.fence);
    r.fence = nullptr;
    uint32_t visible = 0;
    r.staging.download(0, sizeof(visible), &visible);
    stats_.visible = visible;
  }
  // 2. Start a new copy in the next free slot.
  Readback& slot = readbacks_[readbackIndex_];
  if (!slot.fence) {
    glBindBuffer(GL_COPY_READ_BUFFER, drawCommand_.id());
    glBindBuffer(GL_COPY_WRITE_BUFFER, slot.staging.id());
    glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, offsetof(DrawCommand, instanceCount), 0,
                        sizeof(uint32_t));
    slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    readbackIndex_ = (readbackIndex_ + 1) % static_cast<int>(readbacks_.size());
  }
}

}  // namespace gsv
