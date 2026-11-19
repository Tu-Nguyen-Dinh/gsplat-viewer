#include "app/app.h"

#include "gl/framebuffer.h"
#include "gl/gl_loader.h"
#include "scene/demo_scene.h"
#include "scene/ply_io.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <chrono>
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
  flipY_ = o.flipY;
  mode_ = o.points ? RenderMode::Points : RenderMode::Splats;
  splatSettings_.shDegree = o.shDegree;
  splatSettings_.sort = static_cast<SortMode>(o.sortMode);
  if (o.yaw > -999.0f) camera_.yaw = o.yaw;
  splatSettings_.debugView = static_cast<DebugView>(o.debugView);
  distanceScale_ = o.distanceScale;
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
    frame(fbW, fbH);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // A few warm-up frames let the scene load and the GPU timers settle.
    if (screenshotMode && !isLoading() && ++frameIndex == 8) {
      std::string error;
      if (savePng(pathFromUtf8(o.screenshot), readFramebuffer(0, fbW, fbH), error)) {
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

void App::frame(int fbW, int fbH) {
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, fbW, fbH);
  glClearColor(background_.r, background_.g, background_.b, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  const RenderView view = makeRenderView(fbW, fbH);
  if (mode_ == RenderMode::Splats) {
    splats_.render(view, splatSettings_);
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
  const auto t0 = std::chrono::steady_clock::now();
  setScene(std::move(r.scene));  // GPU upload: must run on the thread that owns the GL context
  const double uploadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  loadMs_ = r.milliseconds + uploadMs;
  char buf[256];
  std::snprintf(buf, sizeof(buf), "Loaded %zu splats (parse %.0f ms, upload %.0f ms)%s", scene_.size(),
                r.milliseconds, uploadMs, r.removedInvalid ? ", removed invalid splats: " : "");
  status_ = buf;
  if (r.removedInvalid) status_ += std::to_string(r.removedInvalid);
}

void App::frameScene() {
  const glm::mat4 m = modelMatrix();
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (int i = 0; i < 8; ++i) {  // transform the 8 corners of the bounds
    const glm::vec3 c((i & 1) ? scene_.boundsMax.x : scene_.boundsMin.x,
                      (i & 2) ? scene_.boundsMax.y : scene_.boundsMin.y,
                      (i & 4) ? scene_.boundsMax.z : scene_.boundsMin.z);
    const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
    lo = glm::min(lo, w);
    hi = glm::max(hi, w);
  }
  camera_.frameBounds(lo, hi);
  camera_.distance *= distanceScale_;
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
    if (glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) camera_.orbit(dx, dy);
    if (glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
        glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
      camera_.pan(dx, dy, static_cast<float>(fbHeight));
  }
  if (scrollAccum_ != 0.0f) {
    camera_.zoom(scrollAccum_);
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
    const float sceneSize = glm::length(scene_.boundsMax - scene_.boundsMin);
    const float speed = sceneSize * 0.3f * (down(GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
    camera_.move(glm::normalize(dir), speed * dt);
  }
  if (pressed(GLFW_KEY_F5)) reloadShaders();
  if (pressed(GLFW_KEY_F)) frameScene();
  if (pressed(GLFW_KEY_TAB)) mode_ = mode_ == RenderMode::Splats ? RenderMode::Points : RenderMode::Splats;
}

}  // namespace gsv
