#include "gl/gpu_timer.h"

GpuTimer::~GpuTimer() {
  if (created_) glDeleteQueries(kRing, queries_);
}

void GpuTimer::begin() {
  if (!created_) {
    glGenQueries(kRing, queries_);
    created_ = true;
  }
  const GLuint q = queries_[index_];
  if (pending_[index_]) {
    GLint available = 0;
    glGetQueryObjectiv(q, GL_QUERY_RESULT_AVAILABLE, &available);
    if (available) {
      GLuint64 ns = 0;
      glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
      const double ms = static_cast<double>(ns) * 1e-6;
      smoothedMs_ = smoothedMs_ == 0.0 ? ms : smoothedMs_ * 0.9 + ms * 0.1;
    }
    // If not available yet we simply drop that sample instead of blocking.
    pending_[index_] = false;
  }
  glBeginQuery(GL_TIME_ELAPSED, q);
}

void GpuTimer::end() {
  glEndQuery(GL_TIME_ELAPSED);
  pending_[index_] = true;
  index_ = (index_ + 1) % kRing;
}
