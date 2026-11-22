#pragma once
// OpenGL entry points for the two targets of the project:
//
//  * Desktop OpenGL 4.3 (default): a minimal hand-written loader (replaces glad/GLEW).
//    glcorearb.h provides the types, enums and PFNGL...PROC typedefs. Every function
//    this project uses is listed once in GSV_GL_FUNCTIONS; the X-macro expands that
//    list into extern declarations here and into definitions + loading in the .cpp.
//    To use a new GL function: add one line to the list.
//
//  * OpenGL ES 3.1+ (Android, GSV_GLES defined): the functions are exported by
//    libGLESv3 and declared by <GLES3/gl32.h>, so there is nothing to load. The code
//    only uses the common subset of the two APIs; the few differences are guarded by
//    GSV_GLES where they are used (buffer read-back, timer queries, point size).

#if defined(GSV_GLES)

#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>  // GL_TIME_ELAPSED_EXT (EXT_disjoint_timer_query)

#include <string>

namespace gl {
using ProcLoader = void* (*)(const char* name);
inline bool loadFunctions(ProcLoader, std::string& missing) {
  missing.clear();
  return true;
}
}  // namespace gl

#else  // desktop OpenGL 4.3

// Defining APIENTRY ourselves stops glcorearb.h from pulling in <windows.h>
// (and its min/max macros) on Windows.
#if defined(_WIN32) && !defined(APIENTRY)
#define APIENTRY __stdcall
#endif
#include <GL/glcorearb.h>

#include <string>

#define GSV_GL_FUNCTIONS(X)                                         \
  /* state */                                                       \
  X(PFNGLGETERRORPROC, glGetError)                                  \
  X(PFNGLGETSTRINGPROC, glGetString)                                \
  X(PFNGLGETINTEGERVPROC, glGetIntegerv)                            \
  X(PFNGLENABLEPROC, glEnable)                                      \
  X(PFNGLDISABLEPROC, glDisable)                                    \
  X(PFNGLVIEWPORTPROC, glViewport)                                  \
  X(PFNGLCLEARPROC, glClear)                                        \
  X(PFNGLCLEARCOLORPROC, glClearColor)                              \
  X(PFNGLDEPTHMASKPROC, glDepthMask)                                \
  X(PFNGLDEPTHFUNCPROC, glDepthFunc)                                \
  X(PFNGLBLENDFUNCPROC, glBlendFunc)                                \
  X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                \
  X(PFNGLBLENDEQUATIONPROC, glBlendEquation)                        \
  X(PFNGLPIXELSTOREIPROC, glPixelStorei)                            \
  X(PFNGLREADPIXELSPROC, glReadPixels)                              \
  X(PFNGLREADBUFFERPROC, glReadBuffer)                              \
  /* shaders */                                                     \
  X(PFNGLCREATESHADERPROC, glCreateShader)                          \
  X(PFNGLSHADERSOURCEPROC, glShaderSource)                          \
  X(PFNGLCOMPILESHADERPROC, glCompileShader)                        \
  X(PFNGLGETSHADERIVPROC, glGetShaderiv)                            \
  X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                  \
  X(PFNGLDELETESHADERPROC, glDeleteShader)                          \
  X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                        \
  X(PFNGLATTACHSHADERPROC, glAttachShader)                          \
  X(PFNGLLINKPROGRAMPROC, glLinkProgram)                            \
  X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                          \
  X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                \
  X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                        \
  X(PFNGLUSEPROGRAMPROC, glUseProgram)                              \
  X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)              \
  X(PFNGLUNIFORM1IPROC, glUniform1i)                                \
  X(PFNGLUNIFORM1UIPROC, glUniform1ui)                              \
  X(PFNGLUNIFORM1FPROC, glUniform1f)                                \
  X(PFNGLUNIFORM2FPROC, glUniform2f)                                \
  X(PFNGLUNIFORM3FPROC, glUniform3f)                                \
  X(PFNGLUNIFORM4FPROC, glUniform4f)                                \
  X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv)                  \
  X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                  \
  /* buffers & vertex arrays */                                     \
  X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                    \
  X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                    \
  X(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays)              \
  X(PFNGLGENBUFFERSPROC, glGenBuffers)                              \
  X(PFNGLBINDBUFFERPROC, glBindBuffer)                              \
  X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase)                      \
  X(PFNGLBUFFERDATAPROC, glBufferData)                              \
  X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange)                      \
  X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer)                            \
  X(PFNGLBUFFERSUBDATAPROC, glBufferSubData)                        \
  X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers)                        \
  X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)            \
  X(PFNGLVERTEXATTRIBIPOINTERPROC, glVertexAttribIPointer)          \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)    \
  X(PFNGLVERTEXATTRIBDIVISORPROC, glVertexAttribDivisor)            \
  /* drawing & compute */                                           \
  X(PFNGLDRAWARRAYSPROC, glDrawArrays)                              \
  X(PFNGLDRAWARRAYSINSTANCEDPROC, glDrawArraysInstanced)            \
  X(PFNGLDRAWARRAYSINDIRECTPROC, glDrawArraysIndirect)              \
  X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute)                    \
  X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier)                        \
  X(PFNGLFINISHPROC, glFinish)                                      \
  /* buffer readback & synchronisation */                           \
  X(PFNGLGETBUFFERSUBDATAPROC, glGetBufferSubData)                  \
  X(PFNGLCOPYBUFFERSUBDATAPROC, glCopyBufferSubData)                \
  X(PFNGLFENCESYNCPROC, glFenceSync)                                \
  X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync)                      \
  X(PFNGLDELETESYNCPROC, glDeleteSync)                              \
  /* textures & framebuffers (offscreen rendering for tests) */     \
  X(PFNGLGENTEXTURESPROC, glGenTextures)                            \
  X(PFNGLDELETETEXTURESPROC, glDeleteTextures)                      \
  X(PFNGLBINDTEXTUREPROC, glBindTexture)                            \
  X(PFNGLTEXIMAGE2DPROC, glTexImage2D)                              \
  X(PFNGLTEXPARAMETERIPROC, glTexParameteri)                        \
  X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)                    \
  X(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers)              \
  X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer)                    \
  X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)          \
  X(PFNGLDRAWBUFFERSPROC, glDrawBuffers)                            \
  X(PFNGLCLEARBUFFERFVPROC, glClearBufferfv)                        \
  X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus)      \
  X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)                    \
  /* GPU timer queries */                                           \
  X(PFNGLGENQUERIESPROC, glGenQueries)                              \
  X(PFNGLDELETEQUERIESPROC, glDeleteQueries)                        \
  X(PFNGLBEGINQUERYPROC, glBeginQuery)                              \
  X(PFNGLENDQUERYPROC, glEndQuery)                                  \
  X(PFNGLGETQUERYOBJECTIVPROC, glGetQueryObjectiv)                  \
  X(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv)                \
  X(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v)            \
  /* debug output (GL 4.3) */                                       \
  X(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback)          \
  X(PFNGLDEBUGMESSAGECONTROLPROC, glDebugMessageControl)

#define GSV_DECLARE_GL(type, name) extern type name;
GSV_GL_FUNCTIONS(GSV_DECLARE_GL)
#undef GSV_DECLARE_GL

namespace gl {

using ProcLoader = void* (*)(const char* name);

// Resolves every function in GSV_GL_FUNCTIONS. Returns false and lists the
// missing names if the driver does not expose some of them.
bool loadFunctions(ProcLoader loader, std::string& missing);

}  // namespace gl

#endif  // GSV_GLES
