#include "scene/demo_scene.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <random>

namespace gsv {

namespace {

// Quaternion rotating +Z onto `normal`, so a splat flattened along its local Z
// lies tangent to the surface.
glm::vec4 orientToNormal(const glm::vec3& normal, float spin) {
  glm::quat q = glm::rotation(glm::vec3(0, 0, 1), glm::normalize(normal));
  q = q * glm::angleAxis(spin, glm::vec3(0, 0, 1));
  return {q.w, q.x, q.y, q.z};
}

glm::vec3 hsv(float h, float s, float v) {
  glm::vec3 k = glm::mod(glm::vec3(5.0f, 3.0f, 1.0f) + h * 6.0f, 6.0f);
  return v - v * s * glm::clamp(glm::min(k, 4.0f - k), 0.0f, 1.0f);
}

}  // namespace

GaussianScene makeDemoScene(size_t count, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u01(0.0f, 1.0f);
  const float twoPi = glm::two_pi<float>();

  GaussianScene s;
  s.position.reserve(count);
  auto push = [&](glm::vec3 p, glm::vec3 n, glm::vec3 color, float size, float opacity) {
    s.position.push_back(p);
    s.logScale.push_back(glm::log(glm::vec3(size, size * (0.6f + 0.4f * u01(rng)), size * 0.15f)));
    s.rotation.push_back(orientToNormal(n, u01(rng) * twoPi));
    s.opacityLogit.push_back(logit(opacity));
    s.shDC.push_back((color - 0.5f) / kShC0);
  };

  const size_t nSphere = count * 4 / 10, nTorus = count * 3 / 10, nFloor = count - nSphere - nTorus;

  // Sphere shell, colored by direction.
  const glm::vec3 sc(-1.2f, 1.0f, 0.0f);
  for (size_t i = 0; i < nSphere; ++i) {
    float z = u01(rng) * 2.0f - 1.0f, a = u01(rng) * twoPi, r = std::sqrt(1.0f - z * z);
    glm::vec3 n(r * std::cos(a), z, r * std::sin(a));
    push(sc + n * 0.9f, n, hsv(a / twoPi, 0.65f, 0.95f), 0.025f, 0.9f);
  }
  // Torus.
  const glm::vec3 tc(1.3f, 0.9f, 0.0f);
  for (size_t i = 0; i < nTorus; ++i) {
    float a = u01(rng) * twoPi, b = u01(rng) * twoPi;
    glm::vec3 ring(std::cos(a), 0.0f, std::sin(a));
    glm::vec3 n = ring * std::cos(b) + glm::vec3(0, std::sin(b), 0);
    glm::vec3 p = tc + ring * 0.7f + n * 0.25f;
    float g = 0.5f + 0.5f * std::cos(b);
    push(p, n, glm::mix(glm::vec3(0.85f, 0.55f, 0.15f), glm::vec3(1.0f, 0.9f, 0.6f), g), 0.02f, 0.95f);
  }
  // Checkered floor disc.
  for (size_t i = 0; i < nFloor; ++i) {
    float r = 4.0f * std::sqrt(u01(rng)), a = u01(rng) * twoPi;
    glm::vec3 p(r * std::cos(a), 0.0f, r * std::sin(a));
    bool checker = (static_cast<int>(std::floor(p.x * 2.0f)) + static_cast<int>(std::floor(p.z * 2.0f))) & 1;
    glm::vec3 c = checker ? glm::vec3(0.82f) : glm::vec3(0.28f, 0.32f, 0.4f);
    push(p, {0, 1, 0}, c * (1.0f - 0.12f * r / 4.0f), 0.035f, 0.98f);
  }

  computeRobustBounds(s);
  s.sourceName = "demo scene";
  return s;
}

}  // namespace gsv
