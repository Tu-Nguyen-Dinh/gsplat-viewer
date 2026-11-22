#pragma once

#include "app/options.h"
#include "core/camera.h"
#include "gl/framebuffer.h"
#include "gl/gpu_timer.h"
#include "core/geo_frame.h"
#include "core/kdtree.h"
#include "core/image.h"
#include "core/settings.h"
#include "render/cloud_renderer.h"
#include "render/point_renderer.h"
#include "render/splat_renderer.h"
#include "scene/colmap_io.h"
#include "scene/gaussian_scene.h"
#include "scene/scene_edit.h"
#include "scene/edit_history.h"
#include "scene/edit_ops.h"
#include "scene/project_file.h"
#include "scene/lod.h"
#include "scene/point_cloud.h"
#include "scene/scene_io.h"

#include <functional>
#include <future>
#include <optional>
#include <unordered_map>
#include <string>

struct GLFWwindow;

namespace gsv {

enum class RenderMode : int { Splats = 0, Points = 1 };

// Interactive tools (tools_ui.cpp, Core 5). Keys 1-9 pick them; Esc goes back to Navigate.
enum class Tool : int { Navigate = 0, Rect, Lasso, Brush, Sphere, Flood, Transform, Measure, Annotate, Count };

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
  void applyRemoval(const SplatMask& mask, const char* what);  // recorded in history_
  void undoEdit();
  void redoEdit();
  void uploadScene();                                         // after an edit: no reframing
  void applyBatchEdits();                                     // --min-opacity / --crop-sphere / --save-edited

  // Layers, measuring and LOD (layers_ui.cpp, Core 1-3; see docs/core/INDEX.md).
  void loadCloudFile(const std::string& utf8Path);
  GeoFrame cloudToDisplay() const;            // cloud local offsets -> display (world) coordinates
  void frameCloud();
  void drawLayersPanel();
  void setLodEnabled(bool on);
  void updateLod();

  // Interactive tools (tools_ui.cpp, Core 5; see docs/core/CORE5-editing.md).
  void drawToolbar();
  void drawToolPanel();
  void drawProjectPanel();
  void drawToolOverlay(const RenderView& view);
  void handleToolInput(bool lmb, double x, double y, bool clicked, bool shift, bool alt);
  void handleToolKeys(bool ctrl, bool shift, bool alt);
  void setTool(Tool t);
  DepthImage renderSplatDepth();   // expected depth of the current view (visible-only selection)
  void applyVisibleOnly(SplatMask& mask);
  void growOrShrink(bool grow);
  bool keyPressed(int glfwKey);  // rising edge, shares keyWasDown_ with handleInput
  std::optional<glm::vec3> pickSurface(double cursorX, double cursorY, double* ms = nullptr);  // display coords
  glm::mat4 sceneToDisplay() const { return modelMatrix(); }
  glm::vec3 toScene(const glm::vec3& display) const;
  glm::vec3 toDisplay(const glm::vec3& scenePoint) const;
  glm::mat4 screenMvp() const;  // scene -> clip for window-pixel selection
  void setSelection(const SplatMask& mask, SelectOp op, const char* what);
  void selectionChanged();
  void syncSelectionFlags();
  void finishLiveEdit(bool commit);
  void liveTransformPreview(bool final);
  void livePreviewUpload(bool final);
  void deleteSelection();
  void hideSelection();
  void isolateSelection();
  void unhideAll();
  void autoCleanFloaters();
  void levelFromSelection();
  void addMeasurementPoint(const glm::vec3& displayPoint);
  void finishMeasurement();
  void addAnnotation(const glm::vec3& displayPoint);
  ViewBookmark currentView(const std::string& name) const;
  void goToBookmark(const ViewBookmark& b);
  void saveProjectTo(const std::string& path);
  void openProject(const std::string& path);
  std::string defaultProjectPath() const;

  // Application shell (shell_ui.cpp, Phase 6; docs/core/CORE6-ux.md): menu bar, status bar,
  // welcome screen with recent files, shortcut sheet, first-run tour, settings, fonts / DPI.
  void loadUserSettings();
  void saveUserSettings();
  void setupFonts();                         // (re)builds the font atlas for uiScale_
  float effectiveUiScale() const;
  void drawMenuBar();
  void drawStatusBar();
  void drawWelcome();
  void drawShortcuts();
  void drawTour();
  void drawPathPrompt();                     // fallback when there is no native dialog
  void openAnyFile(const std::string& utf8Path);  // .gsvproj / .las / scene, by extension
  void showOpenDialog(int kind);             // 0 scene/any, 1 project
  void showSaveProjectDialog();
  void showSaveEditedDialog();
  void rememberRecent(const std::string& utf8Path);
  void captureThumbnail();                   // after a scene loaded: small image for the welcome screen
  unsigned int thumbnailTexture(const std::string& utf8Path);
  float menuBarHeight_ = 0.0f, statusBarHeight_ = 0.0f;
  float panelRight_ = 0.0f;                  // right edge of the side panel (0 when hidden)

  // Data safety (session_ui.cpp, Phase 6 batch 2): unsaved-changes tracking, prompts before losing
  // work, background saving, autosave + crash recovery, history panel, cancelling long operations.
  bool sceneDirty() const { return history_.revision() != savedRevision_; }
  bool projectDirty() const;
  void markSceneSaved() { savedRevision_ = history_.revision(); }
  void markProjectSaved();
  void requestDiscard(std::function<void()> action);  // runs `action` now, or after the user agreed
  void drawConfirmDiscard();
  bool saveEverything();                             // project + edited scene, synchronously
  void startSceneSave(const std::string& utf8Path);  // background thread, on a copy
  void pollBackgroundJobs();
  void autosaveTick();
  void clearRecovery();
  void checkRecoveryAtStartup();
  void drawRecoveryPrompt();
  void recoverSession();
  void applyProject(const Project& p, const std::string& projectFile);
  void drawHistoryPanel();
  void cancelLoading();
  static void onWindowClose(GLFWwindow* window);

  // Navigation and responsiveness (nav_ui.cpp, Phase 6 batch 3).
  void animateCameraTo(const OrbitCamera& end, float seconds = 0.35f);
  void updateCameraAnimation();
  void quickView(int axis, bool negative);  // 0 X (right), 1 Y (top), 2 Z (front)
  void frameSelection();
  void drawCompass();
  void renderMainView(int fbW, int fbH, float dt);  // auto quality while the camera moves

  // Export (export_ui.cpp, Phase 6 batch 5).
  bool renderToPng(const std::string& utf8Path, int w, int h, const OrbitCamera* cam, std::string& error);
  void exportPicture(int scaleMode);  // 0 window, 1 2x, 2 4K
  void startVideoExport();
  void stepVideoExport();
  void exportReport();
  void drawExportProgress();
  void drawExportMenu();

  static void onDrop(GLFWwindow* window, int count, const char** paths);
  static void onMouseButton(GLFWwindow* window, int button, int action, int mods);
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
  EditHistory history_;             // multi-level undo / redo (Ctrl+Z / Ctrl+Y)
  // Interactive tools.
  Tool tool_ = Tool::Navigate;
  SplatMask selected_, hidden_;     // scene order; resized when the scene changes
  size_t selectedCount_ = 0, hiddenCount_ = 0;
  bool flagsDirty_ = true;
  bool toolDragging_ = false;
  SelectOp dragOp_ = SelectOp::Replace;
  SplatMask dragBase_, dragStroke_;
  glm::vec2 dragStart_{0.0f}, dragLast_{0.0f};
  std::vector<glm::vec2> lasso_;
  double lastFlagsUpload_ = 0.0;
  float brushRadiusPx_ = 40.0f;
  float sphereRadius_ = 0.05f;      // fraction of the scene's focus radius
  float floodGap_ = 5.0f;           // multiple of the typical splat spacing
  float floodColor_ = 0.3f;
  float floodRadius_ = 0.0f;        // fraction of the focus radius, 0 = no limit
  bool visibleOnly_ = true;         // Rect / Lasso / Brush skip splats hidden behind a surface
  DepthImage strokeDepth_;          // depth of the view for the current brush stroke
  KdTree editTree_;                 // splat centres, for Flood / auto-clean; rebuilt after edits
  bool editTreeValid_ = false;
  float spacing_ = 0.0f;            // typicalSpacing() of the scene
  const KdTree& editTree();
  std::optional<glm::vec3> hover_;  // surface under the cursor (Sphere tool), display coords
  double lastHoverPick_ = 0.0;
  double hoverX_ = -1.0, hoverY_ = -1.0;
  // Transform gizmo (display space).
  int gizmoHot_ = -1, gizmoActive_ = -1;  // 0..2 axis, 3 free move, 4 Y ring
  glm::vec3 gizmoPivot_{0.0f};
  glm::vec3 gizmoMove_{0.0f};
  float gizmoAngle_ = 0.0f, gizmoStartAngle_ = 0.0f;
  float gizmoScale_ = 1.0f;
  double lastPreview_ = 0.0;
  bool previewPending_ = false;
  float numMove_[3] = {0, 0, 0}, numRotate_[3] = {0, 0, 0}, numScale_ = 1.0f;
  ColorAdjust colorAdjust_;
  // Project: measurements, notes, bookmarks (scene coordinates), unit, level.
  Project project_;
  glm::mat4 level_{1.0f};           // from project_.level
  MeasurementRecord::Kind measureKind_ = MeasurementRecord::Kind::Distance;
  std::vector<glm::dvec3> measureDraft_;  // scene coordinates
  int scaleSource_ = -1;
  float realLength_ = 1.0f;
  int editAnnotation_ = -1;
  char projectPath_[1024] = {};
  char noteTitle_[128] = {}, noteText_[1024] = {};
  float floaterRatio_ = 2.5f;
  int floaterViews_ = 1;
  bool pivotDirty_ = true;
  // Shell state.
  Settings settings_;
  bool fontsDirty_ = true;
  float appliedUiScale_ = 0.0f;
  bool showWelcome_ = false, showShortcuts_ = false, showAbout_ = false;
  int tourStep_ = -1;                       // -1: tour not shown
  bool thumbnailPending_ = false;
  int thumbnailDelay_ = 0;                  // frames to wait before capturing
  std::unordered_map<std::string, unsigned int> thumbTextures_;
  bool interactive_ = false;                // false for screenshots / benchmarks: settings untouched
  struct CameraAnimation {
    bool active = false;
    double start = 0.0;
    float duration = 0.35f;
    OrbitCamera from, to;
  } camAnim_;
  Framebuffer lowRes_;                      // auto quality: reduced-resolution render target
  float renderScale_ = 1.0f;
  glm::mat4 lastView_{0.0f};
  double lastMotion_ = 0.0;
  bool showCompass_ = true;
  struct VideoExport {
    bool active = false;
    int next = 0, total = 0, width = 1920, height = 1080;
    std::string mp4, framesDir;
  } video_;
  float videoSecondsPerView_ = 3.0f;
  int videoWidth_ = 1920;
  std::string historyDir_;
  bool forceOpen_ = false;                  // openAnyFile after the unsaved-changes prompt
  std::string windowTitle_;
  bool showPanel_ = true;                   // the side panel (View menu); the shell stays
  bool tourOffered_ = false;
  std::string pendingProject_;              // dropped .gsvproj, opened on the main loop
  uint64_t savedRevision_ = 0;
  std::string savedProjectJson_;
  std::function<void()> afterConfirm_;      // pending action behind the "unsaved changes" prompt
  bool confirmOpen_ = false;
  std::future<std::string> saveJob_;        // background scene save: "" = ok, else the error
  std::string saveJobPath_;
  uint64_t saveJobRevision_ = 0;
  std::future<std::string> autosaveJob_;
  double lastAutosave_ = 0.0;
  uint64_t autosavedRevision_ = 0;
  std::string autosavedProjectJson_;
  bool recoveryPrompt_ = false;
  std::string recoveryInfo_;
  std::string recoverOriginalPath_;         // set while a recovered scene loads: its real file name
  std::vector<std::future<LoadResult>> abandonedLoads_;  // cancelled loads, dropped when they finish
  std::vector<std::future<SplatMask>> abandonedCleans_;
  int pathPromptKind_ = -1;                 // -1 none, 0 open, 1 open project, 2 save project, 3 save PLY
  char pathPrompt_[1024] = {};
  std::future<SplatMask> cleanJob_;  // auto-clean runs off the UI thread
  size_t cleanJobSize_ = 0;
  double cleanJobStart_ = 0.0;
  std::string scenePath_;           // UTF-8 path of the loaded file
  char savePath_[1024] = {};
  // Fly mode (F): mouse looks around the eye, the wheel sets the speed instead of zooming.
  bool flyMode_ = false;
  float flySpeed_ = 1.0f;  // multiplier on the scene-size based speed
  bool keyWasDown_[512] = {};
  // Point-cloud layer.
  PointCloud cloud_;
  CloudRenderer cloudRenderer_;
  CloudSettings cloudSettings_;
  bool haveCloud_ = false, showCloud_ = true;
  bool hasCloudTransform_ = false;
  GeoFrame cloudTransform_;                 // LAS world -> scene coordinates (--las-transform)
  std::string pendingCloud_;
  // Left-button press/release from the GLFW callback (polling once per frame misses quick clicks).
  double pressX_ = 0.0, pressY_ = 0.0;
  bool pressPending_ = false, clickReady_ = false;
  double clickX_ = 0.0, clickY_ = 0.0;
  // Level of detail (--lod / Layers panel).
  bool lodEnabled_ = false;
  LodTree lodTree_;
  long long lodBudget_ = 2000000;
  float lodPixelError_ = 1.5f;
  glm::vec3 lodLastEye_{1e30f};
  std::future<GaussianScene> lodPending_;
  size_t lodShown_ = 0;
  double lodBuildMs_ = 0.0, lodCutMs_ = 0.0;
  double lodCutStart_ = 0.0;
  char pathInput_[1024] = {};
};

}  // namespace gsv
