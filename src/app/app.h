#pragma once

#include "app/options.h"
#include "core/camera.h"
#include "gl/framebuffer.h"
#include "gl/gpu_timer.h"
#include "render/point_renderer.h"
#include "render/splat_renderer.h"
#include "scene/colmap_io.h"
#include "scene/gaussian_scene.h"
#include "scene/scene_edit.h"
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
  int runFlythrough(const Options& options); // benchmark.cpp: along the COLMAP cameras
  int runEvaluation(const Options& options);  // evaluate.cpp

  // COLMAP cameras of the current scene (scenes trained by tools/capture.ps1).
  void loadColmapFor(const std::string& sceneUtf8Path, const std::string& explicitDir);
  void goToCamera(int index);  // view the scene from a training camera
  void drawCameras();          // ui.cpp: frustums on top of the scene

  // Editing (edit_ui.cpp): selection volume, delete / crop / filter, one-step undo, save.
  void drawEditPanel();
  void drawSelection();                                       // wireframe of the volume
  void resetSelection();                                      // around the scene's focus
  void applyRemoval(const SplatMask& mask, const char* what);  // keeps the old scene for undo
  void undoEdit();
  void uploadScene();                                         // after an edit: no reframing
  void applyBatchEdits();                                     // --min-opacity / --crop-sphere / --save-edited

  static void onDrop(GLFWwindow* window, int count, const char** paths);
  static void onScroll(GLFWwindow* window, double dx, double dy);

  GLFWwindow* window_ = nullptr;
  Options options_;  // command line, kept for the edits applied once the scene has loaded
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
  // Precision of the target splats blend into (--blend). The window is 8-bit, which rounds after
  // every blend and loses faint splats (-0.35 dB on a real scene); with a finer format the splats
  // go through sceneTarget_ and a single blit converts the result for the window.
  Framebuffer::Format blendFormat_ = Framebuffer::Format::RGBA16F;
  Framebuffer sceneTarget_;
  Framebuffer::Format targetFormat() const { return blendFormat_; }

  ColmapModel colmap_;
  std::string colmapDir_;           // UTF-8; empty when the scene has no cameras
  std::string colmapOverride_;      // --colmap
  glm::mat4 orientation_{1.0f};     // rotates the cameras' average "up" onto +Y
  bool hasOrientation_ = false;
  bool showCameras_ = true;
  int cameraIndex_ = -1;

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
  // Editing state.
  SelectionShape selection_;        // in display space (after modelMatrix())
  bool showSelection_ = false;
  size_t selectionCount_ = 0;       // splats inside, refreshed when the shape changes
  bool selectionDirty_ = true;
  float filterOpacity_ = 0.02f;
  float filterScale_ = 0.0f;        // 0 = off
  size_t filterCount_ = 0;
  bool selectionDirtyForFilter_ = true;
  GaussianScene undoScene_;         // the scene before the last edit (one-step undo)
  std::string undoLabel_;
  std::string scenePath_;           // UTF-8 path of the loaded file
  char savePath_[1024] = {};
  // Fly mode (F): mouse looks around the eye, the wheel sets the speed instead of zooming.
  bool flyMode_ = false;
  float flySpeed_ = 1.0f;  // multiplier on the scene-size based speed
  bool keyWasDown_[512] = {};
  char pathInput_[1024] = {};
};

}  // namespace gsv
