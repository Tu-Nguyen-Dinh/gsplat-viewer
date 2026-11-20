// Tests for the camera focus region (src/scene/scene_bounds.cpp).
#include "scene/gaussian_scene.h"
#include "test_framework.h"

#include <random>

using namespace gsv;

namespace {

glm::vec3 randomInBall(std::mt19937& rng, float radius) {
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  for (;;) {
    const glm::vec3 p(u(rng), u(rng), u(rng));
    if (glm::dot(p, p) <= 1.0f) return p * radius;
  }
}

void addSplat(GaussianScene& s, const glm::vec3& p, float opacity) {
  s.position.push_back(p);
  s.opacityLogit.push_back(logit(opacity));
}

}  // namespace

// ER 2.1: a 10k-splat cluster plus 50 outliers 100x further away -> the focus hugs the cluster (< 5 %).
TEST_CASE(focus_ignores_far_outliers) {
  std::mt19937 rng(1);
  GaussianScene s;
  const glm::vec3 c(5.0f, -2.0f, 3.0f);
  for (int i = 0; i < 10000; ++i) addSplat(s, c + randomInBall(rng, 1.0f), 0.9f);
  for (int i = 0; i < 50; ++i) addSplat(s, c + glm::normalize(randomInBall(rng, 1.0f)) * 100.0f, 0.9f);
  computeRobustBounds(s);
  CHECK(glm::length(s.focusCenter - c) < 0.05f);
  CHECK_NEAR(s.focusRadius, 1.0, 0.05);
}

// A dense subject inside a sparse background shell (the truck among buildings and trees).
TEST_CASE(focus_finds_dense_subject_in_background) {
  std::mt19937 rng(2);
  GaussianScene s;
  for (int i = 0; i < 10000; ++i) addSplat(s, randomInBall(rng, 1.0f), 0.9f);
  // 20k splats spread over a ball of radius 20: 1/400 of the subject's density.
  for (int i = 0; i < 20000; ++i) {
    glm::vec3 p = randomInBall(rng, 20.0f);
    if (glm::length(p) < 2.0f) p *= 2.0f / std::max(glm::length(p), 1e-3f);
    addSplat(s, p, 0.9f);
  }
  computeRobustBounds(s);
  CHECK(glm::length(s.focusCenter) < 0.1f);
  CHECK(s.focusRadius > 0.9f && s.focusRadius < 1.15f);
  // The per-axis bounds still cover the background (the renderer needs the full depth range).
  CHECK(s.boundsMax.x > 10.0f);
}

// Near-transparent floaters barely count: weighting by opacity keeps the focus on the subject.
TEST_CASE(focus_weights_by_opacity) {
  std::mt19937 rng(3);
  GaussianScene s;
  for (int i = 0; i < 5000; ++i) addSplat(s, randomInBall(rng, 1.0f), 0.95f);
  for (int i = 0; i < 20000; ++i) addSplat(s, randomInBall(rng, 6.0f), 0.002f);
  computeRobustBounds(s);
  CHECK(s.focusRadius < 1.3f);
}

// No distinct subject (uniform scene): the focus covers everything.
TEST_CASE(focus_of_uniform_scene_is_whole_scene) {
  std::mt19937 rng(4);
  GaussianScene s;
  for (int i = 0; i < 20000; ++i) addSplat(s, randomInBall(rng, 3.0f), 0.5f);
  computeRobustBounds(s);
  CHECK(glm::length(s.focusCenter) < 0.1f);
  CHECK_NEAR(s.focusRadius, 3.0, 0.15);
}
