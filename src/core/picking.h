#pragma once
// Core 3: picking 3D points from the expected-depth image (core/image.h DepthImage) and simple
// measurements. CPU only, no OpenGL: the depth image comes from gl/framebuffer.h readDepthOutput().

#include "core/image.h"

#include <glm/glm.hpp>

#include <optional>

namespace gsv {

struct PickCamera {
  glm::mat4 view{1.0f};           // world (relative to the render origin) -> camera
  glm::vec2 focal{1.0f};          // pixels
  glm::vec2 principal{0.0f};      // pixels, origin bottom-left (OpenGL), like RenderView
};

// Pixel (x, y) with y counted from the TOP (mouse / image convention). Returns the world point
// (relative to the render origin) when the coverage there is at least `minCoverage`. Uses the
// median of the covered depths in a (2r+1)^2 window to ignore single-pixel speckles.
std::optional<glm::vec3> pickPoint(const DepthImage& depth, int x, int y, const PickCamera& cam,
                                   float minCoverage = 0.5f, int windowRadius = 1);

struct Measurement {
  double distance = 0.0;     // straight line
  double horizontal = 0.0;   // perpendicular to `up`
  double vertical = 0.0;     // along `up` (signed: b above a is positive)
};
Measurement measure(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& up);

}  // namespace gsv
