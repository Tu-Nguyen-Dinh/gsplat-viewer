#include "core/settings.h"

#include "core/json.h"
#include "core/utf8_path.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace gsv {

namespace {
std::string envUtf8(const char* name) {
#ifdef _WIN32
  std::wstring wname(name, name + std::strlen(name));
  const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
  if (n == 0) return {};
  std::wstring w(n, L'\0');
  GetEnvironmentVariableW(wname.c_str(), w.data(), n);
  w.resize(n - 1);
  return pathToUtf8(std::filesystem::path(w));
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}
}  // namespace

std::string settingsDirectory() {
  std::string dir = envUtf8("GSV_SETTINGS_DIR");
  if (dir.empty()) {
#ifdef _WIN32
    const std::string appData = envUtf8("APPDATA");
    dir = appData.empty() ? std::string(".") : pathToUtf8(pathFromUtf8(appData) / "gsplat-viewer");
#else
    const std::string xdg = envUtf8("XDG_CONFIG_HOME"), home = envUtf8("HOME");
    dir = !xdg.empty()    ? pathToUtf8(pathFromUtf8(xdg) / "gsplat-viewer")
          : !home.empty() ? pathToUtf8(pathFromUtf8(home) / ".config" / "gsplat-viewer")
                          : std::string(".");
#endif
  }
  std::error_code ec;
  std::filesystem::create_directories(pathFromUtf8(dir), ec);
  return dir;
}

std::string settingsPath() { return pathToUtf8(pathFromUtf8(settingsDirectory()) / "settings.json"); }

std::string settingsToJson(const Settings& s) {
  std::ostringstream o;
  o << "{\n  \"format\": \"gsplat-viewer settings\",\n  \"version\": " << s.version << ",\n";
  o << "  \"language\": " << jsonQuote(s.language) << ",\n";
  o << "  \"uiScale\": " << jsonNumber(s.uiScale) << ",\n";
  o << "  \"recentFiles\": [";
  for (size_t i = 0; i < s.recentFiles.size(); ++i) o << (i ? ", " : "") << jsonQuote(s.recentFiles[i]);
  o << "],\n  \"lastDirectory\": " << jsonQuote(s.lastDirectory) << ",\n";
  o << "  \"firstRunDone\": " << (s.firstRunDone ? "true" : "false") << ",\n";
  o << "  \"autosaveSeconds\": " << s.autosaveSeconds << ",\n";
  o << "  \"shDegree\": " << s.shDegree << ",\n";
  o << "  \"vsync\": " << (s.vsync ? "true" : "false") << ",\n";
  o << "  \"background\": [" << jsonNumber(s.background[0]) << ", " << jsonNumber(s.background[1]) << ", "
    << jsonNumber(s.background[2]) << "],\n";
  o << "  \"window\": [" << s.windowWidth << ", " << s.windowHeight << "],\n";
  o << "  \"maximized\": " << (s.maximized ? "true" : "false") << ",\n";
  o << "  \"autoQuality\": " << (s.autoQuality ? "true" : "false") << ",\n";
  o << "  \"flipYFiles\": [";
  for (size_t i = 0; i < s.flipYFiles.size(); ++i) o << (i ? ", " : "") << jsonQuote(s.flipYFiles[i]);
  o << "]\n}\n";
  return o.str();
}

bool settingsFromJson(const std::string& text, Settings& out, std::string& error) {
  out = Settings{};
  Json root;
  if (!parseJson(text, root, error, 16)) return false;
  if (!root.isObject()) {
    error = "settings file is not a JSON object";
    return false;
  }
  auto num = [&](const char* key, double lo, double hi, auto& field) {
    const Json* j = root.get(key);
    if (j && j->isNumber() && j->n >= lo && j->n <= hi) field = static_cast<std::decay_t<decltype(field)>>(j->n);
  };
  auto boolean = [&](const char* key, bool& field) {
    const Json* j = root.get(key);
    if (j && j->type == Json::Type::Bool) field = j->b;
  };
  if (const Json* j = root.get("language"); j && j->isString() && (j->s == "auto" || j->s == "en" || j->s == "vi"))
    out.language = j->s;
  num("uiScale", 0.0, 3.0, out.uiScale);
  if (out.uiScale > 0.0f && out.uiScale < 0.75f) out.uiScale = 0.75f;
  if (const Json* j = root.get("recentFiles"); j && j->isArray())
    for (const Json& f : j->a)
      if (f.isString() && !f.s.empty() && out.recentFiles.size() < kMaxRecentFiles) out.recentFiles.push_back(f.s);
  if (const Json* j = root.get("lastDirectory"); j && j->isString()) out.lastDirectory = j->s;
  boolean("firstRunDone", out.firstRunDone);
  num("autosaveSeconds", 0.0, 3600.0, out.autosaveSeconds);
  num("shDegree", 0.0, 3.0, out.shDegree);
  boolean("vsync", out.vsync);
  if (const Json* j = root.get("background"); j && j->isArray() && j->a.size() == 3) {
    bool ok = true;
    for (const Json& c : j->a) ok = ok && c.isNumber() && c.n >= 0.0 && c.n <= 1.0;
    if (ok)
      for (int k = 0; k < 3; ++k) out.background[k] = static_cast<float>(j->a[static_cast<size_t>(k)].n);
  }
  if (const Json* j = root.get("window"); j && j->isArray() && j->a.size() == 2 && j->a[0].isNumber() && j->a[1].isNumber()) {
    const double w = j->a[0].n, h = j->a[1].n;
    if (w >= 320 && w <= 16384 && h >= 240 && h <= 16384) {
      out.windowWidth = static_cast<int>(w);
      out.windowHeight = static_cast<int>(h);
    }
  }
  boolean("maximized", out.maximized);
  boolean("autoQuality", out.autoQuality);
  if (const Json* j = root.get("flipYFiles"); j && j->isArray())
    for (const Json& f : j->a)
      if (f.isString() && !f.s.empty() && out.flipYFiles.size() < 64) out.flipYFiles.push_back(f.s);
  return true;
}

bool loadSettings(const std::string& path, Settings& out, std::string& error) {
  out = Settings{};
  std::ifstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) return true;  // first start: defaults
  std::stringstream ss;
  ss << f.rdbuf();
  if (ss.str().size() > (size_t(1) << 20)) {
    error = "settings file larger than 1 MB";
    return false;
  }
  return settingsFromJson(ss.str(), out, error);
}

bool writeFileAtomic(const std::string& path, const std::string& bytes, std::string& error) {
  const std::filesystem::path target = pathFromUtf8(path);
  std::filesystem::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      error = "cannot write " + pathToUtf8(tmp);
      return false;
    }
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) {
      error = "write error on " + pathToUtf8(tmp);
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, target, ec);  // replaces the target atomically (MoveFileEx on Windows)
  if (ec) {
    error = "cannot replace " + path + ": " + ec.message();
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

bool saveSettings(const std::string& path, const Settings& s, std::string& error) {
  return writeFileAtomic(path, settingsToJson(s), error);
}

long long currentProcessId() {
#ifdef _WIN32
  return static_cast<long long>(GetCurrentProcessId());
#else
  return static_cast<long long>(getpid());
#endif
}

bool samePath(const std::string& a, const std::string& b) {
  auto norm = [](std::string s) {
    for (char& c : s) {
      if (c == '\\') c = '/';
#ifdef _WIN32
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    }
    return s;
  };
  return norm(a) == norm(b);
}

void removeRecentFile(Settings& s, const std::string& path) {
  s.recentFiles.erase(std::remove_if(s.recentFiles.begin(), s.recentFiles.end(),
                                     [&](const std::string& f) { return samePath(f, path); }),
                      s.recentFiles.end());
}

bool isFlipY(const Settings& s, const std::string& path) {
  for (const std::string& f : s.flipYFiles)
    if (samePath(f, path)) return true;
  return false;
}

void setFlipY(Settings& s, const std::string& path, bool flip) {
  if (path.empty()) return;
  s.flipYFiles.erase(std::remove_if(s.flipYFiles.begin(), s.flipYFiles.end(), [&](const std::string& f) { return samePath(f, path); }),
                     s.flipYFiles.end());
  if (flip) {
    s.flipYFiles.insert(s.flipYFiles.begin(), path);
    if (s.flipYFiles.size() > 64) s.flipYFiles.resize(64);
  }
}

void addRecentFile(Settings& s, const std::string& path) {
  if (path.empty()) return;
  removeRecentFile(s, path);
  s.recentFiles.insert(s.recentFiles.begin(), path);
  if (s.recentFiles.size() > kMaxRecentFiles) s.recentFiles.resize(kMaxRecentFiles);
}

}  // namespace gsv
