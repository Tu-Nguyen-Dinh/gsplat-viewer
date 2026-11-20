// Tests for the image quality metrics used by --eval (PSNR, SSIM) and the PPM reader.
#include "core/image.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace gsv;

namespace {

Image gradientImage(int w, int h) {
  Image img(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = img.pixel(x, y);
      p[0] = static_cast<uint8_t>(40 + (x * 150) / w);
      p[1] = static_cast<uint8_t>(40 + (y * 150) / h);
      p[2] = static_cast<uint8_t>(((x / 8 + y / 8) & 1) ? 180 : 60);  // some structure for SSIM
    }
  return img;
}

// Adds +d / -d in a checkerboard: every value changes by exactly d, so MSE = d^2 exactly.
Image withNoise(const Image& src, int d) {
  Image out = src;
  for (size_t i = 0; i < out.rgb.size(); ++i) out.rgb[i] = static_cast<uint8_t>(out.rgb[i] + ((i / 3) % 2 ? d : -d));
  return out;
}

}  // namespace

TEST_CASE(psnr_matches_closed_form) {
  // PLAN 1.7: PSNR of a known error matches 10 log10(255^2 / MSE) within 0.1 dB.
  const Image a = gradientImage(64, 48);
  for (int d : {1, 4, 16}) {
    const double expected = 20.0 * std::log10(255.0 / d);
    CHECK_NEAR(psnr(a, withNoise(a, d)), expected, 0.1);
  }
  CHECK(std::isinf(psnr(a, a)));
}

TEST_CASE(ssim_is_one_for_identical_and_drops_with_noise) {
  const Image a = gradientImage(96, 64);
  CHECK_NEAR(ssim(a, a), 1.0, 1e-9);
  const double s4 = ssim(a, withNoise(a, 4)), s16 = ssim(a, withNoise(a, 16));
  CHECK(s4 < 1.0 && s16 < s4);  // more noise, less similar
  CHECK(s16 > 0.0);
  CHECK(ssim(a, Image(10, 10)) == 0.0);  // size mismatch
}

TEST_CASE(ssim_ignores_constant_offset_less_than_psnr) {
  // A uniform brightness shift barely changes structure: SSIM stays high where PSNR drops.
  const Image a = gradientImage(96, 64);
  Image b = a;
  for (uint8_t& v : b.rgb) v = static_cast<uint8_t>(v + 10);
  CHECK(psnr(a, b) < 29.0);  // 20 log10(255 / 10) = 28.1 dB
  CHECK(ssim(a, b) > 0.95);
}

TEST_CASE(ppm_stream_reads_rgb_frames) {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  const unsigned char px[2 * 1 * 3] = {255, 0, 0, 1, 2, 3};
  std::fputs("P6\n2 1\n255\n", f);
  std::fwrite(px, 1, sizeof px, f);
  std::fputs("P5\n1 1\n255\nx", f);  // wrong type: grayscale
  std::rewind(f);
  Image img;
  bool error = true;
  REQUIRE(readPpmFrame(f, img, error));
  CHECK(!error && img.width == 2 && img.height == 1 && std::memcmp(img.rgb.data(), px, sizeof px) == 0);
  CHECK(!readPpmFrame(f, img, error));
  CHECK(error);
  std::fclose(f);
}
