// Navigation and responsiveness (Phase 6 batch 3, docs/core/CORE6-ux.md):
//   - smooth camera transitions (quick views, frame selection, saved views, notes)
//   - quick views from the 6 axis directions: View menu, numpad 1 / 3 / 7 (Ctrl: opposite side)
//   - axis compass in the top-right corner: shows the orientation, click an axis to look along it
//   - frame the selection (Shift+R)
//   - auto quality: while the camera moves, render at a lower resolution chosen from the measured
//     frame time and upscale; full resolution as soon as the view is still again
#include "app/app.h"
#include "core/i18n.h"
#include "gl/gl_loader.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gsv {

namespace {
float wrapAngle(float a) {  // to (-pi, pi]
  const float twoPi = 6.28318530718f;
  a = std::fmod(a + 3.14159265359f, twoPi);
  if (a < 0.0f) a += twoPi;
  return a - 3.14159265359f;
}
}  // namespace

// ------------------------------------------------------------------ camera animation
void App::animateCameraTo(const OrbitCamera& end, float seconds) {
  camAnim_.active = true;
  camAnim_.start = glfwGetTime();
  camAnim_.duration = std::max(seconds, 0.01f);
  camAnim_.from = camera_;
  camAnim_.to = end;
  // Shortest way around.
  camAnim_.to.yaw = camAnim_.from.yaw + wrapAngle(end.yaw - camAnim_.from.yaw);
}

void App::updateCameraAnimation() {
  if (!camAnim_.active) return;
  const ImGuiIO& io = ImGui::GetIO();
  const bool userMoves = !io.WantCaptureMouse && (io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2] || io.MouseWheel != 0.0f);
  if (userMoves) {  // the user takes over
    camAnim_.active = false;
    return;
  }
  float t = static_cast<float>((glfwGetTime() - camAnim_.start) / camAnim_.duration);
  if (t >= 1.0f) {
    t = 1.0f;
    camAnim_.active = false;
  }
  const float e = t * t * (3.0f - 2.0f * t);  // smoothstep
  const OrbitCamera& a = camAnim_.from;
  const OrbitCamera& b = camAnim_.to;
  camera_.target = glm::mix(a.target, b.target, e);
  camera_.yaw = a.yaw + (b.yaw - a.yaw) * e;
  camera_.pitch = a.pitch + (b.pitch - a.pitch) * e;
  camera_.distance = std::exp(std::log(std::max(a.distance, 1e-6f)) * (1.0f - e) + std::log(std::max(b.distance, 1e-6f)) * e);
  camera_.fovY = a.fovY + (b.fovY - a.fovY) * e;
}

void App::quickView(int axis, bool negative) {
  // Look along -axis from the +axis side (or the opposite), keeping target and distance.
  OrbitCamera end = camera_;
  glm::vec3 dir(0.0f);
  dir[axis] = negative ? -1.0f : 1.0f;
  if (axis == 1) dir += glm::vec3(0.0f, 0.0f, 0.001f);  // top / bottom: avoid the pole singularity
  end.lookAt(camera_.target + glm::normalize(dir) * camera_.distance, camera_.target);
  animateCameraTo(end);
  static const char* kNames[3][2] = {{"Right view", "Left view"}, {"Top view", "Bottom view"}, {"Front view", "Back view"}};
  status_ = tr(kNames[axis][negative ? 1 : 0]);
}

void App::frameSelection() {
  if (selectedCount_ == 0) {
    status_ = tr("Frame selection: nothing selected");
    return;
  }
  // Robust extent: 90th percentile distance from the centroid (a few strays do not zoom out).
  const glm::vec3 c = selectionCentroid(scene_, selected_);
  std::vector<float> d;
  d.reserve(selectedCount_);
  for (size_t i = 0; i < scene_.size(); ++i)
    if (selected_[i]) d.push_back(glm::length(scene_.position[i] - c));
  const size_t k = std::min(d.size() - 1, d.size() * 9 / 10);
  std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(k), d.end());
  const float radius = std::max(d[k], 1e-4f);
  OrbitCamera end = camera_;
  end.frameSphere(toDisplay(c), radius);
  animateCameraTo(end);
  status_ = tr("Framed the selection");
}

void App::drawCompass() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float s = std::max(appliedUiScale_, 1.0f);
  const float size = 96.0f * s;
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - size - 10.0f * s, vp->Pos.y + menuBarHeight_ + 56.0f * s));
  ImGui::SetNextWindowSize(ImVec2(size, size));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;
  if (!ImGui::Begin("##compass", nullptr, flags)) {
    ImGui::End();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0 = ImGui::GetWindowPos();
  const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);
  const float r = size * 0.36f;
  dl->AddCircleFilled(c, size * 0.48f, IM_COL32(20, 22, 28, 110));
  const glm::mat3 view(camera_.view());  // display -> camera rotation
  struct End {
    int axis;
    bool neg;
    glm::vec3 v;
  };
  End ends[6];
  for (int a = 0; a < 3; ++a) {
    glm::vec3 e(0.0f);
    e[a] = 1.0f;
    ends[2 * a] = {a, false, view * e};
    ends[2 * a + 1] = {a, true, -(view * e)};
  }
  // Far ones first so the near ones are drawn on top.
  std::sort(ends, ends + 6, [](const End& x, const End& y) { return x.v.z < y.v.z; });
  static const ImU32 kCol[3] = {IM_COL32(230, 80, 80, 255), IM_COL32(110, 210, 100, 255), IM_COL32(90, 140, 240, 255)};
  static const char* kLabel[3] = {"X", "Y", "Z"};
  int clicked = -1;
  for (int k = 0; k < 6; ++k) {
    const End& e = ends[k];
    const ImVec2 q(c.x + e.v.x * r, c.y - e.v.y * r);
    const ImU32 col = e.neg ? (kCol[e.axis] & 0x00FFFFFFu) | 0x90000000u : kCol[e.axis];
    if (!e.neg) dl->AddLine(c, q, col, 2.0f * s);
    const float rr = (e.neg ? 5.0f : 8.0f) * s;
    dl->AddCircleFilled(q, rr, col);
    if (!e.neg) {
      const ImVec2 ts = ImGui::CalcTextSize(kLabel[e.axis]);
      dl->AddText(ImVec2(q.x - ts.x * 0.5f, q.y - ts.y * 0.5f), IM_COL32(15, 15, 15, 255), kLabel[e.axis]);
    }
    ImGui::SetCursorScreenPos(ImVec2(q.x - rr, q.y - rr));
    ImGui::PushID(k);
    if (ImGui::InvisibleButton("##axis", ImVec2(2 * rr, 2 * rr))) clicked = k;
    if (ImGui::IsItemHovered()) {
      static const char* kTip[3][2] = {{"Right view", "Left view"}, {"Top view", "Bottom view"}, {"Front view", "Back view"}};
      ImGui::SetTooltip("%s", tr(kTip[e.axis][e.neg ? 1 : 0]));
    }
    ImGui::PopID();
  }
  if (clicked >= 0) quickView(ends[clicked].axis, ends[clicked].neg);
  ImGui::End();
}

// ------------------------------------------------------------------ auto quality
void App::renderMainView(int fbW, int fbH, float dt) {
  // Moving = the view changed since the last frame (orbit, keys, animation, gizmo preview).
  const glm::mat4 v = camera_.view();
  const bool moving = v != lastView_ || camAnim_.active || splatSettings_.previewActive;
  lastView_ = v;
  const double now = glfwGetTime();
  if (moving) lastMotion_ = now;
  const bool active = settings_.autoQuality && interactive_ && mode_ == RenderMode::Splats && now - lastMotion_ < 0.25;
  if (active) {
    // Frame time includes the vsync wait: ~16.7 ms means "fast enough".
    const float ms = dt * 1000.0f;
    if (ms > 24.0f) renderScale_ = std::max(0.35f, renderScale_ * 0.85f);
    else if (ms < 19.0f) renderScale_ = std::min(1.0f, renderScale_ * 1.08f);
  } else {
    renderScale_ = 1.0f;  // still: full quality
  }
  if (renderScale_ > 0.98f) {
    frame(0, fbW, fbH);
    return;
  }
  const int w = std::max(16, static_cast<int>(fbW * renderScale_)), h = std::max(16, static_cast<int>(fbH * renderScale_));
  if (lowRes_.width() != w || lowRes_.height() != h) lowRes_.create(w, h, targetFormat());
  frame(lowRes_.id(), w, h);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, lowRes_.id());
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  glBlitFramebuffer(0, 0, w, h, 0, 0, fbW, fbH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, fbW, fbH);
}

}  // namespace gsv
