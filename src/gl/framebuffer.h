#pragma once

#include "core/image.h"
#include "gl/gl_loader.h"

namespace gsv {

// Offscreen render target with one color texture. Used by tests and benchmarks so
// results do not depend on the window being visible.
//   RGBA8   : like the window (8 bits per channel, rounding after every blend)
//   RGB10A2 : 10 bits per color channel (4x finer), same 32 bits per pixel as RGBA8
//   RGBA16F : half floats, blending happens with ~11 bits of precision (64 bits per pixel)
class Framebuffer {
 public:
  enum class Format { RGBA8, RGB10A2, RGBA16F };

  Framebuffer() = default;
  ~Framebuffer();
  Framebuffer(const Framebuffer&) = delete;
  Framebuffer& operator=(const Framebuffer&) = delete;

  bool create(int width, int height, Format format = Format::RGBA8);
  void bind() const;  // binds and sets the viewport
  int width() const { return width_; }
  int height() const { return height_; }
  Format format() const { return format_; }
  GLuint id() const { return fbo_; }

 private:
  GLuint fbo_ = 0, color_ = 0;
  int width_ = 0, height_ = 0;
  Format format_ = Format::RGBA8;
};

// Reads the color buffer of `fbo` (0 = window) into an Image (rows flipped to top-down).
Image readFramebuffer(GLuint fbo, int width, int height);

}  // namespace gsv
