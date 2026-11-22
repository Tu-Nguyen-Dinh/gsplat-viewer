#include "gl/framebuffer.h"

#include <cstring>

namespace gsv {

Framebuffer::~Framebuffer() {
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  if (color_) glDeleteTextures(1, &color_);
  if (depthOut_) glDeleteTextures(1, &depthOut_);
  if (depthBuffer_) glDeleteTextures(1, &depthBuffer_);
}

bool Framebuffer::create(int width, int height, Format format, Attachments attachments) {
  width_ = width;
  height_ = height;
  format_ = format;
  attachments_ = attachments;
  if (!color_) glGenTextures(1, &color_);
  glBindTexture(GL_TEXTURE_2D, color_);
  if (format == Format::RGBA16F)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
  else if (format == Format::RGB10A2)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB10_A2, width, height, 0, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
  else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (!fbo_) glGenFramebuffers(1, &fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_, 0);
  auto makeTexture = [&](GLuint& tex, GLenum internalFormat, GLenum fmt, GLenum type) {
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0, fmt, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  };
  if (attachments.depthOutput) {
#if defined(GSV_GLES)
    makeTexture(depthOut_, GL_RG16F, GL_RG, GL_HALF_FLOAT);  // 32-bit float blending is optional on ES
#else
    makeTexture(depthOut_, GL_RG32F, GL_RG, GL_FLOAT);
#endif
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, depthOut_, 0);
  }
  if (attachments.depthBuffer) {
    makeTexture(depthBuffer_, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthBuffer_, 0);
  }
  const GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
  glDrawBuffers(attachments.depthOutput ? 2 : 1, buffers);
  const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return ok;
}

void Framebuffer::bind() const {
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glViewport(0, 0, width_, height_);
}

void Framebuffer::clear(const glm::vec3& background) const {
  bind();
  const float color[4] = {background.r, background.g, background.b, 1.0f};
  glClearBufferfv(GL_COLOR, 0, color);
  if (attachments_.depthOutput) {
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 1, zero);
  }
  if (attachments_.depthBuffer) {
    const float far = 1.0f;
    glDepthMask(GL_TRUE);
    glClearBufferfv(GL_DEPTH, 0, &far);
  }
}

DepthImage readDepthOutput(const Framebuffer& fb) {
  DepthImage out;
  if (!fb.attachments().depthOutput) return out;
  const int w = fb.width(), h = fb.height();
  out.width = w;
  out.height = h;
  out.depth.assign(static_cast<size_t>(w) * h, 0.0f);
  out.coverage.assign(static_cast<size_t>(w) * h, 0.0f);
  std::vector<float> rg(static_cast<size_t>(w) * h * 4);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.id());
  glReadBuffer(GL_COLOR_ATTACHMENT1);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, rg.data());  // RGBA/FLOAT: always a valid read format
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const float* px = &rg[(static_cast<size_t>(h - 1 - y) * w + x) * 4];
      const size_t i = static_cast<size_t>(y) * w + x;
      out.coverage[i] = px[1];
      out.depth[i] = px[1] > 1e-6f ? px[0] / px[1] : 0.0f;
    }
  return out;
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
