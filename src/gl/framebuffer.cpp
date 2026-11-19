#include "gl/framebuffer.h"

#include <cstring>

namespace gsv {

Framebuffer::~Framebuffer() {
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (color_) glDeleteTextures(1, &color_);
}

bool Framebuffer::create(int width, int height, Format format) {
  width_ = width;
  height_ = height;
  if (!color_) glGenTextures(1, &color_);
  glBindTexture(GL_TEXTURE_2D, color_);
  if (format == Format::RGBA16F)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
  else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (!fbo_) glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
  const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return ok;
}

void Framebuffer::bind() const {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glViewport(0, 0, width_, height_);
}

Image readFramebuffer(GLuint fbo, int width, int height) {
  Image img(width, height);
  std::vector<uint8_t> rows(img.rgb.size());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
  glReadBuffer(fbo == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);  // rows of 3*width bytes are not 4-byte aligned in general
  glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rows.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  // OpenGL row 0 is the bottom row; images are stored top-down.
  const size_t stride = static_cast<size_t>(width) * 3;
  for (int y = 0; y < height; ++y)
    std::memcpy(img.rgb.data() + static_cast<size_t>(y) * stride,
                rows.data() + static_cast<size_t>(height - 1 - y) * stride, stride);
  return img;
}

}  // namespace gsv
