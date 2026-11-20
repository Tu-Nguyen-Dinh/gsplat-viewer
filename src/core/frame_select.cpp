#include "core/frame_select.h"

#include "core/image.h"

#include <algorithm>
#include <cctype>

namespace gsv {

double laplacianVariance(const GrayImage& image) {
  if (image.width < 3 || image.height < 3) return 0.0;
  // One pass with sum and sum of squares (values are small integers, so double is exact enough).
  double sum = 0.0, sumSq = 0.0;
  for (int y = 1; y < image.height - 1; ++y) {
    const uint8_t* up = &image.pixels[static_cast<size_t>(y - 1) * image.width];
    const uint8_t* row = up + image.width;
    const uint8_t* down = row + image.width;
    for (int x = 1; x < image.width - 1; ++x) {
      const int lap = up[x] + down[x] + row[x - 1] + row[x + 1] - 4 * row[x];
      sum += lap;
      sumSq += static_cast<double>(lap) * lap;
    }
  }
  const double n = static_cast<double>(image.width - 2) * (image.height - 2);
  const double mean = sum / n;
  return std::max(0.0, sumSq / n - mean * mean);
}

FrameSelection selectSharpestFrames(const std::vector<double>& scores, int window, double minRelative) {
  FrameSelection result;
  if (scores.empty()) return result;
  window = std::max(1, window);

  std::vector<double> sorted = scores;
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
  result.medianScore = sorted[sorted.size() / 2];
  const double threshold = minRelative * result.medianScore;

  for (size_t start = 0; start < scores.size(); start += window) {
    const size_t end = std::min(scores.size(), start + window);
    size_t best = start;
    for (size_t i = start + 1; i < end; ++i)
      if (scores[i] > scores[best]) best = i;  // strict: earliest index wins ties
    if (scores[best] < threshold) {
      ++result.rejectedBlurry;
      continue;
    }
    result.selected.push_back(static_cast<int>(best));
  }
  return result;
}

bool readPgmFrame(std::FILE* stream, GrayImage& out, bool& error) {
  error = false;
  const int p = std::fgetc(stream);
  if (p == EOF) return false;  // clean end of stream
  int maxval = 0;
  if (p != 'P' || std::fgetc(stream) != '5' || !readPnmNumber(stream, out.width) || !readPnmNumber(stream, out.height) ||
      !readPnmNumber(stream, maxval) || maxval != 255 || out.width <= 0 || out.height <= 0) {
    error = true;
    return false;
  }
  out.pixels.resize(static_cast<size_t>(out.width) * out.height);
  if (std::fread(out.pixels.data(), 1, out.pixels.size(), stream) != out.pixels.size()) {
    error = true;  // truncated frame
    return false;
  }
  return true;
}

}  // namespace gsv
