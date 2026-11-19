// gsplat-viewer: real-time 3D Gaussian Splatting viewer (C++20 / OpenGL 4.3).
//
// Usage:
//   gsplat_viewer [scene.ply] [--flip-y] [--size WxH] [--screenshot out.png]
//   gsplat_viewer --gen out.ply [N]     write the demo scene (N splats) to a PLY and exit
//
// Without a scene the built-in demo scene is shown. --screenshot renders a few
// frames without UI, saves the image and exits (README images, regression tests).

#include "camera.h"
#include "gl/gl_loader.h"
#include "gl/gpu_timer.h"
#include "gl/screenshot.h"
#include "render/point_renderer.h"
#include "scene/demo_scene.h"
#include "scene/ply_io.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

enum class RenderMode { Points = 0, Splats = 1 };

struct App {
  GLFWwindow* window = nullptr;
  GaussianScene scene;
  OrbitCamera camera;
  PointRenderer points;
  GpuTimer drawTimer;

  RenderMode mode = RenderMode::Points;
  bool flipY = false;  // INRIA/COLMAP scenes are stored with Y pointing down
  bool vsync = true;
  float pointSize = 2.0f;
  float minOpacity = 0.05f;
  glm::vec3 background{0.07f, 0.08f, 0.1f};

  std::string pendingLoad;  // set by drag & drop, processed in the main loop
  std::string status;
  std::string shaderLog;
  double loadMs = 0.0;
  float scrollAccum = 0.0f;
  double lastX = 0.0, lastY = 0.0;
  bool f5WasDown = false, fWasDown = false;
  char pathInput[1024] = {};
  std::string glRenderer;
};

App* gApp = nullptr;

glm::mat4 modelMatrix(const App& app) {
  return app.flipY ? glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1, 0, 0)) : glm::mat4(1.0f);
}

void frameScene(App& app) {
  const glm::mat4 m = modelMatrix(app);
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (int i = 0; i < 8; ++i) {  // transform the 8 corners of the bounds
    glm::vec3 c((i & 1) ? app.scene.boundsMax.x : app.scene.boundsMin.x,
                (i & 2) ? app.scene.boundsMax.y : app.scene.boundsMin.y,
                (i & 4) ? app.scene.boundsMax.z : app.scene.boundsMin.z);
    glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
    lo = glm::min(lo, w);
    hi = glm::max(hi, w);
  }
  app.camera.frameBounds(lo, hi);
}

void setScene(App& app, GaussianScene&& scene) {
  app.scene = std::move(scene);
  app.points.upload(app.scene);
  frameScene(app);
  const std::string title = "gsplat-viewer - " + app.scene.sourceName;
  glfwSetWindowTitle(app.window, title.c_str());
}

void loadSceneFile(App& app, const std::string& utf8Path) {
  const auto t0 = std::chrono::steady_clock::now();
  GaussianScene scene;
  std::string error;
  if (!loadPly(pathFromUtf8(utf8Path), scene, error)) {
    app.status = "Load failed: " + error;
    return;
  }
  setScene(app, std::move(scene));
  app.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  app.status = "Loaded " + std::to_string(app.scene.size()) + " splats";
  std::snprintf(app.pathInput, sizeof(app.pathInput), "%s", utf8Path.c_str());
}

void reloadShaders(App& app) {
  std::string log;
  app.shaderLog = app.points.reloadShaders(log) ? "" : log;
  app.status = app.shaderLog.empty() ? "Shaders reloaded" : "Shader error (previous shader kept)";
}

#ifndef NDEBUG
void APIENTRY onGlDebug(GLenum, GLenum, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
  std::fprintf(stderr, "[GL] %s\n", message);
}
#endif

void onDrop(GLFWwindow*, int count, const char** paths) {
  if (count > 0) gApp->pendingLoad = paths[0];
}

void onScroll(GLFWwindow*, double, double y) {
  if (!ImGui::GetIO().WantCaptureMouse) gApp->scrollAccum += static_cast<float>(y);
}

bool keyDown(GLFWwindow* w, int key) { return glfwGetKey(w, key) == GLFW_PRESS; }

void handleInput(App& app, float dt, int fbHeight) {
  ImGuiIO& io = ImGui::GetIO();
  double x, y;
  glfwGetCursorPos(app.window, &x, &y);
  const float dx = static_cast<float>(x - app.lastX), dy = static_cast<float>(y - app.lastY);
  app.lastX = x;
  app.lastY = y;

  if (!io.WantCaptureMouse) {
    if (glfwGetMouseButton(app.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) app.camera.orbit(dx, dy);
    if (glfwGetMouseButton(app.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
        glfwGetMouseButton(app.window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS)
      app.camera.pan(dx, dy, static_cast<float>(fbHeight));
  }
  if (app.scrollAccum != 0.0f) {
    app.camera.zoom(app.scrollAccum);
    app.scrollAccum = 0.0f;
  }

  if (io.WantCaptureKeyboard) return;
  glm::vec3 dir(0.0f);
  if (keyDown(app.window, GLFW_KEY_W)) dir.z += 1;
  if (keyDown(app.window, GLFW_KEY_S)) dir.z -= 1;
  if (keyDown(app.window, GLFW_KEY_D)) dir.x += 1;
  if (keyDown(app.window, GLFW_KEY_A)) dir.x -= 1;
  if (keyDown(app.window, GLFW_KEY_E)) dir.y += 1;
  if (keyDown(app.window, GLFW_KEY_Q)) dir.y -= 1;
  if (dir != glm::vec3(0.0f)) {
    const float sceneSize = glm::length(app.scene.boundsMax - app.scene.boundsMin);
    const float speed = sceneSize * 0.3f * (keyDown(app.window, GLFW_KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
    app.camera.move(glm::normalize(dir), speed * dt);
  }
  const bool f5 = keyDown(app.window, GLFW_KEY_F5), f = keyDown(app.window, GLFW_KEY_F);
  if (f5 && !app.f5WasDown) reloadShaders(app);
  if (f && !app.fWasDown) frameScene(app);
  app.f5WasDown = f5;
  app.fWasDown = f;
}

void drawUi(App& app) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
  ImGui::Begin("Gaussian Splat Viewer");

  ImGui::SeparatorText("Scene");
  ImGui::Text("%s", app.scene.sourceName.c_str());
  ImGui::Text("%zu splats  |  SH degree %d", app.scene.size(), app.scene.shDegree);
  ImGui::SetNextItemWidth(-60);
  const bool enter = ImGui::InputText("##path", app.pathInput, sizeof(app.pathInput),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if (ImGui::Button("Load") || enter) app.pendingLoad = app.pathInput;
  ImGui::TextDisabled("or drag & drop a .ply onto the window");
  if (ImGui::Button("Demo scene")) {
    setScene(app, makeDemoScene());
    app.status = "Demo scene";
  }
  if (!app.status.empty()) ImGui::TextWrapped("%s", app.status.c_str());

  ImGui::SeparatorText("Performance");
  ImGui::Text("FPS %.1f   frame %.2f ms", io.Framerate, 1000.0f / std::max(io.Framerate, 0.001f));
  ImGui::Text("GPU draw   %.3f ms", app.drawTimer.milliseconds());
  ImGui::Text("Load time  %.0f ms", app.loadMs);

  ImGui::SeparatorText("Render");
  int mode = static_cast<int>(app.mode);
  ImGui::RadioButton("Points", &mode, 0);
  ImGui::SameLine();
  ImGui::BeginDisabled();  // TODO(Day 2-3): enable once SplatRenderer exists
  ImGui::RadioButton("Gaussian splats (todo)", &mode, 1);
  ImGui::EndDisabled();
  app.mode = static_cast<RenderMode>(mode);
  ImGui::SliderFloat("Point size", &app.pointSize, 1.0f, 12.0f, "%.1f px");
  ImGui::SliderFloat("Min opacity", &app.minOpacity, 0.0f, 1.0f);
  ImGui::ColorEdit3("Background", &app.background.x);
  if (ImGui::Checkbox("Flip Y (INRIA / COLMAP scenes)", &app.flipY)) frameScene(app);
  if (ImGui::Checkbox("VSync", &app.vsync)) glfwSwapInterval(app.vsync ? 1 : 0);

  ImGui::SeparatorText("Camera");
  const glm::vec3 p = app.camera.position();
  ImGui::Text("pos (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
  if (ImGui::Button("Frame scene (F)")) frameScene(app);
  ImGui::TextDisabled("LMB orbit | RMB/MMB pan | wheel zoom\nWASD/QE move, Shift = faster");

  ImGui::SeparatorText("Dev");
  if (ImGui::Button("Reload shaders (F5)")) reloadShaders(app);
  if (!app.shaderLog.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", app.shaderLog.c_str());
  ImGui::TextDisabled("%s", app.glRenderer.c_str());
  ImGui::End();
}

struct Options {
  std::string scene;
  std::string screenshot;
  bool flipY = false;
  int width = 1600, height = 900;
};

bool parseOptions(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--flip-y") o.flipY = true;
    else if (a == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
    else if (a == "--size" && i + 1 < argc) {
      if (std::sscanf(argv[++i], "%dx%d", &o.width, &o.height) != 2) return false;
    } else if (!a.empty() && a[0] != '-') o.scene = a;
    else return false;
  }
  return true;
}

int generateDemo(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: gsplat_viewer --gen out.ply [count]\n");
    return 1;
  }
  const size_t n = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 200000;
  std::string error;
  if (!savePly(argv[2], makeDemoScene(n), error)) {
    std::fprintf(stderr, "Failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("Wrote %zu splats to %s\n", n, argv[2]);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--gen") == 0) return generateDemo(argc, argv);
  Options opt;
  if (!parseOptions(argc, argv, opt)) {
    std::printf(
        "gsplat_viewer [scene.ply] [--flip-y] [--size WxH] [--screenshot out.png]\n"
        "gsplat_viewer --gen out.ply [count]\n");
    return 1;
  }

  if (!glfwInit()) {
    std::fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifndef NDEBUG
  glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif
  App app;
  gApp = &app;
  app.flipY = opt.flipY;
  if (!opt.screenshot.empty()) glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  app.window = glfwCreateWindow(opt.width, opt.height, "gsplat-viewer", nullptr, nullptr);
  if (!app.window) {
    std::fprintf(stderr, "Could not create an OpenGL 4.3 core context (update your GPU driver)\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(app.window);
  glfwSwapInterval(1);

  std::string missing;
  if (!gl::loadFunctions([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); },
                         missing)) {
    std::fprintf(stderr, "Missing OpenGL functions: %s\n", missing.c_str());
    return 1;
  }
  app.glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  std::printf("OpenGL: %s | %s\n", glGetString(GL_VERSION), app.glRenderer.c_str());
#ifndef NDEBUG
  glEnable(GL_DEBUG_OUTPUT);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  glDebugMessageCallback(onGlDebug, nullptr);
#endif

  // Our callbacks must be set before ImGui installs its own, which chain to ours.
  glfwSetDropCallback(app.window, onDrop);
  glfwSetScrollCallback(app.window, onScroll);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(app.window, true);
  ImGui_ImplOpenGL3_Init("#version 430");

  std::string log;
  if (!app.points.init(log)) {
    std::fprintf(stderr, "%s\n", log.c_str());
    app.shaderLog = log;
  }

  if (!opt.scene.empty()) app.pendingLoad = opt.scene;
  else setScene(app, makeDemoScene());

  const bool screenshotMode = !opt.screenshot.empty();
  int frameIndex = 0;
  int exitCode = 0;
  double lastTime = glfwGetTime();
  while (!glfwWindowShouldClose(app.window)) {
    glfwPollEvents();
    const double now = glfwGetTime();
    const float dt = static_cast<float>(now - lastTime);
    lastTime = now;

    if (!app.pendingLoad.empty()) {
      std::string path;
      path.swap(app.pendingLoad);
      loadSceneFile(app, path);
    }

    int fbW, fbH;
    glfwGetFramebufferSize(app.window, &fbW, &fbH);
    if (fbW == 0 || fbH == 0) {  // minimized
      glfwWaitEvents();
      continue;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    if (!screenshotMode) {
      handleInput(app, dt, fbH);
      drawUi(app);
    }

    glViewport(0, 0, fbW, fbH);
    glClearColor(app.background.r, app.background.g, app.background.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    app.drawTimer.begin();
    PointRenderer::Params params;
    params.model = modelMatrix(app);
    params.view = app.camera.view();
    params.projection = app.camera.projection(static_cast<float>(fbW) / static_cast<float>(fbH));
    params.pointSize = app.pointSize;
    params.minOpacity = app.minOpacity;
    app.points.draw(params);
    app.drawTimer.end();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // A few warm-up frames let the scene load and the GPU timer settle.
    if (screenshotMode && ++frameIndex == 8) {
      std::string error;
      if (saveFramebufferPng(pathFromUtf8(opt.screenshot), fbW, fbH, error)) {
        std::printf("Saved %s (%dx%d) | %zu splats | GPU draw %.3f ms\n", opt.screenshot.c_str(), fbW, fbH,
                    app.scene.size(), app.drawTimer.milliseconds());
      } else {
        std::fprintf(stderr, "Screenshot failed: %s\n", error.c_str());
        exitCode = 1;
      }
      if (!app.status.empty()) std::printf("%s\n", app.status.c_str());
      glfwSetWindowShouldClose(app.window, GLFW_TRUE);
    }
    glfwSwapBuffers(app.window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(app.window);
  glfwTerminate();
  return exitCode;
}
