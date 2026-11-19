#pragma once

#include "app/options.h"
#include "core/camera.h"
#include "gl/gpu_timer.h"
#include "render/point_renderer.h"
#include "render/splat_renderer.h"
#include "scene/gaussian_scene.h"
#include "scene/scene_io.h"

#include <future>
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
  void frame(GLuint framebuffer, int width, int height);  // renders the scene into `framebuffer` (0 = window)

  void setScene(GaussianScene&& scene);
  void loadSceneFile(const std::string& utf8Path);  // starts a background load
  void pollLoading();                               // finishes it on the GL thread
  bool isLoading() const { return loading_.valid(); }
  void frameScene();
  void reloadShaders();
  RenderView makeRenderView(int fbWidth, int fbHeight) const;
  glm::mat4 modelMatrix() const;

  void handleInput(float dt, int fbHeight);
  void drawUi();                             // ui.cpp
  int runBenchmark(const Options& options);  // benchmark.cpp
  int runTurntable(const Options& options);  // benchmark.cpp

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

  float distanceScale_ = 1.0f;  // multiplier on the framing distance (--distance)
  bool fixedCamera_ = false;    // --camera-pos / --camera-target
  glm::vec3 fixedEye_{0.0f}, fixedTarget_{0.0f};

  // Background loading.
  std::future<LoadResult> loading_;
  std::string loadingName_;
  double loadingStart_ = 0.0;

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
