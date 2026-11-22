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
  std::string blendFormat = "rgba16f";  // --blend: rgba8 (v1, fastest) | rgb10a2 | rgba16f (most precise)
  bool points = false;         // start in point mode
  int width = 1600, height = 900;
  // Given on the command line: they win over the saved settings (core/settings.h).
  bool sizeGiven = false, shGiven = false, backgroundGiven = false;
  float yaw = -1000.0f;        // camera yaw in radians (default: keep the camera's)
  int shDegree = 3;
  bool noCompress = false;      // --no-compress: 64-byte fp32 splats instead of the 24-byte format
  int keyBits = 24;             // --key-bits 16|24|32: GPU sort key precision (24 = same image as 32, 6 passes)
  int sortMode = 3;            // SortMode: 0 none, 1 cpu-std, 2 cpu-radix, 3 gpu
  int debugView = 0;           // DebugView: 0 color, 1 depth, 2 overdraw, 3 ellipses
  float distanceScale = 1.0f;  // < 1 moves the camera closer than the default framing
  bool hasCamera = false;      // --camera-pos / --camera-target given
  float cameraPos[3] = {0, 0, 5}, cameraTarget[3] = {0, 0, 0};
  float background[3] = {0.07f, 0.08f, 0.1f};
  bool benchmark = false;      // --bench: measure several renderer configurations and exit
  int benchFrames = 240;       // frames per benchmark configuration / turntable frames
  std::string turntableDir;    // --turntable DIR: save an orbit as PNG frames and exit
  std::string flythroughDir;   // --flythrough DIR: fly along the training cameras, PNG frames, exit
  std::string benchCsv;        // append results as CSV rows
  std::string shaderDir;       // --shader-dir: override where shader sources are read from
  std::string colmapDir;       // --colmap DIR: camera poses (default: found next to the scene file)
  bool evaluate = false;       // --eval: render the held-out views, PSNR / SSIM vs the photos, exit
  int evalEvery = 8;           // --eval-every N: held-out split when Brush's eval folder is missing
  // Batch editing, applied right after loading (same operations as the Edit panel):
  float editMinOpacity = 0.0f;     // --min-opacity A: delete splats with opacity < A
  float editMaxSize = 0.0f;        // --max-size S: delete splats whose largest axis > S (scene units)
  bool hasCropSphere = false;      // --crop-sphere x,y,z,r: delete everything outside (display space)
  float cropSphere[4] = {0, 0, 0, 1};
  std::string saveEdited;          // --save-edited out.ply: write the edited scene
  // Core 1-3 layers (docs/core): a LAS point cloud shown with the splats, measuring, LOD.
  std::string las;                 // --las FILE: point-cloud layer (opaque, occludes splats)
  int lasStride = 1;               // --las-stride N: keep every N-th point (previews of huge scans)
  bool hasLasTransform = false;    // --las-transform s,qw,qx,qy,qz,tx,ty,tz: LAS world -> scene coordinates
  double lasTransform[8] = {1, 1, 0, 0, 0, 0, 0, 0};
  long long lodBudget = 0;         // --lod N: draw at most N splats chosen by the LOD cut (0 = off)
  float lodPixelError = 1.5f;      // --lod-error PX
};

// Returns false (and prints usage) on invalid arguments.
bool parseOptions(int argc, char** argv, Options& out);

}  // namespace gsv
