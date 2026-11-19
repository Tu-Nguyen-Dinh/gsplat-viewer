#pragma once

#include "gl/gl_loader.h"

#include <cstddef>

namespace gsv {

// RAII owner of one OpenGL buffer object. Movable, not copyable.
// The same buffer can be bound to different targets (SSBO, indirect, vertex...).
class GlBuffer {
 public:
  GlBuffer() = default;
  ~GlBuffer() { release(); }
  GlBuffer(const GlBuffer&) = delete;
  GlBuffer& operator=(const GlBuffer&) = delete;
  GlBuffer(GlBuffer&& o) noexcept : id_(o.id_), size_(o.size_) { o.id_ = 0, o.size_ = 0; }
  GlBuffer& operator=(GlBuffer&& o) noexcept;

  // (Re)allocates `bytes` of storage, optionally filled from `data`.
  void allocate(size_t bytes, const void* data = nullptr, GLenum usage = GL_DYNAMIC_DRAW);
  // Allocates only if the current storage is smaller (contents are NOT preserved).
  void reserve(size_t bytes, GLenum usage = GL_DYNAMIC_DRAW);
  void upload(size_t offset, size_t bytes, const void* data);
  // Synchronous read back (stalls until the GPU has finished writing). Use for tests/tools.
  void download(size_t offset, size_t bytes, void* out) const;
  void bindBase(GLenum target, GLuint index) const;  // e.g. GL_SHADER_STORAGE_BUFFER, binding = index
  void bind(GLenum target) const;

  GLuint id() const { return id_; }
  size_t size() const { return size_; }

 private:
  void release();
  GLuint id_ = 0;
  size_t size_ = 0;
};

}  // namespace gsv
