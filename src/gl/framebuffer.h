#pragma once

#include "core/image.h"
#include "gl/gl_loader.h"

namespace gsv {

// Offscreen render target with one RGBA8 color texture. Used by tests and
// benchmarks so results do not depend on the window being visible.
class Framebuffer {
 public:
  Framebuffer() = default;
  ~Framebuffer();
  Framebuffer(const Framebuffer&) = delete;
  Framebuffer& operator=(const Framebuffer&) = delete;

  bool create(int width, int height);
  void bind() const;  // binds and sets the viewport
  int width() const { return width_; }
  int height() const { return height_; }
  GLuint id() const { return fbo_; }

 private:
  GLuint fbo_ = 0, color_ = 0;
  int width_ = 0, height_ = 0;
};

// Reads the color buffer of `fbo` (0 = window) into an Image (rows flipped to top-down).
Image readFramebuffer(GLuint fbo, int width, int height);

}  // namespace gsv
