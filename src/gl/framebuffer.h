#pragma once

#include "core/image.h"
#include "gl/gl_loader.h"

#include <glm/glm.hpp>

#include <vector>

namespace gsv {

// Offscreen render target with one color texture. Used by tests and benchmarks so
// results do not depend on the window being visible.
//   RGBA8   : like the window (8 bits per channel, rounding after every blend)
//   RGB10A2 : 10 bits per color channel (4x finer), same 32 bits per pixel as RGBA8
//   RGBA16F : half floats, blending happens with ~11 bits of precision (64 bits per pixel)
//
// Optional attachments (Core 1, see docs/core/CORE1-scene.md):
//   depthOutput : COLOR_ATTACHMENT1, RG32F (RG16F on ES). Splat shaders write (z*alpha, alpha) there with
//                 the same "over" blending, giving sum(w_i z_i) and coverage 1 - T: the expected depth
//                 of the blended splats is r / g (readDepthOutput).
//   depthBuffer : DEPTH_COMPONENT32F. Opaque layers (point clouds, meshes) write it; splats can test
//                 against it (SplatSettings::depthTest) so opaque geometry occludes them.
struct FramebufferAttachments {
  bool depthOutput = false;
  bool depthBuffer = false;
};

class Framebuffer {
 public:
  enum class Format { RGBA8, RGB10A2, RGBA16F };
  using Attachments = FramebufferAttachments;

  Framebuffer() = default;
  ~Framebuffer();
  Framebuffer(const Framebuffer&) = delete;
  Framebuffer& operator=(const Framebuffer&) = delete;

  bool create(int width, int height, Format format = Format::RGBA8, Attachments attachments = FramebufferAttachments());
  void bind() const;  // binds, selects the draw buffers and sets the viewport
  // Clears color to `background`, the depth output to 0 and the depth buffer to 1 (far).
  void clear(const glm::vec3& background) const;
  const Attachments& attachments() const { return attachments_; }
  int width() const { return width_; }
  int height() const { return height_; }
  Format format() const { return format_; }
  GLuint id() const { return fbo_; }

 private:
  GLuint fbo_ = 0, color_ = 0, depthOut_ = 0, depthBuffer_ = 0;
  Attachments attachments_;
  int width_ = 0, height_ = 0;
  Format format_ = Format::RGBA8;
};

// Reads the color buffer of `fbo` (0 = window) into an Image (rows flipped to top-down).
Image readFramebuffer(GLuint fbo, int width, int height);

// Expected depth + coverage of a framebuffer created with Attachments::depthOutput (DepthImage is
// defined in core/image.h so CPU code such as picking does not depend on OpenGL).
DepthImage readDepthOutput(const Framebuffer& fb);

}  // namespace gsv
