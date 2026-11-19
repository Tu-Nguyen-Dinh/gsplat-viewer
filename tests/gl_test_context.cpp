#include "gl_test_context.h"

#include "gl/gl_loader.h"

#include <GLFW/glfw3.h>

namespace test {

namespace {
GLFWwindow* gWindow = nullptr;
}

bool createGlContext(std::string& reason) {
  if (!glfwInit()) {
    reason = "glfwInit failed";
    return false;
  }
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  gWindow = glfwCreateWindow(64, 64, "gsplat-tests", nullptr, nullptr);
  if (!gWindow) {
    reason = "no OpenGL 4.3 core context";
    glfwTerminate();
    return false;
  }
  glfwMakeContextCurrent(gWindow);
  std::string missing;
  if (!gl::loadFunctions([](const char* n) { return reinterpret_cast<void*>(glfwGetProcAddress(n)); }, missing)) {
    reason = "missing GL functions: " + missing;
    return false;
  }
  return true;
}

void destroyGlContext() {
  if (gWindow) glfwDestroyWindow(gWindow);
  gWindow = nullptr;
  glfwTerminate();
}

}  // namespace test
