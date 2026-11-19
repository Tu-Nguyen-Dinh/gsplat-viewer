#include "gl/screenshot.h"

#include "gl/gl_loader.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdint>
#include <fstream>
#include <vector>

namespace {
void appendToVector(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}
}  // namespace

bool saveFramebufferPng(const std::filesystem::path& path, int width, int height, std::string& error) {
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadBuffer(GL_BACK);
  glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

  // Encode to memory, then write with std::filesystem::path so Unicode paths work on Windows.
  std::vector<uint8_t> png;
  stbi_flip_vertically_on_write(1);  // OpenGL rows start at the bottom
  if (!stbi_write_png_to_func(appendToVector, &png, width, height, 3, pixels.data(), width * 3)) {
    error = "PNG encoding failed";
    return false;
  }
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!f) {
    error = "Cannot write file";
    return false;
  }
  return true;
}
