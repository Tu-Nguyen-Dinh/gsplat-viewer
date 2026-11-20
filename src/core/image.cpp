#include "core/image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
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

double ssim(const Image& a, const Image& b) {
  if (a.width != b.width || a.height != b.height || a.width < 11 || a.height < 11) return 0.0;
  // 11-tap Gaussian, sigma 1.5 (Wang et al. 2004), applied separably: rows then columns.
  constexpr int kR = 5;
  float w[2 * kR + 1];
  float wsum = 0.0f;
  for (int i = -kR; i <= kR; ++i) wsum += (w[i + kR] = std::exp(-static_cast<float>(i * i) / (2.0f * 1.5f * 1.5f)));
  for (float& x : w) x /= wsum;

  const int W = a.width, H = a.height;
  const size_t n = static_cast<size_t>(W) * H;
  // Five blurred planes per channel: mu_a, mu_b, E[a^2], E[b^2], E[ab].
  std::vector<float> src[5], blur[5], tmp(n);
  for (auto& v : src) v.resize(n);
  for (auto& v : blur) v.resize(n);
  auto blurPlane = [&](const std::vector<float>& in, std::vector<float>& out) {
    for (int y = 0; y < H; ++y)  // horizontal, edges clamped
      for (int x = 0; x < W; ++x) {
        float s = 0.0f;
        for (int k = -kR; k <= kR; ++k) s += w[k + kR] * in[static_cast<size_t>(y) * W + std::clamp(x + k, 0, W - 1)];
        tmp[static_cast<size_t>(y) * W + x] = s;
      }
    for (int y = 0; y < H; ++y)  // vertical
      for (int x = 0; x < W; ++x) {
        float s = 0.0f;
        for (int k = -kR; k <= kR; ++k)
          s += w[k + kR] * tmp[static_cast<size_t>(std::clamp(y + k, 0, H - 1)) * W + x];
        out[static_cast<size_t>(y) * W + x] = s;
      }
  };

  constexpr double C1 = (0.01 * 255) * (0.01 * 255), C2 = (0.03 * 255) * (0.03 * 255);
  double total = 0.0;
  for (int c = 0; c < 3; ++c) {
    for (size_t i = 0; i < n; ++i) {
      const float x = a.rgb[i * 3 + c], y = b.rgb[i * 3 + c];
      src[0][i] = x;
      src[1][i] = y;
      src[2][i] = x * x;
      src[3][i] = y * y;
      src[4][i] = x * y;
    }
    for (int p = 0; p < 5; ++p) blurPlane(src[p], blur[p]);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double ma = blur[0][i], mb = blur[1][i];
      const double va = blur[2][i] - ma * ma, vb = blur[3][i] - mb * mb, cov = blur[4][i] - ma * mb;
      sum += ((2 * ma * mb + C1) * (2 * cov + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
    }
    total += sum / static_cast<double>(n);
  }
  return total / 3.0;
}

bool readPnmNumber(std::FILE* f, int& value) {
  int c = std::fgetc(f);
  for (;;) {
    while (c != EOF && std::isspace(c)) c = std::fgetc(f);
    if (c != '#') break;
    while (c != EOF && c != '\n') c = std::fgetc(f);
  }
  if (c == EOF || !std::isdigit(c)) return false;
  value = 0;
  while (c != EOF && std::isdigit(c)) {
    if (value > 1'000'000) return false;
    value = value * 10 + (c - '0');
    c = std::fgetc(f);
  }
  return c != EOF && std::isspace(c);
}

bool readPpmFrame(std::FILE* stream, Image& out, bool& error) {
  error = false;
  const int p = std::fgetc(stream);
  if (p == EOF) return false;  // clean end of stream
  int w = 0, h = 0, maxval = 0;
  if (p != 'P' || std::fgetc(stream) != '6' || !readPnmNumber(stream, w) || !readPnmNumber(stream, h) ||
      !readPnmNumber(stream, maxval) || maxval != 255 || w <= 0 || h <= 0) {
    error = true;
    return false;
  }
  out = Image(w, h);
  if (std::fread(out.rgb.data(), 1, out.rgb.size(), stream) != out.rgb.size()) {
    error = true;  // truncated
    return false;
  }
  return true;
}

}  // namespace gsv
