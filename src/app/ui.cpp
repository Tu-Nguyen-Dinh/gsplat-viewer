// Dear ImGui control panel of the viewer.
#include "app/app.h"
#include "core/i18n.h"
#include "core/utf8_path.h"

#include <filesystem>
#include "scene/demo_scene.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>

namespace gsv {

void App::drawUi() {
  if (!showPanel_) {
    panelRight_ = 0.0f;
    drawToolbar();
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  const float s = std::max(appliedUiScale_, 1.0f);
  ImGui::SetNextWindowPos(ImVec2(8.0f, menuBarHeight_ + 8.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(400.0f * s, io.DisplaySize.y - menuBarHeight_ - statusBarHeight_ - 16.0f), ImGuiCond_FirstUseEver);
  ImGui::Begin(trId("Panel"), &showPanel_);
  panelRight_ = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
  auto hint = [](const char* text) {  // greyed, wrapped help text
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
  };

  // ---- Scene ---------------------------------------------------------------
  ImGui::SeparatorText(tr("Scene"));
  if (scene_.empty() && !isLoading()) {
    ImGui::TextDisabled("%s", tr("No scene loaded"));
  } else {
    ImGui::TextUnformatted(scene_.sourceName.c_str());
    ImGui::TextDisabled(tr("%zu splats, SH degree %d"), scene_.size(), scene_.shDegree);
  }
  ImGui::BeginDisabled(isLoading());
  if (ImGui::Button(trId("Open..."))) showOpenDialog(0);
  ImGui::SameLine();
  if (ImGui::Button(trId("Recent"))) ImGui::OpenPopup("##recent");
  ImGui::SameLine();
  if (ImGui::Button(trId("Demo scene"))) {
    requestDiscard([this] {
      loadColmapFor("", "");  // the demo has no cameras
      setScene(makeDemoScene());
    });
    status_ = tr("Demo scene");
  }
  ImGui::EndDisabled();
  if (ImGui::BeginPopup("##recent")) {
    if (settings_.recentFiles.empty()) ImGui::TextDisabled("%s", tr("Nothing yet: the files you open appear here."));
    std::string open;
    for (const std::string& f : settings_.recentFiles)
      if (ImGui::Selectable(f.c_str())) open = f;
    ImGui::EndPopup();
    if (!open.empty()) openAnyFile(open);
  }
  if (isLoading()) {
    static const char kSpinner[] = "|/-\\";
    const double t = glfwGetTime();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), tr("%c Loading %s ... %.1f s"), kSpinner[int(t * 8) & 3],
                       loadingName_.c_str(), t - loadingStart_);
    ImGui::SameLine();
    if (ImGui::SmallButton(trId("Cancel"))) cancelLoading();
  }

  // ---- View ------------------------------------------------------------------
  if (ImGui::CollapsingHeader(trId("View"), ImGuiTreeNodeFlags_DefaultOpen)) {
    int mode = static_cast<int>(mode_);
    ImGui::RadioButton(trId("Gaussian splats"), &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(trId("Points (Tab)"), &mode, 1);
    mode_ = static_cast<RenderMode>(mode);
    if (mode_ == RenderMode::Splats) {
      ImGui::SliderInt(trId("Colour detail (SH)"), &splatSettings_.shDegree, 0, 3);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Spherical harmonics degree: 3 = full view-dependent colour, 0 = fastest"));
    } else {
      ImGui::SliderFloat(trId("Point size"), &pointSize_, 1.0f, 12.0f, "%.1f px");
    }
    ImGui::ColorEdit3(trId("Background"), &background_.x, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::BeginDisabled(hasOrientation_);  // with cameras the orientation comes from them
    if (ImGui::Checkbox(trId("Upside down? Flip Y"), &flipY_)) {
      frameScene();
      if (!scenePath_.empty()) {  // remembered for this file (settings)
        std::error_code ec;
        const std::string abs = pathToUtf8(std::filesystem::absolute(pathFromUtf8(scenePath_), ec));
        setFlipY(settings_, ec ? scenePath_ : abs, flipY_);
        saveUserSettings();
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", tr("Scenes from INRIA / COLMAP are often stored upside down"));
    int camMode = flyMode_ ? 1 : 0;
    ImGui::RadioButton(trId("Orbit"), &camMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(trId("Fly (F)"), &camMode, 1);
    flyMode_ = camMode == 1;  // shared camera state: switching never moves the view
    ImGui::SameLine();
    if (ImGui::Button(trId("Frame scene (R)"))) frameScene();
    if (flyMode_) {
      ImGui::SliderFloat(trId("Fly speed"), &flySpeed_, 0.01f, 100.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
      hint(tr("Drag to look | middle drag pan | wheel = speed\nWASD move, Q/E down/up, Shift = faster"));
    } else {
      hint(tr("Left drag orbit | right / middle drag pan | wheel zoom\nWASD/QE move, Shift = faster"));
    }
  }

  // ---- Training cameras (COLMAP) -------------------------------------------------
  if (!colmap_.images.empty() && ImGui::CollapsingHeader(trId("Training cameras"))) {
    ImGui::Text(tr("%zu photos, %zu camera model(s)"), colmap_.images.size(), colmap_.cameras.size());
    ImGui::TextDisabled("%s", colmapDir_.c_str());
    ImGui::Checkbox(trId("Show frustums"), &showCameras_);
    if (ImGui::Button(trId("< Prev"))) goToCamera(cameraIndex_ - 1);
    ImGui::SameLine();
    if (ImGui::Button(trId("Next (C) >"))) goToCamera(cameraIndex_ + 1);
    if (cameraIndex_ >= 0) {
      ImGui::SameLine();
      ImGui::Text("%s", colmap_.images[static_cast<size_t>(cameraIndex_)].name.c_str());
    }
  }

  drawToolPanel();  // tools_ui.cpp: selection actions, transform, colour, measurements
  drawEditPanel();  // edit_ui.cpp: crop volume and filters (collapsible)
  drawProjectPanel();  // tools_ui.cpp: notes, bookmarks, project file
  drawHistoryPanel();  // session_ui.cpp: every undo step, click to jump
  drawLayersPanel();  // layers_ui.cpp: point cloud, measuring, LOD

  // ---- Advanced (performance, renderer internals) ------------------------------------
  if (ImGui::CollapsingHeader(trId("Advanced"))) {
    ImGui::SeparatorText(tr("Performance"));
    ImGui::Text(tr("FPS %.1f   frame %.2f ms"), io.Framerate, 1000.0f / std::max(io.Framerate, 0.001f));
    if (mode_ == RenderMode::Splats) {
      const SplatStats& st = splats_.stats();
      ImGui::Text(tr("Visible    %u / %u (%.0f%%)"), st.visible, st.total, st.total ? 100.0 * st.visible / st.total : 0.0);
      ImGui::Text("GPU preprocess %.3f ms", st.preprocessMs);
      ImGui::Text("GPU draw       %.3f ms", st.drawMs);
      if (splatSettings_.sort == SortMode::GpuRadix)
        ImGui::Text("GPU sort       %.3f ms", st.gpuSortMs);
      else if (splatSettings_.sort != SortMode::None)
        ImGui::Text("CPU sort       %.3f ms + upload %.3f ms%s", st.cpuSortMs, st.uploadMs, st.sortSkipped ? "  (cached)" : "");
    } else {
      ImGui::Text("GPU points     %.3f ms", pointTimer_.milliseconds());
    }
    ImGui::Text(tr("Load time  %.0f ms"), loadMs_);
    ImGui::SeparatorText(tr("Renderer"));
    if (mode_ == RenderMode::Splats) {
      int view = static_cast<int>(splatSettings_.debugView);
      ImGui::Combo(trId("Debug view"), &view, "Color\0Depth\0Overdraw\0Ellipses\0");
      splatSettings_.debugView = static_cast<DebugView>(view);
      int sort = static_cast<int>(splatSettings_.sort);
      ImGui::Combo(trId("Sort"), &sort, "None (wrong order)\0CPU std::sort\0CPU radix sort\0GPU radix sort\0");
      splatSettings_.sort = static_cast<SortMode>(sort);
      if (splatSettings_.sort == SortMode::GpuRadix) {
        int keys = splatSettings_.depthKeyBits == 16 ? 0 : splatSettings_.depthKeyBits == 24 ? 1 : 2;
        if (ImGui::Combo(trId("Depth keys"), &keys, "16-bit linear (4 passes)\0" "24-bit float (6 passes)\0"
                                                    "32-bit float (8 passes)\0"))
          splatSettings_.depthKeyBits = keys == 0 ? 16 : keys == 1 ? 24 : 32;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Sort key precision. 24-bit renders the same image as 32-bit on real scenes;\n"
                            "16-bit loses ~0.45 dB on a captured room (depth range clamping / steps).");
      }
      int blend = static_cast<int>(blendFormat_);  // enum order: RGBA8, RGB10A2, RGBA16F
      ImGui::Combo(trId("Blending"), &blend, "8-bit (fastest)\0" "10-bit\0" "16-bit float (most precise)\0");
      blendFormat_ = static_cast<Framebuffer::Format>(blend);
      ImGui::SliderFloat(trId("Splat scale"), &splatSettings_.splatScale, 0.1f, 2.0f);
      ImGui::Checkbox(trId("Oriented quads"), &splatSettings_.orientedQuads);
      ImGui::SameLine();
      ImGui::Checkbox(trId("Viewport culling"), &splatSettings_.cullOutsideViewport);
    } else {
      ImGui::SliderFloat(trId("Min opacity"), &minOpacity_, 0.0f, 1.0f);
    }
    if (ImGui::Checkbox(trId("VSync"), &vsync_)) glfwSwapInterval(vsync_ ? 1 : 0);
    ImGui::SetNextItemWidth(-60.0f * s);
    const bool enter = ImGui::InputText("##path", pathInput_, sizeof(pathInput_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(trId("Load")) || enter) openAnyFile(pathInput_);
    if (ImGui::Button(trId("Reload shaders (F5)"))) reloadShaders();
    if (!shaderLog_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", shaderLog_.c_str());
    ImGui::TextDisabled("%s", glRenderer_.c_str());
  }
  ImGui::End();

  if (showCameras_) drawCameras();
  drawSelection();
  drawToolbar();  // tools_ui.cpp: floating tool strip
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
