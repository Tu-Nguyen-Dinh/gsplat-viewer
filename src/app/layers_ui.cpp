// Viewer integration of the core layers (docs/core/INDEX.md): a LAS point-cloud layer drawn
// opaque with the splats, a two-point measuring tool using the expected-depth output, and
// level-of-detail rendering of large scenes.
#include "app/app.h"
#include "core/i18n.h"
#include "gl/gl_loader.h"
#include "core/picking.h"
#include "core/ray_pick.h"
#include "scene/las_io.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <chrono>
#include <cmath>

namespace gsv {

namespace {
double now() { return glfwGetTime(); }

// Survey data is Z-up; the viewer's orbit camera is Y-up: x' = x, y' = z, z' = -y.
GeoFrame zUpToYUp() {
  GeoFrame f;
  f.rotation = glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, 0, -1), glm::dvec3(0, 1, 0));  // columns
  return f;
}
}  // namespace

GeoFrame App::cloudToDisplay() const {
  if (hasCloudTransform_) {
    // local offset -> LAS world (origin, double) -> scene coordinates -> display (model matrix)
    const glm::mat4 m = modelMatrix();
    GeoFrame model;
    model.rotation = glm::dmat3(glm::mat3(m));
    model.translation = glm::dvec3(m[3]);
    return model * cloudTransform_ * GeoFrame::fromTranslation(cloud_.origin);
  }
  // No registration given: the cloud is shown around its own origin, Z up.
  return zUpToYUp();
}

void App::loadCloudFile(const std::string& path) {
  const double t0 = now();
  PointCloud cloud;
  LasInfo info;
  std::string err;
  if (!readLas(path, cloud, err, &info, static_cast<uint32_t>(options_.lasStride))) {
    status_ = std::string(tr("LAS load failed: ")) + err;
    return;
  }
  cloud_ = std::move(cloud);
  cloudRenderer_.upload(cloud_);
  haveCloud_ = true;
  showCloud_ = true;
  cloudSettings_.colorMode = cloud_.hasColor() ? CloudColorMode::Rgb : CloudColorMode::Height;
  char buf[320];
  std::snprintf(buf, sizeof(buf), tr("Loaded %zu LiDAR points (LAS 1.%d format %d) in %.0f ms, origin (%.0f, %.0f, %.0f)%s"),
                cloud_.size(), info.versionMinor, info.pointFormat, (now() - t0) * 1000.0, cloud_.origin.x,
                cloud_.origin.y, cloud_.origin.z,
                (!scene_.empty() && !hasCloudTransform_) ? " - not registered to the splats (--las-transform)" : "");
  status_ = buf;
  if (scene_.empty()) frameCloud();
}

void App::frameCloud() {
  if (!haveCloud_ || cloud_.empty()) return;
  const GeoFrame f = cloudToDisplay();
  glm::dvec3 lo(1e300), hi(-1e300);
  for (int c = 0; c < 8; ++c) {
    const glm::dvec3 corner((c & 1) ? cloud_.localMax.x : cloud_.localMin.x, (c & 2) ? cloud_.localMax.y : cloud_.localMin.y,
                            (c & 4) ? cloud_.localMax.z : cloud_.localMin.z);
    const glm::dvec3 p = f.apply(corner);
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  const float radius = static_cast<float>(0.5 * glm::length(hi - lo));
  camera_.frameSphere(glm::vec3(0.5 * (lo + hi)), radius, 2.0f * radius);
}

// Surface point under a window-pixel cursor position, in display coordinates (or nothing).
std::optional<glm::vec3> App::pickSurface(double cx, double cy, double* msOut) {
  // Framebuffer pixels may differ from window coordinates (HiDPI).
  int winW, winH, fbW, fbH;
  glfwGetWindowSize(window_, &winW, &winH);
  glfwGetFramebufferSize(window_, &fbW, &fbH);
  if (fbW <= 0 || fbH <= 0) return std::nullopt;
  const double sx = static_cast<double>(fbW) / std::max(winW, 1), sy = static_cast<double>(fbH) / std::max(winH, 1);
  const RenderView v = makeRenderView(fbW, fbH);
  const glm::mat4 invView = glm::inverse(v.view);
  const glm::vec3 eye = glm::vec3(invView[3]);
  const glm::vec2 f = v.focalPixels(), c = v.principalPixels();
  auto rayThrough = [&](double wx, double wy) {
    const double px = wx * sx + 0.5, py = (fbH - 1) - wy * sy + 0.5;  // OpenGL pixel centre, origin bottom-left
    const glm::vec3 dirCam(static_cast<float>((px - c.x) / f.x), static_cast<float>((py - c.y) / f.y), -1.0f);
    return glm::normalize(glm::vec3(invView * glm::vec4(dirCam, 0.0f)));
  };

  // 1. Splats: exact 3D ray / Gaussian intersection, median depth (core/ray_pick.h). The expected
  //    depth used before averaged the table edge with the floor behind it and with floaters, so the
  //    point floated in the air and slid off the edge when the view changed.
  std::optional<glm::vec3> best;
  float bestDistance = 1e30f;
  const auto t0 = std::chrono::steady_clock::now();
  if (mode_ == RenderMode::Splats && !scene_.empty()) {
    // Clicks a pixel or two off an edge still snap to the nearest surface around the cursor.
    static const int kRing[][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}, {2, 0}, {-2, 0}, {0, 2}, {0, -2}};
    for (const auto& o : kRing) {
      const glm::vec3 dir = rayThrough(cx + o[0], cy + o[1]);
      if (const auto h = raycastSplats(scene_, modelMatrix(), eye, dir)) {
        // Keep the click's own ray when it hits; neighbours only fill in a miss.
        best = eye + rayThrough(cx, cy) * h->distance;
        if (o[0] == 0 && o[1] == 0) best = h->point;
        bestDistance = h->distance;
        break;
      }
    }
  }
  // 2. Point cloud (opaque): its depth comes from the GPU (one frame with the depth output).
  if (haveCloud_ && showCloud_) {
    Framebuffer target;
    if (target.create(fbW, fbH, Framebuffer::Format::RGBA16F, {true, true})) {
      target.clear(background_);
      cloudRenderer_.draw(v, cloudToDisplay(), glm::dvec3(0.0), cloudSettings_);
      const DepthImage depth = readDepthOutput(target);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      const int px = static_cast<int>(cx * sx), py = static_cast<int>(cy * sy);
      if (const auto p = pickPoint(depth, px, py, PickCamera{v.view, f, c}, 0.5f, 1)) {
        const float d = glm::length(*p - eye);
        if (d < bestDistance) {
          best = *p;
          bestDistance = d;
        }
      }
    }
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (msOut) *msOut = ms;
  return best;
}

void App::setLodEnabled(bool on) {
  if (lodPending_.valid()) lodPending_.wait();  // the worker reads lodTree_
  lodPending_ = {};
  if (on == lodEnabled_ && (!on || !lodTree_.empty())) return;
  if (!on) {
    lodEnabled_ = false;
    if (!scene_.empty()) splats_.upload(scene_);
    lodShown_ = 0;
    return;
  }
  if (scene_.empty()) return;
  if (lodTree_.empty()) {
    const double t0 = now();
    lodTree_ = buildLod(scene_);
    lodBuildMs_ = (now() - t0) * 1000.0;
  }
  lodEnabled_ = true;
  lodLastEye_ = glm::vec3(1e30f);  // force a cut on the next frame
}

void App::updateLod() {
  if (!lodEnabled_ || lodTree_.empty()) return;
  if (lodPending_.valid()) {
    if (lodPending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    GaussianScene cut = lodPending_.get();
    lodCutMs_ = (now() - lodCutStart_) * 1000.0;
    lodShown_ = cut.size();
    splats_.upload(cut);  // GPU upload on the GL thread
    return;
  }
  // Re-cut when the eye moved by more than 5 % of its distance to the subject.
  const glm::vec3 eye = glm::vec3(glm::inverse(modelMatrix()) * glm::vec4(camera_.position(), 1.0f));
  const float dist = std::max(glm::length(eye - scene_.focusCenter), 1e-3f);
  if (glm::length(eye - lodLastEye_) < 0.05f * dist) return;
  lodLastEye_ = eye;
  int fbW, fbH;
  glfwGetFramebufferSize(window_, &fbW, &fbH);
  LodCutParams p;
  p.cameraPosition = eye;
  p.focalPixels = camera_.focalPixels(static_cast<float>(std::max(fbH, 1)));
  p.pixelError = lodPixelError_;
  p.splatBudget = static_cast<uint64_t>(std::max<long long>(lodBudget_, 1000));
  lodCutStart_ = now();
  const LodTree* tree = &lodTree_;
  lodPending_ = std::async(std::launch::async, [tree, p] { return gatherCut(*tree, selectCut(*tree, p)); });
}

void App::drawLayersPanel() {
  if (!ImGui::CollapsingHeader(trId("Layers, measure, LOD"))) return;
  // ---- point cloud
  ImGui::SeparatorText(tr("Point cloud (LAS)"));
  if (!haveCloud_) {
    ImGui::TextDisabled("%s", tr("drop a .las file or use --las FILE"));
  } else {
    ImGui::Text(tr("%zu points  |  origin %.0f, %.0f, %.0f"), cloud_.size(), cloud_.origin.x, cloud_.origin.y, cloud_.origin.z);
    ImGui::Checkbox(trId("Show points"), &showCloud_);
    const char* modes[] = {"RGB", "Intensity", "Class", "Height"};
    int mode = static_cast<int>(cloudSettings_.colorMode);
    if (ImGui::Combo(trId("Color"), &mode, modes, 4)) cloudSettings_.colorMode = static_cast<CloudColorMode>(mode);
    ImGui::SliderFloat(trId("Point size (px)"), &cloudSettings_.pointSize, 1.0f, 12.0f);
    if (!hasCloudTransform_ && !scene_.empty())
      ImGui::TextWrapped("%s", tr("Not registered to the splats: pass --las-transform (e.g. the result of gsv_icp)."));
    if (ImGui::Button(trId("Frame cloud"))) frameCloud();
    ImGui::SameLine();
    if (ImGui::Button(trId("Remove cloud"))) {
      haveCloud_ = false;
      cloud_ = {};
    }
  }
  // ---- measure
  ImGui::SeparatorText(tr("Measure (M)"));
  if (ImGui::Button(tool_ == Tool::Measure ? trId("Leave measure tool") : trId("Measure tool"))) {
    setTool(tool_ == Tool::Measure ? Tool::Navigate : Tool::Measure);
  }
  ImGui::SameLine();
  ImGui::TextDisabled(tr("%zu measurement(s), see Tools"), project_.measurements.size());
  // ---- LOD
  ImGui::SeparatorText(tr("Level of detail"));
  bool lod = lodEnabled_;
  ImGui::BeginDisabled(scene_.empty());
  if (ImGui::Checkbox(trId("LOD"), &lod)) setLodEnabled(lod);
  ImGui::EndDisabled();
  int budgetK = static_cast<int>(lodBudget_ / 1000);
  if (ImGui::SliderInt(trId("Budget (k splats)"), &budgetK, 50, 8000)) {
    lodBudget_ = static_cast<long long>(budgetK) * 1000;
    lodLastEye_ = glm::vec3(1e30f);
  }
  if (ImGui::SliderFloat(trId("Pixel error"), &lodPixelError_, 0.5f, 8.0f)) lodLastEye_ = glm::vec3(1e30f);
  if (lodEnabled_)
    ImGui::Text(tr("%zu / %zu splats drawn  |  tree %zu nodes (%.0f ms)  |  last cut %.0f ms"), lodShown_, scene_.size(),
                lodTree_.nodes.size(), lodBuildMs_, lodCutMs_);
}

}  // namespace gsv
