#include "core/i18n.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gsv {

namespace {

const std::pair<const char*, const char*> kVietnamese[] = {
#include "core/i18n_vi.inc"
};

Language gLanguage = Language::English;

struct Tables {
  std::unordered_map<std::string, const char*> vi;     // english -> vietnamese
  std::unordered_map<std::string, std::string> ids;    // english -> "vietnamese###english"
};

Tables& tables() {
  static Tables t = [] {
    Tables r;
    for (const auto& kv : kVietnamese) r.vi.emplace(kv.first, kv.second);
    return r;
  }();
  return t;
}
std::mutex gIdMutex;  // trId may be called from the UI thread only, but stay safe

}  // namespace

void setLanguage(Language lang) { gLanguage = lang; }
Language currentLanguage() { return gLanguage; }

Language systemLanguage() {
#ifdef _WIN32
  const LANGID id = GetUserDefaultUILanguage();
  return PRIMARYLANGID(id) == LANG_VIETNAMESE ? Language::Vietnamese : Language::English;
#else
  for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* v = std::getenv(var);
    if (v && *v) return std::strncmp(v, "vi", 2) == 0 ? Language::Vietnamese : Language::English;
  }
  return Language::English;
#endif
}

Language languageFromSetting(const char* s) {
  if (s && std::strcmp(s, "vi") == 0) return Language::Vietnamese;
  if (s && std::strcmp(s, "en") == 0) return Language::English;
  return systemLanguage();
}

const char* tr(const char* english) {
  if (gLanguage == Language::English || !english) return english;
  const auto& vi = tables().vi;
  const auto it = vi.find(english);
  return it == vi.end() ? english : it->second;
}

const char* trId(const char* english) {
  if (gLanguage == Language::English || !english) return english;
  const char* t = tr(english);
  if (t == english) return english;  // untranslated: the label is its own ID already
  std::lock_guard<std::mutex> lock(gIdMutex);
  auto& ids = tables().ids;
  auto it = ids.find(english);
  if (it == ids.end()) it = ids.emplace(english, std::string(t) + "###" + english).first;
  return it->second.c_str();  // node-based map: stays valid when the map grows
}

const std::pair<const char*, const char*>* translationTable(size_t& count) {
  count = sizeof(kVietnamese) / sizeof(kVietnamese[0]);
  return kVietnamese;
}

}  // namespace gsv
