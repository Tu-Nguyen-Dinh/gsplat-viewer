#include "gl/buffer.h"

#include <utility>

namespace gsv {

GlBuffer& GlBuffer::operator=(GlBuffer&& o) noexcept {
  if (this != &o) {
    release();
    id_ = std::exchange(o.id_, 0);
    size_ = std::exchange(o.size_, 0);
  }
  return *this;
}

void GlBuffer::release() {
  if (id_) glDeleteBuffers(1, &id_);
  id_ = 0;
  size_ = 0;
}

void GlBuffer::allocate(size_t bytes, const void* data, GLenum usage) {
  if (!id_) glGenBuffers(1, &id_);
  // Bind to the generic COPY_WRITE target so we never disturb other bindings.
  glBindBuffer(GL_COPY_WRITE_BUFFER, id_);
  glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(bytes > 0 ? bytes : 4), data, usage);
  size_ = bytes;
}

void GlBuffer::reserve(size_t bytes, GLenum usage) {
  if (!id_ || size_ < bytes) allocate(bytes, nullptr, usage);
}

void GlBuffer::upload(size_t offset, size_t bytes, const void* data) {
  glBindBuffer(GL_COPY_WRITE_BUFFER, id_);
  glBufferSubData(GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), data);
}

void GlBuffer::download(size_t offset, size_t bytes, void* out) const {
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
  glBindBuffer(GL_COPY_READ_BUFFER, id_);
  glGetBufferSubData(GL_COPY_READ_BUFFER, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), out);
}

void GlBuffer::bindBase(GLenum target, GLuint index) const { glBindBufferBase(target, index, id_); }
void GlBuffer::bind(GLenum target) const { glBindBuffer(target, id_); }

}  // namespace gsv
