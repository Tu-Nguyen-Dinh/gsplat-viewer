#pragma once

#include "gl/gl_loader.h"

namespace gsv {

// Measures GPU time of a block of GL commands with GL_TIME_ELAPSED queries.
// A ring of queries is used so reading a result never stalls the CPU waiting
// for the GPU; the value reported is a few frames old, which is fine for stats.
class GpuTimer {
 public:
  GpuTimer() = default;
  ~GpuTimer();
  GpuTimer(const GpuTimer&) = delete;
  GpuTimer& operator=(const GpuTimer&) = delete;

  void begin();
  void end();
  // Exponentially smoothed GPU time in milliseconds.
  double milliseconds() const { return smoothedMs_; }

 private:
  static constexpr int kRing = 4;
  GLuint queries_[kRing] = {};
  bool pending_[kRing] = {};
  int index_ = 0;
  bool created_ = false;
  double smoothedMs_ = 0.0;
};

}  // namespace gsv
