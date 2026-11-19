#pragma once

#include "gl/shader.h"
#include "scene/gaussian_scene.h"

#include <glm/glm.hpp>

#include <string>

// Day 1 renderer: draws each Gaussian center as a round point with its base
// (SH degree 0) color. Useful as a debug view and as a baseline for performance.
class PointRenderer {
 public:
  struct Params {
    glm::mat4 model{1.0f};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    float pointSize = 2.0f;      // pixels
    float minOpacity = 0.05f;    // hide nearly transparent splats
  };

  PointRenderer() = default;
  ~PointRenderer();
  PointRenderer(const PointRenderer&) = delete;
  PointRenderer& operator=(const PointRenderer&) = delete;

  bool init(std::string& log);
  bool reloadShaders(std::string& log) { return program_.reload(log); }
  void upload(const GaussianScene& scene);
  void draw(const Params& p);

 private:
  ShaderProgram program_;
  GLuint vao_ = 0;
  GLuint vbo_ = 0;
  GLsizei count_ = 0;
};
