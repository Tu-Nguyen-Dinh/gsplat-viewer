#include "app/options.h"

#include <cstdio>
#include <cstdlib>

namespace gsv {

namespace {
void printUsage() {
  std::printf(
      "Usage:\n"
      "  gsplat_viewer [scene.ply] [options]\n"
      "  gsplat_viewer --gen out.ply [count]\n\n"
      "Options:\n"
      "  --flip-y             scene is stored Y-down (INRIA / COLMAP)\n"
      "  --size WxH           window / image size (default 1600x900)\n"
      "  --screenshot F.png   render without UI, save the image and exit\n"
      "  --points             start in point mode\n");
}
}  // namespace

bool parseOptions(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--flip-y") {
      o.flipY = true;
    } else if (a == "--points") {
      o.points = true;
    } else if (a == "--screenshot") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.screenshot = v;
    } else if (a == "--size") {
      const char* v = next();
      if (!v || std::sscanf(v, "%dx%d", &o.width, &o.height) != 2) return printUsage(), false;
    } else if (a == "--gen") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.generateTo = v;
      if (i + 1 < argc && argv[i + 1][0] != '-') o.generateCount = std::strtoull(argv[++i], nullptr, 10);
    } else if (a == "--help" || a == "-h") {
      printUsage();
      return false;
    } else if (!a.empty() && a[0] != '-') {
      o.scene = a;
    } else {
      std::fprintf(stderr, "Unknown option: %s\n\n", a.c_str());
      printUsage();
      return false;
    }
  }
  return true;
}

}  // namespace gsv
