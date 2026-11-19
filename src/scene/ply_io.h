#pragma once

#include "scene/gaussian_scene.h"

#include <filesystem>
#include <string>

namespace gsv {

// Reads a binary little-endian PLY. Supports
//  - 3DGS training output (INRIA layout: x,y,z, f_dc_*, f_rest_*, opacity, scale_*, rot_*),
//    including 2DGS files that only have scale_0/scale_1;
//  - plain point clouds with red/green/blue (shown as small isotropic splats).
bool loadPly(const std::filesystem::path& path, GaussianScene& out, std::string& error);

// Writes a scene in the standard 3DGS PLY layout (readable by other viewers and tools).
bool savePly(const std::filesystem::path& path, const GaussianScene& scene, std::string& error);

// Builds a path from a UTF-8 string (GLFW drop events, ImGui text) so that
// non-ASCII folder names work on Windows.
std::filesystem::path pathFromUtf8(const std::string& utf8);

}  // namespace gsv
