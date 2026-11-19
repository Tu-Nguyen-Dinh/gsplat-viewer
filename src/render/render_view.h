#pragma once

#include <glm/glm.hpp>

#include <cmath>

namespace gsv {

// Everything a renderer needs to know about the camera for one frame.
struct RenderView {
  glm::mat4 model{1.0f};       // scene -> world (e.g. Y flip for COLMAP scenes)
  glm::mat4 view{1.0f};        // world -> camera
  glm::mat4 projection{1.0f};  // camera -> clip
  glm::vec3 cameraPosition{0.0f};  // world space
  glm::ivec2 viewport{1, 1};   // pixels
  float fovY = 1.0f;           // radians
  float nearPlane = 0.01f;

  float aspect() const { return static_cast<float>(viewport.x) / static_cast<float>(viewport.y); }
  glm::vec2 tanHalfFov() const {
    const float ty = std::tan(0.5f * fovY);
    return {ty * aspect(), ty};
  }
  // Focal length in pixels: f = (size / 2) / tan(fov / 2).
  glm::vec2 focalPixels() const {
    const glm::vec2 t = tanHalfFov();
    return {0.5f * static_cast<float>(viewport.x) / t.x, 0.5f * static_cast<float>(viewport.y) / t.y};
  }
};

}  // namespace gsv
