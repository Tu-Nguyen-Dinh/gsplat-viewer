#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

namespace gsv {

// 8-bit single-channel image (row-major, no padding).
struct GrayImage {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels;
  uint8_t at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

// Sharpness score: variance of the 3x3 Laplacian (0 1 0 / 1 -4 1 / 0 1 0) over interior pixels.
// Blur removes high frequencies, so a blurred frame has a much smaller Laplacian response.
// The score depends on image size and content, so only compare frames of the same video.
double laplacianVariance(const GrayImage& image);

struct FrameSelection {
  std::vector<int> selected;  // indices into the score list, ascending
  double medianScore = 0.0;
  int rejectedBlurry = 0;     // window winners dropped by the relative threshold
};

// Splits the frames into consecutive windows of `window` frames and keeps the sharpest frame of
// each window (the earliest one on ties, so the result is deterministic). A winner whose score is
// below `minRelative` x median(all scores) is dropped: that window is all motion blur (shaking).
FrameSelection selectSharpestFrames(const std::vector<double>& scores, int window, double minRelative = 0.35);

// Reads one binary PGM ("P5", maxval 255) frame from a stream such as `ffmpeg -f image2pipe
// -vcodec pgm -`. Returns false at end of stream or on malformed data (`error` says which).
bool readPgmFrame(std::FILE* stream, GrayImage& out, bool& error);

}  // namespace gsv
