#include "core/camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace gsv {

void OrbitCamera::frameBounds(const glm::vec3& bmin, const glm::vec3& bmax) {
  target = 0.5f * (bmin + bmax);
  const float radius = std::max(0.5f * glm::length(bmax - bmin), 1e-3f);
  distance = radius / std::sin(0.5f * fovY) * 1.05f;
  nearPlane = std::max(distance * 0.002f, 1e-3f);
  farPlane = distance + radius * 50.0f;
}

void OrbitCamera::lookAt(const glm::vec3& eye, const glm::vec3& center) {
  target = center;
  const glm::vec3 offset = eye - center;
  distance = std::max(glm::length(offset), 1e-4f);
  const glm::vec3 d = offset / distance;  // = (cos p sin y, sin p, cos p cos y)
  pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
  yaw = std::atan2(d.x, d.z);
  nearPlane = std::max(distance * 0.002f, 1e-3f);
  farPlane = std::max(farPlane, distance * 50.0f);
}

void OrbitCamera::orbit(float dx, float dy) {
  yaw -= dx * 0.005f;
  pitch = std::clamp(pitch + dy * 0.005f, -1.55f, 1.55f);
}

void OrbitCamera::pan(float dx, float dy, float viewportHeight) {
  const float worldPerPixel = 2.0f * distance * std::tan(0.5f * fovY) / std::max(viewportHeight, 1.0f);
  target += (-right() * dx + up() * dy) * worldPerPixel;
}

void OrbitCamera::zoom(float steps) { distance = std::max(distance * std::pow(0.88f, steps), 1e-3f); }

void OrbitCamera::move(const glm::vec3& d, float amount) {
  target += (right() * d.x + up() * d.y + forward() * d.z) * amount;
}

glm::vec3 OrbitCamera::position() const {
  const glm::vec3 offset(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
  return target + offset * distance;
}

glm::vec3 OrbitCamera::forward() const { return glm::normalize(target - position()); }
glm::vec3 OrbitCamera::right() const { return glm::normalize(glm::cross(forward(), glm::vec3(0, 1, 0))); }
glm::vec3 OrbitCamera::up() const { return glm::cross(right(), forward()); }

glm::mat4 OrbitCamera::view() const { return glm::lookAt(position(), target, glm::vec3(0, 1, 0)); }

glm::mat4 OrbitCamera::projection(float aspect) const {
  return glm::perspective(fovY, aspect, nearPlane, farPlane);
}

float OrbitCamera::focalPixels(float viewportHeight) const { return 0.5f * viewportHeight / std::tan(0.5f * fovY); }

}  // namespace gsv
