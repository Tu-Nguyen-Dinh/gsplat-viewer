#pragma once

#include <filesystem>
#include <string>

namespace gsv {

// std::filesystem::path <-> UTF-8 std::string. GLFW, ImGui and the command line hand us UTF-8;
// going through u8string keeps non-ASCII folder names working on Windows.
inline std::filesystem::path pathFromUtf8(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
  const std::u8string s = path.generic_u8string();
  return std::string(s.begin(), s.end());
}

}  // namespace gsv
