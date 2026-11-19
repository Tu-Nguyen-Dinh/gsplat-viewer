#pragma once

#include "scene/gaussian_scene.h"

#include <cstddef>
#include <filesystem>
#include <future>
#include <string>

namespace gsv {

// Antimatter15 ".splat": 32 bytes per splat, no header, no spherical harmonics.
//   float32 position[3] | float32 scale[3] (linear) | uint8 rgba[4] | uint8 rotation[4] (w,x,y,z)
// Used by many web viewers; also the target of `splat-transform in.ply out.splat`.
bool loadSplat(const std::filesystem::path& path, GaussianScene& out, std::string& error);
bool saveSplat(const std::filesystem::path& path, const GaussianScene& scene, std::string& error);

// Removes splats with NaN/Inf values or a zero-length quaternion (they break the
// GPU math: one NaN covariance can produce a full-screen garbage quad). Returns
// the number of splats removed.
size_t removeInvalidSplats(GaussianScene& scene);

struct LoadResult {
  bool ok = false;
  GaussianScene scene;
  std::string error;
  double milliseconds = 0.0;
  size_t removedInvalid = 0;
};

// Picks the reader from the file extension (.ply or .splat), validates the data
// and computes the bounds. Pure CPU work: safe to run on a worker thread.
LoadResult loadScene(const std::filesystem::path& path);

// Runs loadScene on a background thread. The OpenGL upload must still happen on
// the thread that owns the context, after the future is ready.
std::future<LoadResult> loadSceneAsync(const std::filesystem::path& path);

}  // namespace gsv
