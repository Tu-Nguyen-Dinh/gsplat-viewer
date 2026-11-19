#include "gl/shader.h"

#include <glm/gtc/type_ptr.hpp>

#include <fstream>
#include <sstream>

#ifndef GSV_SHADER_DIR
#define GSV_SHADER_DIR "shaders"
#endif

std::string shaderPath(const std::string& file) { return std::string(GSV_SHADER_DIR) + "/" + file; }

namespace {

bool readText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

GLuint compileStage(GLenum stage, const std::string& file, std::string& log) {
  std::string src;
  if (!readText(shaderPath(file), src)) {
    log += "Cannot read shader file: " + shaderPath(file) + "\n";
    return 0;
  }
  GLuint s = glCreateShader(stage);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    std::string msg(static_cast<size_t>(len > 1 ? len : 1), '\0');
    glGetShaderInfoLog(s, len, nullptr, msg.data());
    log += file + ":\n" + msg + "\n";
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint linkProgram(std::initializer_list<GLuint> stages, std::string& log) {
  GLuint prog = glCreateProgram();
  for (GLuint s : stages) glAttachShader(prog, s);
  glLinkProgram(prog);
  for (GLuint s : stages) glDeleteShader(s);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    std::string msg(static_cast<size_t>(len > 1 ? len : 1), '\0');
    glGetProgramInfoLog(prog, len, nullptr, msg.data());
    log += "Link error:\n" + msg + "\n";
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

}  // namespace

ShaderProgram::~ShaderProgram() {
  if (program_) glDeleteProgram(program_);
}

void ShaderProgram::replace(GLuint newProgram) {
  if (program_) glDeleteProgram(program_);
  program_ = newProgram;
  uniformCache_.clear();
}

bool ShaderProgram::loadGraphics(const std::string& vertFile, const std::string& fragFile, std::string& log) {
  log.clear();
  GLuint vs = compileStage(GL_VERTEX_SHADER, vertFile, log);
  GLuint fs = compileStage(GL_FRAGMENT_SHADER, fragFile, log);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return false;
  }
  GLuint prog = linkProgram({vs, fs}, log);
  if (!prog) return false;
  replace(prog);
  files_[0] = vertFile;
  files_[1] = fragFile;
  isCompute_ = false;
  return true;
}

bool ShaderProgram::loadCompute(const std::string& compFile, std::string& log) {
  log.clear();
  GLuint cs = compileStage(GL_COMPUTE_SHADER, compFile, log);
  if (!cs) return false;
  GLuint prog = linkProgram({cs}, log);
  if (!prog) return false;
  replace(prog);
  files_[0] = compFile;
  files_[1].clear();
  isCompute_ = true;
  return true;
}

bool ShaderProgram::reload(std::string& log) {
  if (files_[0].empty()) return false;
  const std::string a = files_[0], b = files_[1];
  return isCompute_ ? loadCompute(a, log) : loadGraphics(a, b, log);
}

void ShaderProgram::use() const { glUseProgram(program_); }

GLint ShaderProgram::location(const char* name) {
  auto it = uniformCache_.find(name);
  if (it != uniformCache_.end()) return it->second;
  GLint loc = glGetUniformLocation(program_, name);
  uniformCache_.emplace(name, loc);
  return loc;
}

void ShaderProgram::set(const char* n, int v) { glUniform1i(location(n), v); }
void ShaderProgram::set(const char* n, unsigned v) { glUniform1ui(location(n), v); }
void ShaderProgram::set(const char* n, float v) { glUniform1f(location(n), v); }
void ShaderProgram::set(const char* n, const glm::vec2& v) { glUniform2f(location(n), v.x, v.y); }
void ShaderProgram::set(const char* n, const glm::vec3& v) { glUniform3f(location(n), v.x, v.y, v.z); }
void ShaderProgram::set(const char* n, const glm::vec4& v) { glUniform4f(location(n), v.x, v.y, v.z, v.w); }
void ShaderProgram::set(const char* n, const glm::mat4& m) {
  glUniformMatrix4fv(location(n), 1, GL_FALSE, glm::value_ptr(m));
}
