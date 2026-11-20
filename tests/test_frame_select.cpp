// Tests for gsplat_prep's frame scoring and selection (core/frame_select).
#include "core/frame_select.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace gsv;

namespace {

GrayImage checkerboard(int w, int h, int cell) {
  GrayImage img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h)};
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) img.pixels[static_cast<size_t>(y) * w + x] = ((x / cell + y / cell) & 1) ? 220 : 30;
  return img;
}

// Box blur with a (2r+1)^2 kernel, edges clamped: a stand-in for camera motion / defocus blur.
GrayImage boxBlur(const GrayImage& src, int r) {
  GrayImage out = src;
  for (int y = 0; y < src.height; ++y)
    for (int x = 0; x < src.width; ++x) {
      int sum = 0, n = 0;
      for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
          const int sx = std::clamp(x + dx, 0, src.width - 1), sy = std::clamp(y + dy, 0, src.height - 1);
          sum += src.at(sx, sy);
          ++n;
        }
      out.pixels[static_cast<size_t>(y) * src.width + x] = static_cast<uint8_t>(sum / n);
    }
  return out;
}

}  // namespace

TEST_CASE(sharpness_flat_image_is_zero) {
  GrayImage flat{64, 48, std::vector<uint8_t>(64 * 48, 128)};
  CHECK(laplacianVariance(flat) == 0.0);
  GrayImage tiny{2, 2, std::vector<uint8_t>(4, 9)};
  CHECK(laplacianVariance(tiny) == 0.0);  // no interior pixels
}

TEST_CASE(sharpness_drops_with_blur) {
  const GrayImage sharp = checkerboard(160, 120, 8);
  const double s0 = laplacianVariance(sharp);
  const double s1 = laplacianVariance(boxBlur(sharp, 1));  // 3x3
  const double s2 = laplacianVariance(boxBlur(sharp, 2));  // 5x5
  CHECK(s0 > 0.0);
  CHECK(s1 < s0 && s2 < s1);  // monotonic in blur strength
  CHECK(s0 >= 5.0 * s2);      // PLAN 1.3: sharp scores >= 5x the 5x5-blurred copy
}

TEST_CASE(select_keeps_sharpest_per_window) {
  //                       window 0          window 1          window 2 (partial)
  const std::vector<double> s{1, 5, 3, 2, 4, 9, 9, 1, 7, 8};
  const FrameSelection r = selectSharpestFrames(s, 4, 0.0);
  CHECK((r.selected == std::vector<int>{1, 5, 9}));  // ties (5, 6 both 9): earliest wins
  CHECK(r.rejectedBlurry == 0);
  CHECK(selectSharpestFrames(s, 4, 0.0).selected == r.selected);  // deterministic
}

TEST_CASE(select_window_one_keeps_everything_and_handles_empty) {
  const std::vector<double> s{3, 1, 2};
  CHECK((selectSharpestFrames(s, 1, 0.0).selected == std::vector<int>{0, 1, 2}));
  CHECK((selectSharpestFrames(s, 0, 0.0).selected == std::vector<int>{0, 1, 2}));  // window clamped to 1
  CHECK(selectSharpestFrames({}, 5).selected.empty());
}

TEST_CASE(select_drops_windows_that_are_all_blur) {
  // A steady video (scores ~100) with a shaken stretch in the middle (scores ~5).
  std::vector<double> s;
  for (int i = 0; i < 30; ++i) s.push_back((i >= 10 && i < 20) ? 5.0 + i % 3 : 100.0 + i % 7);
  const FrameSelection r = selectSharpestFrames(s, 5, 0.35);
  CHECK(r.rejectedBlurry == 2);  // windows [10,15) and [15,20)
  CHECK(r.selected.size() == 4);
  for (int i : r.selected) CHECK(i < 10 || i >= 20);
  CHECK(r.medianScore > 90.0);
}

TEST_CASE(pgm_stream_reads_frames_until_eof) {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  const unsigned char a[6] = {0, 1, 2, 3, 4, 5}, b[6] = {9, 9, 9, 9, 9, 9};
  std::fputs("P5\n3 2\n255\n", f);
  std::fwrite(a, 1, 6, f);
  std::fputs("P5 # comment\n3 2 255\n", f);  // comments and single-line headers are legal
  std::fwrite(b, 1, 6, f);
  std::rewind(f);

  GrayImage img;
  bool error = true;
  REQUIRE(readPgmFrame(f, img, error));
  CHECK(!error && img.width == 3 && img.height == 2 && std::memcmp(img.pixels.data(), a, 6) == 0);
  REQUIRE(readPgmFrame(f, img, error));
  CHECK(img.at(2, 1) == 9);
  CHECK(!readPgmFrame(f, img, error));
  CHECK(!error);  // clean end of stream
  std::fclose(f);
}

TEST_CASE(pgm_stream_rejects_bad_data) {
  for (const char* text : {"P6\n3 2\n255\n", "P5\n3 2\n65535\n", "P5\n3 2\n255\nabc"}) {
    std::FILE* f = std::tmpfile();
    REQUIRE(f != nullptr);
    std::fputs(text, f);
    std::rewind(f);
    GrayImage img;
    bool error = false;
    CHECK(!readPgmFrame(f, img, error));
    CHECK(error);  // wrong magic, 16-bit maxval, truncated pixels
    std::fclose(f);
  }
}
