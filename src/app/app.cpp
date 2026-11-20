#include "app/app.h"

#include "gl/framebuffer.h"
#include "gl/gl_loader.h"
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
  glfwSetScrollCallback(window_, onScroll);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window_, true);
  ImGui_ImplOpenGL3_Init("#version 430");
  return true;
}

void App::shutdown() {
  if (!window_) return;
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window_);
  glfwTerminate();
  window_ = nullptr;
}

int App::run(const Options& o) {
  options_ = o;
  flipY_ = o.flipY;
  blendFormat_ = o.blendFormat == "rgba8"     ? Framebuffer::Format::RGBA8
                 : o.blendFormat == "rgb10a2" ? Framebuffer::Format::RGB10A2
                                              : Framebuffer::Format::RGBA16F;
  mode_ = o.points ? RenderMode::Points : RenderMode::Splats;
  splatSettings_.shDegree = o.shDegree;
  splatSettings_.depthKeyBits = o.keyBits;
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

  std::string log;
  bool ok = points_.init(log);
  std::string log2;
  ok = splats_.init(log2) && ok;
  if (!ok) {
    shaderLog_ = log + log2;
    std::fprintf(stderr, "%s\n", shaderLog_.c_str());
  }

  if (!o.scene.empty()) pendingLoad_ = o.scene;
  else setScene(makeDemoScene());

  colmapOverride_ = o.colmapDir;
  if (o.benchmark || !o.turntableDir.empty() || o.evaluate) {
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
      loadSceneFile(path);
    }
    pollLoading();

    int fbW, fbH;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    if (fbW == 0 || fbH == 0) {  // minimized
      glfwWaitEvents();
      continue;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (showUi_) {
      handleInput(dt, fbH);
      drawUi();
    }
    frame(0, fbW, fbH);
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
  if (hasOrientation_) return orientation_;  // known cameras: their average "up" becomes +Y
  return flipY_ ? glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1, 0, 0)) : glm::mat4(1.0f);
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
  const bool viaTarget = fbo == 0 && blendFormat_ != Framebuffer::Format::RGBA8 && mode_ == RenderMode::Splats;
  if (viaTarget && (sceneTarget_.width() != fbW || sceneTarget_.height() != fbH || sceneTarget_.format() != blendFormat_))
    sceneTarget_.create(fbW, fbH, blendFormat_);
  glBindFramebuffer(GL_FRAMEBUFFER, viaTarget ? sceneTarget_.id() : fbo);
  glViewport(0, 0, fbW, fbH);
  glClearColor(background_.r, background_.g, background_.b, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  const RenderView view = makeRenderView(fbW, fbH);
  if (mode_ == RenderMode::Splats) {
    splats_.render(view, splatSettings_);
    if (viaTarget) {  // one conversion to 8 bits at the end, like the trainer
      glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneTarget_.id());
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
  scene_ = std::move(scene);
  points_.upload(scene_);
  splats_.upload(scene_);
  frameScene();
  // A new scene: forget the edit state of the previous one.
  undoScene_ = {};
  undoLabel_.clear();
  scenePath_.clear();  // pollLoading() sets it for files; the demo scene has none
  savePath_[0] = 0;
  resetSelection();
  selectionDirtyForFilter_ = true;
  const std::string title = "gsplat-viewer - " + scene_.sourceName;
  glfwSetWindowTitle(window_, title.c_str());
}

void App::loadSceneFile(const std::string& utf8Path) {
  if (isLoading()) {
    status_ = "Already loading " + loadingName_;
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
    status_ = "Load failed: " + r.error;
    return;
  }
  loadColmapFor(loadingName_, colmapOverride_);  // before setScene: framing uses the orientation
  const auto t0 = std::chrono::steady_clock::now();
  setScene(std::move(r.scene));  // GPU upload: must run on the thread that owns the GL context
  scenePath_ = loadingName_;
  applyBatchEdits();
  const double uploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  loadMs_ = r.milliseconds + uploadMs;
  // A captured scene is only well reconstructed from where the phone was: framing its bounds from
  // outside shows the floaters around it, so start at the first training camera (F frames all).
  if (!colmap_.images.empty() && !fixedCamera_) goToCamera(0);
  char buf[256];
  std::snprintf(buf, sizeof(buf), "Loaded %zu splats (parse %.0f ms, upload %.0f ms)%s", scene_.size(),
                r.milliseconds, uploadMs, r.removedInvalid ? ", removed invalid splats: " : "");
  status_ = buf;
  if (r.removedInvalid) status_ += std::to_string(r.removedInvalid);
}

void App::frameScene() {
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
  status_ = "Camera " + std::to_string(cameraIndex_ + 1) + " / " + std::to_string(n) + ": " + im.name;
}

void App::reloadShaders() {
  std::string a, b;
  const bool ok = points_.reloadShaders(a) & splats_.reloadShaders(b);
  shaderLog_ = ok ? "" : a + b;
  status_ = ok ? "Shaders reloaded" : "Shader error (previous shaders kept)";
}

// --------------------------------------------------------------------------------------
// Input
// --------------------------------------------------------------------------------------

void App::onDrop(GLFWwindow* w, int count, const char** paths) {
  if (count > 0) appFrom(w)->pendingLoad_ = paths[0];
}

void App::onScroll(GLFWwindow* w, double, double y) {
  if (!ImGui::GetIO().WantCaptureMouse) appFrom(w)->scrollAccum_ += static_cast<float>(y);
}

void App::handleInput(float dt, int fbHeight) {
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
    if (flyMode_) {  // LMB/RMB look around, MMB pan
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
  if (down(GLFW_KEY_W)) dir.z += 1;
  if (down(GLFW_KEY_S)) dir.z -= 1;
  if (down(GLFW_KEY_D)) dir.x += 1;
  if (down(GLFW_KEY_A)) dir.x -= 1;
  if (down(GLFW_KEY_E)) dir.y += 1;
  if (down(GLFW_KEY_Q)) dir.y -= 1;
  if (dir != glm::vec3(0.0f)) {
    // Speed in scene units per second, times dt: the distance covered does not depend on the FPS.
    const float sceneSize = 2.0f * scene_.focusRadius;  // the subject's size, not the background's
    const float speed = sceneSize * 0.3f * (down(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f) * (flyMode_ ? flySpeed_ : 1.0f);
    camera_.move(glm::normalize(dir), speed * dt, flyMode_);
  }
  if (pressed(GLFW_KEY_F5)) reloadShaders();
  if (pressed(GLFW_KEY_F)) {
    flyMode_ = !flyMode_;  // the camera state is shared, so the view does not jump
    status_ = flyMode_ ? "Fly mode: drag to look, WASD/QE move, wheel = speed (F: back to orbit)" : "Orbit mode";
  }
  if (pressed(GLFW_KEY_R)) frameScene();
  const bool ctrl = down(GLFW_KEY_LEFT_CONTROL) || down(GLFW_KEY_RIGHT_CONTROL);
  if (pressed(GLFW_KEY_Z) && ctrl) undoEdit();
  if (pressed(GLFW_KEY_TAB)) mode_ = mode_ == RenderMode::Splats ? RenderMode::Points : RenderMode::Splats;
  if (pressed(GLFW_KEY_C)) goToCamera(cameraIndex_ + (down(GLFW_KEY_LEFT_SHIFT) ? -1 : 1));  // training cameras
}

}  // namespace gsv
