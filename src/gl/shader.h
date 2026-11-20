#pragma once

#include "gl/gl_loader.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>

namespace gsv {

// RAII wrapper around a linked GLSL program.
//
// Source files are read from shaderDirectory() (see below) and pre-processed on the CPU:
//   - `#include "file.glsl"` lines are replaced by the file contents (GLSL has no #include),
//   - `defines` (e.g. "#define WORKGROUP 256\n") are inserted right after #version.
// Reloading keeps the previous program alive when the new source fails to compile,
// so a typo during hot-reload never leaves the viewer without a shader.
class ShaderProgram {
 public:
  ShaderProgram() = default;
  ~ShaderProgram();
  ShaderProgram(const ShaderProgram&) = delete;
  ShaderProgram& operator=(const ShaderProgram&) = delete;

  bool loadGraphics(const std::string& vertFile, const std::string& fragFile, std::string& log,
                    const std::string& defines = {});
  bool loadCompute(const std::string& compFile, std::string& log, const std::string& defines = {});
  // Re-reads the files passed to the last successful load*().
  bool reload(std::string& log);

  void use() const;
  GLuint id() const { return program_; }
  bool valid() const { return program_ != 0; }

  // Dispatches enough work groups to cover `items` invocations (1D).
  void dispatch1D(unsigned items, unsigned groupSize) const;

  void set(const char* name, int v);
  void set(const char* name, unsigned v);
  void set(const char* name, float v);
  void set(const char* name, const glm::vec2& v);
  void set(const char* name, const glm::vec3& v);
  void set(const char* name, const glm::vec4& v);
  void set(const char* name, const glm::mat4& m);

 private:
  GLint location(const char* name);
  void replace(GLuint newProgram);

  GLuint program_ = 0;
  std::string files_[2];
  std::string defines_;
  bool isCompute_ = false;
  std::unordered_map<std::string, GLint> uniformCache_;
};

// Where shader sources live. Defaults to the build-time GSV_SHADER_DIR (the source tree, so
// shaders can be edited and hot-reloaded); a packaged build ships them next to the executable.
const std::string& shaderDirectory();
void setShaderDirectory(const std::string& dir);
std::string shaderPath(const std::string& file);  // shaderDirectory() + "/" + file

// Picks the shader directory: the first of `override` (if not empty), `<exeDir>/shaders` and
// GSV_SHADER_DIR that contains `marker`. Returns an empty string if none does.
std::string findShaderDirectory(const std::string& override, const std::string& exeDir,
                                const std::string& marker = "splat.vert");

}  // namespace gsv
