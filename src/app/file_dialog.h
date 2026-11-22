#pragma once
// Native Open / Save dialogs (Windows: comdlg32). Other platforms return false and the caller shows
// its own path input. Paths are UTF-8. The dialog blocks the calling thread (modal), like any app.

#include <string>
#include <vector>

namespace gsv {

struct FileFilter {
  std::string name;      // "3DGS scenes"
  std::string patterns;  // "*.ply;*.splat;*.spz"
};

bool nativeDialogsAvailable();
// Returns true and fills `path` when the user picked a file.
bool openFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startDir,
                    std::string& path);
bool saveFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startPath,
                    const std::string& defaultExtension, std::string& path);

}  // namespace gsv
