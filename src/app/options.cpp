#include "app/options.h"

#include <algorithm>
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
      "  --points             start in point mode\n"
      "  --yaw RADIANS        camera yaw around the scene\n"
      "  --sh 0..3            spherical harmonics degree\n"
      "  --sort MODE          none | cpu-std | cpu-radix | gpu (default)\n"
      "  --view MODE          color | depth | overdraw | ellipses\n"
      "  --distance SCALE     camera distance relative to the default framing\n"
      "  --camera-pos X,Y,Z   exact camera position (with --camera-target)\n"
      "  --camera-target X,Y,Z\n"
      "  --background R,G,B   clear color in [0,1]\n"
      "  --bench              benchmark several configurations (orbit), print a table, exit\n"
      "  --frames N           measured frames per configuration (default 240)\n"
      "  --csv FILE           append benchmark results to a CSV file\n");
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
    } else if (a == "--yaw") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.yaw = std::strtof(v, nullptr);
    } else if (a == "--sh") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.shDegree = std::atoi(v);
    } else if (a == "--sort") {
      const char* v = next();
      const std::string m = v ? v : "";
      if (m == "none") o.sortMode = 0;
      else if (m == "cpu-std") o.sortMode = 1;
      else if (m == "cpu-radix") o.sortMode = 2;
      else if (m == "gpu") o.sortMode = 3;
      else return printUsage(), false;
    } else if (a == "--view") {
      const char* v = next();
      const std::string m = v ? v : "";
      if (m == "color") o.debugView = 0;
      else if (m == "depth") o.debugView = 1;
      else if (m == "overdraw") o.debugView = 2;
      else if (m == "ellipses") o.debugView = 3;
      else return printUsage(), false;
    } else if (a == "--distance") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.distanceScale = std::strtof(v, nullptr);
    } else if (a == "--camera-pos" || a == "--camera-target" || a == "--background") {
      const char* v = next();
      float* dst = a == "--camera-pos" ? o.cameraPos : a == "--camera-target" ? o.cameraTarget : o.background;
      if (!v || std::sscanf(v, "%f,%f,%f", &dst[0], &dst[1], &dst[2]) != 3) return printUsage(), false;
      if (a != "--background") o.hasCamera = true;
    } else if (a == "--bench") {
      o.benchmark = true;
    } else if (a == "--frames") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.benchFrames = std::max(1, std::atoi(v));
    } else if (a == "--csv") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.benchCsv = v;
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
