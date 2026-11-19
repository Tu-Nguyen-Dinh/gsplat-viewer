#pragma once

#include "gl/gl_loader.h"

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>

// RAII wrapper around a linked GLSL program. Reloading keeps the previous
// program alive when the new source fails to compile, so a typo during
// hot-reload never leaves the viewer without a shader.
class ShaderProgram {
 public:
  ShaderProgram() = default;
  ~ShaderProgram();
  ShaderProgram(const ShaderProgram&) = delete;
  ShaderProgram& operator=(const ShaderProgram&) = delete;

  // Paths are relative to GSV_SHADER_DIR. On failure `log` holds the compiler output.
  bool loadGraphics(const std::string& vertFile, const std::string& fragFile, std::string& log);
  bool loadCompute(const std::string& compFile, std::string& log);
  // Re-reads the files passed to the last successful load*().
  bool reload(std::string& log);

  void use() const;
  GLuint id() const { return program_; }
  bool valid() const { return program_ != 0; }

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
  bool isCompute_ = false;
  std::unordered_map<std::string, GLint> uniformCache_;
};

std::string shaderPath(const std::string& file);
