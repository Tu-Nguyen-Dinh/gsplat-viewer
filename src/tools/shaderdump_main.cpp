// gsplat_shaderdump: writes every shader of the project with its #includes expanded, in the
// desktop (GLSL 4.30) or the OpenGL ES (GLSL ES 3.10) dialect. Lets the ES versions be checked
// offline with a reference compiler (tools/check_shaders_es.ps1 runs the NDK's glslc), without
// an Android device.
//
//   gsplat_shaderdump OUT_DIR [--es] [--shader-dir DIR]
#include "core/utf8_path.h"
#include "gl/shader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: gsplat_shaderdump OUT_DIR [--es] [--shader-dir DIR]\n");
    return 2;
  }
  const std::filesystem::path out = gsv::pathFromUtf8(argv[1]);
  bool es = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--es") es = true;
    else if (a == "--shader-dir" && i + 1 < argc) gsv::setShaderDirectory(argv[++i]);
  }
  std::error_code ec;
  std::filesystem::create_directories(out, ec);

  // Every program stage the renderers load (file name = stage for glslc: .vert / .frag / .comp).
  static const char* kStages[] = {"points.vert",           "points.frag",          "cloud.vert",
                                  "cloud.frag",            "splat.vert",
                                  "splat.frag",            "splat_preprocess.comp", "radix_histogram.comp",
                                  "radix_scatter.comp",    "scan_local.comp",       "scan_add.comp"};
  int failures = 0;
  for (const char* file : kStages) {
    std::string src, log;
    if (!gsv::expandShaderSource(file, es, src, log)) {
      std::fprintf(stderr, "%s", log.c_str());
      ++failures;
      continue;
    }
    std::ofstream f(out / file, std::ios::binary);
    f << src;
    std::printf("wrote %s (%zu bytes)\n", file, src.size());
  }
  return failures ? 1 : 0;
}
