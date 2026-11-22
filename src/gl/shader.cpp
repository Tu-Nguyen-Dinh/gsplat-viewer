#include "gl/shader.h"

#include "core/utf8_path.h"

#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#ifndef GSV_SHADER_DIR
#define GSV_SHADER_DIR "shaders"
#endif

namespace gsv {

namespace {
std::string& shaderDirStorage() {
  static std::string dir = GSV_SHADER_DIR;
  return dir;
}
}  // namespace

const std::string& shaderDirectory() { return shaderDirStorage(); }
void setShaderDirectory(const std::string& dir) { shaderDirStorage() = dir; }
std::string shaderPath(const std::string& file) { return shaderDirectory() + "/" + file; }

std::string findShaderDirectory(const std::string& override, const std::string& exeDir, const std::string& marker) {
  namespace fs = std::filesystem;
  std::error_code ec;
  auto has = [&](const fs::path& dir) { return fs::is_regular_file(dir / marker, ec); };
  if (!override.empty()) return has(pathFromUtf8(override)) ? override : std::string();
  if (!exeDir.empty()) {
    const fs::path packaged = pathFromUtf8(exeDir) / "shaders";
    if (has(packaged)) return pathToUtf8(packaged);
  }
  return has(pathFromUtf8(GSV_SHADER_DIR)) ? std::string(GSV_SHADER_DIR) : std::string();
}

namespace {

bool readText(const std::string& path, std::string& out) {
  std::ifstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// The shaders are written once, for desktop GLSL 4.30. For OpenGL ES the first line becomes
// GLSL ES 3.10 plus default precisions (ES fragment shaders have no default float precision);
// everything else in the sources sticks to the common subset of the two languages.
#if defined(GSV_GLES)
constexpr bool kDefaultEs = true;
#else
constexpr bool kDefaultEs = false;
#endif
const char* versionLine(bool es) {
  // 3.10 (not 3.20): the shaders need nothing newer, and it also runs on ES 3.1 devices and the
  // Android emulator (which exposes OpenGL ES 3.1).
  return es ? "#version 310 es\nprecision highp float;\nprecision highp int;\n#define GSV_GLES 1\n" : "#version 430 core\n";
}

// Expands `#include "x"` recursively (each file at most once) and injects `defines`
// after the #version line. Returns false if a file is missing.
bool preprocess(const std::string& file, const std::string& defines, std::string& out, std::string& log,
                std::set<std::string>& included, bool es = kDefaultEs) {
  std::string src;
  if (!readText(shaderPath(file), src)) {
    log += "Cannot read shader file: " + shaderPath(file) + "\n";
    return false;
  }
  std::istringstream in(src);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("#include", 0) == 0) {
      const size_t a = line.find('"'), b = line.rfind('"');
      if (a == std::string::npos || b <= a) {
        log += file + ": malformed #include\n";
        return false;
      }
      const std::string inc = line.substr(a + 1, b - a - 1);
      if (included.insert(inc).second && !preprocess(inc, {}, out, log, included, es)) return false;
      continue;
    }
    if (line.rfind("#version", 0) == 0) {
      out += versionLine(es);
      out += defines;
      continue;
    }
    out += line + "\n";
  }
  return true;
}

GLuint compileStage(GLenum stage, const std::string& file, const std::string& defines, std::string& log) {
  std::string src;
  std::set<std::string> included;
  if (!preprocess(file, defines, src, log, included)) return 0;
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
    log += file + " (line numbers count the expanded source):\n" + msg + "\n";
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

bool expandShaderSource(const std::string& file, bool openglEs, std::string& out, std::string& log) {
  out.clear();
  std::set<std::string> included;
  return preprocess(file, {}, out, log, included, openglEs);
}

ShaderProgram::~ShaderProgram() {
  if (program_) glDeleteProgram(program_);
}

void ShaderProgram::replace(GLuint newProgram) {
  if (program_) glDeleteProgram(program_);
  program_ = newProgram;
  uniformCache_.clear();
}

bool ShaderProgram::loadGraphics(const std::string& vertFile, const std::string& fragFile, std::string& log,
                                 const std::string& defines) {
  log.clear();
  GLuint vs = compileStage(GL_VERTEX_SHADER, vertFile, defines, log);
  GLuint fs = compileStage(GL_FRAGMENT_SHADER, fragFile, defines, log);
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
  defines_ = defines;
  isCompute_ = false;
  return true;
}

bool ShaderProgram::loadCompute(const std::string& compFile, std::string& log, const std::string& defines) {
  log.clear();
  GLuint cs = compileStage(GL_COMPUTE_SHADER, compFile, defines, log);
  if (!cs) return false;
  GLuint prog = linkProgram({cs}, log);
  if (!prog) return false;
  replace(prog);
  files_[0] = compFile;
  files_[1].clear();
  defines_ = defines;
  isCompute_ = true;
  return true;
}

bool ShaderProgram::reload(std::string& log) {
  if (files_[0].empty()) return false;
  const std::string a = files_[0], b = files_[1], d = defines_;
  return isCompute_ ? loadCompute(a, log, d) : loadGraphics(a, b, log, d);
}

void ShaderProgram::use() const { glUseProgram(program_); }

void ShaderProgram::dispatch1D(unsigned items, unsigned groupSize) const {
  const unsigned groups = (items + groupSize - 1) / groupSize;
  if (groups > 0) glDispatchCompute(groups, 1, 1);
}

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
void ShaderProgram::set(const char* n, const glm::mat3& m) {
  glUniformMatrix3fv(location(n), 1, GL_FALSE, glm::value_ptr(m));
}

void ShaderProgram::set(const char* n, const glm::mat4& m) {
  glUniformMatrix4fv(location(n), 1, GL_FALSE, glm::value_ptr(m));
}

}  // namespace gsv
