// Dear ImGui control panel of the viewer.
#include "app/app.h"
#include "scene/demo_scene.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>

namespace gsv {

void App::drawUi() {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(390, 0), ImGuiCond_FirstUseEver);
  ImGui::Begin("Gaussian Splat Viewer");

  // ---- Scene ---------------------------------------------------------------
  ImGui::SeparatorText("Scene");
  ImGui::Text("%s", scene_.sourceName.c_str());
  ImGui::Text("%zu splats  |  SH degree %d", scene_.size(), scene_.shDegree);
  ImGui::SetNextItemWidth(-60);
  const bool enter = ImGui::InputText("##path", pathInput_, sizeof(pathInput_), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  ImGui::BeginDisabled(isLoading());
  if (ImGui::Button("Load") || enter) pendingLoad_ = pathInput_;
  ImGui::EndDisabled();
  ImGui::TextDisabled("or drag & drop a .ply / .splat / .spz onto the window");
  if (isLoading()) {
    static const char kSpinner[] = "|/-\\";
    const double t = glfwGetTime();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%c Loading %s ... %.1f s", kSpinner[int(t * 8) & 3],
                       loadingName_.c_str(), t - loadingStart_);
  }
  if (ImGui::Button("Demo scene")) {
    loadColmapFor("", "");  // the demo has no cameras
    setScene(makeDemoScene());
    status_ = "Demo scene";
  }
  if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());

  // ---- Performance ---------------------------------------------------------
  ImGui::SeparatorText("Performance");
  ImGui::Text("FPS %.1f   frame %.2f ms", io.Framerate, 1000.0f / std::max(io.Framerate, 0.001f));
  if (mode_ == RenderMode::Splats) {
    const SplatStats& s = splats_.stats();
    ImGui::Text("Visible    %u / %u (%.0f%%)", s.visible, s.total, s.total ? 100.0 * s.visible / s.total : 0.0);
    ImGui::Text("GPU preprocess %.3f ms", s.preprocessMs);
    ImGui::Text("GPU draw       %.3f ms", s.drawMs);
    if (splatSettings_.sort == SortMode::GpuRadix)
      ImGui::Text("GPU sort       %.3f ms", s.gpuSortMs);
    else if (splatSettings_.sort != SortMode::None)
      ImGui::Text("CPU sort       %.3f ms + upload %.3f ms%s", s.cpuSortMs, s.uploadMs,
                  s.sortSkipped ? "  (cached)" : "");
  } else {
    ImGui::Text("GPU points     %.3f ms", pointTimer_.milliseconds());
  }
  ImGui::Text("Load time  %.0f ms", loadMs_);

  // ---- Render --------------------------------------------------------------
  ImGui::SeparatorText("Render");
  int mode = static_cast<int>(mode_);
  ImGui::RadioButton("Gaussian splats", &mode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Points (Tab)", &mode, 1);
  mode_ = static_cast<RenderMode>(mode);

  if (mode_ == RenderMode::Splats) {
    int view = static_cast<int>(splatSettings_.debugView);
    ImGui::Combo("View", &view, "Color\0Depth\0Overdraw\0Ellipses\0");
    splatSettings_.debugView = static_cast<DebugView>(view);
    int sort = static_cast<int>(splatSettings_.sort);
    ImGui::Combo("Sort", &sort, "None (wrong order)\0CPU std::sort\0CPU radix sort\0GPU radix sort\0");
    splatSettings_.sort = static_cast<SortMode>(sort);
    if (splatSettings_.sort == SortMode::GpuRadix) {
      int keys = splatSettings_.depthKeyBits == 16 ? 0 : splatSettings_.depthKeyBits == 24 ? 1 : 2;
      if (ImGui::Combo("Depth keys", &keys, "16-bit linear (4 passes)\0" "24-bit float (6 passes)\0"
                                             "32-bit float (8 passes)\0"))
        splatSettings_.depthKeyBits = keys == 0 ? 16 : keys == 1 ? 24 : 32;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Sort key precision. 24-bit renders the same image as 32-bit on real scenes;\n"
                          "16-bit loses ~0.45 dB on a captured room (depth range clamping / steps).");
    }
    int blend = static_cast<int>(blendFormat_);  // enum order: RGBA8, RGB10A2, RGBA16F
    ImGui::Combo("Blending", &blend, "8-bit (fastest)\0" "10-bit\0" "16-bit float (most precise)\0");
    blendFormat_ = static_cast<Framebuffer::Format>(blend);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Precision of the buffer splats are blended into.\n8-bit rounds after every splat and loses "
                        "faint ones\n(-0.35 dB PSNR on a real scene) but blends at full speed on most GPUs.");
    ImGui::SliderInt("SH degree", &splatSettings_.shDegree, 0, 3);
    ImGui::SliderFloat("Splat scale", &splatSettings_.splatScale, 0.1f, 2.0f);
    ImGui::Checkbox("Oriented quads", &splatSettings_.orientedQuads);
    ImGui::SameLine();
    ImGui::Checkbox("Viewport culling", &splatSettings_.cullOutsideViewport);
  } else {
    ImGui::SliderFloat("Point size", &pointSize_, 1.0f, 12.0f, "%.1f px");
    ImGui::SliderFloat("Min opacity", &minOpacity_, 0.0f, 1.0f);
  }
  ImGui::ColorEdit3("Background", &background_.x);
  ImGui::BeginDisabled(hasOrientation_);  // with cameras the orientation comes from them
  if (ImGui::Checkbox("Flip Y (INRIA / COLMAP scenes)", &flipY_)) frameScene();
  ImGui::EndDisabled();
  if (ImGui::Checkbox("VSync", &vsync_)) glfwSwapInterval(vsync_ ? 1 : 0);

  // ---- Camera --------------------------------------------------------------
  ImGui::SeparatorText("Camera");
  const glm::vec3 p = camera_.position();
  ImGui::Text("pos (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
  int camMode = flyMode_ ? 1 : 0;
  ImGui::RadioButton("Orbit", &camMode, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Fly (F)", &camMode, 1);
  flyMode_ = camMode == 1;  // shared camera state: switching never moves the view
  ImGui::SameLine();
  if (ImGui::Button("Frame scene (R)")) frameScene();
  if (flyMode_) {
    ImGui::SliderFloat("Fly speed", &flySpeed_, 0.01f, 100.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("LMB/RMB look | MMB pan | wheel = speed\nWASD move, Q/E down/up, Shift = faster");
  } else {
    ImGui::TextDisabled("LMB orbit | RMB/MMB pan | wheel zoom\nWASD/QE move, Shift = faster");
  }

  // ---- Training cameras (COLMAP) -------------------------------------------------
  if (!colmap_.images.empty()) {
    ImGui::SeparatorText("Training cameras");
    ImGui::Text("%zu photos, %zu camera model(s)", colmap_.images.size(), colmap_.cameras.size());
    ImGui::TextDisabled("%s", colmapDir_.c_str());
    ImGui::Checkbox("Show frustums", &showCameras_);
    if (ImGui::Button("< Prev")) goToCamera(cameraIndex_ - 1);
    ImGui::SameLine();
    if (ImGui::Button("Next (C) >")) goToCamera(cameraIndex_ + 1);
    if (cameraIndex_ >= 0) {
      ImGui::SameLine();
      ImGui::Text("%s", colmap_.images[static_cast<size_t>(cameraIndex_)].name.c_str());
    }
  }

  drawEditPanel();  // edit_ui.cpp (collapsible)

  // ---- Dev -----------------------------------------------------------------
  ImGui::SeparatorText("Dev");
  if (ImGui::Button("Reload shaders (F5)")) reloadShaders();
  if (!shaderLog_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", shaderLog_.c_str());
  ImGui::TextDisabled("%s", glRenderer_.c_str());
  ImGui::End();

  if (showCameras_) drawCameras();
  drawSelection();
}

// Draws each training camera as a small pyramid (frustum) with ImGui's background draw list:
// projected on the CPU, so no extra GL pipeline is needed for a few hundred line segments.
void App::drawCameras() {
  if (colmap_.images.empty()) return;
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  if (size.x <= 0 || size.y <= 0) return;
  const glm::mat4 viewProj = camera_.projection(size.x / size.y) * camera_.view() * modelMatrix();
  auto toScreen = [&](const glm::vec3& world, ImVec2& out) {
    const glm::vec4 c = viewProj * glm::vec4(world, 1.0f);
    if (c.w <= 1e-6f) return false;  // behind the eye
    out = ImVec2((c.x / c.w * 0.5f + 0.5f) * size.x, (0.5f - c.y / c.w * 0.5f) * size.y);
    return true;
  };

  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const float depth = 0.03f * glm::length(scene_.boundsMax - scene_.boundsMin);  // pyramid size
  for (size_t i = 0; i < colmap_.images.size(); ++i) {
    const ColmapImage& im = colmap_.images[i];
    const ColmapCamera* cam = colmap_.camera(im.cameraId);
    if (!cam) continue;
    const glm::dvec4 k = cam->pinhole();  // fx, fy, cx, cy
    const glm::dmat3 camToWorld = glm::transpose(im.worldToCameraRotation());
    const glm::dvec3 c = im.center();
    // Image corners on the plane z = depth in camera space (COLMAP: x right, y down, z forward).
    const double u[4] = {0, double(cam->width), double(cam->width), 0}, v[4] = {0, 0, double(cam->height), double(cam->height)};
    ImVec2 apex, corner[4];
    bool ok = toScreen(glm::vec3(c), apex);
    for (int j = 0; j < 4 && ok; ++j) {
      const glm::dvec3 p((u[j] - k.z) / k.x * depth, (v[j] - k.w) / k.y * depth, depth);
      ok = toScreen(glm::vec3(c + camToWorld * p), corner[j]);
    }
    if (!ok) continue;
    const bool current = static_cast<int>(i) == cameraIndex_;
    const ImU32 color = current ? IM_COL32(255, 210, 60, 255) : IM_COL32(90, 200, 255, 160);
    const float width = current ? 2.5f : 1.0f;
    for (int j = 0; j < 4; ++j) {
      dl->AddLine(apex, corner[j], color, width);
      dl->AddLine(corner[j], corner[(j + 1) % 4], color, width);
    }
    dl->AddLine(corner[0], corner[1], current ? color : IM_COL32(255, 120, 90, 200), width * 2.0f);  // image top
  }
}

}  // namespace gsv
