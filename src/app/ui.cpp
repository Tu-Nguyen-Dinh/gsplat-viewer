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
  if (ImGui::Button("Load") || enter) pendingLoad_ = pathInput_;
  ImGui::TextDisabled("or drag & drop a .ply onto the window");
  if (ImGui::Button("Demo scene")) {
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
  if (ImGui::Checkbox("Flip Y (INRIA / COLMAP scenes)", &flipY_)) frameScene();
  if (ImGui::Checkbox("VSync", &vsync_)) glfwSwapInterval(vsync_ ? 1 : 0);

  // ---- Camera --------------------------------------------------------------
  ImGui::SeparatorText("Camera");
  const glm::vec3 p = camera_.position();
  ImGui::Text("pos (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
  if (ImGui::Button("Frame scene (F)")) frameScene();
  ImGui::TextDisabled("LMB orbit | RMB/MMB pan | wheel zoom\nWASD/QE move, Shift = faster");

  // ---- Dev -----------------------------------------------------------------
  ImGui::SeparatorText("Dev");
  if (ImGui::Button("Reload shaders (F5)")) reloadShaders();
  if (!shaderLog_.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", shaderLog_.c_str());
  ImGui::TextDisabled("%s", glRenderer_.c_str());
  ImGui::End();
}

}  // namespace gsv
