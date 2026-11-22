#include "app/app.h"

#include "gl/framebuffer.h"
#include "gl/gl_loader.h"
#include "core/i18n.h"
#include "core/utf8_path.h"
#include "scene/demo_scene.h"
#include "scene/ply_io.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>  // glm::rotation(from, to)
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <cstdio>

namespace gsv {

namespace {

#ifndef NDEBUG
void APIENTRY onGlDebug(GLenum, GLenum, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
  std::fprintf(stderr, "[GL] %s\n", message);
}
#endif

App* appFrom(GLFWwindow* w) { return static_cast<App*>(glfwGetWindowUserPointer(w)); }

}  // namespace

// --------------------------------------------------------------------------------------
// Setup / teardown
// --------------------------------------------------------------------------------------

bool App::createWindow(const Options& o) {
  if (!glfwInit()) {
    std::fprintf(stderr, "glfwInit failed\n");
    return false;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifndef NDEBUG
  glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif
  if (!o.screenshot.empty()) glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  window_ = glfwCreateWindow(o.width, o.height, "gsplat-viewer", nullptr, nullptr);
  if (!window_) {
    std::fprintf(stderr, "Could not create an OpenGL 4.3 core context (update your GPU driver)\n");
    return false;
  }
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);

  std::string missing;
  if (!gl::loadFunctions([](const char* n) { return reinterpret_cast<void*>(glfwGetProcAddress(n)); }, missing)) {
    std::fprintf(stderr, "Missing OpenGL functions: %s\n", missing.c_str());
    return false;
  }
  glRenderer_ = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  std::printf("OpenGL: %s | %s\n", glGetString(GL_VERSION), glRenderer_.c_str());
#ifndef NDEBUG
  glEnable(GL_DEBUG_OUTPUT);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  glDebugMessageCallback(onGlDebug, nullptr);
#endif

  // Callbacks reach the App through the window user pointer (no globals).
  // They must be set before ImGui installs its own, which chain to ours.
  glfwSetWindowUserPointer(window_, this);
  glfwSetDropCallback(window_, onDrop);
  glfwSetWindowCloseCallback(window_, onWindowClose);  // unsaved changes: ask first (session_ui.cpp)
  glfwSetScrollCallback(window_, onScroll);
  glfwSetMouseButtonCallback(window_, onMouseButton);
  // Sticky input: a key or button tapped between two frames still reads as pressed once. Heavy
  // scenes can drop to a few FPS, where polling alone lost quick taps (tool keys, Delete...).
  glfwSetInputMode(window_, GLFW_STICKY_KEYS, GLFW_TRUE);
  glfwSetInputMode(window_, GLFW_STICKY_MOUSE_BUTTONS, GLFW_TRUE);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  // Window layout next to the settings, not in the current folder (it littered repositories).
  static std::string iniPath;
  iniPath = pathToUtf8(pathFromUtf8(settingsDirectory()) / "imgui.ini");
  ImGui::GetIO().IniFilename = iniPath.c_str();
  ImGui_ImplGlfw_InitForOpenGL(window_, true);
  ImGui_ImplOpenGL3_Init("#version 430");
  return true;
}

void App::shutdown() {
  if (!window_) return;
  if (interactive_) {
    saveUserSettings();  // interactive runs only (screenshots / benchmarks keep the settings)
    if (saveJob_.valid()) saveJob_.wait();  // a background save must finish before we exit
    if (!recoveryPrompt_) clearRecovery();  // clean exit: nothing to recover next time
    history_.clear();                       // deletes its files
    std::error_code ec;
    if (!historyDir_.empty()) std::filesystem::remove_all(pathFromUtf8(historyDir_), ec);
  }
  for (auto& kv : thumbTextures_) {
    GLuint tex = kv.second;
    if (tex) glDeleteTextures(1, &tex);
  }
  thumbTextures_.clear();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window_);
  glfwTerminate();
  window_ = nullptr;
}

int App::run(const Options& cmdLine) {
  // Saved settings first; what the command line gives explicitly wins.
  const bool interactive = cmdLine.screenshot.empty() && !cmdLine.benchmark && cmdLine.turntableDir.empty() &&
                           cmdLine.flythroughDir.empty() && !cmdLine.evaluate;
  Options o = cmdLine;
  interactive_ = interactive;
  if (interactive) {
    loadUserSettings();
    if (!o.sizeGiven) {
      o.width = settings_.windowWidth;
      o.height = settings_.windowHeight;
    }
    if (!o.shGiven) o.shDegree = settings_.shDegree;
    if (!o.backgroundGiven) std::copy(settings_.background, settings_.background + 3, o.background);
    vsync_ = settings_.vsync;
  }
  options_ = o;
  flipY_ = o.flipY;
  blendFormat_ = o.blendFormat == "rgba8"     ? Framebuffer::Format::RGBA8
                 : o.blendFormat == "rgb10a2" ? Framebuffer::Format::RGB10A2
                                              : Framebuffer::Format::RGBA16F;
  mode_ = o.points ? RenderMode::Points : RenderMode::Splats;
  splatSettings_.shDegree = o.shDegree;
  splatSettings_.depthKeyBits = o.keyBits;
  splats_.setCompressed(!o.noCompress);
  splatSettings_.sort = static_cast<SortMode>(o.sortMode);
  if (o.yaw > -999.0f) camera_.yaw = o.yaw;
  splatSettings_.debugView = static_cast<DebugView>(o.debugView);
  distanceScale_ = o.distanceScale;
  background_ = {o.background[0], o.background[1], o.background[2]};
  if (o.hasCamera) {
    fixedCamera_ = true;
    fixedEye_ = {o.cameraPos[0], o.cameraPos[1], o.cameraPos[2]};
    fixedTarget_ = {o.cameraTarget[0], o.cameraTarget[1], o.cameraTarget[2]};
  }
  if (!createWindow(o)) {
    if (window_) shutdown();
    return 1;
  }
  if (interactive) {
    if (settings_.maximized && !o.sizeGiven) glfwMaximizeWindow(window_);
    glfwSwapInterval(vsync_ ? 1 : 0);
    checkRecoveryAtStartup();  // the last session crashed: offer its autosave
    markProjectSaved();        // an empty project is "saved": nothing to lose yet
    // Large undo steps go to disk (one folder per running viewer; old crash leftovers removed).
    {
      const std::filesystem::path root = pathFromUtf8(settingsDirectory()) / "history";
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        const auto age = std::filesystem::file_time_type::clock::now() - e.last_write_time(ec);
        if (!ec && age > std::chrono::hours(24)) std::filesystem::remove_all(e.path(), ec);
      }
      historyDir_ = pathToUtf8(root / std::to_string(currentProcessId()));
      history_.setSpill(historyDir_);
    }
  }

  std::string log;
  bool ok = points_.init(log);
  std::string log2;
  ok = splats_.init(log2) && ok;
  ok = cloudRenderer_.init(log2) && ok;
  if (!ok) {
    shaderLog_ = log + log2;
    std::fprintf(stderr, "%s\n", shaderLog_.c_str());
  }

  lodBudget_ = o.lodBudget > 0 ? o.lodBudget : lodBudget_;
  lodPixelError_ = o.lodPixelError;
  if (o.hasLasTransform) {
    const double* t = o.lasTransform;
    cloudTransform_ = GeoFrame::fromQuaternion(glm::dquat(t[1], t[2], t[3], t[4]), {t[5], t[6], t[7]}, t[0]);
    hasCloudTransform_ = true;
  }
  if (!o.scene.empty()) pendingLoad_ = o.scene;
  else if (!o.las.empty()) setScene(GaussianScene{});
  else if (interactive) {  // no file: the welcome screen (open, recent files, demo)
    setScene(GaussianScene{});
    showWelcome_ = true;
  } else {
    setScene(makeDemoScene());  // screenshots / benchmarks without a file: the demo scene
  }
  if (!o.las.empty()) loadCloudFile(o.las);

  colmapOverride_ = o.colmapDir;
  if (o.benchmark || !o.turntableDir.empty() || !o.flythroughDir.empty() || o.evaluate) {
    if (!pendingLoad_.empty()) {
      loadSceneFile(pendingLoad_);
      pendingLoad_.clear();
      while (isLoading()) {  // nothing else to do: block until the scene is in
        glfwWaitEventsTimeout(0.01);
        pollLoading();
      }
      if (!status_.empty()) std::printf("%s\n", status_.c_str());
    }
    const int code = scene_.empty()  ? 1
                     : o.evaluate    ? runEvaluation(o)
                     : o.benchmark   ? runBenchmark(o)
                     : !o.flythroughDir.empty() ? runFlythrough(o)
                                     : runTurntable(o);
    shutdown();
    return code;
  }

  const bool screenshotMode = !o.screenshot.empty();
  showUi_ = !screenshotMode;
  int frameIndex = 0, exitCode = 0;
  double lastTime = glfwGetTime();

  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();
    const double now = glfwGetTime();
    const float dt = static_cast<float>(now - lastTime);
    lastTime = now;

    if (!pendingLoad_.empty()) {
      std::string path;
      path.swap(pendingLoad_);
      if (interactive_) openAnyFile(path);  // asks first when there are unsaved changes
      else loadSceneFile(path);
    }
    if (interactive_) {
      pollBackgroundJobs();
      autosaveTick();
      stepVideoExport();  // one video frame per loop iteration
    }
    if (!pendingProject_.empty()) {
      std::string path;
      path.swap(pendingProject_);
      openAnyFile(path);
    }
    if (!pendingCloud_.empty()) {
      std::string path;
      path.swap(pendingCloud_);
      loadCloudFile(path);
    }
    pollLoading();
    updateLod();

    int fbW, fbH;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    if (fbW == 0 || fbH == 0) {  // minimized
      glfwWaitEvents();
      continue;
    }

    if (showUi_ && fontsDirty_) setupFonts();  // between frames: the font atlas is rebuilt
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (showUi_) {
      handleInput(dt, fbH);
      drawMenuBar();
      drawStatusBar();
      drawUi();
      if (showCompass_ && !scene_.empty()) drawCompass();
      drawToolOverlay(makeRenderView(fbW, fbH));
      drawWelcome();
      drawShortcuts();
      drawPathPrompt();
      drawConfirmDiscard();
      drawExportProgress();
      drawRecoveryPrompt();
      if (!settings_.firstRunDone && !tourOffered_ && !showWelcome_ && !scene_.empty() && !isLoading()) {
        tourOffered_ = true;  // first start: the 5-step tour once a scene is visible
        tourStep_ = 0;
      }
      drawTour();
    }
    if (interactive_ && showUi_) renderMainView(fbW, fbH, dt);  // auto quality while moving
    else frame(0, fbW, fbH);
    if (showUi_) captureThumbnail();  // a few frames after a load: picture for the recent-files list
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // A few warm-up frames let the scene load and the GPU timers settle. The image itself is
    // rendered offscreen at exactly the requested size (the OS may shrink the window).
    if (screenshotMode && !isLoading() && ++frameIndex == 8) {
      Framebuffer offscreen;
      offscreen.create(o.width, o.height, targetFormat());
      frame(offscreen.id(), o.width, o.height);
      fbW = o.width;
      fbH = o.height;
      std::string error;
      if (savePng(pathFromUtf8(o.screenshot), readFramebuffer(offscreen.id(), fbW, fbH), error)) {
        const SplatStats& s = splats_.stats();
        std::printf("Saved %s (%dx%d) | %zu splats | %u visible | preprocess %.3f ms | gpu sort %.3f ms"
                    " | cpu sort %.2f ms | draw %.3f ms\n",
                    o.screenshot.c_str(), fbW, fbH, scene_.size(), s.visible, s.preprocessMs, s.gpuSortMs,
                    s.cpuSortMs, s.drawMs);
      } else {
        std::fprintf(stderr, "Screenshot failed: %s\n", error.c_str());
        exitCode = 1;
      }
      if (!status_.empty()) std::printf("%s\n", status_.c_str());
      glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
    glfwSwapBuffers(window_);
  }
  shutdown();
  return exitCode;
}

// --------------------------------------------------------------------------------------
// Rendering
// --------------------------------------------------------------------------------------

glm::mat4 App::modelMatrix() const {
  // level_ (project "Level from selection") turns the user's floor horizontal on top of the base.
  if (hasOrientation_) return level_ * orientation_;  // known cameras: their average "up" becomes +Y
  return level_ * (flipY_ ? glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1, 0, 0)) : glm::mat4(1.0f));
}

RenderView App::makeRenderView(int w, int h) const {
  RenderView v;
  v.model = modelMatrix();
  v.view = camera_.view();
  v.viewport = {w, h};
  v.projection = camera_.projection(v.aspect());
  v.cameraPosition = camera_.position();
  v.fovY = camera_.fovY;
  v.nearPlane = camera_.nearPlane;
  return v;
}

void App::frame(GLuint fbo, int fbW, int fbH) {
  // The window's back buffer is 8-bit: splats go through a half-float target first. Offscreen
  // targets are created in targetFormat() already, and points are opaque (rounding is harmless).
  // A depth buffer is attached only while a point cloud is shown (it occludes splats). The
  // expected-depth output is never attached here: blending a second RG32F target doubled the cost
  // of every fragment (phong full screen, RTX 3060: draw 36.6 ms vs 5.8 ms). Picking renders one
  // frame into its own target instead (pickMeasurePoint).
  const bool drawCloud = haveCloud_ && showCloud_;
  const bool viaTarget = fbo == 0 && blendFormat_ != Framebuffer::Format::RGBA8 && mode_ == RenderMode::Splats;
  if (viaTarget && (sceneTarget_.width() != fbW || sceneTarget_.height() != fbH || sceneTarget_.format() != blendFormat_ ||
                    sceneTarget_.attachments().depthBuffer != drawCloud))
    sceneTarget_.create(fbW, fbH, blendFormat_, {false, drawCloud});
  glBindFramebuffer(GL_FRAMEBUFFER, viaTarget ? sceneTarget_.id() : fbo);
  glViewport(0, 0, fbW, fbH);
  glClearColor(background_.r, background_.g, background_.b, 1.0f);
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (viaTarget) sceneTarget_.clear(background_);

  const RenderView view = makeRenderView(fbW, fbH);
  if (drawCloud) cloudRenderer_.draw(view, cloudToDisplay(), glm::dvec3(0.0), cloudSettings_);
  if (mode_ == RenderMode::Splats) {
    syncSelectionFlags();  // selection highlight / hidden splats (tools_ui.cpp)
    SplatSettings s = splatSettings_;
    s.depthTest = drawCloud;
    splats_.render(view, s);
    if (viaTarget) {  // one conversion to 8 bits at the end, like the trainer
      glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneTarget_.id());
      glReadBuffer(GL_COLOR_ATTACHMENT0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glBlitFramebuffer(0, 0, fbW, fbH, 0, 0, fbW, fbH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
  } else {
    PointRenderer::Params p;
    p.model = view.model;
    p.view = view.view;
    p.projection = view.projection;
    p.pointSize = pointSize_;
    p.minOpacity = minOpacity_;
    pointTimer_.begin();
    points_.draw(p);
    pointTimer_.end();
  }
}

// --------------------------------------------------------------------------------------
// Scene management
// --------------------------------------------------------------------------------------

void App::setScene(GaussianScene&& scene) {
  setLodEnabled(false);  // the LOD tree belongs to the previous scene
  lodTree_ = {};
  finishLiveEdit(false);
  scene_ = std::move(scene);
  points_.upload(scene_);
  splats_.upload(scene_);
  frameScene();
  // A new scene: forget the edit state of the previous one.
  history_.clear();
  savedRevision_ = history_.revision();  // a freshly loaded scene has nothing to save
  autosavedRevision_ = savedRevision_;
  editTreeValid_ = false;
  selected_.assign(scene_.size(), 0);
  hidden_.assign(scene_.size(), 0);
  selectionChanged();
  measureDraft_.clear();
  scenePath_.clear();  // pollLoading() sets it for files; the demo scene has none
  savePath_[0] = 0;
  resetSelection();
  selectionDirtyForFilter_ = true;
  const std::string title = "gsplat-viewer - " + scene_.sourceName;
  glfwSetWindowTitle(window_, title.c_str());
}

void App::loadSceneFile(const std::string& utf8Path) {
  if (isLoading()) {
    status_ = std::string(tr("Already loading ")) + loadingName_;
    return;
  }
  // Parsing a large file takes seconds: do it on a worker thread so the window
  // keeps rendering the current scene and stays responsive.
  loading_ = loadSceneAsync(pathFromUtf8(utf8Path));
  loadingName_ = utf8Path;
  loadingStart_ = glfwGetTime();
  std::snprintf(pathInput_, sizeof(pathInput_), "%s", utf8Path.c_str());
}

void App::pollLoading() {
  if (!isLoading() || loading_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  LoadResult r = loading_.get();  // leaves the future invalid -> isLoading() == false
  if (!r.ok) {
    status_ = std::string(tr("Load failed: ")) + r.error;
    return;
  }
  // Before setScene: framing uses the orientation. A recovered autosave uses its original's cameras.
  loadColmapFor(recoverOriginalPath_.empty() ? loadingName_ : recoverOriginalPath_, colmapOverride_);
  const auto t0 = std::chrono::steady_clock::now();
  setScene(std::move(r.scene));  // GPU upload: must run on the thread that owns the GL context
  scenePath_ = loadingName_;
  const bool recovered = !recoverOriginalPath_.empty();
  if (recovered) {  // recovered autosave: show and save it under the original file's name
    scenePath_ = recoverOriginalPath_;
    scene_.sourceName = pathToUtf8(pathFromUtf8(scenePath_).filename());
    recoverOriginalPath_.clear();
  }
  if (interactive_ && !hasOrientation_) {  // Flip Y is remembered per file (settings)
    std::error_code ec;
    const std::string abs = pathToUtf8(std::filesystem::absolute(pathFromUtf8(scenePath_), ec));
    const bool want = (options_.flipY && scenePath_ == options_.scene) || isFlipY(settings_, ec ? scenePath_ : abs);
    if (want != flipY_) {
      flipY_ = want;
      frameScene();
    }
  }
  if (recovered) {
    savedRevision_ = ~uint64_t(0);  // recovered work stays "unsaved" until the user saves it
  } else {
    rememberRecent(scenePath_);
  }
  thumbnailPending_ = true;
  thumbnailDelay_ = 5;
  showWelcome_ = false;
  applyBatchEdits();
  const double uploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  loadMs_ = r.milliseconds + uploadMs;
  // A captured scene is only well reconstructed from where the phone was: framing its bounds from
  // outside shows the floaters around it, so start at the first training camera (F frames all).
  if (!colmap_.images.empty() && !fixedCamera_) goToCamera(0);
  if (options_.lodBudget > 0) setLodEnabled(true);
  char buf[256];
  std::snprintf(buf, sizeof(buf), tr("Loaded %zu splats (parse %.0f ms, upload %.0f ms)%s"), scene_.size(),
                r.milliseconds, uploadMs, r.removedInvalid ? tr(", removed invalid splats: ") : "");
  status_ = buf;
  if (r.removedInvalid) status_ += std::to_string(r.removedInvalid);
  if (recovered) status_ = tr("Recovered the autosaved session: save it to keep it");
}

void App::frameScene() {
  if (scene_.empty() && haveCloud_) {
    frameCloud();
    return;
  }
  // Frame the dense subject (scene_bounds.cpp), not the background and floaters around it.
  // The model matrix only rotates, so the radius is unchanged.
  const glm::vec3 center = glm::vec3(modelMatrix() * glm::vec4(scene_.focusCenter, 1.0f));
  camera_.frameSphere(center, scene_.focusRadius, glm::length(scene_.boundsMax - scene_.boundsMin));
  camera_.distance *= distanceScale_;
  if (fixedCamera_) camera_.lookAt(fixedEye_, fixedTarget_);  // explicit camera from the command line
}

void App::loadColmapFor(const std::string& sceneUtf8Path, const std::string& explicitDir) {
  colmap_ = {};
  colmapDir_.clear();
  hasOrientation_ = false;
  cameraIndex_ = -1;
  if (sceneUtf8Path.empty() && explicitDir.empty()) return;  // e.g. the demo scene
  const std::filesystem::path dir =
      explicitDir.empty() ? findColmapModelFor(pathFromUtf8(sceneUtf8Path)) : pathFromUtf8(explicitDir);
  if (dir.empty()) return;  // a scene without cameras: keep --flip-y behaviour
  std::string error;
  if (!loadColmapModel(dir, colmap_, error) || colmap_.images.empty()) {
    std::fprintf(stderr, "COLMAP model %s ignored: %s\n", pathToUtf8(dir).c_str(), error.c_str());
    colmap_ = {};
    return;
  }
  colmapDir_ = pathToUtf8(dir);
  // COLMAP's world axes are arbitrary; people hold the phone upright, so rotate the average camera
  // up vector onto +Y. glm::rotation handles the antiparallel case (up = -Y, the usual result).
  orientation_ = glm::mat4_cast(glm::rotation(averageCameraUp(colmap_), glm::vec3(0.0f, 1.0f, 0.0f)));
  hasOrientation_ = true;
}

void App::goToCamera(int index) {
  if (colmap_.images.empty()) return;
  const int n = static_cast<int>(colmap_.images.size());
  cameraIndex_ = ((index % n) + n) % n;
  const ColmapImage& im = colmap_.images[static_cast<size_t>(cameraIndex_)];
  const glm::mat3 r(orientation_);
  const glm::vec3 eye = r * glm::vec3(im.center());
  const glm::vec3 forward = glm::normalize(r * glm::vec3(im.forwardInWorld()));
  // Orbit around a point in front of the camera, at the depth of the scene center.
  const glm::vec3 sceneCenter = glm::vec3(modelMatrix() * glm::vec4(0.5f * (scene_.boundsMin + scene_.boundsMax), 1.0f));
  const float depth = std::max(glm::dot(sceneCenter - eye, forward), 0.1f * glm::length(scene_.boundsMax - scene_.boundsMin));
  camera_.lookAt(eye, eye + forward * depth);
  if (const ColmapCamera* cam = colmap_.camera(im.cameraId)) {
    const glm::dvec4 k = cam->pinhole();
    if (k.y > 0.0) camera_.fovY = static_cast<float>(2.0 * std::atan(0.5 * cam->height / k.y));  // match the photo
  }
  status_ = std::string(tr("Camera ")) + std::to_string(cameraIndex_ + 1) + " / " + std::to_string(n) + ": " + im.name;
}

void App::reloadShaders() {
  std::string a, b;
  const bool ok = points_.reloadShaders(a) & splats_.reloadShaders(b);
  shaderLog_ = ok ? "" : a + b;
  status_ = ok ? tr("Shaders reloaded") : tr("Shader error (previous shaders kept)");
}

// --------------------------------------------------------------------------------------
// Input
// --------------------------------------------------------------------------------------

void App::onDrop(GLFWwindow* w, int count, const char** paths) {
  for (int i = 0; i < count; ++i) {
    std::string p = paths[i];
    std::string ext = p.size() >= 4 ? p.substr(p.size() - 4) : "";
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool project = p.size() >= 8 && [&] {
      std::string e = p.substr(p.size() - 8);
      for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return e == ".gsvproj";
    }();
    if (project) appFrom(w)->pendingProject_ = p;
    else if (ext == ".las" || ext == ".laz") appFrom(w)->pendingCloud_ = p;
    else appFrom(w)->pendingLoad_ = p;
  }
}

void App::onMouseButton(GLFWwindow* w, int button, int action, int) {
  if (button != GLFW_MOUSE_BUTTON_LEFT) return;
  App* app = appFrom(w);
  double x, y;
  glfwGetCursorPos(w, &x, &y);
  if (action == GLFW_PRESS) {
    app->pressPending_ = !ImGui::GetIO().WantCaptureMouse;  // a click on the panel is not a pick
    app->pressX_ = x;
    app->pressY_ = y;
  } else if (action == GLFW_RELEASE && app->pressPending_) {
    app->pressPending_ = false;
    // A click, not a drag (drags orbit): released within 4 px of the press.
    if (std::abs(x - app->pressX_) + std::abs(y - app->pressY_) < 4.0) {
      app->clickReady_ = true;
      app->clickX_ = x;
      app->clickY_ = y;
    }
  }
}

void App::onScroll(GLFWwindow* w, double, double y) {
  if (!ImGui::GetIO().WantCaptureMouse) appFrom(w)->scrollAccum_ += static_cast<float>(y);
}

void App::handleInput(float dt, int fbHeight) {
  updateCameraAnimation();  // nav_ui.cpp: smooth transitions, cancelled by any user camera input
  ImGuiIO& io = ImGui::GetIO();
  double x, y;
  glfwGetCursorPos(window_, &x, &y);
  const float dx = static_cast<float>(x - lastX_), dy = static_cast<float>(y - lastY_);
  lastX_ = x;
  lastY_ = y;

  if (!io.WantCaptureMouse) {
    const bool lmb = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool rmb = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool mmb = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    // A click is a press and release without dragging (onMouseButton). Tools other than
    // Navigate own the left button; the right button orbits and the middle button pans then.
    const bool clicked = clickReady_;
    clickReady_ = false;
    const bool shift = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                       glfwGetKey(window_, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    const bool alt = glfwGetKey(window_, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                     glfwGetKey(window_, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    if (tool_ != Tool::Navigate && !flyMode_) {
      handleToolInput(lmb, x, y, clicked, shift, alt);
      if (rmb) camera_.orbit(dx, dy);
      if (mmb) camera_.pan(dx, dy, static_cast<float>(fbHeight));
    } else if (flyMode_) {  // LMB/RMB look around, MMB pan
      if (lmb || rmb) camera_.look(dx, dy);
      if (mmb) camera_.pan(dx, dy, static_cast<float>(fbHeight));
    } else {         // LMB orbit, RMB/MMB pan
      if (lmb) camera_.orbit(dx, dy);
      if (rmb || mmb) camera_.pan(dx, dy, static_cast<float>(fbHeight));
    }
  }
  if (scrollAccum_ != 0.0f) {
    if (flyMode_) flySpeed_ = std::clamp(flySpeed_ * std::pow(1.25f, scrollAccum_), 0.01f, 100.0f);
    else camera_.zoom(scrollAccum_);
    scrollAccum_ = 0.0f;
  }
  if (io.WantCaptureKeyboard) return;

  auto down = [&](int key) { return glfwGetKey(window_, key) == GLFW_PRESS; };
  // Rising edge: pressed this frame but not the previous one.
  auto pressed = [&](int key) {
    const bool d = down(key), was = keyWasDown_[key];
    keyWasDown_[key] = d;
    return d && !was;
  };

  glm::vec3 dir(0.0f);
  // Ctrl+S / Ctrl+A / Ctrl+D are shortcuts, not camera moves.
  const bool ctrlHeld = down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL);
  if (!ctrlHeld) {
    if (down(GLFW_KEY_W)) dir.z += 1;
    if (down(GLFW_KEY_S)) dir.z -= 1;
    if (down(GLFW_KEY_D)) dir.x += 1;
    if (down(GLFW_KEY_A)) dir.x -= 1;
    if (down(GLFW_KEY_E)) dir.y += 1;
    if (down(GLFW_KEY_Q)) dir.y -= 1;
  }
  if (dir != glm::vec3(0.0f)) {
    // Speed in scene units per second, times dt: the distance covered does not depend on the FPS.
    const float sceneSize = 2.0f * scene_.focusRadius;  // the subject's size, not the background's
    const float speed = sceneSize * 0.3f * (down(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f) * (flyMode_ ? flySpeed_ : 1.0f);
    camera_.move(glm::normalize(dir), speed * dt, flyMode_);
  }
  if (pressed(GLFW_KEY_F5)) reloadShaders();
  if (pressed(GLFW_KEY_F1)) showShortcuts_ = !showShortcuts_;
  if (pressed(GLFW_KEY_F)) {
    flyMode_ = !flyMode_;  // the camera state is shared, so the view does not jump
    status_ = flyMode_ ? tr("Fly mode: drag to look, WASD/QE move, wheel = speed (F: back to orbit)") : tr("Orbit mode");
  }
  const bool ctrl = ctrlHeld;  // read once: sticky keys report a tap only to the first read
  const bool shiftKey = down(GLFW_KEY_LEFT_SHIFT) || down(GLFW_KEY_RIGHT_SHIFT);
  if (pressed(GLFW_KEY_R)) {
    if (shiftKey) {
      frameSelection();
    } else {  // smooth: compute the framed camera, then fly there
      const OrbitCamera before = camera_;
      frameScene();
      const OrbitCamera end = camera_;
      camera_ = before;
      animateCameraTo(end);
    }
  }
  // Quick views (numpad, like Blender): 1 front, 3 right, 7 top; Ctrl = opposite side.
  if (pressed(GLFW_KEY_KP_1)) quickView(2, ctrl);
  if (pressed(GLFW_KEY_KP_3)) quickView(0, ctrl);
  if (pressed(GLFW_KEY_KP_7)) quickView(1, ctrl);
  const bool altKey = down(GLFW_KEY_LEFT_ALT) || down(GLFW_KEY_RIGHT_ALT);
  const bool zPressed = pressed(GLFW_KEY_Z), yPressed = pressed(GLFW_KEY_Y);
  if (zPressed && ctrl) shiftKey ? redoEdit() : undoEdit();
  if (yPressed && ctrl) redoEdit();
  if (pressed(GLFW_KEY_M)) setTool(tool_ == Tool::Measure ? Tool::Navigate : Tool::Measure);
  handleToolKeys(ctrl, shiftKey, altKey);  // tools_ui.cpp: 1-9, Esc, Delete, H, Ctrl+A/D/I/S...
  if (pressed(GLFW_KEY_TAB)) mode_ = mode_ == RenderMode::Splats ? RenderMode::Points : RenderMode::Splats;
  if (pressed(GLFW_KEY_C)) goToCamera(cameraIndex_ + (down(GLFW_KEY_LEFT_SHIFT) ? -1 : 1));  // training cameras
}

}  // namespace gsv
