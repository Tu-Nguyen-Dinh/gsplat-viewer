// gsplat-viewer: real-time 3D Gaussian Splatting viewer (C++20 / OpenGL 4.3).
#include "app/app.h"
#include "app/options.h"
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

  gsv::App app;
  return app.run(options);
}
