// Android CPU test runner (gsplat_tests_cpu): no desktop OpenGL window, so GPU tests are skipped.
// The renderer itself is exercised on the device by the app.
#include "gl_test_context.h"

namespace test {
bool createGlContext(std::string& reason) {
  reason = "CPU-only build for Android (run the app for the GPU path)";
  return false;
}
void destroyGlContext() {}
}  // namespace test
