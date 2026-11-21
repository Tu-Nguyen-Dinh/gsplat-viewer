#include "gl/gl_loader.h"

// Desktop OpenGL only: on OpenGL ES the functions come straight from libGLESv3.
#if !defined(GSV_GLES)

#define GSV_DEFINE_GL(type, name) type name = nullptr;
GSV_GL_FUNCTIONS(GSV_DEFINE_GL)
#undef GSV_DEFINE_GL

namespace gl {

bool loadFunctions(ProcLoader loader, std::string& missing) {
  missing.clear();
#define GSV_LOAD_GL(type, name)                                \
  name = reinterpret_cast<type>(loader(#name));                \
  if (!name) missing += std::string(missing.empty() ? "" : ", ") + #name;
  GSV_GL_FUNCTIONS(GSV_LOAD_GL)
#undef GSV_LOAD_GL
  return missing.empty();
}

}  // namespace gl

#endif  // !GSV_GLES
