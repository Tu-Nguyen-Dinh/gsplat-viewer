#include "gl/gpu_timer.h"

#if defined(GSV_GLES)
#include <EGL/egl.h>
#include <cstring>
#endif

namespace gsv {

namespace {

#if defined(GSV_GLES)
// OpenGL ES has no GL_TIME_ELAPSED in core: it comes with EXT_disjoint_timer_query (Adreno, Mali
// and PowerVR drivers expose it). Without the extension the timers stay at 0.
using GetQueryObjectui64vEXT = void (*)(GLuint, GLenum, GLuint64*);
GetQueryObjectui64vEXT timerQueryResult() {
  static const GetQueryObjectui64vEXT fn = []() -> GetQueryObjectui64vEXT {
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!ext || !std::strstr(ext, "GL_EXT_disjoint_timer_query")) return nullptr;
    return reinterpret_cast<GetQueryObjectui64vEXT>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
  }();
  return fn;
}
constexpr GLenum kTimeElapsed = GL_TIME_ELAPSED_EXT;
bool timersAvailable() { return timerQueryResult() != nullptr; }
void queryResult64(GLuint q, GLuint64* out) { timerQueryResult()(q, GL_QUERY_RESULT, out); }
#else
constexpr GLenum kTimeElapsed = GL_TIME_ELAPSED;
bool timersAvailable() { return true; }
void queryResult64(GLuint q, GLuint64* out) { glGetQueryObjectui64v(q, GL_QUERY_RESULT, out); }
#endif

}  // namespace

GpuTimer::~GpuTimer() {
  if (created_) glDeleteQueries(kRing, queries_);
}

void GpuTimer::begin() {
  if (!timersAvailable()) return;
  if (!created_) {
    glGenQueries(kRing, queries_);
    created_ = true;
  }
  collect(index_);
  glBeginQuery(kTimeElapsed, queries_[index_]);
}

void GpuTimer::collect(int slot) {
  if (!pending_[slot]) return;
  GLuint available = 0;
  glGetQueryObjectuiv(queries_[slot], GL_QUERY_RESULT_AVAILABLE, &available);
  if (available) {
    GLuint64 ns = 0;
    queryResult64(queries_[slot], &ns);
    const double ms = static_cast<double>(ns) * 1e-6;
    smoothedMs_ = smoothedMs_ == 0.0 ? ms : smoothedMs_ * 0.9 + ms * 0.1;
    sumMs_ += ms;
    ++samples_;
  }
  // If not available yet we simply drop that sample instead of blocking.
  pending_[slot] = false;
}

void GpuTimer::end() {
  if (!timersAvailable()) return;
  glEndQuery(kTimeElapsed);
  pending_[index_] = true;
  index_ = (index_ + 1) % kRing;
}

}  // namespace gsv
