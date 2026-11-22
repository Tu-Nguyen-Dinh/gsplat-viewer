#pragma once
// User-interface translation (Phase 6, docs/core/CORE6-ux.md). The English text IS the key:
//   tr("Delete")        -> "Xoá" in Vietnamese, "Delete" in English (plain text, tooltips, formats)
//   trId("Delete")      -> "Xoá###Delete": for ImGui widget labels. The part after ### is the widget
//                          ID, so switching the language at runtime keeps open headers, focus, etc.
// Missing translations fall back to English. Format strings keep their printf specifiers (checked
// by the test i18n_formats_match).

#include <cstddef>
#include <utility>

namespace gsv {

enum class Language { English = 0, Vietnamese = 1 };

void setLanguage(Language lang);
Language currentLanguage();
Language systemLanguage();  // Windows UI language / LANG starting with "vi"
Language languageFromSetting(const char* setting);  // "en", "vi", "auto"

const char* tr(const char* english);
const char* trId(const char* english);

// The Vietnamese table, for tests and tools: {english, vietnamese} pairs.
const std::pair<const char*, const char*>* translationTable(size_t& count);

}  // namespace gsv
