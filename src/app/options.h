#pragma once

#include <cstddef>
#include <string>

namespace gsv {

// Command line options.
//   gsplat_viewer [scene.ply] [--flip-y] [--size WxH] [--screenshot out.png] [--points]
//   gsplat_viewer --gen out.ply [count]
struct Options {
  std::string scene;
  std::string screenshot;      // render a few frames without UI, save, exit
  std::string generateTo;      // write the demo scene to this PLY and exit
  size_t generateCount = 300000;
  bool flipY = false;
  bool points = false;         // start in point mode
  int width = 1600, height = 900;
};

// Returns false (and prints usage) on invalid arguments.
bool parseOptions(int argc, char** argv, Options& out);

}  // namespace gsv
