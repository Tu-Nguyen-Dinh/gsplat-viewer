#include "core/image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>

namespace gsv {

namespace {
void appendToVector(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}
}  // namespace

bool savePng(const std::filesystem::path& path, const Image& image, std::string& error) {
  // Encode to memory, then write with std::filesystem::path so Unicode paths work on Windows.
  std::vector<uint8_t> png;
  if (!stbi_write_png_to_func(appendToVector, &png, image.width, image.height, 3, image.rgb.data(),
                              image.width * 3)) {
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

double psnr(const Image& a, const Image& b) {
  if (a.width != b.width || a.height != b.height || a.rgb.empty()) return 0.0;
  double sum = 0.0;
  for (size_t i = 0; i < a.rgb.size(); ++i) {
    const double d = static_cast<double>(a.rgb[i]) - static_cast<double>(b.rgb[i]);
    sum += d * d;
  }
  const double mse = sum / static_cast<double>(a.rgb.size());
  if (mse == 0.0) return std::numeric_limits<double>::infinity();
  return 10.0 * std::log10(255.0 * 255.0 / mse);
}

int maxAbsDifference(const Image& a, const Image& b) {
  if (a.rgb.size() != b.rgb.size()) return 255;
  int m = 0;
  for (size_t i = 0; i < a.rgb.size(); ++i) m = std::max(m, std::abs(int(a.rgb[i]) - int(b.rgb[i])));
  return m;
}

}  // namespace gsv
