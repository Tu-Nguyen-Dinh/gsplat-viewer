// gsplat-viewer for Android: NativeActivity + EGL (OpenGL ES 3.1+) + Dear ImGui.
//
// The renderer, scene readers and camera are the desktop code unchanged (src/); this file only
// adds what a phone needs:
//   * EGL context and window surface, recreated when the app goes to the background;
//   * shaders packaged in the APK (assets/shaders), copied to internal storage at start-up so the
//     file-based shader loader works as on desktop;
//   * touch input: two floating virtual joysticks (bottom left: move, bottom right: look),
//     one-finger drag elsewhere: orbit, two fingers: pinch zoom;
//   * a scene list: every .ply / .splat / .spz under the app's external files folder
//     (Android/data/<package>/files, reachable over USB), plus the procedural demo scene.
// Splats are rendered at a reduced resolution (the S23 Ultra screen is 3088x1440) into a
// half-float target and scaled up to the window with one blit.
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "core/camera.h"
#include "core/net.h"
#include "gl/framebuffer.h"
#include "gl/gl_loader.h"
#include "gl/shader.h"
#include "render/splat_renderer.h"
#include "scene/colmap_io.h"
#include "scene/demo_scene.h"
#include "scene/scene_io.h"

#include <imgui.h>
#include <imgui_impl_android.h>
#include <imgui_impl_opengl3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "gsplat", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "gsplat", __VA_ARGS__)

namespace fs = std::filesystem;
using namespace gsv;

namespace {

// ---------------------------------------------------------------------------------------------
// Virtual joysticks: a stick appears where the finger lands (floating), its value is the finger
// offset divided by the stick radius, clamped to the unit disc.
struct Joystick {
  int32_t pointer = -1;  // Android pointer id holding the stick, -1 = released
  glm::vec2 origin{0.0f}, current{0.0f};
  glm::vec2 value(float radius) const {
    if (pointer < 0) return glm::vec2(0.0f);
    glm::vec2 v = (current - origin) / radius;
    const float len = glm::length(v);
    return len > 1.0f ? v / len : v;
  }
};

struct Touch {  // a finger used for orbit / pinch
  int32_t pointer = -1;
  glm::vec2 last{0.0f};
};

struct SceneEntry {
  std::string label;
  fs::path path;  // empty = procedural demo scene
};

// The PC that trains the videos (tools: gsplat_server). Found with a UDP broadcast.
struct RemoteScene {
  std::string name, state, message;
};
struct NetResult {
  std::string ip, error, downloaded;  // downloaded: local path of the scene file to open
  uint16_t port = 0;
  std::vector<RemoteScene> scenes;
};

struct App {
  android_app* android = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLSurface surface = EGL_NO_SURFACE;
  EGLContext context = EGL_NO_CONTEXT;
  EGLConfig config = nullptr;
  int width = 0, height = 0;
  bool glReady = false;
  bool imguiReady = false;
  std::string fatal;  // shown instead of the scene when something essential is missing

  SplatRenderer splats;
  SplatSettings settings;
  Framebuffer target;
  float renderScale = 0.5f;
  bool halfFloat = true;

  GaussianScene scene;
  OrbitCamera camera;
  glm::mat4 orientation{1.0f};
  bool flipY = false;
  ColmapModel colmap;
  std::future<LoadResult> loading;
  std::string loadingName, status;
  std::vector<SceneEntry> scenes;

  // PC capture server.
  std::string serverIp, netStatus;
  uint16_t serverPort = 0;
  std::vector<RemoteScene> remote;
  std::future<NetResult> netTask;
  std::shared_ptr<std::atomic<long long>> netBytes = std::make_shared<std::atomic<long long>>(0);
  std::shared_ptr<std::atomic<long long>> netTotal = std::make_shared<std::atomic<long long>>(0);

  Joystick move, look;
  Touch touches[2];
  float uiScale = 3.0f;
  ImVec2 uiMin{0, 0}, uiMax{0, 0};  // panel rectangle of the last frame (touches there go to ImGui)
  int32_t uiPointer = -1;
  std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
  float fps = 0.0f;
};

// ---------------------------------------------------------------------------------------------
// Assets: copy assets/<dir>/* to <internal storage>/<dir>/ (AAssetDir lists files, not folders).
void extractAssetDir(AAssetManager* mgr, const std::string& dir, const fs::path& outRoot) {
  AAssetDir* d = AAssetManager_openDir(mgr, dir.c_str());
  if (!d) return;
  std::error_code ec;
  fs::create_directories(outRoot / dir, ec);
  while (const char* name = AAssetDir_getNextFileName(d)) {
    const std::string rel = dir + "/" + name;
    AAsset* a = AAssetManager_open(mgr, rel.c_str(), AASSET_MODE_BUFFER);
    if (!a) continue;
    std::ofstream f(outRoot / rel, std::ios::binary);
    f.write(static_cast<const char*>(AAsset_getBuffer(a)), AAsset_getLength(a));
    AAsset_close(a);
  }
  AAssetDir_close(d);
}

void findScenes(App& app) {
  app.scenes.clear();
  app.scenes.push_back({"Demo scene (procedural, 300 K splats)", {}});
  const char* ext = app.android->activity->externalDataPath;
  if (!ext) return;
  std::error_code ec;
  fs::create_directories(ext, ec);
  for (auto it = fs::recursive_directory_iterator(ext, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it.depth() > 3) it.disable_recursion_pending();
    if (!it->is_regular_file(ec)) continue;
    std::string e = it->path().extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (e == ".ply" || e == ".splat" || e == ".spz") {
      const auto rel = fs::relative(it->path(), ext, ec);
      app.scenes.push_back({rel.string(), it->path()});
    }
  }
}

void startLoading(App& app, const SceneEntry& e);  // below

// ---------------------------------------------------------------------------------------------
// PC capture server (gsplat_server). All network work runs on std::async threads; the frame loop
// polls the future, so the UI never blocks.
std::vector<RemoteScene> parseList(const std::string& text) {
  std::vector<RemoteScene> out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t end = text.find('\n', pos);
    const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 == std::string::npos ? 0 : t1 + 1);
    if (t1 != std::string::npos && t2 != std::string::npos)
      out.push_back({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  return out;
}

// Finds the server (if not known yet) and lists its scenes.
void refreshRemote(App& app) {
  if (app.netTask.valid()) return;
  app.netStatus = app.serverIp.empty() ? "Looking for the PC on this Wi-Fi..." : "Refreshing...";
  const std::string knownIp = app.serverIp;
  const uint16_t knownPort = app.serverPort;
  app.netTask = std::async(std::launch::async, [knownIp, knownPort]() {
    NetResult r;
    r.ip = knownIp;
    r.port = knownPort;
    if (r.ip.empty()) r.ip = net::discoverServer(2000, &r.port);
    if (r.ip.empty()) {
      // The Android emulator sits behind its own NAT (broadcasts do not reach the PC), but it
      // always sees the host PC as 10.0.2.2.
      std::string err;
      if (net::connectTcp("10.0.2.2", net::kHttpPort, 1000, err).valid()) r.ip = "10.0.2.2", r.port = net::kHttpPort;
    }
    if (r.ip.empty()) {
      r.error = "No PC found. Run gsplat_server on the PC (same Wi-Fi) and allow it in the Windows firewall.";
      return r;
    }
    net::Response resp;
    if (!net::httpGet(r.ip, r.port, "/api/list", resp, r.error)) return r;
    r.scenes = parseList(resp.body);
    return r;
  });
}

// Downloads <name>.ply and its COLMAP cameras into <external files>/<name>/.
void downloadRemote(App& app, const std::string& name) {
  if (app.netTask.valid()) return;
  app.netStatus = "Downloading " + name + "...";
  app.netBytes->store(0);
  app.netTotal->store(0);
  const fs::path root = fs::path(app.android->activity->externalDataPath) / name;
  app.netTask = std::async(std::launch::async, [ip = app.serverIp, port = app.serverPort, name, root,
                                                bytes = app.netBytes, total = app.netTotal]() {
    NetResult r;
    r.ip = ip;
    r.port = port;
    net::Response list;
    if (!net::httpGet(ip, port, "/api/files?name=" + net::urlEncode(name), list, r.error)) return r;
    if (list.status != 200) {
      r.error = "The PC has no files for " + name + " (HTTP " + std::to_string(list.status) + ")";
      return r;
    }
    std::vector<std::pair<std::string, long long>> files;  // "relative/path \t size" per line
    size_t pos = 0;
    while (pos < list.body.size()) {
      const size_t end = list.body.find('\n', pos);
      const std::string line = list.body.substr(pos, end - pos);
      const size_t tab = line.find('\t');
      if (tab != std::string::npos) files.push_back({line.substr(0, tab), std::atoll(line.c_str() + tab + 1)});
      if (end == std::string::npos) break;
      pos = end + 1;
    }
    long long sum = 0;
    for (const auto& f : files) sum += f.second;
    total->store(sum);
    if (files.empty()) {
      r.error = "The PC listed no files for " + name;
      return r;
    }
    for (const auto& [rel, size] : files) {
      const fs::path dst = root / rel;
      std::error_code ec;
      fs::create_directories(dst.parent_path(), ec);
      const fs::path part = dst.string() + ".part";
      std::ofstream out(part, std::ios::binary);
      net::Response resp;
      const bool ok = net::httpGet(ip, port, "/files/" + name + "/" + rel, resp, r.error,
                                   [&](const char* p, size_t n) {
                                     out.write(p, static_cast<std::streamsize>(n));
                                     *bytes += static_cast<long long>(n);
                                     return static_cast<bool>(out);
                                   },
                                   30000);
      out.close();
      if (!ok || resp.status != 200) {
        if (r.error.empty()) r.error = "HTTP " + std::to_string(resp.status) + " for " + rel;
        fs::remove(part, ec);
        return r;
      }
      fs::rename(part, dst, ec);
      const std::string e = dst.extension().string();
      if (e == ".ply" || e == ".spz" || e == ".splat") r.downloaded = dst.string();
    }
    return r;
  });
}

void pollNetwork(App& app) {
  if (!app.netTask.valid() || app.netTask.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  NetResult r = app.netTask.get();
  if (!r.error.empty()) {
    app.netStatus = r.error;
    return;
  }
  app.serverIp = r.ip;
  app.serverPort = r.port;
  if (!r.scenes.empty()) app.remote = r.scenes;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "PC %s:%u", app.serverIp.c_str(), app.serverPort);
  app.netStatus = buf;
  if (!r.downloaded.empty()) {  // open the scene that just arrived
    findScenes(app);
    startLoading(app, {fs::path(r.downloaded).filename().string(), r.downloaded});
  }
}

// ---------------------------------------------------------------------------------------------
// EGL
bool initEgl(App& app) {
  if (app.display == EGL_NO_DISPLAY) {
    app.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(app.display, nullptr, nullptr);
    const EGLint attribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(app.display, attribs, &app.config, 1, &count) || count == 0) {
      app.fatal = "No EGL config with OpenGL ES 3";
      LOGE("%s (EGL error 0x%x)", app.fatal.c_str(), eglGetError());
      return false;
    }
  }
  if (app.context == EGL_NO_CONTEXT) {
    // ES 3.2 on recent phones (S23 Ultra), 3.1 is enough (the shaders are GLSL ES 3.10) and is what
    // the Android emulator exposes.
    for (EGLint minor : {2, 1}) {
      const EGLint ctx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, minor, EGL_NONE};
      app.context = eglCreateContext(app.display, app.config, EGL_NO_CONTEXT, ctx);
      if (app.context != EGL_NO_CONTEXT) break;
      LOGI("No OpenGL ES 3.%d context (EGL error 0x%x)", minor, eglGetError());
    }
    if (app.context == EGL_NO_CONTEXT) {
      app.fatal = "This device has no OpenGL ES 3.1 context";
      LOGE("%s", app.fatal.c_str());
      return false;
    }
  }
  app.surface = eglCreateWindowSurface(app.display, app.config, app.android->window, nullptr);
  if (app.surface == EGL_NO_SURFACE || !eglMakeCurrent(app.display, app.surface, app.surface, app.context)) {
    app.fatal = "Cannot create the EGL window surface";
    LOGE("%s (EGL error 0x%x)", app.fatal.c_str(), eglGetError());
    return false;
  }
  eglSwapInterval(app.display, 1);
  return true;
}

void destroySurface(App& app) {
  if (app.display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(app.display, EGL_NO_SURFACE, EGL_NO_SURFACE, app.context);  // keep the context (GPU data)
  if (app.surface != EGL_NO_SURFACE) eglDestroySurface(app.display, app.surface);
  app.surface = EGL_NO_SURFACE;
}

// ---------------------------------------------------------------------------------------------
// Scene
glm::mat4 modelMatrix(const App& app) {
  if (!app.colmap.images.empty()) return app.orientation;
  return app.flipY ? glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1, 0, 0)) : glm::mat4(1.0f);
}

void frameScene(App& app) {
  const glm::vec3 c = glm::vec3(modelMatrix(app) * glm::vec4(app.scene.focusCenter, 1.0f));
  app.camera.frameSphere(c, app.scene.focusRadius, glm::length(app.scene.boundsMax - app.scene.boundsMin));
}

// Same as the desktop viewer: a captured scene opens at its first training camera.
void goToFirstCamera(App& app) {
  if (app.colmap.images.empty()) return;
  const ColmapImage& im = app.colmap.images.front();
  const glm::mat3 r(app.orientation);
  const glm::vec3 eye = r * glm::vec3(im.center());
  const glm::vec3 forward = glm::normalize(r * glm::vec3(im.forwardInWorld()));
  app.camera.lookAt(eye, eye + forward * std::max(app.scene.focusRadius, 0.1f));
  if (const ColmapCamera* cam = app.colmap.camera(im.cameraId)) {
    const glm::dvec4 k = cam->pinhole();
    if (k.y > 0.0) app.camera.fovY = static_cast<float>(2.0 * std::atan(0.5 * cam->height / k.y));
  }
}

void startLoading(App& app, const SceneEntry& e) {
  if (app.loading.valid()) return;
  if (e.path.empty()) {
    app.colmap = {};
    app.scene = makeDemoScene();
    app.splats.upload(app.scene);
    frameScene(app);
    app.status = "Demo scene";
    return;
  }
  app.loadingName = e.label;
  app.loading = loadSceneAsync(e.path);
  // COLMAP model next to the file (dataset/sparse/0 etc.): orientation + first camera.
  app.colmap = {};
  const fs::path dir = findColmapModelFor(e.path);
  std::string error;
  if (!dir.empty() && loadColmapModel(dir, app.colmap, error) && !app.colmap.images.empty())
    app.orientation = glm::mat4_cast(glm::rotation(averageCameraUp(app.colmap), glm::vec3(0, 1, 0)));
  else
    app.colmap = {};
}

void pollLoading(App& app) {
  if (!app.loading.valid() || app.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  LoadResult r = app.loading.get();
  if (!r.ok) {
    app.status = "Load failed: " + r.error;
    return;
  }
  app.scene = std::move(r.scene);
  app.splats.upload(app.scene);
  frameScene(app);
  goToFirstCamera(app);
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s: %zu splats, %.1f s", app.loadingName.c_str(), app.scene.size(),
                r.milliseconds / 1000.0);
  app.status = buf;
}

// ---------------------------------------------------------------------------------------------
// GL resources
void initGl(App& app) {
  if (app.glReady) return;
  const fs::path internal = app.android->activity->internalDataPath;
  extractAssetDir(app.android->activity->assetManager, "shaders", internal);
  extractAssetDir(app.android->activity->assetManager, "shaders/include", internal);
  setShaderDirectory((internal / "shaders").string());

  GLint vertexSsbos = 0;
  glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &vertexSsbos);
  LOGI("GL_RENDERER %s | GL_VERSION %s | vertex SSBOs %d", glGetString(GL_RENDERER), glGetString(GL_VERSION),
       vertexSsbos);
  if (vertexSsbos < 2) {
    app.fatal = "This GPU cannot read storage buffers in vertex shaders (needed by the splat renderer)";
    return;
  }
  std::string log;
  if (!app.splats.init(log)) {
    app.fatal = "Shader error:\n" + log;
    LOGE("%s", log.c_str());
    return;
  }
  app.settings.depthKeyBits = 24;
  app.scene = makeDemoScene();
  app.splats.upload(app.scene);
  frameScene(app);
  app.status = "Demo scene";
  app.glReady = true;
}

void initImGui(App& app) {
  if (app.imguiReady) return;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui_ImplAndroid_Init(app.android->window);
  ImGui_ImplOpenGL3_Init("#version 300 es");
  // Size everything for the screen density (160 dpi = scale 1).
  AConfiguration* cfg = AConfiguration_new();
  AConfiguration_fromAssetManager(cfg, app.android->activity->assetManager);
  const int density = AConfiguration_getDensity(cfg);
  AConfiguration_delete(cfg);
  app.uiScale = std::clamp(density / 160.0f, 1.0f, 4.0f);
  ImFontConfig font;
  font.SizePixels = 13.0f * app.uiScale;
  io.Fonts->AddFontDefault(&font);
  ImGui::GetStyle().ScaleAllSizes(app.uiScale);
  ImGui::GetStyle().TouchExtraPadding = ImVec2(4.0f * app.uiScale, 4.0f * app.uiScale);
  app.imguiReady = true;
}

// ---------------------------------------------------------------------------------------------
// Input
bool inUi(const App& app, glm::vec2 p) {
  return p.x >= app.uiMin.x && p.x <= app.uiMax.x && p.y >= app.uiMin.y && p.y <= app.uiMax.y;
}

int32_t onInput(android_app* a, AInputEvent* event) {
  App& app = *static_cast<App*>(a->userData);
  if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION || !app.imguiReady) return 0;
  ImGuiIO& io = ImGui::GetIO();
  const int32_t action = AMotionEvent_getAction(event);
  const int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
  const size_t index = static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                           AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  auto pos = [&](size_t i) { return glm::vec2(AMotionEvent_getX(event, i), AMotionEvent_getY(event, i)); };

  if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
    const int32_t id = AMotionEvent_getPointerId(event, index);
    const glm::vec2 p = pos(index);
    if (app.uiPointer < 0 && inUi(app, p)) {  // the panel gets this finger as the mouse
      app.uiPointer = id;
      io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
      io.AddMousePosEvent(p.x, p.y);
      io.AddMouseButtonEvent(0, true);
    } else if (p.y > app.height * 0.45f && p.x < app.width * 0.35f && app.move.pointer < 0) {
      app.move = {id, p, p};
    } else if (p.y > app.height * 0.45f && p.x > app.width * 0.65f && app.look.pointer < 0) {
      app.look = {id, p, p};
    } else {
      for (Touch& t : app.touches)
        if (t.pointer < 0) {
          t = {id, p};
          break;
        }
    }
  } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
    // One MOVE event carries every pointer. Two orbit fingers = pinch zoom + pan.
    glm::vec2 now[2] = {app.touches[0].last, app.touches[1].last};
    for (size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i) {
      const int32_t id = AMotionEvent_getPointerId(event, i);
      const glm::vec2 p = pos(i);
      if (id == app.uiPointer) io.AddMousePosEvent(p.x, p.y);
      if (id == app.move.pointer) app.move.current = p;
      if (id == app.look.pointer) app.look.current = p;
      for (int k = 0; k < 2; ++k)
        if (id == app.touches[k].pointer) now[k] = p;
    }
    const bool two = app.touches[0].pointer >= 0 && app.touches[1].pointer >= 0;
    if (two) {
      const float before = glm::length(app.touches[0].last - app.touches[1].last);
      const float after = glm::length(now[0] - now[1]);
      if (before > 1.0f && after > 1.0f) app.camera.zoom(std::log(after / before) / std::log(1.0f / 0.88f));
      const glm::vec2 mid0 = 0.5f * (app.touches[0].last + app.touches[1].last), mid1 = 0.5f * (now[0] + now[1]);
      app.camera.pan(mid1.x - mid0.x, mid1.y - mid0.y, static_cast<float>(app.height));
    } else {
      for (int k = 0; k < 2; ++k)
        if (app.touches[k].pointer >= 0) {
          const glm::vec2 d = now[k] - app.touches[k].last;
          app.camera.orbit(d.x * 0.6f, d.y * 0.6f);
        }
    }
    for (int k = 0; k < 2; ++k) app.touches[k].last = now[k];
  } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP ||
             kind == AMOTION_EVENT_ACTION_CANCEL) {
    const bool all = kind != AMOTION_EVENT_ACTION_POINTER_UP;
    const int32_t id = AMotionEvent_getPointerId(event, index);
    auto released = [&](int32_t p) { return p >= 0 && (all || p == id); };
    if (released(app.uiPointer)) {
      io.AddMouseButtonEvent(0, false);
      app.uiPointer = -1;
    }
    if (released(app.move.pointer)) app.move.pointer = -1;
    if (released(app.look.pointer)) app.look.pointer = -1;
    for (Touch& t : app.touches)
      if (released(t.pointer)) t.pointer = -1;
  }
  return 1;
}

void onCommand(android_app* a, int32_t cmd) {
  App& app = *static_cast<App*>(a->userData);
  switch (cmd) {
    case APP_CMD_INIT_WINDOW:
      if (a->window && initEgl(app)) {
        initGl(app);
        if (app.imguiReady) {  // back from the background: a new window, the old pointer is dead
          ImGui_ImplAndroid_Shutdown();
          ImGui_ImplAndroid_Init(a->window);
        }
        initImGui(app);
        findScenes(app);
      }
      break;
    case APP_CMD_TERM_WINDOW:
      destroySurface(app);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------------------------
// Frame
void drawJoystick(ImDrawList* dl, const Joystick& j, float radius, ImU32 color) {
  if (j.pointer < 0) return;
  const glm::vec2 v = j.value(radius);
  dl->AddCircle(ImVec2(j.origin.x, j.origin.y), radius, color, 48, 4.0f);
  dl->AddCircleFilled(ImVec2(j.origin.x + v.x * radius, j.origin.y + v.y * radius), radius * 0.35f, color);
}

void drawUi(App& app) {
  ImGui::SetNextWindowPos(ImVec2(16 * app.uiScale, 16 * app.uiScale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(330 * app.uiScale, 0), ImGuiCond_FirstUseEver);
  ImGui::Begin("gsplat-viewer");
  ImGui::Text("%.0f FPS  |  %zu splats", app.fps, app.scene.size());
  const SplatStats& s = app.splats.stats();
  ImGui::Text("GPU: pre %.2f  sort %.2f  draw %.2f ms", s.preprocessMs, s.gpuSortMs, s.drawMs);
  if (app.loading.valid()) ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "Loading %s ...", app.loadingName.c_str());
  if (!app.status.empty()) ImGui::TextWrapped("%s", app.status.c_str());
  if (ImGui::CollapsingHeader("Scenes", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const SceneEntry& e : app.scenes)
      if (ImGui::Selectable(e.label.c_str())) startLoading(app, e);
    if (ImGui::Button("Refresh list")) findScenes(app);
    ImGui::TextDisabled("Copy scenes to Android/data/<app>/files");
  }
  if (ImGui::CollapsingHeader("PC capture", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextDisabled("Send videos from the phone's browser:\nhttp://<PC>:%u", net::kHttpPort);
    ImGui::BeginDisabled(app.netTask.valid());
    if (ImGui::Button(app.serverIp.empty() ? "Find PC" : "Refresh")) refreshRemote(app);
    ImGui::EndDisabled();
    if (!app.netStatus.empty()) ImGui::TextWrapped("%s", app.netStatus.c_str());
    if (app.netTask.valid() && *app.netTotal > 0)
      ImGui::ProgressBar(static_cast<float>(static_cast<double>(*app.netBytes) / static_cast<double>(*app.netTotal)));
    for (const RemoteScene& r : app.remote) {
      ImGui::PushID(r.name.c_str());
      ImGui::Text("%s  [%s]", r.name.c_str(), r.state.c_str());
      if (r.state == "done") {
        ImGui::SameLine();
        ImGui::BeginDisabled(app.netTask.valid());
        if (ImGui::SmallButton("Download")) downloadRemote(app, r.name);
        ImGui::EndDisabled();
      } else if (!r.message.empty()) {
        ImGui::TextDisabled("  %s", r.message.c_str());
      }
      ImGui::PopID();
    }
  }
  if (ImGui::CollapsingHeader("View")) {
    ImGui::SliderFloat("Resolution", &app.renderScale, 0.25f, 1.0f, "%.2f x");
    ImGui::SliderInt("SH degree", &app.settings.shDegree, 0, 3);
    ImGui::Checkbox("16-bit float blending", &app.halfFloat);
    if (app.colmap.images.empty() && ImGui::Checkbox("Flip Y (COLMAP scenes)", &app.flipY)) frameScene(app);
    if (ImGui::Button("Frame scene")) frameScene(app);
    ImGui::SameLine();
    if (!app.colmap.images.empty() && ImGui::Button("First camera")) goToFirstCamera(app);
  }
  ImGui::TextDisabled("Left stick: move | right stick: look\nDrag: orbit | pinch: zoom");
  const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
  app.uiMin = pos;
  app.uiMax = ImVec2(pos.x + size.x, pos.y + size.y);
  ImGui::End();

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float radius = 70.0f * app.uiScale;
  drawJoystick(dl, app.move, radius, IM_COL32(255, 255, 255, 140));
  drawJoystick(dl, app.look, radius, IM_COL32(255, 200, 80, 140));
}

void drawFrame(App& app) {
  if (app.surface == EGL_NO_SURFACE) return;
  eglQuerySurface(app.display, app.surface, EGL_WIDTH, &app.width);
  eglQuerySurface(app.display, app.surface, EGL_HEIGHT, &app.height);
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::min(0.1f, std::chrono::duration<float>(now - app.lastFrame).count());
  app.lastFrame = now;
  app.fps = app.fps == 0.0f ? 1.0f / std::max(dt, 1e-4f) : app.fps * 0.95f + 0.05f / std::max(dt, 1e-4f);

  pollLoading(app);
  pollNetwork(app);

  // Joysticks: left = move in the view direction / sideways, right = look around the eye.
  const float radius = 70.0f * app.uiScale;
  const glm::vec2 m = app.move.value(radius), l = app.look.value(radius);
  if (glm::length(m) > 0.05f)
    app.camera.move(glm::normalize(glm::vec3(m.x, 0.0f, -m.y)) * glm::length(m), 2.0f * app.scene.focusRadius * 0.4f * dt,
                    true);
  if (glm::length(l) > 0.05f) app.camera.look(l.x * 500.0f * dt, l.y * 500.0f * dt);

  if (app.fatal.empty()) {
    const int w = std::max(1, static_cast<int>(app.width * app.renderScale));
    const int h = std::max(1, static_cast<int>(app.height * app.renderScale));
    const Framebuffer::Format fmt = app.halfFloat ? Framebuffer::Format::RGBA16F : Framebuffer::Format::RGBA8;
    if (app.target.width() != w || app.target.height() != h || app.target.format() != fmt) {
      if (!app.target.create(w, h, fmt) && fmt == Framebuffer::Format::RGBA16F) {
        app.halfFloat = false;  // no renderable half float: fall back to 8 bits
        app.target.create(w, h, Framebuffer::Format::RGBA8);
      }
    }
    app.target.bind();
    glClearColor(0.07f, 0.08f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    RenderView v;
    v.model = modelMatrix(app);
    v.view = app.camera.view();
    v.viewport = {w, h};
    v.projection = app.camera.projection(v.aspect());
    v.cameraPosition = app.camera.position();
    v.fovY = app.camera.fovY;
    v.nearPlane = app.camera.nearPlane;
    app.splats.render(v, app.settings);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, app.target.id());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, w, h, 0, 0, app.width, app.height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.25f, 0.05f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glViewport(0, 0, app.width, app.height);

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplAndroid_NewFrame();
  ImGui::NewFrame();
  if (app.fatal.empty()) {
    drawUi(app);
  } else {
    ImGui::Begin("Error");
    ImGui::TextWrapped("%s", app.fatal.c_str());
    ImGui::End();
  }
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  eglSwapBuffers(app.display, app.surface);
}

}  // namespace

void android_main(android_app* state) {
  App app;
  app.android = state;
  state->userData = &app;
  state->onAppCmd = onCommand;
  state->onInputEvent = onInput;

  for (;;) {
    int events = 0;
    android_poll_source* source = nullptr;
    // Block while there is no window, otherwise poll and draw continuously.
    const int timeout = app.surface != EGL_NO_SURFACE ? 0 : -1;
    while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
      if (source) source->process(state, source);
      if (state->destroyRequested) {
        if (app.imguiReady) {
          ImGui_ImplOpenGL3_Shutdown();
          ImGui_ImplAndroid_Shutdown();
          ImGui::DestroyContext();
        }
        destroySurface(app);
        if (app.context != EGL_NO_CONTEXT) eglDestroyContext(app.display, app.context);
        if (app.display != EGL_NO_DISPLAY) eglTerminate(app.display);
        return;
      }
      if (app.surface != EGL_NO_SURFACE) break;  // draw a frame between event batches
    }
    drawFrame(app);
  }
}
