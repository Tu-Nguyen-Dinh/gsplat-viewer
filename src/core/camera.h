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
  // Places the camera so the sphere fills the view; sceneExtent (diameter of everything) sets the far plane.
  void frameSphere(const glm::vec3& center, float radius, float sceneExtent = 0.0f);
  // Places the camera at `eye` looking at `center` (inverse of position(): yaw/pitch from the direction).
  void lookAt(const glm::vec3& eye, const glm::vec3& center);

  void orbit(float dxPixels, float dyPixels);
  // Fly-mode mouse look: turns the view around the eye (the eye stays put, the target moves).
  // Same state as orbit(), so switching between the two modes never changes the view matrix.
  void look(float dxPixels, float dyPixels);
  void pan(float dxPixels, float dyPixels, float viewportHeight);
  void zoom(float wheelSteps);
  // Moves the target (and the eye with it): x = right, y = up, z = forward. With worldUp, y is
  // the world's +Y instead of the camera's up (fly mode: Q/E go straight up and down).
  void move(const glm::vec3& cameraSpaceDir, float amount, bool worldUp = false);

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
