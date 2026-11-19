#pragma once

#include <glm/glm.hpp>

namespace gsv {

// Orbit camera around a target point, plus free movement of the target (WASD).
// World up is +Y; the camera looks along -Z in view space (OpenGL convention).
class OrbitCamera {
 public:
  glm::vec3 target{0.0f};
  float yaw = 0.6f;       // radians around +Y
  float pitch = 0.35f;    // radians, clamped to avoid flipping over the pole
  float distance = 6.0f;
  float fovY = glm::radians(55.0f);
  float nearPlane = 0.02f;
  float farPlane = 500.0f;

  // Places the camera so the axis-aligned box is fully visible.
  void frameBounds(const glm::vec3& boundsMin, const glm::vec3& boundsMax);
  // Places the camera at `eye` looking at `center` (inverse of position(): yaw/pitch from the direction).
  void lookAt(const glm::vec3& eye, const glm::vec3& center);

  void orbit(float dxPixels, float dyPixels);
  void pan(float dxPixels, float dyPixels, float viewportHeight);
  void zoom(float wheelSteps);
  // Moves the target in camera space: x = right, y = up, z = forward.
  void move(const glm::vec3& cameraSpaceDir, float amount);

  glm::vec3 position() const;
  glm::vec3 forward() const;
  glm::vec3 right() const;
  glm::vec3 up() const;
  glm::mat4 view() const;
  glm::mat4 projection(float aspect) const;
  // Focal length in pixels for a viewport of the given height (needed by the splat projection).
  float focalPixels(float viewportHeight) const;
};

}  // namespace gsv
