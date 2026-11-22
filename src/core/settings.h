#pragma once
// Viewer settings that survive a restart (Phase 6, docs/core/CORE6-ux.md): language, UI scale,
// recent files, first-run state, autosave interval, a few render preferences, window size.
// Stored as JSON in settingsDirectory()/settings.json, written atomically (temp file + rename) so a
// crash while saving never leaves a truncated file. Unknown keys are ignored, bad values fall back
// to the defaults (a damaged settings file must never stop the viewer from starting).

#include <string>
#include <vector>

namespace gsv {

struct Settings {
  int version = 1;
  std::string language = "auto";        // "auto" (system), "en", "vi"
  float uiScale = 0.0f;                 // 0 = from the monitor's DPI; else 0.75 .. 3
  std::vector<std::string> recentFiles; // UTF-8, most recent first (scenes and projects)
  std::string lastDirectory;            // UTF-8, for the Open dialog
  bool firstRunDone = false;            // the 5-step tour was shown
  int autosaveSeconds = 120;            // 0 = off
  int shDegree = 3;
  bool vsync = true;
  float background[3] = {0.07f, 0.08f, 0.1f};
  int windowWidth = 1600, windowHeight = 900;
  bool maximized = false;
  bool autoQuality = true;              // lower the render resolution while the camera moves
  std::vector<std::string> flipYFiles;  // scenes the user turned upside down (Flip Y), max 64
};

constexpr size_t kMaxRecentFiles = 12;

// Per-user folder: %APPDATA%\gsplat-viewer (Windows), $XDG_CONFIG_HOME/gsplat-viewer or
// ~/.config/gsplat-viewer. The environment variable GSV_SETTINGS_DIR overrides it (tests, portable
// installs). Created on demand. UTF-8.
std::string settingsDirectory();
std::string settingsPath();  // settingsDirectory()/settings.json

std::string settingsToJson(const Settings& s);
// Always succeeds in giving usable settings: on a parse error `out` keeps the defaults and `error`
// says why (returns false). Values out of range are clamped / replaced individually.
bool settingsFromJson(const std::string& json, Settings& out, std::string& error);
bool loadSettings(const std::string& path, Settings& out, std::string& error);  // missing file: defaults, true
bool saveSettings(const std::string& path, const Settings& s, std::string& error);

// Puts `path` first, removes duplicates (case-insensitive on Windows, also '/' vs '\'), keeps at
// most kMaxRecentFiles.
void addRecentFile(Settings& s, const std::string& path);
bool isFlipY(const Settings& s, const std::string& path);
void setFlipY(Settings& s, const std::string& path, bool flip);
void removeRecentFile(Settings& s, const std::string& path);
bool samePath(const std::string& a, const std::string& b);

// Writes `bytes` to `path` through a temporary file in the same folder and a rename.
long long currentProcessId();

bool writeFileAtomic(const std::string& path, const std::string& bytes, std::string& error);

}  // namespace gsv
