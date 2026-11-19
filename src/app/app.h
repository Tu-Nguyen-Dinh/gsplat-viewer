#pragma once

#include "app/options.h"
#include "core/camera.h"
#include "gl/gpu_timer.h"
#include "render/point_renderer.h"
#include "render/splat_renderer.h"
#include "scene/gaussian_scene.h"

#include <string>

struct GLFWwindow;

namespace gsv {

enum class RenderMode : int { Splats = 0, Points = 1 };

// Interactive viewer: window, input, UI and the frame loop.
class App {
 public:
  int run(const Options& options);

 private:
  bool createWindow(const Options& options);
  void shutdown();
  void frame(int fbWidth, int fbHeight);

  void setScene(GaussianScene&& scene);
  void loadSceneFile(const std::string& utf8Path);
  void frameScene();
  void reloadShaders();
  RenderView makeRenderView(int fbWidth, int fbHeight) const;
  glm::mat4 modelMatrix() const;

  void handleInput(float dt, int fbHeight);
  void drawUi();  // ui.cpp

  static void onDrop(GLFWwindow* window, int count, const char** paths);
  static void onScroll(GLFWwindow* window, double dx, double dy);

  GLFWwindow* window_ = nullptr;
  GaussianScene scene_;
  OrbitCamera camera_;
  SplatRenderer splats_;
  PointRenderer points_;
  GpuTimer pointTimer_;

  // Settings edited by the UI.
  RenderMode mode_ = RenderMode::Splats;
  SplatSettings splatSettings_;
  bool flipY_ = false;
  bool vsync_ = true;
  bool showUi_ = true;
  float pointSize_ = 2.0f;
  float minOpacity_ = 0.05f;
  glm::vec3 background_{0.07f, 0.08f, 0.1f};

  // Input / status.
  std::string pendingLoad_;
  std::string status_;
  std::string shaderLog_;
  std::string glRenderer_;
  double loadMs_ = 0.0;
  float scrollAccum_ = 0.0f;
  double lastX_ = 0.0, lastY_ = 0.0;
  bool keyWasDown_[512] = {};
  char pathInput_[1024] = {};
};

}  // namespace gsv
