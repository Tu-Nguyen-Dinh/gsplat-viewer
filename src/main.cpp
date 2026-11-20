// gsplat-viewer: real-time 3D Gaussian Splatting viewer (C++20 / OpenGL 4.3).
#include "app/app.h"
#include "app/options.h"
#include "core/platform.h"
#include "gl/shader.h"
#include "scene/demo_scene.h"
#include "scene/ply_io.h"

#include <cstdio>

int main(int argc, char** argv) {
  gsv::Options options;
  if (!gsv::parseOptions(argc, argv, options)) return 1;

  if (!options.generateTo.empty()) {
    std::string error;
    if (!gsv::savePly(gsv::pathFromUtf8(options.generateTo), gsv::makeDemoScene(options.generateCount), error)) {
      std::fprintf(stderr, "Failed: %s\n", error.c_str());
      return 1;
    }
    std::printf("Wrote %zu splats to %s\n", options.generateCount, options.generateTo.c_str());
    return 0;
  }

  // Shaders: --shader-dir, else <exe dir>/shaders (packaged build), else the source tree (dev build).
  const std::string shaderDir = gsv::findShaderDirectory(options.shaderDir, gsv::executableDirectory());
  if (shaderDir.empty()) {
    std::fprintf(stderr, "Cannot find the shaders/ folder (--shader-dir %s, <exe dir>/shaders, %s).\n",
                 options.shaderDir.empty() ? "not given" : options.shaderDir.c_str(),
                 gsv::shaderDirectory().c_str());
    return 1;
  }
  gsv::setShaderDirectory(shaderDir);

  gsv::App app;
  return app.run(options);
}
