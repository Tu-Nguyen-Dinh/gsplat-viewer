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
  float yaw = -1000.0f;        // camera yaw in radians (default: keep the camera's)
  int shDegree = 3;
  int sortMode = 3;            // SortMode: 0 none, 1 cpu-std, 2 cpu-radix, 3 gpu
};

// Returns false (and prints usage) on invalid arguments.
bool parseOptions(int argc, char** argv, Options& out);

}  // namespace gsv
