#include "gl/headless_context.h"

#include "gl/gl_loader.h"

#if !defined(GSV_GLES)
#include <GLFW/glfw3.h>
#endif

#if defined(__linux__) && !defined(GSV_GLES)
#include <dlfcn.h>

#include <cstdint>
#endif

namespace gsv {

#if !defined(GSV_GLES)
namespace {
GLFWwindow* gWindow = nullptr;
bool gActive = false;
bool gGlfwInitialized = false;

#if defined(__linux__)
void* gEglLib = nullptr;
using EglProc = void* (*)(const char*);
EglProc gEglGetProcAddress = nullptr;
void* gEglDisplay = nullptr;
void* gEglContext = nullptr;

bool createEglSurfaceless(std::string& reason) {
  gEglLib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!gEglLib) {
    reason += "; no libEGL.so.1";
    return false;
  }
  using GetPlatformDisplay = void* (*)(unsigned, void*, const intptr_t*);
  using Initialize = unsigned (*)(void*, int*, int*);
  using BindApi = unsigned (*)(unsigned);
  using CreateContext = void* (*)(void*, void*, void*, const int*);
  using MakeCurrent = unsigned (*)(void*, void*, void*, void*);
  auto getDisplay = reinterpret_cast<GetPlatformDisplay>(dlsym(gEglLib, "eglGetPlatformDisplay"));
  auto initialize = reinterpret_cast<Initialize>(dlsym(gEglLib, "eglInitialize"));
  auto bindApi = reinterpret_cast<BindApi>(dlsym(gEglLib, "eglBindAPI"));
  auto createContext = reinterpret_cast<CreateContext>(dlsym(gEglLib, "eglCreateContext"));
  auto makeCurrent = reinterpret_cast<MakeCurrent>(dlsym(gEglLib, "eglMakeCurrent"));
  gEglGetProcAddress = reinterpret_cast<EglProc>(dlsym(gEglLib, "eglGetProcAddress"));
  if (!getDisplay || !initialize || !bindApi || !createContext || !makeCurrent || !gEglGetProcAddress) {
    reason += "; EGL 1.5 entry points missing";
    return false;
  }
  constexpr unsigned kPlatformSurfaceless = 0x31DD;  // EGL_PLATFORM_SURFACELESS_MESA
  constexpr unsigned kOpenGlApi = 0x30A2;            // EGL_OPENGL_API
  gEglDisplay = getDisplay(kPlatformSurfaceless, nullptr, nullptr);
  int major = 0, minor = 0;
  if (!gEglDisplay || !initialize(gEglDisplay, &major, &minor) || !bindApi(kOpenGlApi)) {
    reason += "; EGL surfaceless display unavailable";
    return false;
  }
  // EGL_CONTEXT_MAJOR_VERSION 4, MINOR 3, core profile; no config (EGL_KHR_no_config_context).
  const int attribs[] = {0x3098, 4, 0x30FB, 3, 0x30FD, 0x1, 0x3038};
  gEglContext = createContext(gEglDisplay, nullptr, nullptr, attribs);
  if (!gEglContext || !makeCurrent(gEglDisplay, nullptr, nullptr, gEglContext)) {
    reason += "; no OpenGL 4.3 core EGL context";
    return false;
  }
  std::string missing;
  if (!gl::loadFunctions([](const char* n) { return gEglGetProcAddress(n); }, missing)) {
    reason += "; missing GL functions: " + missing;
    return false;
  }
  return true;
}
#endif
}  // namespace

bool createHeadlessGlContext(std::string& reason) {
  if (gActive) return true;
  reason.clear();
  if (glfwInit()) {
    gGlfwInitialized = true;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    gWindow = glfwCreateWindow(64, 64, "gsplat-headless", nullptr, nullptr);
    if (gWindow) {
      glfwMakeContextCurrent(gWindow);
      std::string missing;
      if (gl::loadFunctions([](const char* n) { return reinterpret_cast<void*>(glfwGetProcAddress(n)); }, missing)) {
        gActive = true;
        return true;
      }
      reason = "missing GL functions: " + missing;
      glfwDestroyWindow(gWindow);
      gWindow = nullptr;
    } else {
      reason = "no OpenGL 4.3 core window context";
    }
    glfwTerminate();
    gGlfwInitialized = false;
  } else {
    reason = "glfwInit failed (no display)";
  }
#if defined(__linux__)
  if (createEglSurfaceless(reason)) {
    gActive = true;
    return true;
  }
#endif
  return false;
}

void destroyHeadlessGlContext() {
  if (gWindow) glfwDestroyWindow(gWindow);
  gWindow = nullptr;
  if (gGlfwInitialized) glfwTerminate();
  gGlfwInitialized = false;
  // The EGL context is left to process exit: some drivers crash when terminating EGL from atexit.
  gActive = false;
}

bool headlessGlContextActive() { return gActive; }

std::string glRendererName() {
  if (!gActive || !glGetString) return {};
  const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  return r ? std::string(r) : std::string();
}

#else  // GSV_GLES
bool createHeadlessGlContext(std::string& reason) {
  reason = "headless contexts are not provided on OpenGL ES builds";
  return false;
}
void destroyHeadlessGlContext() {}
bool headlessGlContextActive() { return false; }
std::string glRendererName() { return {}; }
#endif

}  // namespace gsv
