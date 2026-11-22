#include "app/file_dialog.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include "core/utf8_path.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#endif

namespace gsv {

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) { return pathFromUtf8(s).wstring(); }

// "Name\0*.a;*.b\0Name2\0*.c\0\0"
std::wstring filterString(const std::vector<FileFilter>& filters) {
  std::wstring f;
  for (const FileFilter& x : filters) {
    f += widen(x.name + " (" + x.patterns + ")");
    f.push_back(L'\0');
    f += widen(x.patterns);
    f.push_back(L'\0');
  }
  f.push_back(L'\0');
  return f;
}

bool run(bool save, const std::string& title, const std::vector<FileFilter>& filters, const std::string& start,
         const std::string& defExt, std::string& path) {
  std::wstring buffer(32768, L'\0');
  std::wstring dir;
  if (save && !start.empty()) {
    const std::wstring w = widen(start);
    std::copy(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(w.size(), buffer.size() - 1)), buffer.begin());
  } else if (!start.empty()) {
    dir = widen(start);
  }
  const std::wstring filter = filterString(filters), wtitle = widen(title), ext = widen(defExt);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = GetActiveWindow();
  ofn.lpstrFilter = filter.c_str();
  ofn.lpstrFile = buffer.data();
  ofn.nMaxFile = static_cast<DWORD>(buffer.size());
  ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
  ofn.lpstrTitle = wtitle.c_str();
  ofn.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
  ofn.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
  const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
  if (!ok) return false;
  buffer.resize(wcslen(buffer.c_str()));
  path = pathToUtf8(std::filesystem::path(buffer));
  return !path.empty();
}
}  // namespace

bool nativeDialogsAvailable() { return true; }
bool openFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startDir,
                    std::string& path) {
  return run(false, title, filters, startDir, "", path);
}
bool saveFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startPath,
                    const std::string& defaultExtension, std::string& path) {
  return run(true, title, filters, startPath, defaultExtension, path);
}
#else
bool nativeDialogsAvailable() { return false; }
bool openFileDialog(const std::string&, const std::vector<FileFilter>&, const std::string&, std::string&) { return false; }
bool saveFileDialog(const std::string&, const std::vector<FileFilter>&, const std::string&, const std::string&,
                    std::string&) {
  return false;
}
#endif

}  // namespace gsv
