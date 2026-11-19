#include "gl/gpu_timer.h"

namespace gsv {

GpuTimer::~GpuTimer() {
  if (created_) glDeleteQueries(kRing, queries_);
}

void GpuTimer::begin() {
  if (!created_) {
    glGenQueries(kRing, queries_);
    created_ = true;
  }
  collect(index_);
  glBeginQuery(GL_TIME_ELAPSED, queries_[index_]);
}

void GpuTimer::collect(int slot) {
  if (!pending_[slot]) return;
  GLint available = 0;
  glGetQueryObjectiv(queries_[slot], GL_QUERY_RESULT_AVAILABLE, &available);
  if (available) {
    GLuint64 ns = 0;
    glGetQueryObjectui64v(queries_[slot], GL_QUERY_RESULT, &ns);
    const double ms = static_cast<double>(ns) * 1e-6;
    smoothedMs_ = smoothedMs_ == 0.0 ? ms : smoothedMs_ * 0.9 + ms * 0.1;
    sumMs_ += ms;
    ++samples_;
  }
  // If not available yet we simply drop that sample instead of blocking.
  pending_[slot] = false;
}

void GpuTimer::end() {
  glEndQuery(GL_TIME_ELAPSED);
  pending_[index_] = true;
  index_ = (index_ + 1) % kRing;
}

}  // namespace gsv
