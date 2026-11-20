#include "core/platform.h"

#include "core/utf8_path.h"

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsv {

std::string executableDirectory() {
#ifdef _WIN32
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0) return {};
    if (n < buffer.size()) {
      buffer.resize(n);
      break;
    }
    buffer.resize(buffer.size() * 2);  // path longer than the buffer: retry
  }
  return pathToUtf8(std::filesystem::path(buffer).parent_path());
#else
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : pathToUtf8(exe.parent_path());
#endif
}

}  // namespace gsv
