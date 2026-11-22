// Phase 6: settings that survive a restart and the Vietnamese / English interface (ER 6.7).
#include "core/i18n.h"
#include "core/settings.h"
#include "core/utf8_path.h"
#include "test_framework.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace gsv;

namespace {
std::string tempDir(const char* name) {
  const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
  std::filesystem::remove_all(p);
  std::filesystem::create_directories(p);
  return pathToUtf8(p);
}

// printf conversion specifiers in order ("%zu", "%.1f", "%%" ...).
std::vector<std::string> specifiers(const char* s) {
  std::vector<std::string> out;
  for (const char* p = s; *p; ++p) {
    if (*p != '%') continue;
    std::string spec = "%";
    ++p;
    while (*p && std::strchr("0123456789.-+ #lhzjt", *p)) spec += *p++;
    if (!*p) break;
    spec += *p;
    out.push_back(spec);
  }
  return out;
}

// Code points of a UTF-8 string; false on malformed UTF-8.
bool decodeUtf8(const char* s, std::vector<unsigned>& cps) {
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;) {
    unsigned cp;
    int n;
    if (*p < 0x80) cp = *p, n = 1;
    else if ((*p & 0xE0) == 0xC0) cp = *p & 0x1Fu, n = 2;
    else if ((*p & 0xF0) == 0xE0) cp = *p & 0x0Fu, n = 3;
    else return false;
    for (int k = 1; k < n; ++k) {
      if ((p[k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[k] & 0x3Fu);
    }
    cps.push_back(cp);
    p += n;
  }
  return true;
}
}  // namespace

TEST_CASE(ux_settings_round_trip_and_defaults) {
  Settings s;
  s.language = "vi";
  s.uiScale = 1.5f;
  s.recentFiles = {"F:/scenes/phòng/phong.ply", "C:\\a b\\\"q\".gsvproj"};
  s.lastDirectory = "F:/scenes";
  s.firstRunDone = true;
  s.autosaveSeconds = 60;
  s.shDegree = 2;
  s.vsync = false;
  s.background[1] = 0.5f;
  s.windowWidth = 1920;
  s.windowHeight = 1080;
  s.maximized = true;
  s.autoQuality = false;
  setFlipY(s, "F:/a.ply", true);
  setFlipY(s, "F:/b.ply", true);
  setFlipY(s, "F:/a.ply", false);
  Settings r;
  std::string err;
  REQUIRE(settingsFromJson(settingsToJson(s), r, err));
  CHECK(r.language == "vi" && r.uiScale == 1.5f && r.recentFiles == s.recentFiles && r.lastDirectory == s.lastDirectory);
  CHECK(r.firstRunDone && r.autosaveSeconds == 60 && r.shDegree == 2 && !r.vsync && r.background[1] == 0.5f);
  CHECK(r.windowWidth == 1920 && r.windowHeight == 1080 && r.maximized && !r.autoQuality);
  CHECK(isFlipY(r, "F:/b.ply") && !isFlipY(r, "F:/a.ply") && r.flipYFiles.size() == 1);

  // Damaged or hostile files: never fail to start, defaults where values are bad.
  CHECK(!settingsFromJson("{ not json", r, err) && r.language == "auto" && r.shDegree == 3);
  REQUIRE(settingsFromJson("{\"language\": \"klingon\", \"uiScale\": 99, \"shDegree\": -4, \"window\": [10, 10],"
                           " \"background\": [2, 0, 0], \"autosaveSeconds\": 1e9, \"future\": {\"x\": 1}}",
                           r, err));
  CHECK(r.language == "auto" && r.uiScale == 0.0f && r.shDegree == 3 && r.windowWidth == 1600);
  CHECK(r.background[0] == Settings{}.background[0] && r.autosaveSeconds == 120);
  REQUIRE(settingsFromJson("{\"uiScale\": 0.3}", r, err));
  CHECK(r.uiScale == 0.75f);  // clamped up, not ignored
  std::string deep;
  for (int k = 0; k < 1000; ++k) deep += "[";
  CHECK(!settingsFromJson(deep, r, err));
}

TEST_CASE(ux_settings_file_and_recent_list) {
  const std::string dir = tempDir("gsv_settings_test");
#ifdef _WIN32
  _putenv_s("GSV_SETTINGS_DIR", dir.c_str());
#else
  setenv("GSV_SETTINGS_DIR", dir.c_str(), 1);
#endif
  CHECK(settingsDirectory() == dir);
  Settings s, r;
  std::string err;
  CHECK(loadSettings(settingsPath(), r, err) && r.recentFiles.empty());  // first start: defaults
  for (int k = 0; k < 20; ++k) addRecentFile(s, "scene" + std::to_string(k) + ".ply");
  addRecentFile(s, "scene5.ply");  // again: moves to the front, no duplicate
  CHECK(s.recentFiles.size() == kMaxRecentFiles && s.recentFiles[0] == "scene5.ply" && s.recentFiles[1] == "scene19.ply");
  int count5 = 0;
  for (const std::string& f : s.recentFiles) count5 += f == "scene5.ply";
  CHECK(count5 == 1);
  removeRecentFile(s, "scene19.ply");
  CHECK(s.recentFiles[1] == "scene18.ply");
  REQUIRE(saveSettings(settingsPath(), s, err));
  CHECK(!std::filesystem::exists(pathFromUtf8(settingsPath() + ".tmp")));  // atomic: temp file renamed
  CHECK(loadSettings(settingsPath(), r, err) && r.recentFiles == s.recentFiles);
  // A truncated file (crash while another program wrote it) still starts with defaults.
  {
    std::ofstream f(pathFromUtf8(settingsPath()), std::ios::binary);
    f << "{\"language\": \"vi\", \"recentFi";
  }
  CHECK(!loadSettings(settingsPath(), r, err) && r.language == "auto");
#ifdef _WIN32
  CHECK(samePath("C:\\Scenes\\A.PLY", "c:/scenes/a.ply"));
#else
  CHECK(samePath("/a/b.ply", "/a/b.ply") && !samePath("/a/B.ply", "/a/b.ply"));
#endif
#ifdef _WIN32
  _putenv_s("GSV_SETTINGS_DIR", "");
#else
  unsetenv("GSV_SETTINGS_DIR");
#endif
  std::filesystem::remove_all(pathFromUtf8(dir));
}

TEST_CASE(ux_i18n_formats_match) {
  size_t n = 0;
  const auto* table = translationTable(n);
  REQUIRE(n > 250);
  std::set<std::string> keys;
  size_t bad = 0;
  for (size_t i = 0; i < n; ++i) {
    const char* en = table[i].first;
    const char* vi = table[i].second;
    if (!keys.insert(en).second) {
      std::printf("    duplicate key: %s\n", en);
      ++bad;
    }
    if (!vi[0] || specifiers(en) != specifiers(vi)) {
      std::printf("    format mismatch: \"%s\" -> \"%s\"\n", en, vi);
      ++bad;
    }
  }
  std::printf("    %zu Vietnamese strings\n", n);
  CHECK(bad == 0);
}

TEST_CASE(ux_i18n_glyphs_in_font_ranges) {
  // Must match ImFontAtlas::GetGlyphRangesVietnamese() (the font loads only these glyphs).
  static const unsigned kRanges[][2] = {{0x0020, 0x00FF}, {0x0102, 0x0103}, {0x0110, 0x0111}, {0x0128, 0x0129},
                                        {0x0168, 0x0169}, {0x01A0, 0x01A1}, {0x01AF, 0x01B0}, {0x1EA0, 0x1EF9}};
  size_t n = 0;
  const auto* table = translationTable(n);
  size_t bad = 0;
  for (size_t i = 0; i < n; ++i) {
    std::vector<unsigned> cps;
    if (!decodeUtf8(table[i].second, cps)) {
      std::printf("    bad UTF-8: %s\n", table[i].first);
      ++bad;
      continue;
    }
    for (unsigned cp : cps) {
      if (cp == '\n') continue;
      bool ok = false;
      for (const auto& r : kRanges) ok = ok || (cp >= r[0] && cp <= r[1]);
      if (!ok) {
        std::printf("    U+%04X not in the font ranges: \"%s\"\n", cp, table[i].second);
        ++bad;
      }
    }
  }
  CHECK(bad == 0);
}

TEST_CASE(ux_i18n_lookup_and_ids) {
  setLanguage(Language::English);
  CHECK(std::strcmp(tr("Delete selection"), "Delete selection") == 0);
  CHECK(std::strcmp(trId("Delete selection"), "Delete selection") == 0);
  setLanguage(Language::Vietnamese);
  CHECK(std::strcmp(tr("Delete selection"), "Xoá vùng chọn") == 0);
  const char* id = trId("Delete selection");
  CHECK(std::strcmp(id, "Xoá vùng chọn###Delete selection") == 0);  // stable ImGui ID across languages
  CHECK(trId("Delete selection") == id);                              // cached, pointer stays valid
  CHECK(std::strcmp(tr("no such key"), "no such key") == 0);          // fallback to English
  CHECK(std::strcmp(trId("no such key"), "no such key") == 0);
  CHECK(languageFromSetting("vi") == Language::Vietnamese && languageFromSetting("en") == Language::English);
  setLanguage(Language::English);
}
