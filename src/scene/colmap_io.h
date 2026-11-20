#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gsv {

// Cameras and image poses of a COLMAP sparse model (cameras.{bin,txt} + images.{bin,txt}).
//
// COLMAP conventions: the camera looks down +Z with +Y pointing down in the image (computer
// vision), and an image stores the WORLD -> CAMERA transform x_cam = R x_world + t.
// Pixel coordinates have their origin at the top-left corner of the top-left pixel.
struct ColmapCamera {
  uint32_t id = 0;
  int model = 0;  // COLMAP model id (0 SIMPLE_PINHOLE, 1 PINHOLE, 2 SIMPLE_RADIAL, 4 OPENCV, ...)
  int width = 0, height = 0;
  std::vector<double> params;

  // Pinhole part (fx, fy, cx, cy) of any supported model; distortion terms are ignored, so use
  // undistorted (PINHOLE) models when exact projection matters.
  glm::dvec4 pinhole() const;
};

struct ColmapImage {
  uint32_t id = 0;
  glm::dquat rotation{1.0, 0.0, 0.0, 0.0};  // world -> camera (w, x, y, z)
  glm::dvec3 translation{0.0};              // world -> camera
  uint32_t cameraId = 0;
  std::string name;  // relative to the image folder

  glm::dmat3 worldToCameraRotation() const { return glm::mat3_cast(rotation); }
  glm::dvec3 center() const { return -glm::transpose(worldToCameraRotation()) * translation; }  // C = -R^T t
  glm::dvec3 upInWorld() const { return -glm::transpose(worldToCameraRotation())[1]; }           // image -Y
  glm::dvec3 forwardInWorld() const { return glm::transpose(worldToCameraRotation())[2]; }       // +Z
  // World -> OpenGL view matrix (camera looks down -Z, +Y up): flip the Y and Z axes of COLMAP's camera.
  glm::mat4 viewMatrixGl() const;
};

struct ColmapModel {
  std::vector<ColmapCamera> cameras;
  std::vector<ColmapImage> images;  // sorted by name
  const ColmapCamera* camera(uint32_t id) const;
};

// Number of parameters of a COLMAP camera model, or -1 if the model id is unknown.
int colmapModelParamCount(int model);

// Loads <dir>/cameras + <dir>/images, preferring .bin over .txt. Points are not loaded.
bool loadColmapModel(const std::filesystem::path& dir, ColmapModel& out, std::string& error);

// Looks for the model that belongs to a trained scene file: <scene dir>/dataset/sparse/0,
// <scene dir>/sparse/0 or <scene dir>/sparse. Returns an empty path if there is none.
std::filesystem::path findColmapModelFor(const std::filesystem::path& sceneFile);

// Average "up" direction of the cameras in world space (normalized). People hold phones upright,
// so this is a good estimate of the scene's vertical axis, which COLMAP leaves arbitrary.
glm::vec3 averageCameraUp(const ColmapModel& model);

}  // namespace gsv
