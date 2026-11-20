// Tests for the orbit / fly camera (src/core/camera.*).
#include "core/camera.h"
#include "test_framework.h"

#include <cmath>

using namespace gsv;

namespace {

OrbitCamera testCamera() {
  OrbitCamera c;
  c.target = {1.0f, 2.0f, -3.0f};
  c.yaw = 0.7f;
  c.pitch = 0.3f;
  c.distance = 8.0f;
  return c;
}

double maxDifference(const glm::mat4& a, const glm::mat4& b) {
  double d = 0;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) d = std::max(d, static_cast<double>(std::abs(a[c][r] - b[c][r])));
  return d;
}

}  // namespace

// Fly-mode look turns around the eye: the eye does not move.
TEST_CASE(camera_look_keeps_eye_fixed) {
  OrbitCamera c = testCamera();
  const glm::vec3 eye = c.position();
  c.look(120.0f, -45.0f);
  CHECK(glm::length(c.position() - eye) < 1e-5f);
  CHECK(glm::length(c.target - eye) > 7.99f);  // still looking at a point `distance` ahead
}

// ER 2.2: orbit -> fly -> orbit keeps the view matrix (< 1e-5). The modes share one state, so a
// round trip of mouse looks returns exactly to the starting view, and orbiting still works after.
TEST_CASE(camera_orbit_fly_orbit_roundtrip) {
  OrbitCamera c = testCamera();
  const glm::mat4 before = c.view();
  c.look(80.0f, 30.0f);   // fly: look right and down
  c.look(-80.0f, -30.0f); // and back
  CHECK(maxDifference(before, c.view()) < 1e-5);
  c.orbit(50.0f, 0.0f);   // back in orbit mode
  c.orbit(-50.0f, 0.0f);
  CHECK(maxDifference(before, c.view()) < 1e-5);
}

// Dragging right in fly mode turns the view to the right (first-person convention).
TEST_CASE(camera_look_right_turns_right) {
  OrbitCamera c = testCamera();
  const glm::vec3 right = c.right(), forward = c.forward();
  c.look(40.0f, 0.0f);
  CHECK(glm::dot(c.forward() - forward, right) > 0.0f);
}

// ER 2.2: movement depends on time, not on the frame rate (30 FPS vs 144 FPS for one second).
TEST_CASE(camera_movement_is_frame_rate_independent) {
  OrbitCamera slow = testCamera(), fast = testCamera();
  const float speed = 2.5f;  // units per second
  for (int i = 0; i < 30; ++i) slow.move({0, 0, 1}, speed * (1.0f / 30.0f), true);
  for (int i = 0; i < 144; ++i) fast.move({0, 0, 1}, speed * (1.0f / 144.0f), true);
  CHECK(glm::length(slow.position() - fast.position()) < 1e-4f);
  CHECK_NEAR(glm::length(slow.position() - testCamera().position()), 2.5, 1e-4);
}

// Fly mode Q/E move along the world's vertical axis even when looking down.
TEST_CASE(camera_world_up_move_is_vertical) {
  OrbitCamera c = testCamera();
  const glm::vec3 eye = c.position();
  c.move({0, 1, 0}, 1.0f, true);
  const glm::vec3 d = c.position() - eye;
  CHECK_NEAR(d.y, 1.0, 1e-5);
  CHECK_NEAR(d.x, 0.0, 1e-5);
  CHECK_NEAR(d.z, 0.0, 1e-5);
}
