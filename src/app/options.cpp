#include "app/options.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace gsv {

namespace {
void printUsage() {
  std::printf(
      "gsplat-viewer " GSV_VERSION "\n\n"
      "Usage:\n"
      "  gsplat_viewer [scene.ply] [options]\n"
      "  gsplat_viewer --gen out.ply [count]\n\n"
      "Options:\n"
      "  --flip-y             scene is stored Y-down (INRIA / COLMAP)\n"
      "  --blend FORMAT       target splats blend into: rgba8 (fastest), rgb10a2, rgba16f (default)\n"
      "  --size WxH           window / image size (default 1600x900)\n"
      "  --screenshot F.png   render without UI, save the image and exit\n"
      "  --points             start in point mode\n"
      "  --yaw RADIANS        camera yaw around the scene\n"
      "  --sh 0..3            spherical harmonics degree\n"
      "  --key-bits 16|24|32  depth key precision of the GPU sort (default 24, same image as 32)\n"
      "  --no-compress        upload 64-byte fp32 splats instead of the 24-byte compressed format\n"
      "  --sort MODE          none | cpu-std | cpu-radix | gpu (default)\n"
      "  --view MODE          color | depth | overdraw | ellipses\n"
      "  --distance SCALE     camera distance relative to the default framing\n"
      "  --camera-pos X,Y,Z   exact camera position (with --camera-target)\n"
      "  --camera-target X,Y,Z\n"
      "  --background R,G,B   clear color in [0,1]\n"
      "  --bench              benchmark several configurations (orbit), print a table, exit\n"
      "  --frames N           frames per benchmark configuration / turntable (default 240)\n"
      "  --turntable DIR      save a 360 degree orbit as PNG frames and exit\n"
      "  --csv FILE           append benchmark results to a CSV file\n"
      "  --shader-dir DIR     read shaders from DIR (default: <exe dir>/shaders, then the source tree)\n"
      "  --colmap DIR         COLMAP model with the training cameras (default: <scene dir>/dataset/sparse/0)\n"
      "  --min-opacity A      edit: delete splats with opacity < A (floaters)\n"
      "  --max-size S         edit: delete splats whose largest axis > S scene units\n"
      "  --crop-sphere x,y,z,r  edit: delete everything outside this sphere (display space)\n"
      "  --save-edited F.ply  write the edited scene as a standard 3DGS PLY\n"
      "  --eval               render every held-out training view, compare with the photo (PSNR, SSIM)\n"
      "                       and with Brush's render; writes <scene dir>/eval/ and exits\n"
      "  --eval-every N       held-out split if Brush's eval folder is missing (default 8)\n");
}
}  // namespace

bool parseOptions(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--blend") {
      const char* v = next();
      if (!v || (std::string(v) != "rgba8" && std::string(v) != "rgb10a2" && std::string(v) != "rgba16f")) return false;
      o.blendFormat = v;
    } else if (a == "--flip-y") {
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
    } else if (a == "--no-compress") {
      o.noCompress = true;
    } else if (a == "--key-bits") {
      const char* v = next();
      if (!v || (std::atoi(v) != 16 && std::atoi(v) != 24 && std::atoi(v) != 32)) return printUsage(), false;
      o.keyBits = std::atoi(v);
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
    } else if (a == "--min-opacity" || a == "--max-size") {
      const char* v = next();
      if (!v) return printUsage(), false;
      (a == "--min-opacity" ? o.editMinOpacity : o.editMaxSize) = std::strtof(v, nullptr);
    } else if (a == "--crop-sphere") {
      const char* v = next();
      float* c = o.cropSphere;
      if (!v || std::sscanf(v, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) != 4) return printUsage(), false;
      o.hasCropSphere = true;
    } else if (a == "--save-edited") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.saveEdited = v;
    } else if (a == "--turntable") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.turntableDir = v;
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
    } else if (a == "--shader-dir") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.shaderDir = v;
    } else if (a == "--colmap") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.colmapDir = v;
    } else if (a == "--eval") {
      o.evaluate = true;
    } else if (a == "--eval-every") {
      const char* v = next();
      if (!v) return printUsage(), false;
      o.evalEvery = std::max(2, std::atoi(v));
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
