#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace gsv {

// 8-bit RGB image, rows stored top to bottom (like PNG files).
struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgb;  // width * height * 3

  Image() = default;
  Image(int w, int h) : width(w), height(h), rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3, 0) {}
  uint8_t* pixel(int x, int y) { return rgb.data() + (static_cast<size_t>(y) * width + x) * 3; }
  const uint8_t* pixel(int x, int y) const { return rgb.data() + (static_cast<size_t>(y) * width + x) * 3; }
};

bool savePng(const std::filesystem::path& path, const Image& image, std::string& error);

// Peak signal-to-noise ratio in dB (higher = more similar, identical images -> +inf).
// 30 dB is "visibly similar", above 40 dB differences are hard to see.
double psnr(const Image& a, const Image& b);

// Largest per-channel absolute difference (0..255).
int maxAbsDifference(const Image& a, const Image& b);

// Structural similarity (Wang et al. 2004): 11x11 Gaussian window (sigma 1.5), averaged over the
// RGB channels. 1 = identical; standard 3DGS papers report it next to PSNR.
double ssim(const Image& a, const Image& b);

// Reads one binary PPM ("P6", maxval 255) image, e.g. from `ffmpeg -i x.jpg -f image2pipe -vcodec ppm -`.
// Returns false at end of stream or on malformed data (`error` is set for the latter).
bool readPpmFrame(std::FILE* stream, Image& out, bool& error);

// Next whitespace-separated number of a PNM (PGM / PPM) header; '#' comments are skipped. Exactly
// one whitespace byte ends a field, so after the last one the binary pixels start.
bool readPnmNumber(std::FILE* stream, int& value);

}  // namespace gsv
