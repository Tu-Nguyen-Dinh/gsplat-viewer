#include "core/platform.h"

#include "core/utf8_path.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
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

std::vector<std::string> utf8Arguments(int argc, char** argv) {
#ifdef _WIN32
  int n = 0;
  if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n)) {
    std::vector<std::string> args;
    for (int i = 0; i < n; ++i) {
      const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
      std::string s(static_cast<size_t>(bytes > 0 ? bytes - 1 : 0), '\0');  // without the terminator
      if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), bytes, nullptr, nullptr);
      args.push_back(std::move(s));
    }
    LocalFree(wide);
    return args;
  }
#endif
  return std::vector<std::string>(argv, argv + argc);
}

Utf8Argv::Utf8Argv(int argc, char** argv) : strings(utf8Arguments(argc, argv)) {
  for (std::string& s : strings) pointers.push_back(s.data());
  pointers.push_back(nullptr);
}

std::string quoteArgument(const std::string& arg) {
  std::string out = "\"";
  for (char c : arg) {
    if (c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
  return w;
}
// cmd.exe /c strips the first and last quote of a command that starts with a quote, so a command
// with quoted program and quoted arguments must be wrapped in one more pair of quotes.
std::wstring shellCommand(const std::string& utf8) { return L"\"" + widen(utf8) + L"\""; }
}  // namespace

std::FILE* openProcessRead(const std::string& utf8Command) { return _wpopen(shellCommand(utf8Command).c_str(), L"rb"); }
int closeProcess(std::FILE* process) { return _pclose(process); }
int runProcess(const std::string& utf8Command) {
  std::fflush(nullptr);  // keep our own output ordered before the child's
  return _wsystem(shellCommand(utf8Command).c_str());
}
#else
namespace {
int exitCode(int status) { return status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1; }
}  // namespace
std::FILE* openProcessRead(const std::string& utf8Command) { return popen(utf8Command.c_str(), "r"); }
int closeProcess(std::FILE* process) { return exitCode(pclose(process)); }
int runProcess(const std::string& utf8Command) {
  std::fflush(nullptr);
  return exitCode(std::system(utf8Command.c_str()));
}
#endif

}  // namespace gsv
