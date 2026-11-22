#include "gl_test_context.h"

#include "gl/headless_context.h"
#include "test_framework.h"

namespace test {

bool createGlContext(std::string& reason) { return gsv::createHeadlessGlContext(reason); }

void destroyGlContext() { gsv::destroyHeadlessGlContext(); }

bool softwareRenderer() {
  const std::string name = gsv::glRendererName();
  return name.find("llvmpipe") != std::string::npos || name.find("softpipe") != std::string::npos ||
         name.find("SwiftShader") != std::string::npos;
}

}  // namespace test
