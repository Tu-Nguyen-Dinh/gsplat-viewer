// Application shell (Phase 6, docs/core/CORE6-ux.md): what makes the viewer usable without reading
// the documentation.
//   - menu bar (File / Edit / View / Help) with every action and its shortcut
//   - status bar: last message, tool hint, selection, splat count, FPS, unit, unsaved state
//   - welcome screen: open buttons, demo scene, recent files with thumbnails, language
//   - shortcut sheet (F1) and a 5-step first-run tour
//   - settings that survive a restart (core/settings.h), Vietnamese / English (core/i18n.h),
//     a Unicode font with Vietnamese glyphs, sizes scaled to the monitor's DPI
#include "app/app.h"
#include "app/file_dialog.h"
#include "core/i18n.h"
#include "core/utf8_path.h"
#include "gl/gl_loader.h"
#include "gl/framebuffer.h"
#include "scene/demo_scene.h"
#include "scene/ply_io.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace gsv {

namespace {

std::string lowerExtension(const std::string& path) {
  std::string ext = pathToUtf8(pathFromUtf8(path).extension());
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

std::string fileName(const std::string& path) { return pathToUtf8(pathFromUtf8(path).filename()); }

std::string parentDir(const std::string& path) { return pathToUtf8(pathFromUtf8(path).parent_path()); }

// FNV-1a of the path: name of the thumbnail file.
std::string thumbName(const std::string& path) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : path) {
    h ^= (c == '\\' ? '/' : static_cast<unsigned char>(std::tolower(c)));
    h *= 1099511628211ull;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx.thumb", static_cast<unsigned long long>(h));
  return buf;
}

std::string thumbPath(const std::string& scenePath) {
  const std::filesystem::path dir = pathFromUtf8(settingsDirectory()) / "thumbs";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return pathToUtf8(dir / thumbName(scenePath));
}

const std::vector<FileFilter>& sceneFilters() {
  static std::vector<FileFilter> f;
  f = {{tr("Scenes and projects"), "*.ply;*.splat;*.spz;*.gsvlod;*.gsvproj;*.las"},
       {tr("3DGS scenes"), "*.ply;*.splat;*.spz;*.gsvlod"},
       {tr("Projects"), "*.gsvproj"},
       {tr("LiDAR point clouds"), "*.las"},
       {tr("All files"), "*.*"}};
  return f;
}

struct Shortcut {
  const char* keys;
  const char* action;
};
constexpr Shortcut kShortcuts[] = {
    {"Left drag", "Orbit (Navigate tool)"},
    {"Right drag", "Orbit (other tools) / pan"},
    {"Middle drag", "Pan"},
    {"Wheel", "Zoom"},
    {"W A S D  Q E", "Move the camera (Shift = faster)"},
    {"F", "Fly mode on / off"},
    {"R", "Frame the scene"},
    {"Shift+R", "Frame the selection"},
    {"Num 1 / 3 / 7", "Front / right / top view (Ctrl: opposite side)"},
    {"1 ... 9", "Tools: Navigate, Rect, Lasso, Brush, Sphere, Flood, Transform, Measure, Note"},
    {"M", "Measure tool"},
    {"Esc", "Cancel / back to Navigate"},
    {"Shift / Alt + select", "Add to / subtract from the selection"},
    {"Ctrl+A / Ctrl+D / Ctrl+I", "Select all / none / invert"},
    {"Del", "Delete the selection"},
    {"H / Alt+H", "Hide the selection / show everything"},
    {"I", "Isolate the selection"},
    {"[  ]", "Brush size"},
    {"V", "Visible only on / off (Rect, Lasso, Brush)"},
    {"=  /  -", "Grow / shrink the selection by one layer"},
    {"Enter / Backspace", "Finish / undo the last measurement point"},
    {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
    {"Ctrl+O", "Open a file"},
    {"Ctrl+S", "Save the project"},
    {"Tab", "Splats / points"},
    {"C / Shift+C", "Next / previous training camera"},
    {"F1", "This list"},
    {"F5", "Reload shaders (developers)"},
};

}  // namespace

// ------------------------------------------------------------------ settings, fonts
void App::loadUserSettings() {
  std::string error;
  if (!loadSettings(settingsPath(), settings_, error)) {
    std::fprintf(stderr, "Settings ignored (%s): using the defaults\n", error.c_str());
    settings_ = Settings{};
  }
  setLanguage(languageFromSetting(settings_.language.c_str()));
}

void App::saveUserSettings() {
  if (!interactive_) return;  // screenshots / benchmarks never touch the user's settings
  if (window_) {
    settings_.maximized = glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
    if (!settings_.maximized) glfwGetWindowSize(window_, &settings_.windowWidth, &settings_.windowHeight);
  }
  settings_.vsync = vsync_;
  settings_.shDegree = splatSettings_.shDegree;
  settings_.background[0] = background_.r;
  settings_.background[1] = background_.g;
  settings_.background[2] = background_.b;
  std::string error;
  if (!saveSettings(settingsPath(), settings_, error)) std::fprintf(stderr, "Cannot save settings: %s\n", error.c_str());
}

float App::effectiveUiScale() const {
  if (settings_.uiScale > 0.0f) return settings_.uiScale;
  float sx = 1.0f, sy = 1.0f;
  if (window_) glfwGetWindowContentScale(window_, &sx, &sy);
  return std::clamp(std::max(sx, sy), 1.0f, 3.0f);
}

void App::setupFonts() {
  const float scale = effectiveUiScale();
  fontsDirty_ = false;
  if (std::abs(scale - appliedUiScale_) < 1e-3f && ImGui::GetIO().Fonts->Fonts.Size > 0) return;
  ImGuiIO& io = ImGui::GetIO();
  io.Fonts->Clear();
  // A system font with Vietnamese glyphs (the built-in ImGui font is ASCII only).
  static const char* kCandidates[] = {
#ifdef _WIN32
      "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf", "C:\\Windows\\Fonts\\tahoma.ttf",
#else
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
  };
  ImFont* font = nullptr;
  for (const char* path : kCandidates) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) continue;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    font = io.Fonts->AddFontFromFileTTF(path, std::round(16.0f * scale), &cfg, io.Fonts->GetGlyphRangesVietnamese());
    if (font) break;
  }
  if (!font) {  // no system font: ASCII font, and stay in English (Vietnamese would show '?')
    ImFontConfig cfg;
    cfg.SizePixels = std::round(13.0f * scale);
    io.Fonts->AddFontDefault(&cfg);
    setLanguage(Language::English);
  }
  ImGui_ImplOpenGL3_DestroyFontsTexture();
  ImGui_ImplOpenGL3_CreateFontsTexture();
  ImGuiStyle style;
  ImGui::StyleColorsDark(&style);
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.GrabRounding = 4.0f;
  style.ScaleAllSizes(scale);
  ImGui::GetStyle() = style;
  appliedUiScale_ = scale;
}

// ------------------------------------------------------------------ files
void App::rememberRecent(const std::string& path) {
  if (path.empty()) return;
  std::error_code ec;
  const std::string abs = pathToUtf8(std::filesystem::absolute(pathFromUtf8(path), ec));
  addRecentFile(settings_, ec ? path : abs);
  settings_.lastDirectory = parentDir(ec ? path : abs);
  saveUserSettings();
}

void App::openAnyFile(const std::string& path) {
  if (path.empty()) return;
  const bool replacesWork = lowerExtension(path) != ".las" && lowerExtension(path) != ".laz";
  if (replacesWork && !forceOpen_ && (sceneDirty() || projectDirty())) {
    requestDiscard([this, path] {  // "unsaved changes" prompt first
      forceOpen_ = true;
      openAnyFile(path);
      forceOpen_ = false;
    });
    return;
  }
  std::error_code ec;
  if (!std::filesystem::exists(pathFromUtf8(path), ec)) {
    status_ = std::string(tr("File not found: ")) + path;
    removeRecentFile(settings_, path);
    saveUserSettings();
    return;
  }
  const std::string ext = lowerExtension(path);
  if (ext == ".gsvproj") {
    openProject(path);
    rememberRecent(path);
  } else if (ext == ".las" || ext == ".laz") {
    options_.las = path;
    loadCloudFile(path);
    rememberRecent(path);
  } else {
    if (isLoading()) cancelLoading();  // the newest request wins
    loadSceneFile(path);  // rememberRecent when the load succeeded (pollLoading)
  }
  showWelcome_ = false;
}

void App::showOpenDialog(int kind) {
  std::string path;
  const std::string title = kind == 1 ? tr("Open a project") : tr("Open a scene");
  std::vector<FileFilter> filters = sceneFilters();
  if (kind == 1) filters = {filters[2], filters[4]};
  if (nativeDialogsAvailable()) {
    if (openFileDialog(title, filters, settings_.lastDirectory, path)) openAnyFile(path);
  } else {
    pathPromptKind_ = kind == 1 ? 1 : 0;
    std::snprintf(pathPrompt_, sizeof(pathPrompt_), "%s", settings_.lastDirectory.c_str());
  }
}

void App::showSaveProjectDialog() {
  std::string path;
  const std::string start = projectPath_[0] ? std::string(projectPath_) : defaultProjectPath();
  if (nativeDialogsAvailable()) {
    if (saveFileDialog(tr("Save the project"), {{tr("Projects"), "*.gsvproj"}}, start, "gsvproj", path)) {
      std::snprintf(projectPath_, sizeof(projectPath_), "%s", path.c_str());
      saveProjectTo(path);
      rememberRecent(path);
    }
  } else {
    pathPromptKind_ = 2;
    std::snprintf(pathPrompt_, sizeof(pathPrompt_), "%s", start.c_str());
  }
}

void App::showSaveEditedDialog() {
  std::string start = savePath_;
  if (start.empty() && !scenePath_.empty()) {
    const std::filesystem::path p = pathFromUtf8(scenePath_);
    start = pathToUtf8(p.parent_path() / (pathToUtf8(p.stem()) + "_edited.ply"));
  }
  auto save = [&](const std::string& path) { startSceneSave(path); };  // background thread
  std::string path;
  if (nativeDialogsAvailable()) {
    if (saveFileDialog(tr("Save the edited scene"), {{tr("3DGS scenes"), "*.ply"}}, start, "ply", path)) save(path);
  } else {
    pathPromptKind_ = 3;
    std::snprintf(pathPrompt_, sizeof(pathPrompt_), "%s", start.c_str());
  }
}

void App::drawPathPrompt() {
  if (pathPromptKind_ < 0) return;
  static const char* kTitles[] = {"Open a file", "Open a project", "Save the project", "Save the edited scene"};
  ImGui::OpenPopup("##pathprompt");
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("##pathprompt", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(tr(kTitles[pathPromptKind_]));
    ImGui::SetNextItemWidth(560.0f * appliedUiScale_);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##p", pathPrompt_, sizeof(pathPrompt_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextDisabled("%s", tr("Type or paste the full path of the file."));
    if (ImGui::Button(trId("OK")) || enter) {
      const std::string p = pathPrompt_;
      const int kind = pathPromptKind_;
      pathPromptKind_ = -1;
      ImGui::CloseCurrentPopup();
      if (kind <= 1) openAnyFile(p);
      else if (kind == 2) {
        std::snprintf(projectPath_, sizeof(projectPath_), "%s", p.c_str());
        saveProjectTo(p);
        rememberRecent(p);
      } else {
        startSceneSave(p);
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(trId("Cancel"))) {
      pathPromptKind_ = -1;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ thumbnails
void App::captureThumbnail() {
  if (!thumbnailPending_ || scenePath_.empty() || scene_.empty()) return;
  if (--thumbnailDelay_ > 0) return;
  thumbnailPending_ = false;
  constexpr int kW = 320, kH = 180;
  Framebuffer target;
  if (!target.create(kW, kH, targetFormat())) return;
  frame(target.id(), kW, kH);
  const Image img = readFramebuffer(target.id(), kW, kH);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (img.rgb.size() != static_cast<size_t>(kW) * kH * 3) return;
  // Tiny raw format: "GSVT", width, height (uint32 LE), RGB rows top to bottom.
  std::string bytes = "GSVT";
  const uint32_t dims[2] = {kW, kH};
  bytes.append(reinterpret_cast<const char*>(dims), sizeof(dims));
  bytes.append(reinterpret_cast<const char*>(img.rgb.data()), img.rgb.size());
  std::string error;
  writeFileAtomic(thumbPath(scenePath_), bytes, error);
  // The welcome screen shows the new image next time.
  std::error_code ec;
  const std::string abs = pathToUtf8(std::filesystem::absolute(pathFromUtf8(scenePath_), ec));
  for (const std::string& key : {scenePath_, abs}) {
    auto it = thumbTextures_.find(key);
    if (it != thumbTextures_.end()) {
      GLuint tex = it->second;
      if (tex) glDeleteTextures(1, &tex);
      thumbTextures_.erase(it);
    }
  }
}

unsigned int App::thumbnailTexture(const std::string& path) {
  auto it = thumbTextures_.find(path);
  if (it != thumbTextures_.end()) return it->second;
  GLuint tex = 0;
  std::ifstream f(pathFromUtf8(thumbPath(path)), std::ios::binary);
  char magic[4];
  uint32_t dims[2] = {0, 0};
  if (f && f.read(magic, 4) && std::memcmp(magic, "GSVT", 4) == 0 && f.read(reinterpret_cast<char*>(dims), sizeof(dims)) &&
      dims[0] > 0 && dims[0] <= 1024 && dims[1] > 0 && dims[1] <= 1024) {
    std::vector<uint8_t> rgb(static_cast<size_t>(dims[0]) * dims[1] * 3);
    if (f.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()))) {
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, static_cast<GLsizei>(dims[0]), static_cast<GLsizei>(dims[1]), 0, GL_RGB,
                   GL_UNSIGNED_BYTE, rgb.data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
  }
  thumbTextures_[path] = tex;  // 0 = no thumbnail (cached too)
  return tex;
}

// ------------------------------------------------------------------ menu bar
void App::drawMenuBar() {
  // Window title: file name, * when there is something to save.
  {
    std::string title = "gsplat-viewer";
    if (!scenePath_.empty()) title = fileName(scenePath_) + " - " + title;
    if (sceneDirty() || projectDirty()) title = "* " + title;
    if (title != windowTitle_) {
      windowTitle_ = title;
      glfwSetWindowTitle(window_, title.c_str());
    }
  }
  if (!ImGui::BeginMainMenuBar()) return;
  menuBarHeight_ = ImGui::GetWindowSize().y;
  if (ImGui::BeginMenu(trId("File"))) {
    if (ImGui::MenuItem(trId("Open..."), "Ctrl+O")) showOpenDialog(0);
    if (ImGui::MenuItem(trId("Open project..."))) showOpenDialog(1);
    if (ImGui::BeginMenu(trId("Open recent"), !settings_.recentFiles.empty())) {
      std::string open;
      for (const std::string& f : settings_.recentFiles) {
        if (ImGui::MenuItem(fileName(f).c_str())) open = f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.c_str());
      }
      ImGui::Separator();
      if (ImGui::MenuItem(trId("Clear the list"))) {
        settings_.recentFiles.clear();
        saveUserSettings();
      }
      ImGui::EndMenu();
      if (!open.empty()) openAnyFile(open);
    }
    if (ImGui::MenuItem(trId("Demo scene"))) {
      requestDiscard([this] {
        loadColmapFor("", "");
        setScene(makeDemoScene());
      });
      showWelcome_ = false;
      status_ = tr("Demo scene");
    }
    ImGui::Separator();
    if (ImGui::MenuItem(trId("Save project"), "Ctrl+S")) {
      if (projectPath_[0]) saveProjectTo(projectPath_);
      else showSaveProjectDialog();
    }
    if (ImGui::MenuItem(trId("Save project as..."))) showSaveProjectDialog();
    if (ImGui::MenuItem(trId("Save edited scene as PLY..."), nullptr, false, !scene_.empty())) showSaveEditedDialog();
    drawExportMenu();  // export_ui.cpp: picture, video, report
    ImGui::Separator();
    if (ImGui::MenuItem(trId("Welcome screen"))) showWelcome_ = true;
    if (ImGui::MenuItem(trId("Exit"), "Alt+F4")) requestDiscard([this] { glfwSetWindowShouldClose(window_, GLFW_TRUE); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(trId("Edit"))) {
    const std::string undo = history_.canUndo() ? std::string(tr("Undo")) + " " + history_.undoLabel() : tr("Undo");
    const std::string redo = history_.canRedo() ? std::string(tr("Redo")) + " " + history_.redoLabel() : tr("Redo");
    if (ImGui::MenuItem((undo + "###undo").c_str(), "Ctrl+Z", false, history_.canUndo())) undoEdit();
    if (ImGui::MenuItem((redo + "###redo").c_str(), "Ctrl+Y", false, history_.canRedo())) redoEdit();
    ImGui::Separator();
    if (ImGui::MenuItem(trId("Select all"), "Ctrl+A")) setSelection(selectAll(scene_), SelectOp::Replace, tr("Select all"));
    if (ImGui::MenuItem(trId("Select none"), "Ctrl+D")) setSelection(SplatMask(scene_.size(), 0), SelectOp::Replace, tr("Select none"));
    if (ImGui::MenuItem(trId("Invert selection"), "Ctrl+I")) {
      SplatMask inv = selected_;
      invert(inv);
      setSelection(inv, SelectOp::Replace, tr("Invert selection"));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(trId("Delete selection"), "Delete", false, selectedCount_ > 0)) deleteSelection();
    if (ImGui::MenuItem(trId("Hide selection"), "H", false, selectedCount_ > 0)) hideSelection();
    if (ImGui::MenuItem(trId("Isolate selection"), "I", false, selectedCount_ > 0)) isolateSelection();
    if (ImGui::MenuItem(trId("Show everything"), "Alt+H", false, hiddenCount_ > 0)) unhideAll();
    ImGui::Separator();
    if (ImGui::MenuItem(trId("Find floaters"), nullptr, false, !scene_.empty() && !cleanJob_.valid())) autoCleanFloaters();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(trId("View"))) {
    if (ImGui::MenuItem(trId("Frame the scene"), "R")) {
      const OrbitCamera before = camera_;
      frameScene();
      const OrbitCamera end = camera_;
      camera_ = before;
      animateCameraTo(end);
    }
    if (ImGui::MenuItem(trId("Frame the selection"), "Shift+R", false, selectedCount_ > 0)) frameSelection();
    if (ImGui::BeginMenu(trId("Quick views"))) {
      if (ImGui::MenuItem(trId("Front view"), "Num 1")) quickView(2, false);
      if (ImGui::MenuItem(trId("Back view"), "Ctrl+Num 1")) quickView(2, true);
      if (ImGui::MenuItem(trId("Right view"), "Num 3")) quickView(0, false);
      if (ImGui::MenuItem(trId("Left view"), "Ctrl+Num 3")) quickView(0, true);
      if (ImGui::MenuItem(trId("Top view"), "Num 7")) quickView(1, false);
      if (ImGui::MenuItem(trId("Bottom view"), "Ctrl+Num 7")) quickView(1, true);
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(trId("Show the compass"), nullptr, showCompass_)) showCompass_ = !showCompass_;
    if (ImGui::MenuItem(trId("Fly mode"), "F", flyMode_)) flyMode_ = !flyMode_;
    if (ImGui::MenuItem(trId("Show points instead of splats"), "Tab", mode_ == RenderMode::Points))
      mode_ = mode_ == RenderMode::Splats ? RenderMode::Points : RenderMode::Splats;
    if (ImGui::MenuItem(trId("Show the panel"), nullptr, showPanel_)) showPanel_ = !showPanel_;
    ImGui::Separator();
    if (ImGui::BeginMenu(trId("Language"))) {
      auto item = [&](const char* label, const char* value) {
        if (ImGui::MenuItem(label, nullptr, settings_.language == value)) {
          settings_.language = value;
          setLanguage(languageFromSetting(value));
          saveUserSettings();
        }
      };
      item("English", "en");
      item("Tiếng Việt", "vi");
      item(tr("System language"), "auto");
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(trId("Interface size"))) {
      const float sizes[] = {0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
      for (float s : sizes) {
        char label[32];
        if (s == 0.0f) std::snprintf(label, sizeof(label), "%s", tr("Automatic (DPI)"));
        else std::snprintf(label, sizeof(label), "%.0f%%", s * 100.0f);
        if (ImGui::MenuItem(label, nullptr, settings_.uiScale == s)) {
          settings_.uiScale = s;
          fontsDirty_ = true;
          saveUserSettings();
        }
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(trId("Faster while moving (auto quality)"), nullptr, settings_.autoQuality)) {
      settings_.autoQuality = !settings_.autoQuality;
      saveUserSettings();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(trId("Help"))) {
    if (ImGui::MenuItem(trId("Keyboard shortcuts"), "F1")) showShortcuts_ = true;
    if (ImGui::MenuItem(trId("Show the tour again"))) tourStep_ = 0;
    if (ImGui::MenuItem(trId("About"))) showAbout_ = true;
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();

  if (showAbout_) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin(trId("About"), &showAbout_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
      ImGui::Text("gsplat-viewer %s", GSV_VERSION);
      ImGui::TextUnformatted(tr("3D Gaussian Splatting viewer and editor"));
      ImGui::TextDisabled("%s", glRenderer_.c_str());
      ImGui::TextDisabled("%s: %s", tr("Settings"), settingsPath().c_str());
    }
    ImGui::End();
  }
}

// ------------------------------------------------------------------ status bar
void App::drawStatusBar() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y;
  statusBarHeight_ = h;
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * appliedUiScale_, 3.0f * appliedUiScale_));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (ImGui::Begin("##statusbar", nullptr, flags)) {
    // Right part first (fixed), message on the left takes what is left.
    char right[256];
    const ImGuiIO& io = ImGui::GetIO();
    std::snprintf(right, sizeof(right), tr("%zu selected  |  %zu splats  |  %.0f FPS"), selectedCount_, scene_.size(),
                  io.Framerate);
    std::string r = right;
    if (project_.unitScale != 1.0 || project_.unit != "units") {
      char u[64];
      std::snprintf(u, sizeof(u), "  |  1 = %.4g %s", project_.unitScale, project_.unit.c_str());
      r += u;
    }
    if (isLoading()) r = std::string(tr("Loading...")) + "  |  " + r;
    if (saveJob_.valid()) r = std::string(tr("Saving...")) + "  |  " + r;
    if (sceneDirty() || projectDirty()) r = std::string(tr("Unsaved changes")) + "  |  " + r;
    const float rw = ImGui::CalcTextSize(r.c_str()).x;
    std::string msg = status_;
    if (msg.empty())
      msg = tool_ == Tool::Navigate ? tr("Ready: drag to orbit, wheel to zoom, F1 for all shortcuts")
                                    : tr("Hover a toolbar button to see what the tool does (F1: all shortcuts)");
    ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                        ImVec2(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - rw - 20.0f, vp->Pos.y + vp->Size.y), true);
    ImGui::TextUnformatted(msg.c_str());
    ImGui::PopClipRect();
    ImGui::SameLine(ImGui::GetWindowWidth() - rw - 10.0f * appliedUiScale_);
    ImGui::TextDisabled("%s", r.c_str());
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

// ------------------------------------------------------------------ welcome screen
void App::drawWelcome() {
  if (!showWelcome_) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float s = appliedUiScale_;
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(760.0f * s, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.97f);
  if (!ImGui::Begin(trId("Welcome"), &showWelcome_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                                        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    return;
  }
  ImGui::SetWindowFontScale(1.35f);
  ImGui::TextUnformatted("gsplat-viewer");
  ImGui::SetWindowFontScale(1.0f);
  ImGui::TextDisabled("%s", tr("Open a 3D Gaussian Splatting scene to view, measure and edit it."));
  ImGui::Spacing();
  const ImVec2 big(230.0f * s, 44.0f * s);
  if (ImGui::Button(trId("Open a scene..."), big)) showOpenDialog(0);
  ImGui::SameLine();
  if (ImGui::Button(trId("Open a project..."), big)) showOpenDialog(1);
  ImGui::SameLine();
  if (ImGui::Button(trId("Try the demo scene"), big)) {
    requestDiscard([this] {
      requestDiscard([this] {
        loadColmapFor("", "");
        setScene(makeDemoScene());
      });
    });
    showWelcome_ = false;
    status_ = tr("Demo scene");
  }
  ImGui::TextDisabled("%s", tr("You can also drag a .ply / .splat / .spz / .gsvproj / .las file onto the window."));

  ImGui::SeparatorText(tr("Recent files"));
  if (settings_.recentFiles.empty()) {
    ImGui::TextDisabled("%s", tr("Nothing yet: the files you open appear here."));
  } else {
    const float tw = 168.0f * s, th = tw * 9.0f / 16.0f;
    std::string open, remove;
    int col = 0;
    for (const std::string& f : settings_.recentFiles) {
      ImGui::PushID(f.c_str());
      ImGui::BeginGroup();
      const unsigned int tex = thumbnailTexture(f);
      bool clicked;
      if (tex) {
        clicked = ImGui::ImageButton("##thumb", static_cast<ImTextureID>(static_cast<intptr_t>(tex)), ImVec2(tw, th));
      } else {
        const std::string ext = lowerExtension(f);
        clicked = ImGui::Button(ext.empty() ? "?" : ext.c_str(), ImVec2(tw + 2 * ImGui::GetStyle().FramePadding.x,
                                                                         th + 2 * ImGui::GetStyle().FramePadding.y));
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.c_str());
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem(trId("Remove from the list"))) remove = f;
        ImGui::EndPopup();
      }
      std::string name = fileName(f);
      while (name.size() > 4 && ImGui::CalcTextSize(name.c_str()).x > tw) name = name.substr(0, name.size() - 4) + "...";
      ImGui::TextUnformatted(name.c_str());
      ImGui::EndGroup();
      ImGui::PopID();
      if (clicked) open = f;
      if (++col % 4 != 0) ImGui::SameLine();
    }
    if (col % 4 != 0) ImGui::NewLine();
    if (!remove.empty()) {
      removeRecentFile(settings_, remove);
      saveUserSettings();
    }
    if (!open.empty()) openAnyFile(open);
  }
  ImGui::Separator();
  ImGui::TextUnformatted(tr("Language"));
  ImGui::SameLine();
  if (ImGui::RadioButton("English", settings_.language == "en")) {
    settings_.language = "en";
    setLanguage(Language::English);
    saveUserSettings();
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Tiếng Việt", settings_.language == "vi")) {
    settings_.language = "vi";
    setLanguage(Language::Vietnamese);
    saveUserSettings();
  }
  ImGui::SameLine();
  if (ImGui::RadioButton(tr("System"), settings_.language == "auto")) {
    settings_.language = "auto";
    setLanguage(systemLanguage());
    saveUserSettings();
  }
  ImGui::SameLine(ImGui::GetWindowWidth() - 190.0f * s);
  if (ImGui::Button(trId("Keyboard shortcuts"))) showShortcuts_ = true;
  ImGui::End();
}

// ------------------------------------------------------------------ shortcuts, tour
void App::drawShortcuts() {
  if (!showShortcuts_) return;
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(640.0f * appliedUiScale_, 0.0f), ImGuiCond_Appearing);
  if (ImGui::Begin(trId("Keyboard shortcuts"), &showShortcuts_, ImGuiWindowFlags_NoCollapse)) {
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
      ImGui::TableSetupColumn(tr("Keys"), ImGuiTableColumnFlags_WidthFixed, 210.0f * appliedUiScale_);
      ImGui::TableSetupColumn(tr("Action"));
      ImGui::TableHeadersRow();
      for (const Shortcut& k : kShortcuts) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tr(k.keys));
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", tr(k.action));
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

void App::drawTour() {
  if (tourStep_ < 0) return;
  struct Step {
    const char* title;
    const char* text;
    int anchor;  // 0 centre, 1 toolbar, 2 panel, 3 menu, 4 status bar
  };
  static const Step kSteps[] = {
      {"1/5  Look around", "Drag with the left mouse button to turn around the scene, the right button to move it, the wheel to zoom. R frames the whole scene.", 0},
      {"2/5  Tools", "Pick a tool in the bar at the top or with the keys 1 to 9. Hover a button to see what it does.", 1},
      {"3/5  Measure", "Press M (or 8) and click two points. For real lengths, measure something you know and set the scale in Tools > Scale & level.", 2},
      {"4/5  Clean up", "Flood (6): click an object to select it. Delete removes it, Ctrl+Z brings it back. Edit > Find floaters selects stray splats for review.", 1},
      {"5/5  Save", "Ctrl+S saves the project: measurements, notes and views. File menu: open, recent files, save the edited scene. F1 lists every shortcut.", 3},
  };
  constexpr int kCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));
  const Step& st = kSteps[std::clamp(tourStep_, 0, kCount - 1)];
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float s = appliedUiScale_;
  ImVec2 pos = vp->GetCenter(), pivot(0.5f, 0.5f);
  if (st.anchor == 1) pos = ImVec2(vp->GetCenter().x, vp->Pos.y + menuBarHeight_ + 70.0f * s), pivot = ImVec2(0.5f, 0.0f);
  if (st.anchor == 2) pos = ImVec2(vp->Pos.x + 420.0f * s, vp->Pos.y + 200.0f * s), pivot = ImVec2(0.0f, 0.0f);
  if (st.anchor == 3) pos = ImVec2(vp->Pos.x + 20.0f * s, vp->Pos.y + menuBarHeight_ + 10.0f * s), pivot = ImVec2(0.0f, 0.0f);
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
  ImGui::SetNextWindowSize(ImVec2(420.0f * s, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.96f);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.6f, 0.15f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
  if (ImGui::Begin("##tour", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", tr(st.title));
    ImGui::TextWrapped("%s", tr(st.text));
    ImGui::Spacing();
    auto finish = [&] {
      tourStep_ = -1;
      settings_.firstRunDone = true;
      saveUserSettings();
    };
    ImGui::BeginDisabled(tourStep_ == 0);
    if (ImGui::Button(trId("Back"))) --tourStep_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (tourStep_ < kCount - 1) {
      if (ImGui::Button(trId("Next"))) ++tourStep_;
      ImGui::SameLine();
      if (ImGui::Button(trId("Skip the tour"))) finish();
    } else if (ImGui::Button(trId("Done"))) {
      finish();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

}  // namespace gsv
