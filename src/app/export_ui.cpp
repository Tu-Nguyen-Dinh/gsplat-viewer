// Export (Phase 6 batch 5, docs/core/CORE6-ux.md): results the user can hand to someone else.
//   - picture: the current view at 1x / 2x / 4K, without the interface
//   - video along the saved views: PNG frames, encoded to MP4 when ffmpeg is installed
//   - report: an HTML page (prints to PDF) with the measurements, the notes and a picture of each
//     note's view
// Long exports render one frame per main-loop iteration: progress bar, Cancel, no frozen window.
#include "app/app.h"
#include "app/file_dialog.h"
#include "core/i18n.h"
#include "core/json.h"
#include "core/utf8_path.h"
#include "gl/gl_loader.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace gsv {

namespace {

std::string htmlEscape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\n': o += "<br>"; break;
      default: o.push_back(c);
    }
  }
  return o;
}

// ffmpeg on the PATH? (Checked once; the export then offers MP4.)
bool haveFfmpeg() {
  static int cached = -1;
  if (cached < 0) {
#ifdef _WIN32
    cached = std::system("where ffmpeg >NUL 2>&1") == 0 ? 1 : 0;
#else
    cached = std::system("command -v ffmpeg >/dev/null 2>&1") == 0 ? 1 : 0;
#endif
  }
  return cached == 1;
}

}  // namespace

// ------------------------------------------------------------------ rendering helpers
bool App::renderToPng(const std::string& path, int w, int h, const OrbitCamera* cam, std::string& error) {
  Framebuffer target;
  if (!target.create(w, h, targetFormat())) {
    error = "cannot create a " + std::to_string(w) + "x" + std::to_string(h) + " image";
    return false;
  }
  const OrbitCamera saved = camera_;
  const bool preview = splatSettings_.previewActive;
  const float hl = splatSettings_.highlight.a;
  if (cam) camera_ = *cam;
  splatSettings_.previewActive = false;
  splatSettings_.highlight.a = 0.0f;  // exported pictures show the true colours, not the selection
  frame(target.id(), w, h);
  const Image img = readFramebuffer(target.id(), w, h);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  camera_ = saved;
  splatSettings_.previewActive = preview;
  splatSettings_.highlight.a = hl;
  return savePng(pathFromUtf8(path), img, error);
}

// ------------------------------------------------------------------ picture
void App::exportPicture(int scaleMode) {
  int fbW, fbH;
  glfwGetFramebufferSize(window_, &fbW, &fbH);
  int w = fbW, h = fbH;
  if (scaleMode == 1) w *= 2, h *= 2;
  if (scaleMode == 2) {  // 4K, keeping the window's aspect
    w = 3840;
    h = std::max(1, static_cast<int>(std::lround(3840.0 * fbH / std::max(fbW, 1))));
  }
  std::string start = scenePath_.empty() ? std::string("view.png")
                                         : pathToUtf8(pathFromUtf8(scenePath_).parent_path() /
                                                      (pathToUtf8(pathFromUtf8(scenePath_).stem()) + "_view.png"));
  std::string path;
  if (nativeDialogsAvailable()) {
    if (!saveFileDialog(tr("Save the picture"), {{"PNG", "*.png"}}, start, "png", path)) return;
  } else {
    path = start;
  }
  std::string error;
  if (renderToPng(path, w, h, nullptr, error)) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), tr("Picture saved (%dx%d): %s"), w, h, path.c_str());
    status_ = buf;
  } else {
    status_ = std::string(tr("Export failed: ")) + error;
  }
}

// ------------------------------------------------------------------ video along the saved views
void App::startVideoExport() {
  if (project_.bookmarks.size() < 2) {
    status_ = tr("Video: save at least 2 views first (Project, notes & views > Save current view)");
    return;
  }
  std::string start = scenePath_.empty() ? std::string("flythrough")
                                         : pathToUtf8(pathFromUtf8(scenePath_).parent_path() /
                                                      (pathToUtf8(pathFromUtf8(scenePath_).stem()) + "_flythrough"));
  std::string path;
  if (nativeDialogsAvailable()) {
    if (!saveFileDialog(tr("Save the video"), {{"MP4", "*.mp4"}}, start + ".mp4", "mp4", path)) return;
  } else {
    path = start + ".mp4";
  }
  const std::filesystem::path p = pathFromUtf8(path);
  video_ = VideoExport{};
  video_.mp4 = path;
  video_.framesDir = pathToUtf8(p.parent_path() / (pathToUtf8(p.stem()) + "_frames"));
  std::error_code ec;
  std::filesystem::create_directories(pathFromUtf8(video_.framesDir), ec);
  video_.total = static_cast<int>(project_.bookmarks.size() - 1) * static_cast<int>(videoSecondsPerView_ * 30.0f);
  video_.width = videoWidth_;
  video_.height = videoWidth_ * 9 / 16;
  video_.active = video_.total > 0;
}

void App::stepVideoExport() {
  if (!video_.active) return;
  // One frame per loop iteration: the window stays responsive and shows the progress.
  const int perSegment = std::max(1, static_cast<int>(videoSecondsPerView_ * 30.0f));
  const int seg = std::min(video_.next / perSegment, static_cast<int>(project_.bookmarks.size()) - 2);
  float t = static_cast<float>(video_.next - seg * perSegment) / static_cast<float>(perSegment);
  t = t * t * (3.0f - 2.0f * t);  // ease in / out at every saved view
  const ViewBookmark& a = project_.bookmarks[static_cast<size_t>(seg)];
  const ViewBookmark& b = project_.bookmarks[static_cast<size_t>(seg + 1)];
  OrbitCamera cam = camera_;
  cam.lookAt(toDisplay(glm::mix(a.eye, b.eye, t)), toDisplay(glm::mix(a.target, b.target, t)));
  cam.fovY = a.fovY + (b.fovY - a.fovY) * t;
  char name[32];
  std::snprintf(name, sizeof(name), "frame_%05d.png", video_.next);
  std::string error;
  if (!renderToPng(pathToUtf8(pathFromUtf8(video_.framesDir) / name), video_.width, video_.height, &cam, error)) {
    video_.active = false;
    status_ = std::string(tr("Export failed: ")) + error;
    return;
  }
  if (++video_.next < video_.total) return;
  video_.active = false;
  if (haveFfmpeg()) {
    // H.264, veryfast preset (seconds instead of minutes), yuv420p so every player opens it.
    const std::string cmd = "ffmpeg -y -loglevel error -framerate 30 -i \"" +
                            pathToUtf8(pathFromUtf8(video_.framesDir) / "frame_%05d.png") +
                            "\" -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p \"" + video_.mp4 + "\"";
    const int rc = std::system(cmd.c_str());
    char buf[512];
    std::snprintf(buf, sizeof(buf), rc == 0 ? tr("Video saved: %s") : tr("ffmpeg failed; the frames are in %s"),
                  rc == 0 ? video_.mp4.c_str() : video_.framesDir.c_str());
    status_ = buf;
  } else {
    status_ = std::string(tr("Frames saved (install ffmpeg to get an MP4): ")) + video_.framesDir;
  }
}

// ------------------------------------------------------------------ report
void App::exportReport() {
  std::string start = scenePath_.empty() ? std::string("report.html")
                                         : pathToUtf8(pathFromUtf8(scenePath_).parent_path() /
                                                      (pathToUtf8(pathFromUtf8(scenePath_).stem()) + "_report.html"));
  std::string path;
  if (nativeDialogsAvailable()) {
    if (!saveFileDialog(tr("Save the report"), {{"HTML", "*.html"}}, start, "html", path)) return;
  } else {
    path = start;
  }
  const std::filesystem::path p = pathFromUtf8(path);
  const std::string imgDirName = pathToUtf8(p.stem()) + "_images";
  const std::filesystem::path imgDir = p.parent_path() / imgDirName;
  std::error_code ec;
  std::filesystem::create_directories(imgDir, ec);
  std::string error;
  int images = 0;
  // Overview from the current view, then one picture per note (from its saved view).
  const bool overview = renderToPng(pathToUtf8(imgDir / "overview.png"), 1280, 720, nullptr, error);
  images += overview;
  const glm::dvec3 up = glm::dvec3(glm::inverse(glm::mat3(modelMatrix())) * glm::vec3(0, 1, 0));
  char date[64];
  const std::time_t now = std::time(nullptr);
  std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M", std::localtime(&now));
  std::ostringstream h;
  h << "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>" << htmlEscape(tr("Report")) << " - "
    << htmlEscape(scene_.sourceName) << "</title>\n<style>body{font-family:Segoe UI,Arial,sans-serif;max-width:960px;margin:24px auto;"
       "color:#222}h1{margin-bottom:0}.muted{color:#777}table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccc;"
       "padding:6px 8px;text-align:left}th{background:#f2f2f2}td.num{text-align:right;font-variant-numeric:tabular-nums}"
       "img{max-width:100%;border:1px solid #ddd;margin:6px 0}.note{page-break-inside:avoid;margin:18px 0}"
       "@media print{body{margin:0}}</style></head><body>\n";
  h << "<h1>" << htmlEscape(tr("Report")) << ": " << htmlEscape(scene_.sourceName) << "</h1>\n";
  h << "<p class=\"muted\">" << htmlEscape(date) << " &middot; " << scene_.size() << " splats &middot; "
    << htmlEscape(tr("1 scene unit =")) << " " << project_.unitScale << " " << htmlEscape(project_.unit) << "</p>\n";
  if (overview) h << "<img src=\"" << imgDirName << "/overview.png\" alt=\"overview\">\n";
  h << "<h2>" << htmlEscape(tr("Measurements")) << " (" << project_.measurements.size() << ")</h2>\n";
  if (project_.measurements.empty()) {
    h << "<p class=\"muted\">-</p>\n";
  } else {
    h << "<table><tr><th>" << htmlEscape(tr("Name")) << "</th><th>" << htmlEscape(tr("Type")) << "</th><th>"
      << htmlEscape(tr("Value")) << "</th><th>" << htmlEscape(tr("Points")) << "</th></tr>\n";
    for (const MeasurementRecord& m : project_.measurements) {
      char v[64];
      std::snprintf(v, sizeof(v), "%.4f %s%s", measurementRealValue(m, project_.unitScale, up), project_.unit.c_str(),
                    m.kind == MeasurementRecord::Kind::Area ? "&sup2;" : "");
      h << "<tr><td>" << htmlEscape(m.name) << "</td><td>" << htmlEscape(tr(measurementKindName(m.kind))) << "</td><td class=\"num\">"
        << v << "</td><td>" << m.points.size() << "</td></tr>\n";
    }
    h << "</table>\n";
  }
  h << "<h2>" << htmlEscape(tr("Notes")) << " (" << project_.annotations.size() << ")</h2>\n";
  for (size_t i = 0; i < project_.annotations.size(); ++i) {
    const Annotation& a = project_.annotations[i];
    h << "<div class=\"note\"><h3>" << (i + 1) << ". " << htmlEscape(a.title) << "</h3>\n";
    if (!a.text.empty()) h << "<p>" << htmlEscape(a.text) << "</p>\n";
    if (a.bookmark >= 0 && a.bookmark < static_cast<int>(project_.bookmarks.size())) {
      const ViewBookmark& b = project_.bookmarks[static_cast<size_t>(a.bookmark)];
      OrbitCamera cam = camera_;
      cam.lookAt(toDisplay(b.eye), toDisplay(b.target));
      cam.fovY = b.fovY;
      const std::string img = "note_" + std::to_string(i + 1) + ".png";
      if (renderToPng(pathToUtf8(imgDir / img), 960, 540, &cam, error)) {
        h << "<img src=\"" << imgDirName << "/" << img << "\" alt=\"" << htmlEscape(a.title) << "\">\n";
        ++images;
      }
    }
    h << "</div>\n";
  }
  h << "<p class=\"muted\">gsplat-viewer " << GSV_VERSION << "</p>\n</body></html>\n";
  if (writeFileAtomic(path, h.str(), error)) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), tr("Report saved with %d picture(s): %s"), images, path.c_str());
    status_ = buf;
  } else {
    status_ = std::string(tr("Export failed: ")) + error;
  }
}

void App::drawExportProgress() {
  if (!video_.active) return;
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  if (ImGui::Begin("##export", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextUnformatted(tr("Exporting the video..."));
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%d / %d", video_.next, video_.total);
    ImGui::ProgressBar(static_cast<float>(video_.next) / static_cast<float>(std::max(video_.total, 1)), ImVec2(320.0f * appliedUiScale_, 0), overlay);
    if (ImGui::Button(trId("Cancel"))) {
      video_.active = false;
      status_ = std::string(tr("Video export cancelled; frames so far: ")) + video_.framesDir;
    }
  }
  ImGui::End();
}

void App::drawExportMenu() {
  if (!ImGui::BeginMenu(trId("Export"), !scene_.empty())) return;
  if (ImGui::MenuItem(trId("Picture (window size)"))) exportPicture(0);
  if (ImGui::MenuItem(trId("Picture (2x)"))) exportPicture(1);
  if (ImGui::MenuItem(trId("Picture (4K)"))) exportPicture(2);
  ImGui::Separator();
  const bool canVideo = project_.bookmarks.size() >= 2;
  if (ImGui::MenuItem(trId("Video along the saved views..."), nullptr, false, canVideo && !video_.active)) startVideoExport();
  if (!canVideo && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", tr("Save at least 2 views first"));
  ImGui::SetNextItemWidth(140.0f * appliedUiScale_);
  ImGui::SliderFloat(trId("Seconds per view"), &videoSecondsPerView_, 1.0f, 10.0f, "%.1f s");
  ImGui::SetNextItemWidth(140.0f * appliedUiScale_);
  ImGui::SliderInt(trId("Video width"), &videoWidth_, 640, 3840);
  ImGui::TextDisabled("%s", haveFfmpeg() ? tr("MP4 via ffmpeg") : tr("ffmpeg not found: PNG frames only"));
  ImGui::Separator();
  if (ImGui::MenuItem(trId("Report (HTML, printable to PDF)..."))) exportReport();
  if (ImGui::MenuItem(trId("Measurements as CSV (clipboard)"), nullptr, false, !project_.measurements.empty())) {
    ImGui::SetClipboardText(measurementsToCsv(project_.measurements, project_.unit, project_.unitScale).c_str());
    status_ = tr("Measurements copied to the clipboard");
  }
  ImGui::EndMenu();
}

}  // namespace gsv
