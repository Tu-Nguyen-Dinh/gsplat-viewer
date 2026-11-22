// Data safety (Phase 6 batch 2, docs/core/CORE6-ux.md): the user never loses work silently.
//   - unsaved-changes tracking: scene = EditHistory::revision() at the last save / load;
//     project = its JSON at the last save / open (measurements, notes, views, scale, level)
//   - before quitting or opening another file with unsaved changes: Save / Don't save / Cancel
//   - saving the edited scene runs on a worker thread on a copy: the window never freezes
//   - autosave every settings_.autosaveSeconds into <settings>/recovery; a clean exit deletes it,
//     so finding it at startup means the last session crashed: offer to recover
//   - history panel: every undo step, click to jump there
//   - long operations (loading, auto-clean) can be cancelled
#include "app/app.h"
#include "core/i18n.h"
#include "core/json.h"
#include "core/utf8_path.h"
#include "scene/ply_io.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace gsv {

namespace {

std::filesystem::path recoveryDir() { return pathFromUtf8(settingsDirectory()) / "recovery"; }

template <typename T>
bool ready(const std::future<T>& f) {
  return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

std::string absoluteUtf8(const std::string& p) {
  if (p.empty()) return p;
  std::error_code ec;
  const std::filesystem::path a = std::filesystem::absolute(pathFromUtf8(p), ec);
  return ec ? p : pathToUtf8(a);
}

}  // namespace

// ------------------------------------------------------------------ dirty state
bool App::projectDirty() const { return projectToJson(project_) != savedProjectJson_; }

void App::markProjectSaved() { savedProjectJson_ = projectToJson(project_); }

void App::requestDiscard(std::function<void()> action) {
  if (!sceneDirty() && !projectDirty()) {
    action();
    return;
  }
  afterConfirm_ = std::move(action);
  confirmOpen_ = true;
}

bool App::saveEverything() {
  bool ok = true;
  if (projectDirty()) {
    const std::string path = projectPath_[0] ? std::string(projectPath_) : defaultProjectPath();
    std::snprintf(projectPath_, sizeof(projectPath_), "%s", path.c_str());
    saveProjectTo(path);
    ok = ok && !projectDirty();
  }
  if (sceneDirty()) {
    std::string path = savePath_;
    if (path.empty()) {
      const std::filesystem::path p = pathFromUtf8(scenePath_.empty() ? std::string("scene.ply") : scenePath_);
      path = pathToUtf8(p.parent_path() / (pathToUtf8(p.stem()) + "_edited.ply"));
    }
    std::string error;
    if (savePly(pathFromUtf8(path), scene_, error)) {
      markSceneSaved();
      std::snprintf(savePath_, sizeof(savePath_), "%s", path.c_str());
      rememberRecent(path);
    } else {
      status_ = std::string(tr("Save failed: ")) + error;
      ok = false;
    }
  }
  return ok;
}

void App::drawConfirmDiscard() {
  if (!confirmOpen_) return;
  ImGui::OpenPopup("##unsaved");
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("##unsaved", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::TextUnformatted(tr("You have unsaved changes."));
    if (sceneDirty()) ImGui::BulletText("%s", tr("Edits to the scene (delete, move, colour...)"));
    if (projectDirty()) ImGui::BulletText("%s", tr("Measurements, notes, views, scale or level"));
    ImGui::Spacing();
    auto finish = [&](bool run) {
      confirmOpen_ = false;
      ImGui::CloseCurrentPopup();
      std::function<void()> action = std::move(afterConfirm_);
      afterConfirm_ = nullptr;
      if (run && action) action();
    };
    if (ImGui::Button(trId("Save and continue"))) {
      if (saveEverything()) finish(true);
    }
    if (ImGui::IsItemHovered()) {
      std::string where;
      if (projectDirty()) where += std::string(projectPath_[0] ? projectPath_ : defaultProjectPath().c_str()) + "\n";
      if (sceneDirty()) where += savePath_[0] ? std::string(savePath_) : std::string(tr("<scene>_edited.ply next to the scene"));
      ImGui::SetTooltip("%s", where.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(trId("Don't save"))) finish(true);
    ImGui::SameLine();
    if (ImGui::Button(trId("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) finish(false);
    ImGui::EndPopup();
  }
}

void App::onWindowClose(GLFWwindow* w) {
  App* app = static_cast<App*>(glfwGetWindowUserPointer(w));
  if (!app || !app->interactive_ || (!app->sceneDirty() && !app->projectDirty())) return;
  glfwSetWindowShouldClose(w, GLFW_FALSE);  // ask first
  app->requestDiscard([w] { glfwSetWindowShouldClose(w, GLFW_TRUE); });
}

// ------------------------------------------------------------------ background save
void App::startSceneSave(const std::string& path) {
  if (saveJob_.valid()) {
    status_ = tr("Still saving the previous file...");
    return;
  }
  saveJobPath_ = path;
  saveJobRevision_ = history_.revision();
  // A copy: the user can keep editing while the file is written (costs memory for a moment).
  saveJob_ = std::async(std::launch::async, [copy = scene_, p = path]() {
    std::string error;
    return savePly(pathFromUtf8(p), copy, error) ? std::string() : (error.empty() ? std::string("write error") : error);
  });
  status_ = std::string(tr("Saving ")) + pathToUtf8(pathFromUtf8(path).filename()) + "...";
}

void App::pollBackgroundJobs() {
  if (ready(saveJob_)) {
    const std::string error = saveJob_.get();
    if (error.empty()) {
      savedRevision_ = saveJobRevision_;  // edits made during the save keep the scene dirty
      std::snprintf(savePath_, sizeof(savePath_), "%s", saveJobPath_.c_str());
      status_ = std::string(tr("Saved")) + ": " + saveJobPath_;
      rememberRecent(saveJobPath_);
    } else {
      status_ = std::string(tr("Save failed: ")) + error;
    }
  }
  if (ready(autosaveJob_)) {
    const std::string error = autosaveJob_.get();
    if (!error.empty()) std::fprintf(stderr, "Autosave failed: %s\n", error.c_str());
  }
  // Cancelled work finishes in the background; drop it once done (a future from std::async blocks
  // in its destructor, so it cannot simply be discarded while running).
  for (size_t i = 0; i < abandonedLoads_.size();)
    if (ready(abandonedLoads_[i])) abandonedLoads_.erase(abandonedLoads_.begin() + static_cast<std::ptrdiff_t>(i));
    else ++i;
  for (size_t i = 0; i < abandonedCleans_.size();)
    if (ready(abandonedCleans_[i])) abandonedCleans_.erase(abandonedCleans_.begin() + static_cast<std::ptrdiff_t>(i));
    else ++i;
}

void App::cancelLoading() {
  if (!loading_.valid()) return;
  abandonedLoads_.push_back(std::move(loading_));  // loading_ is now invalid: isLoading() == false
  recoverOriginalPath_.clear();
  status_ = std::string(tr("Cancelled: ")) + loadingName_;
}

// ------------------------------------------------------------------ autosave / recovery
void App::autosaveTick() {
  if (!interactive_ || settings_.autosaveSeconds <= 0) return;
  const double now = glfwGetTime();
  if (now - lastAutosave_ < settings_.autosaveSeconds) return;
  lastAutosave_ = now;
  const bool sceneChanged = sceneDirty() && history_.revision() != autosavedRevision_;
  const std::string projectJson = projectToJson(project_);
  const bool projectChanged = projectDirty() && projectJson != autosavedProjectJson_;
  if (!sceneChanged && !projectChanged) return;
  if (autosaveJob_.valid()) return;  // previous autosave still writing
  std::error_code ec;
  std::filesystem::create_directories(recoveryDir(), ec);
  // Project: scene / LAS paths absolute so the recovery file works from its own folder.
  Project p = project_;
  p.scenePath = absoluteUtf8(scenePath_);
  p.lasPath = absoluteUtf8(options_.las);
  std::string error;
  writeFileAtomic(pathToUtf8(recoveryDir() / "project.gsvproj"), projectToJson(p), error);
  autosavedProjectJson_ = projectJson;
  const bool edited = sceneDirty();
  std::ostringstream session;
  session << "{\n  \"format\": \"gsplat-viewer recovery\",\n  \"scene\": " << jsonQuote(absoluteUtf8(scenePath_))
          << ",\n  \"editedScene\": " << jsonQuote(edited ? "scene.ply" : "") << ",\n  \"projectFile\": "
          << jsonQuote(projectPath_) << ",\n  \"time\": " << static_cast<long long>(std::time(nullptr))
          << ",\n  \"splats\": " << scene_.size() << ",\n  \"edits\": " << history_.cursor() << "\n}\n";
  writeFileAtomic(pathToUtf8(recoveryDir() / "session.json"), session.str(), error);
  if (sceneChanged) {
    autosavedRevision_ = history_.revision();
    const std::string target = pathToUtf8(recoveryDir() / "scene.ply");
    autosaveJob_ = std::async(std::launch::async, [copy = scene_, target]() {
      std::string err;
      const std::string tmp = target + ".part";  // never leave a half-written scene.ply
      if (!savePly(pathFromUtf8(tmp), copy, err)) return err.empty() ? std::string("write error") : err;
      std::error_code e;
      std::filesystem::rename(pathFromUtf8(tmp), pathFromUtf8(target), e);
      return e ? e.message() : std::string();
    });
  }
}

void App::clearRecovery() {
  if (autosaveJob_.valid()) autosaveJob_.wait();
  std::error_code ec;
  std::filesystem::remove_all(recoveryDir(), ec);
}

void App::checkRecoveryAtStartup() {
  std::ifstream f(recoveryDir() / "session.json", std::ios::binary);
  if (!f) return;
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  std::string error;
  if (!parseJson(ss.str(), j, error, 8) || !j.isObject()) {
    clearRecovery();
    return;
  }
  const Json* scene = j.get("scene");
  const Json* time = j.get("time");
  const Json* edits = j.get("edits");
  // Nothing worth offering (no scene edit, empty project): just clean up.
  const Json* edited = j.get("editedScene");
  const bool sceneWork = edits && edits->isNumber() && edits->n > 0 && edited && edited->isString() && !edited->s.empty();
  Project p;
  const bool projectWork = loadProject(pathToUtf8(recoveryDir() / "project.gsvproj"), p, error) &&
                           (!p.measurements.empty() || !p.annotations.empty() || !p.bookmarks.empty() ||
                            p.unitScale != 1.0 || p.level[0] != 1.0);
  if (!sceneWork && !projectWork) {
    clearRecovery();
    return;
  }
  char when[64] = "?";
  if (time && time->isNumber()) {
    const std::time_t t = static_cast<std::time_t>(time->n);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&t));
  }
  char buf[1024];
  std::snprintf(buf, sizeof(buf), tr("Scene: %s\nLast autosave: %s, %d edit(s)"),
                scene && scene->isString() && !scene->s.empty() ? scene->s.c_str() : "-", when,
                edits && edits->isNumber() ? static_cast<int>(edits->n) : 0);
  recoveryInfo_ = buf;
  recoveryPrompt_ = true;
}

void App::applyProject(const Project& p, const std::string& projectFile) {
  project_ = p;
  level_ = glm::mat4_cast(glm::quat(static_cast<float>(p.level[0]), static_cast<float>(p.level[1]),
                                    static_cast<float>(p.level[2]), static_cast<float>(p.level[3])));
  if (!projectFile.empty()) std::snprintf(projectPath_, sizeof(projectPath_), "%s", projectFile.c_str());
  measureDraft_.clear();
  editAnnotation_ = -1;
}

void App::recoverSession() {
  std::ifstream f(recoveryDir() / "session.json", std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  std::string error;
  if (!parseJson(ss.str(), j, error, 8)) return;
  auto str = [&](const char* k) {
    const Json* v = j.get(k);
    return v && v->isString() ? v->s : std::string();
  };
  const std::string original = str("scene"), edited = str("editedScene"), projectFile = str("projectFile");
  Project p;
  if (loadProject(pathToUtf8(recoveryDir() / "project.gsvproj"), p, error)) {
    applyProject(p, projectFile);
    const bool hasWork = !p.measurements.empty() || !p.annotations.empty() || !p.bookmarks.empty() ||
                         p.unitScale != 1.0 || p.level[0] != 1.0;
    if (hasWork) savedProjectJson_.clear();  // recovered measurements / notes: not saved yet
    else markProjectSaved();
    if (!p.lasPath.empty()) {
      options_.las = p.lasPath;
      loadCloudFile(p.lasPath);
    }
  }
  std::error_code ec;
  const std::filesystem::path editedPath = recoveryDir() / (edited.empty() ? "none" : edited);
  if (!edited.empty() && std::filesystem::exists(editedPath, ec)) {
    // Load the autosaved copy, but keep the original's name (Save writes next to the original).
    recoverOriginalPath_ = original;
    std::filesystem::path copy = recoveryDir() / "recovered.ply";  // the autosave may be rewritten meanwhile
    std::filesystem::copy_file(editedPath, copy, std::filesystem::copy_options::overwrite_existing, ec);
    loadSceneFile(pathToUtf8(ec ? editedPath : copy));
  } else if (!original.empty()) {
    loadSceneFile(original);
  }
  showWelcome_ = false;
  status_ = tr("Recovered the autosaved session: save it to keep it");
}

void App::drawRecoveryPrompt() {
  if (!recoveryPrompt_) return;
  ImGui::OpenPopup("##recovery");
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("##recovery", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::TextUnformatted(tr("The last session ended unexpectedly."));
    ImGui::TextUnformatted(tr("Unsaved work was autosaved. Recover it?"));
    ImGui::TextDisabled("%s", recoveryInfo_.c_str());
    ImGui::Spacing();
    if (ImGui::Button(trId("Recover"))) {
      recoveryPrompt_ = false;
      ImGui::CloseCurrentPopup();
      recoverSession();
    }
    ImGui::SameLine();
    if (ImGui::Button(trId("Discard"))) {
      recoveryPrompt_ = false;
      ImGui::CloseCurrentPopup();
      clearRecovery();
    }
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ history panel
void App::drawHistoryPanel() {
  if (!ImGui::CollapsingHeader(trId("History"))) return;
  ImGui::TextDisabled(tr("%zu step(s), %.1f MB"), history_.size(), static_cast<double>(history_.bytes()) / 1e6);
  if (sceneDirty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", tr("(unsaved)"));
  }
  ImGui::BeginChild("##history", ImVec2(0, std::min(220.0f, (history_.size() + 1) * ImGui::GetTextLineHeightWithSpacing() + 8.0f)),
                    ImGuiChildFlags_Borders);
  int target = -1;
  const size_t cur = history_.cursor();
  if (ImGui::Selectable(tr("Opened scene"), cur == 0)) target = 0;
  for (size_t i = 0; i < history_.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    const bool applied = i < cur;
    if (!applied) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    char label[160];
    std::snprintf(label, sizeof(label), "%zu. %s", i + 1, tr(history_.label(i).c_str()));
    if (ImGui::Selectable(label, cur == i + 1)) target = static_cast<int>(i + 1);
    if (!applied) ImGui::PopStyleColor();
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (target >= 0 && static_cast<size_t>(target) != cur) {  // jump: undo / redo until there
    finishLiveEdit(true);
    while (history_.cursor() > static_cast<size_t>(target) && history_.undo(scene_)) {
    }
    while (history_.cursor() < static_cast<size_t>(target) && history_.redo(scene_)) {
    }
    uploadScene();
    char buf[160];
    std::snprintf(buf, sizeof(buf), tr("History: back to step %d"), target);
    status_ = buf;
  }
}

}  // namespace gsv
