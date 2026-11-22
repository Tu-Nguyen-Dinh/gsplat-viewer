// Interactive tools on the 3D view (Core 5, see docs/core/CORE5-editing.md):
//
//   1 Navigate   orbit / pan / zoom (default)
//   2 Rect       drag a rectangle            Shift add, Alt subtract, Shift+Alt intersect
//   3 Lasso      draw a free polygon         (same modifiers)
//   4 Brush      paint with a screen circle  [ ] radius
//   5 Sphere     paint with a 3D ball on the surface under the cursor
//   6 Flood      click a surface: selects the connected, similar-coloured object
//   7 Transform  gizmo: drag an axis arrow (move along it), the centre square (move in the view
//                plane) or the yellow ring (turn around the vertical); one undo step per drag
//   8 Measure    distance / polyline / area / height; Enter finishes, Backspace removes a point
//   9 Note       click to pin a note (title + text + the current view)
//
// With a tool active the left button belongs to the tool, the right button orbits and the
// middle button pans. Screen selections (Rect, Lasso, Brush) take every splat whose centre projects
// inside, including hidden-behind ones (like SuperSplat); Sphere and Flood work in 3D.
//
// Coordinates: the scene's own coordinates ("scene") are what the splat file, the edit history and
// the project file use. The view shows them through modelMatrix() ("display").
#include "app/app.h"
#include "core/i18n.h"
#include "core/registration.h"
#include "gl/gl_loader.h"
#include "core/utf8_path.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace gsv {

namespace {

double nowSeconds() { return glfwGetTime(); }

struct ToolInfo {
  const char* name;
  const char* help;
};
constexpr ToolInfo kTools[] = {
    {"Navigate", "Orbit (left drag), pan (right / middle drag), zoom (wheel)"},
    {"Rect", "Drag a rectangle. Shift add, Alt subtract, Shift+Alt intersect"},
    {"Lasso", "Draw around splats. Shift add, Alt subtract, Shift+Alt intersect"},
    {"Brush", "Paint a selection with a screen circle. [ ] change the radius"},
    {"Sphere", "Paint with a 3D ball on the surface under the cursor. [ ] change the radius"},
    {"Flood", "Click a surface: selects the connected object of similar colour"},
    {"Transform", "Drag an arrow (axis), the square (view plane) or the ring (turn)"},
    {"Measure", "Click points. Enter finishes a polyline / area, Backspace removes a point"},
    {"Note", "Click a surface to pin a note"},
};
static_assert(sizeof(kTools) / sizeof(kTools[0]) == static_cast<size_t>(Tool::Count));

// Labels of undo steps and selections: passed around in English, translated when shown (tr()).
// Listed here so tools/i18n_extract.py finds them.
[[maybe_unused]] constexpr const char* kHistoryLabels[] = {
    "Colour", "Crop", "Delete inside", "Delete selection", "Duplicate", "Filter", "Transform",
    "Invert", "Invert selection", "Lasso", "Rect", "Select all", "Select none", "Selection cleared",
    "distance", "polyline", "area", "height",  // measurementKindName(), shown in reports
    "Distance", "Polyline", "Area", "Height",  // kindLabel(): default measurement names
};

SelectOp opFromModifiers(bool shift, bool alt) {
  if (shift && alt) return SelectOp::Intersect;
  if (shift) return SelectOp::Add;
  if (alt) return SelectOp::Subtract;
  return SelectOp::Replace;
}

float distanceToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  const glm::vec2 ab = b - a;
  const float len2 = glm::dot(ab, ab);
  const float t = len2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
  return glm::length(p - (a + t * ab));
}

const char* kindLabel(MeasurementRecord::Kind k) {
  switch (k) {
    case MeasurementRecord::Kind::Distance: return "Distance";
    case MeasurementRecord::Kind::Polyline: return "Polyline";
    case MeasurementRecord::Kind::Area: return "Area";
    case MeasurementRecord::Kind::Height: return "Height";
  }
  return "";
}

bool isSelectionTool(Tool t) {
  return t == Tool::Rect || t == Tool::Lasso || t == Tool::Brush || t == Tool::Sphere || t == Tool::Flood ||
         t == Tool::Transform;
}

}  // namespace

// ------------------------------------------------------------------ coordinates
glm::vec3 App::toScene(const glm::vec3& p) const { return glm::vec3(glm::inverse(modelMatrix()) * glm::vec4(p, 1.0f)); }
glm::vec3 App::toDisplay(const glm::vec3& p) const { return glm::vec3(modelMatrix() * glm::vec4(p, 1.0f)); }

glm::mat4 App::screenMvp() const {
  int w, h;
  glfwGetWindowSize(window_, &w, &h);
  return camera_.projection(static_cast<float>(w) / std::max(h, 1)) * camera_.view() * modelMatrix();
}

bool App::keyPressed(int key) {
  const bool d = glfwGetKey(window_, key) == GLFW_PRESS, was = keyWasDown_[key];
  keyWasDown_[key] = d;
  return d && !was;
}

// ------------------------------------------------------------------ selection state
void App::selectionChanged() {
  if (selected_.size() != scene_.size()) selected_.assign(scene_.size(), 0);
  if (hidden_.size() != scene_.size()) hidden_.assign(scene_.size(), 0);
  selectedCount_ = countSelected(selected_);
  hiddenCount_ = countSelected(hidden_);
  flagsDirty_ = true;
  pivotDirty_ = true;
}

void App::syncSelectionFlags() {
  if (!flagsDirty_) return;
  flagsDirty_ = false;
  if (lodEnabled_ || selected_.size() != scene_.size() || (selectedCount_ == 0 && hiddenCount_ == 0)) {
    splats_.clearSelectionFlags();
    return;
  }
  std::vector<uint8_t> flags(scene_.size());
  for (size_t i = 0; i < flags.size(); ++i)
    flags[i] = static_cast<uint8_t>((selected_[i] ? 1u : 0u) | (hidden_[i] ? 2u : 0u));
  splats_.setSelectionFlags(flags);
  lastFlagsUpload_ = nowSeconds();
}

void App::setSelection(const SplatMask& mask, SelectOp op, const char* what) {
  finishLiveEdit(true);
  if (selected_.size() != scene_.size()) selected_.assign(scene_.size(), 0);
  combineSelection(selected_, mask, op);
  for (size_t i = 0; i < selected_.size() && i < hidden_.size(); ++i)
    if (hidden_[i]) selected_[i] = 0;  // what is hidden cannot be selected
  selectionChanged();
  if (what) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), tr("%s: %zu splats selected"), tr(what), selectedCount_);
    status_ = buf;
  }
}

void App::setTool(Tool t) {
  if (t == tool_) return;
  splatSettings_.previewActive = false;
  finishLiveEdit(true);
  if (toolDragging_) {
    toolDragging_ = false;
    lasso_.clear();
  }
  if (tool_ == Tool::Measure && !measureDraft_.empty()) finishMeasurement();
  tool_ = t;
  if (isSelectionTool(t) && lodEnabled_) {
    setLodEnabled(false);  // editing works on the full scene, the LOD cut is a different set of splats
    flagsDirty_ = true;
  }
  hover_.reset();
  status_ = std::string(tr(kTools[static_cast<int>(t)].name)) + ": " + tr(kTools[static_cast<int>(t)].help);
}

const KdTree& App::editTree() {
  if (!editTreeValid_) {
    editTree_.build(scene_.position);
    spacing_ = typicalSpacing(scene_, editTree_);
    editTreeValid_ = true;
  }
  return editTree_;
}

DepthImage App::renderSplatDepth() {
  int fbW, fbH;
  glfwGetFramebufferSize(window_, &fbW, &fbH);
  // Half resolution is plenty to decide what is in front (and 4x cheaper to read back).
  const int w = std::max(64, fbW / 2), h = std::max(64, fbH / 2);
  Framebuffer t;
  if (mode_ != RenderMode::Splats || !t.create(w, h, Framebuffer::Format::RGBA16F, {true, false})) return {};
  t.clear(background_);
  SplatSettings s = splatSettings_;
  s.depthOutput = true;
  s.depthTest = false;
  s.previewActive = false;
  splats_.render(makeRenderView(w, h), s);
  DepthImage d = readDepthOutput(t);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return d;
}

void App::applyVisibleOnly(SplatMask& mask) {
  if (!visibleOnly_ || mode_ != RenderMode::Splats) return;
  const DepthImage d = strokeDepth_.empty() ? renderSplatDepth() : strokeDepth_;
  int w, h;
  glfwGetWindowSize(window_, &w, &h);
  keepVisible(scene_, camera_.view() * modelMatrix(), camera_.projection(static_cast<float>(w) / std::max(h, 1)), d, mask);
}

void App::growOrShrink(bool grow) {
  if (selectedCount_ == 0) {
    status_ = tr("Grow / shrink: nothing selected");
    return;
  }
  finishLiveEdit(true);
  const KdTree& tree = editTree();
  const float gap = 2.0f * (spacing_ > 0.0f ? spacing_ : 0.003f * std::max(scene_.focusRadius, 1e-3f));
  if (grow) growSelection(scene_, tree, selected_, gap, floodColor_);
  else shrinkSelection(scene_, tree, selected_, gap);
  for (size_t i = 0; i < selected_.size(); ++i)
    if (hidden_[i]) selected_[i] = 0;
  selectionChanged();
  char buf[160];
  std::snprintf(buf, sizeof(buf), grow ? tr("Grown: %zu splats selected") : tr("Shrunk: %zu splats selected"), selectedCount_);
  status_ = buf;
}

// ------------------------------------------------------------------ live edits
void App::livePreviewUpload(bool final) {
  const double t = nowSeconds();
  // Re-uploading a large scene costs tens of ms: preview at most every 60 ms while dragging.
  if (!final && t - lastPreview_ < 0.06) {
    previewPending_ = true;
    return;
  }
  previewPending_ = false;
  lastPreview_ = t;
  if (final) {
    points_.upload(scene_);
    selectionDirty_ = selectionDirtyForFilter_ = true;
    editTreeValid_ = false;  // positions moved
  }
  splats_.upload(scene_);
  flagsDirty_ = true;
}

void App::finishLiveEdit(bool commit) {
  if (!history_.liveActive()) return;
  if (commit) {
    if (previewPending_) livePreviewUpload(true);
    history_.commit(scene_);
  } else {
    history_.cancel(scene_);
  }
  livePreviewUpload(true);
  gizmoMove_ = glm::vec3(0.0f);
  gizmoAngle_ = 0.0f;
  gizmoScale_ = 1.0f;
  gizmoActive_ = -1;
  colorAdjust_ = ColorAdjust{};
  pivotDirty_ = true;
}

void App::liveTransformPreview(bool final) {
  if (selectedCount_ == 0) return;
  const glm::mat4 T = glm::translate(glm::mat4(1.0f), gizmoPivot_ + gizmoMove_) *
                      glm::rotate(glm::mat4(1.0f), gizmoAngle_, glm::vec3(0, 1, 0)) *
                      glm::scale(glm::mat4(1.0f), glm::vec3(gizmoScale_)) *
                      glm::translate(glm::mat4(1.0f), -gizmoPivot_);
  const glm::mat4 M = modelMatrix();
  const glm::mat4 sceneT = glm::inverse(M) * T * M;  // display-space edit, scene-space data
  const bool gpuPreview = mode_ == RenderMode::Splats && !lodEnabled_ && splats_.hasSelectionFlags();
  if (!final && gpuPreview) {
    // While dragging only a matrix goes to the GPU (preprocess moves the selected splats):
    // no re-upload of the scene, so the drag stays smooth whatever the scene size.
    splatSettings_.previewActive = true;
    splatSettings_.previewAffine = sceneT;
    return;
  }
  if (final && !history_.liveActive()) {  // drag shown on the GPU: apply once, one undo step
    splatSettings_.previewActive = false;
    history_.modify(scene_, selected_, "Transform", [&](GaussianScene& sc, const SplatMask& m) { transformSplats(sc, m, sceneT); });
    livePreviewUpload(true);
    return;
  }
  // Points view (no GPU preview): restore and re-apply the total transform, re-uploading (throttled).
  if (!history_.liveActive()) history_.begin(scene_, selected_, "Transform");
  history_.restore(scene_);
  transformSplats(scene_, selected_, sceneT);
  livePreviewUpload(final);
}

// ------------------------------------------------------------------ actions
void App::deleteSelection() {
  if (selectedCount_ == 0) {
    status_ = tr("Delete: nothing selected");
    return;
  }
  const SplatMask mask = selected_;
  applyRemoval(mask, "Delete selection");
}

void App::hideSelection() {
  finishLiveEdit(true);
  for (size_t i = 0; i < selected_.size(); ++i)
    if (selected_[i]) {
      hidden_[i] = 1;
      selected_[i] = 0;
    }
  selectionChanged();
  status_ = std::to_string(hiddenCount_) + " splats hidden (Alt+H shows them again)";
}

void App::isolateSelection() {
  if (selectedCount_ == 0) {
    status_ = tr("Isolate: nothing selected");
    return;
  }
  finishLiveEdit(true);
  for (size_t i = 0; i < selected_.size(); ++i) hidden_[i] = selected_[i] ? 0 : 1;
  selectionChanged();
  char buf[160];
  std::snprintf(buf, sizeof(buf), tr("Isolated %zu splats (Alt+H shows everything)"), selectedCount_);
  status_ = buf;
}

void App::unhideAll() {
  std::fill(hidden_.begin(), hidden_.end(), 0);
  selectionChanged();
  status_ = tr("All splats shown");
}

void App::autoCleanFloaters() {
  if (cleanJob_.valid() || scene_.empty()) return;
  finishLiveEdit(true);
  // Runs on a worker thread on a copy of what it needs (positions only), the UI stays responsive.
  GaussianScene positions;
  positions.position = scene_.position;
  std::vector<CameraView> cams;
  if (!colmap_.images.empty()) {
    for (const ColmapImage& img : colmap_.images) {
      const ColmapCamera* cam = nullptr;
      for (const ColmapCamera& c : colmap_.cameras)
        if (c.id == img.cameraId) cam = &c;
      if (!cam) continue;
      const glm::dvec4 k = cam->pinhole();
      CameraView v;
      v.view = img.viewMatrixGl();
      v.focal = {static_cast<float>(k.x), static_cast<float>(k.y)};
      v.principal = {static_cast<float>(k.z), static_cast<float>(k.w)};
      v.size = {static_cast<float>(cam->width), static_cast<float>(cam->height)};
      cams.push_back(v);
    }
  }
  const float ratio = floaterRatio_;
  const int minViews = floaterViews_;
  cleanJobSize_ = scene_.size();
  cleanJobStart_ = nowSeconds();
  cleanJob_ = std::async(std::launch::async, [positions = std::move(positions), cams = std::move(cams), ratio, minViews]() {
    SplatMask m = selectOutliers(positions, 16, ratio);
    if (!cams.empty()) {
      const SplatMask unseen = selectUnseen(positions, cams, minViews);
      combineSelection(m, unseen, SelectOp::Add);
    }
    return m;
  });
  status_ = tr("Auto-clean: searching floaters...");
}

void App::levelFromSelection() {
  if (selectedCount_ < 3) {
    status_ = tr("Level: select part of the floor first (Brush or Flood), then press Level");
    return;
  }
  std::vector<glm::vec3> pts;
  pts.reserve(selectedCount_);
  for (size_t i = 0; i < scene_.size(); ++i)
    if (selected_[i]) pts.push_back(toDisplay(scene_.position[i]));
  Plane plane;
  if (!fitPlane(pts, {}, plane)) {
    status_ = tr("Level: could not fit a plane to the selection");
    return;
  }
  glm::vec3 n = glm::normalize(glm::vec3(plane.normal));
  if (n.y < 0.0f) n = -n;
  const float tilt = std::acos(std::clamp(n.y, -1.0f, 1.0f)) * 57.29578f;
  level_ = rotationBetween(n, {0, 1, 0}) * level_;
  const glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(level_)));
  project_.level[0] = q.w;
  project_.level[1] = q.x;
  project_.level[2] = q.y;
  project_.level[3] = q.z;
  selectionDirty_ = true;
  char buf[128];
  std::snprintf(buf, sizeof(buf), tr("Levelled: the selected floor was tilted %.2f deg (undo: Reset level)"), tilt);
  status_ = buf;
}

// ------------------------------------------------------------------ measurements, notes, views
void App::addMeasurementPoint(const glm::vec3& displayPoint) {
  measureDraft_.push_back(glm::dvec3(toScene(displayPoint)));
  const bool twoPoint = measureKind_ == MeasurementRecord::Kind::Distance || measureKind_ == MeasurementRecord::Kind::Height;
  if (twoPoint && measureDraft_.size() == 2) finishMeasurement();
}

void App::finishMeasurement() {
  const size_t need = measureKind_ == MeasurementRecord::Kind::Area ? 3 : 2;
  if (measureDraft_.size() < need) {
    measureDraft_.clear();
    return;
  }
  MeasurementRecord m;
  m.kind = measureKind_;
  m.points = measureDraft_;
  m.name = std::string(tr(kindLabel(m.kind))) + " " + std::to_string(project_.measurements.size() + 1);
  project_.measurements.push_back(m);
  measureDraft_.clear();
  const glm::dvec3 up = glm::dvec3(glm::inverse(glm::mat3(modelMatrix())) * glm::vec3(0, 1, 0));
  char buf[160];
  std::snprintf(buf, sizeof(buf), tr("%s: %.4f %s%s"), m.name.c_str(), measurementRealValue(m, project_.unitScale, up),
                project_.unit.c_str(), m.kind == MeasurementRecord::Kind::Area ? "^2" : "");
  status_ = buf;
}

ViewBookmark App::currentView(const std::string& name) const {
  ViewBookmark b;
  b.name = name;
  b.eye = toScene(camera_.position());
  b.target = toScene(camera_.target);
  b.fovY = camera_.fovY;
  return b;
}

void App::goToBookmark(const ViewBookmark& b) {
  OrbitCamera end = camera_;
  end.lookAt(toDisplay(b.eye), toDisplay(b.target));
  end.fovY = b.fovY;
  animateCameraTo(end, 0.5f);  // a short flight keeps the user oriented
}

void App::addAnnotation(const glm::vec3& displayPoint) {
  Annotation a;
  a.position = toScene(displayPoint);
  a.title = std::string(tr("Note")) + " " + std::to_string(project_.annotations.size() + 1);
  project_.bookmarks.push_back(currentView(a.title));
  a.bookmark = static_cast<int>(project_.bookmarks.size()) - 1;
  project_.annotations.push_back(a);
  editAnnotation_ = static_cast<int>(project_.annotations.size()) - 1;
  std::snprintf(noteTitle_, sizeof(noteTitle_), tr("%s"), a.title.c_str());
  noteText_[0] = 0;
  status_ = a.title + " pinned: type its text in Tools > Notes";
}

// ------------------------------------------------------------------ project file
std::string App::defaultProjectPath() const {
  if (scenePath_.empty()) return "project.gsvproj";
  const std::filesystem::path p = pathFromUtf8(scenePath_);
  return pathToUtf8(p.parent_path() / (pathToUtf8(p.stem()) + ".gsvproj"));
}

void App::saveProjectTo(const std::string& path) {
  const std::filesystem::path dir = pathFromUtf8(path).parent_path();
  auto relative = [&](const std::string& file) {
    if (file.empty()) return file;
    std::error_code ec;
    const std::filesystem::path r = std::filesystem::relative(pathFromUtf8(file), dir.empty() ? "." : dir, ec);
    return ec || r.empty() ? file : pathToUtf8(r);  // other drive: keep it absolute
  };
  project_.scenePath = relative(scenePath_);
  project_.lasPath = relative(options_.las);
  project_.hasLasTransform = hasCloudTransform_ && options_.hasLasTransform;
  if (project_.hasLasTransform) std::copy(options_.lasTransform, options_.lasTransform + 8, project_.lasTransform);
  std::string error;
  if (saveProject(path, project_, error)) {
    markProjectSaved();
    status_ = std::string(tr("Project saved: ")) + path;
    // Measurements as CSV next to the project (spreadsheets, reports).
    if (!project_.measurements.empty()) {
      std::filesystem::path csv = pathFromUtf8(path);
      csv.replace_extension(".measurements.csv");
      std::ofstream f(csv, std::ios::binary);
      f << measurementsToCsv(project_.measurements, project_.unit, project_.unitScale);
    }
  } else {
    status_ = std::string(tr("Project save failed: ")) + error;
  }
}

void App::openProject(const std::string& path) {
  Project p;
  std::string error;
  if (!loadProject(path, p, error)) {
    status_ = std::string(tr("Cannot open project: ")) + error;
    return;
  }
  const std::filesystem::path dir = pathFromUtf8(path).parent_path();
  auto resolve = [&](const std::string& file) {
    if (file.empty()) return file;
    const std::filesystem::path f = pathFromUtf8(file);
    return f.is_absolute() ? file : pathToUtf8(dir / f);
  };
  const std::string scene = resolve(p.scenePath), las = resolve(p.lasPath);
  project_ = p;
  level_ = glm::mat4_cast(glm::quat(static_cast<float>(p.level[0]), static_cast<float>(p.level[1]),
                                    static_cast<float>(p.level[2]), static_cast<float>(p.level[3])));
  std::snprintf(projectPath_, sizeof(projectPath_), tr("%s"), path.c_str());
  measureDraft_.clear();
  editAnnotation_ = -1;
  if (!scene.empty() && scene != scenePath_) loadSceneFile(scene);
  if (!las.empty() && las != options_.las) {
    if (p.hasLasTransform) {
      const double* t = p.lasTransform;
      cloudTransform_ = GeoFrame::fromQuaternion(glm::dquat(t[1], t[2], t[3], t[4]), {t[5], t[6], t[7]}, t[0]);
      hasCloudTransform_ = true;
      options_.hasLasTransform = true;
      std::copy(t, t + 8, options_.lasTransform);
    }
    options_.las = las;
    loadCloudFile(las);
  }
  char opened[200];
  std::snprintf(opened, sizeof(opened), tr("Project opened: %zu measurement(s), %zu note(s), %zu view(s)"), p.measurements.size(),
                p.annotations.size(), p.bookmarks.size());
  status_ = opened;
  markProjectSaved();
}

// ------------------------------------------------------------------ input
void App::handleToolKeys(bool ctrl, bool shift, bool alt) {
  for (int k = 0; k < static_cast<int>(Tool::Count); ++k)
    if (keyPressed(GLFW_KEY_1 + k) && !ctrl) setTool(static_cast<Tool>(k));
  if (keyPressed(GLFW_KEY_ESCAPE)) {
    if (gizmoActive_ >= 0) {  // cancel a gizmo drag: nothing was applied yet
      splatSettings_.previewActive = false;
      if (history_.liveActive()) history_.cancel(scene_), livePreviewUpload(true);
      gizmoMove_ = glm::vec3(0.0f);
      gizmoAngle_ = 0.0f;
      gizmoActive_ = -1;
      status_ = tr("Transform cancelled");
    } else if (history_.liveActive()) finishLiveEdit(false);
    else if (!measureDraft_.empty()) measureDraft_.clear();
    else if (toolDragging_) toolDragging_ = false;
    else setTool(Tool::Navigate);
  }
  const bool del = keyPressed(GLFW_KEY_DELETE);
  if (del) deleteSelection();
  if (keyPressed(GLFW_KEY_BACKSPACE) && !measureDraft_.empty()) measureDraft_.pop_back();
  const bool enter = keyPressed(GLFW_KEY_ENTER) | keyPressed(GLFW_KEY_KP_ENTER);
  if (enter) {
    if (!measureDraft_.empty()) finishMeasurement();
    else finishLiveEdit(true);
  }
  if (keyPressed(GLFW_KEY_H)) alt ? unhideAll() : hideSelection();
  const bool a = keyPressed(GLFW_KEY_A), d = keyPressed(GLFW_KEY_D), i = keyPressed(GLFW_KEY_I),
             s = keyPressed(GLFW_KEY_S), o = keyPressed(GLFW_KEY_O);
  if (ctrl && a) setSelection(selectAll(scene_), SelectOp::Replace, "Select all");
  if (ctrl && d) setSelection(SplatMask(scene_.size(), 0), SelectOp::Replace, "Select none");
  if (ctrl && i) {
    SplatMask inv = selected_;
    invert(inv);
    setSelection(inv, SelectOp::Replace, "Invert");
  }
  if (ctrl && s) {
    if (projectPath_[0]) saveProjectTo(projectPath_);
    else showSaveProjectDialog();
  }
  if (ctrl && o) showOpenDialog(0);
  if (!ctrl && i) isolateSelection();
  if (keyPressed(GLFW_KEY_V) && !ctrl) {
    visibleOnly_ = !visibleOnly_;
    status_ = visibleOnly_ ? tr("Visible only: on") : tr("Visible only: off (select through)");
  }
  const bool growKey = keyPressed(GLFW_KEY_EQUAL) | keyPressed(GLFW_KEY_KP_ADD);
  const bool shrinkKey = keyPressed(GLFW_KEY_MINUS) | keyPressed(GLFW_KEY_KP_SUBTRACT);
  if (growKey && !ctrl) growOrShrink(true);
  if (shrinkKey && !ctrl) growOrShrink(false);
  const bool grow = keyPressed(GLFW_KEY_RIGHT_BRACKET), shrink = keyPressed(GLFW_KEY_LEFT_BRACKET);
  if (grow || shrink) {
    const float f = grow ? 1.25f : 0.8f;
    if (tool_ == Tool::Sphere) sphereRadius_ = std::clamp(sphereRadius_ * f, 0.002f, 2.0f);
    else brushRadiusPx_ = std::clamp(brushRadiusPx_ * f, 4.0f, 400.0f);
  }
  (void)shift;
}

void App::handleToolInput(bool lmb, double x, double y, bool clicked, bool shift, bool alt) {
  const glm::vec2 m(static_cast<float>(x), static_cast<float>(y));
  int winW, winH;
  glfwGetWindowSize(window_, &winW, &winH);
  const glm::vec2 viewport(static_cast<float>(winW), static_cast<float>(winH));
  if (previewPending_) livePreviewUpload(false);

  // Finished auto-clean job: its result becomes the selection to review.
  if (cleanJob_.valid() && cleanJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    SplatMask r = cleanJob_.get();
    const double s = nowSeconds() - cleanJobStart_;
    if (r.size() == scene_.size() && cleanJobSize_ == scene_.size()) {
      setSelection(r, SelectOp::Replace, nullptr);
      char buf[200];
      std::snprintf(buf, sizeof(buf), tr("Auto-clean: %zu floater candidates selected in %.1f s. Review, then Delete"), selectedCount_, s);
      status_ = buf;
    } else {
      status_ = tr("Auto-clean: the scene changed meanwhile, result dropped");
    }
  }
  if (scene_.empty()) return;
  const float focus = std::max(scene_.focusRadius, 1e-6f);

  switch (tool_) {
    case Tool::Rect:
    case Tool::Lasso: {
      if (lmb && !toolDragging_) {
        toolDragging_ = true;
        dragStart_ = dragLast_ = m;
        dragOp_ = opFromModifiers(shift, alt);
        lasso_.assign(1, m);
      } else if (lmb && toolDragging_) {
        dragLast_ = m;
        if (tool_ == Tool::Lasso && glm::length(m - lasso_.back()) >= 3.0f) lasso_.push_back(m);
      } else if (!lmb && toolDragging_) {
        toolDragging_ = false;
        const bool tiny = glm::length(dragLast_ - dragStart_) < 3.0f && lasso_.size() < 3;
        if (tiny) {  // a click on empty space with Replace clears the selection
          if (dragOp_ == SelectOp::Replace) setSelection(SplatMask(scene_.size(), 0), SelectOp::Replace, "Selection cleared");
        } else {
          SplatMask m = tool_ == Tool::Rect ? selectScreenRect(scene_, screenMvp(), viewport, dragStart_, dragLast_)
                                            : selectScreenPolygon(scene_, screenMvp(), viewport, lasso_);
          applyVisibleOnly(m);  // optional: drop what is hidden behind a nearer surface
          setSelection(m, dragOp_, tool_ == Tool::Rect ? "Rect" : "Lasso");
        }
        lasso_.clear();
      }
      break;
    }
    case Tool::Brush: {
      if (lmb && !toolDragging_) {
        finishLiveEdit(true);
        toolDragging_ = true;
        dragOp_ = opFromModifiers(shift, alt);
        if (dragOp_ == SelectOp::Intersect) dragOp_ = SelectOp::Add;  // painting an intersection is not useful
        dragBase_ = selected_;
        if (dragBase_.size() != scene_.size()) dragBase_.assign(scene_.size(), 0);
        if (dragOp_ == SelectOp::Replace) std::fill(dragBase_.begin(), dragBase_.end(), 0);
        dragStroke_.assign(scene_.size(), 0);
        dragLast_ = m + glm::vec2(1e6f);
        strokeDepth_ = visibleOnly_ ? renderSplatDepth() : DepthImage{};  // one depth image per stroke
      }
      if (lmb && glm::length(m - dragLast_) >= 2.0f) {
        dragLast_ = m;
        SplatMask dab = selectScreenCircle(scene_, screenMvp(), viewport, m, brushRadiusPx_);
        applyVisibleOnly(dab);
        combineSelection(dragStroke_, dab, SelectOp::Add);
        selected_ = dragBase_;
        combineSelection(selected_, dragStroke_, dragOp_ == SelectOp::Subtract ? SelectOp::Subtract : SelectOp::Add);
        for (size_t i = 0; i < selected_.size(); ++i)
          if (hidden_[i]) selected_[i] = 0;
        selectionChanged();
      }
      if (!lmb && toolDragging_) {
        toolDragging_ = false;
        strokeDepth_ = DepthImage{};
        status_ = std::string(tr("Brush")) + ": " + std::to_string(selectedCount_) + " " + tr("splats selected");
      }
      break;
    }
    case Tool::Sphere: {
      // The surface under the cursor, re-picked when the mouse moved (picking costs a few ms).
      const bool moved = std::abs(x - hoverX_) + std::abs(y - hoverY_) >= 2.0;
      if (moved && (lmb || nowSeconds() - lastHoverPick_ > 0.08)) {
        hover_ = pickSurface(x, y);
        hoverX_ = x;
        hoverY_ = y;
        lastHoverPick_ = nowSeconds();
        if (lmb && hover_) {
          if (!toolDragging_) {
            finishLiveEdit(true);
            toolDragging_ = true;
            dragOp_ = opFromModifiers(shift, alt);
            if (dragOp_ == SelectOp::Intersect) dragOp_ = SelectOp::Add;
            if (dragOp_ == SelectOp::Replace) std::fill(selected_.begin(), selected_.end(), 0);
          }
          const SplatMask ball = selectSphere(scene_, modelMatrix(), *hover_, sphereRadius_ * focus);
          combineSelection(selected_, ball, dragOp_ == SelectOp::Subtract ? SelectOp::Subtract : SelectOp::Add);
          for (size_t i = 0; i < selected_.size(); ++i)
            if (hidden_[i]) selected_[i] = 0;
          selectionChanged();
        }
      }
      if (!lmb && toolDragging_) {
        toolDragging_ = false;
        status_ = std::string(tr("Sphere brush")) + ": " + std::to_string(selectedCount_) + " " + tr("splats selected");
      }
      break;
    }
    case Tool::Flood: {
      if (!clicked) break;
      const auto p = pickSurface(x, y);
      if (!p) {
        status_ = tr("Flood: no surface under the cursor");
        break;
      }
      const auto t0 = std::chrono::steady_clock::now();
      const KdTree& tree = editTree();
      const uint32_t seed = nearestSplat(scene_, toScene(*p));
      const float gap = floodGap_ * (spacing_ > 0.0f ? spacing_ : 0.003f * focus);
      const SplatMask flood = floodRadius_ > 0.0f ? selectConnectedWithin(scene_, seed, gap, floodColor_, floodRadius_ * focus, &tree)
                                                  : selectConnected(scene_, seed, gap, floodColor_, 0.1f, 5000000, &tree);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      setSelection(flood, opFromModifiers(shift, alt), nullptr);
      char buf[160];
      std::snprintf(buf, sizeof(buf), tr("Flood: %zu splats selected (%.0f ms). Shift adds, Alt subtracts"), selectedCount_, ms);
      status_ = buf;
      break;
    }
    case Tool::Transform: {
      if (selectedCount_ == 0) break;
      if (pivotDirty_ && !history_.liveActive()) {
        gizmoPivot_ = toDisplay(selectionCentroid(scene_, selected_));
        pivotDirty_ = false;
      }
      const glm::mat4 vp = camera_.projection(viewport.x / std::max(viewport.y, 1.0f)) * camera_.view();
      auto screen = [&](const glm::vec3& p, glm::vec2& out) {
        const glm::vec4 c = vp * glm::vec4(p, 1.0f);
        if (c.w <= 1e-5f) return false;
        out = {(c.x / c.w * 0.5f + 0.5f) * viewport.x, (0.5f - c.y / c.w * 0.5f) * viewport.y};
        return true;
      };
      const glm::vec3 center = gizmoPivot_ + gizmoMove_;
      const float depth = std::max(glm::dot(center - camera_.position(), camera_.forward()), 1e-4f);
      const float focal = camera_.focalPixels(viewport.y);
      const float L = 90.0f * depth / focal;  // ~90 pixels long whatever the zoom
      glm::vec2 c2;
      if (!screen(center, c2)) break;
      // Hit test (only when not dragging).
      if (gizmoActive_ < 0) {
        gizmoHot_ = -1;
        float best = 8.0f;
        for (int a = 0; a < 3; ++a) {
          glm::vec3 e(0.0f);
          e[a] = 1.0f;
          glm::vec2 tip;
          if (screen(center + e * L, tip)) {
            const float d = distanceToSegment(m, c2, tip);
            if (d < best) best = d, gizmoHot_ = a;
          }
        }
        if (std::max(std::abs(m.x - c2.x), std::abs(m.y - c2.y)) < 8.0f) gizmoHot_ = 3;
        constexpr int kSeg = 48;
        glm::vec2 prev;
        bool havePrev = false;
        for (int s = 0; s <= kSeg; ++s) {
          const float t = 6.2831853f * s / kSeg;
          glm::vec2 q;
          const bool ok = screen(center + 0.8f * L * glm::vec3(std::cos(t), 0.0f, std::sin(t)), q);
          if (ok && havePrev && distanceToSegment(m, prev, q) < best && gizmoHot_ != 3) {
            best = distanceToSegment(m, prev, q);
            gizmoHot_ = 4;
          }
          prev = q;
          havePrev = ok;
        }
      }
      auto ringAngle = [&](float& angle) {  // angle of the cursor around the pivot in the horizontal plane
        const glm::mat4 inv = glm::inverse(vp);
        const glm::vec2 ndc(m.x / viewport.x * 2.0f - 1.0f, 1.0f - m.y / viewport.y * 2.0f);
        glm::vec4 a = inv * glm::vec4(ndc, -1.0f, 1.0f), b = inv * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 o = glm::vec3(a) / a.w, d = glm::normalize(glm::vec3(b) / b.w - o);
        if (std::abs(d.y) < 1e-4f) return false;
        const float t = (gizmoPivot_.y + gizmoMove_.y - o.y) / d.y;
        if (t <= 0.0f) return false;
        const glm::vec3 h = o + t * d - center;
        angle = std::atan2(h.z, h.x);
        return true;
      };
      if (lmb && gizmoActive_ < 0 && gizmoHot_ >= 0) {  // start a drag
        gizmoActive_ = gizmoHot_;
        dragStart_ = m;
        if (gizmoActive_ == 4 && !ringAngle(gizmoStartAngle_)) gizmoActive_ = -1;
        dragBase_.clear();
        numMove_[0] = numMove_[1] = numMove_[2] = 0.0f;
      }
      if (lmb && gizmoActive_ >= 0) {
        const glm::vec2 d = m - dragStart_;
        static glm::vec3 startMove;
        if (dragBase_.empty()) {  // first frame of the drag: remember where the gizmo was
          startMove = gizmoMove_;
          dragBase_.assign(1, 1);
        }
        if (gizmoActive_ < 3) {
          glm::vec3 e(0.0f);
          e[gizmoActive_] = 1.0f;
          glm::vec2 tip;
          if (screen(gizmoPivot_ + startMove + e * L, tip)) {
            glm::vec2 c0;
            screen(gizmoPivot_ + startMove, c0);
            const glm::vec2 s = tip - c0;
            const float len2 = std::max(glm::dot(s, s), 1.0f);
            gizmoMove_ = startMove + e * (glm::dot(d, s) / len2 * L);
          }
        } else if (gizmoActive_ == 3) {
          gizmoMove_ = startMove + (camera_.right() * d.x - camera_.up() * d.y) * (depth / focal);
        } else {
          float a;
          if (ringAngle(a)) gizmoAngle_ = -(a - gizmoStartAngle_);
        }
        liveTransformPreview(false);
      }
      if (!lmb && gizmoActive_ >= 0) {  // end of the drag: one undo step
        liveTransformPreview(true);
        finishLiveEdit(true);  // (points view path) commits the live command
        gizmoMove_ = glm::vec3(0.0f);
        gizmoAngle_ = 0.0f;
        gizmoScale_ = 1.0f;
        gizmoActive_ = -1;
        pivotDirty_ = true;
        status_ = tr("Transform applied (Ctrl+Z to undo)");
      }
      break;
    }
    case Tool::Measure:
    case Tool::Annotate: {
      if (!clicked) break;
      double ms = 0.0;
      const auto p = pickSurface(x, y, &ms);
      if (!p) {
        status_ = tr("No surface under the cursor");
        break;
      }
      if (tool_ == Tool::Measure) addMeasurementPoint(*p);
      else addAnnotation(*p);
      break;
    }
    default: break;
  }
}

// ------------------------------------------------------------------ drawing
// Small vector icons drawn with ImGui's draw list (no icon font needed, crisp at any DPI).
static void drawToolIcon(ImDrawList* dl, Tool t, ImVec2 c, float r, ImU32 col) {
  const float w = std::max(1.5f, r * 0.16f);
  auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
  switch (t) {
    case Tool::Navigate:  // four arrows
      dl->AddLine(P(-0.8f, 0), P(0.8f, 0), col, w);
      dl->AddLine(P(0, -0.8f), P(0, 0.8f), col, w);
      for (int k = 0; k < 4; ++k) {
        const float sx = k == 0 ? 1.f : k == 1 ? -1.f : 0.f, sy = k == 2 ? 1.f : k == 3 ? -1.f : 0.f;
        const ImVec2 tip = P(0.85f * sx, 0.85f * sy);
        dl->AddTriangleFilled(tip, P(0.5f * sx - 0.25f * sy, 0.5f * sy - 0.25f * sx), P(0.5f * sx + 0.25f * sy, 0.5f * sy + 0.25f * sx), col);
      }
      break;
    case Tool::Rect:  // dashed rectangle
      for (int k = 0; k < 4; ++k) {
        const ImVec2 corners[5] = {P(-0.8f, -0.6f), P(0.8f, -0.6f), P(0.8f, 0.6f), P(-0.8f, 0.6f), P(-0.8f, -0.6f)};
        const ImVec2 a = corners[k], b = corners[k + 1];
        for (int d = 0; d < 4; ++d) {
          const float t0 = d / 4.0f, t1 = t0 + 0.14f;
          dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0), ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1), col, w);
        }
      }
      break;
    case Tool::Lasso: {  // free loop with a tail
      const ImVec2 pts[] = {P(-0.7f, -0.1f), P(-0.4f, -0.6f), P(0.3f, -0.7f), P(0.8f, -0.3f), P(0.6f, 0.2f), P(-0.1f, 0.3f), P(-0.6f, 0.1f)};
      dl->AddPolyline(pts, 7, col, ImDrawFlags_Closed, w);
      dl->AddBezierCubic(P(-0.6f, 0.1f), P(-0.7f, 0.5f), P(-0.3f, 0.6f), P(-0.5f, 0.9f), col, w);
      break;
    }
    case Tool::Brush:  // circle with a dot
      dl->AddCircle(c, 0.75f * r, col, 24, w);
      dl->AddCircleFilled(c, 0.18f * r, col);
      break;
    case Tool::Sphere:  // globe
      dl->AddCircle(c, 0.8f * r, col, 24, w);
      dl->AddEllipse(c, ImVec2(0.8f * r, 0.3f * r), col, 0.0f, 24, w);
      dl->AddEllipse(c, ImVec2(0.3f * r, 0.8f * r), col, 0.0f, 24, w);
      break;
    case Tool::Flood:  // magic wand
      dl->AddLine(P(-0.8f, 0.8f), P(0.2f, -0.2f), col, w * 1.4f);
      for (int k = 0; k < 4; ++k) {
        const float a = k * 0.785398f;
        dl->AddLine(P(0.45f + 0.15f * std::cos(a), -0.45f + 0.15f * std::sin(a)), P(0.45f + 0.45f * std::cos(a), -0.45f + 0.45f * std::sin(a)), col, w);
        dl->AddLine(P(0.45f - 0.15f * std::cos(a), -0.45f - 0.15f * std::sin(a)), P(0.45f - 0.45f * std::cos(a), -0.45f - 0.45f * std::sin(a)), col, w);
      }
      break;
    case Tool::Transform:  // three axes
      dl->AddLine(P(-0.5f, 0.5f), P(0.8f, 0.5f), IM_COL32(230, 80, 80, 255), w);
      dl->AddLine(P(-0.5f, 0.5f), P(-0.5f, -0.8f), IM_COL32(100, 210, 100, 255), w);
      dl->AddLine(P(-0.5f, 0.5f), P(0.3f, -0.2f), IM_COL32(90, 140, 240, 255), w);
      dl->AddCircleFilled(P(-0.5f, 0.5f), 0.14f * r, col);
      break;
    case Tool::Measure:  // ruler
      dl->AddLine(P(-0.85f, 0.35f), P(0.85f, 0.35f), col, w);
      for (int k = 0; k <= 6; ++k) {
        const float x = -0.85f + k * (1.7f / 6.0f);
        dl->AddLine(P(x, 0.35f), P(x, k % 3 == 0 ? -0.35f : 0.0f), col, w);
      }
      break;
    case Tool::Annotate:  // pin
      dl->AddCircleFilled(P(0, -0.35f), 0.4f * r, col);
      dl->AddLine(P(0, 0.05f), P(0, 0.85f), col, w * 1.3f);
      break;
    default: break;
  }
}

void App::drawToolbar() {
  ImGuiIO& io = ImGui::GetIO();
  const float s = std::max(appliedUiScale_, 1.0f);
  // Centred in the free area right of the side panel, so the panel never covers it.
  const float left = std::min(panelRight_, io.DisplaySize.x * 0.5f);
  ImGui::SetNextWindowPos(ImVec2(0.5f * (left + io.DisplaySize.x), menuBarHeight_ + 6.0f * s), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.88f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;
  if (ImGui::Begin("##toolbar", nullptr, flags)) {
    const float bs = 34.0f * s;
    for (int k = 0; k < static_cast<int>(Tool::Count); ++k) {
      if (k) ImGui::SameLine();
      if (k == 1 || k == 6) {  // groups: navigate | select | edit & annotate
        ImGui::TextDisabled("|");
        ImGui::SameLine();
      }
      const bool active = static_cast<int>(tool_) == k;
      if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.1f, 1.0f));
      ImGui::PushID(k);
      if (ImGui::Button("##tool", ImVec2(bs, bs))) setTool(static_cast<Tool>(k));
      ImGui::PopID();
      if (active) ImGui::PopStyleColor();
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      drawToolIcon(ImGui::GetWindowDrawList(), static_cast<Tool>(k), ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                   bs * 0.32f, IM_COL32(235, 235, 235, 255));
      char key[4];
      std::snprintf(key, sizeof(key), "%d", k + 1);
      ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + 3.0f, a.y + 1.0f), IM_COL32(200, 200, 200, 150), key);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (%s %d)\n%s", tr(kTools[k].name), tr("key"), k + 1, tr(kTools[k].help));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.canUndo());
    if (ImGui::Button(trId("Undo"), ImVec2(0, bs))) undoEdit();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Ctrl+Z  %s", history_.canUndo() ? tr(history_.undoLabel().c_str()) : "");
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.canRedo());
    if (ImGui::Button(trId("Redo"), ImVec2(0, bs))) redoEdit();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Ctrl+Y  %s", history_.canRedo() ? tr(history_.redoLabel().c_str()) : "");
    if (selectedCount_ || hiddenCount_) {
      ImGui::SameLine();
      ImGui::AlignTextToFramePadding();
      ImGui::Text(tr("%zu selected"), selectedCount_);
      if (hiddenCount_) {
        ImGui::SameLine();
        ImGui::TextDisabled(tr("%zu hidden"), hiddenCount_);
      }
    }
  }
  ImGui::End();
}

void App::drawToolOverlay(const RenderView& v) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  int winW, winH;
  glfwGetWindowSize(window_, &winW, &winH);
  const glm::mat4 vp = v.projection * v.view;
  const glm::mat4 svp = vp * modelMatrix();  // scene coordinates
  auto toScreen = [&](const glm::mat4& m, const glm::vec3& p, ImVec2& out) {
    const glm::vec4 c = m * glm::vec4(p, 1.0f);
    if (c.w <= 1e-6f) return false;
    out = ImVec2((c.x / c.w * 0.5f + 0.5f) * winW, (1.0f - (c.y / c.w * 0.5f + 0.5f)) * winH);
    return true;
  };
  double mx, my;
  glfwGetCursorPos(window_, &mx, &my);
  const ImVec2 mouse(static_cast<float>(mx), static_cast<float>(my));
  const bool overUi = ImGui::GetIO().WantCaptureMouse;

  // Selection shapes being drawn.
  const ImU32 orange = IM_COL32(255, 160, 40, 255), orangeFill = IM_COL32(255, 160, 40, 40);
  if (toolDragging_ && tool_ == Tool::Rect) {
    const ImVec2 a(dragStart_.x, dragStart_.y), b(dragLast_.x, dragLast_.y);
    dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), orangeFill);
    dl->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), orange, 0.0f, 0, 1.5f);
  }
  if (toolDragging_ && tool_ == Tool::Lasso && lasso_.size() > 1) {
    std::vector<ImVec2> pts;
    for (const glm::vec2& p : lasso_) pts.emplace_back(p.x, p.y);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), orange, ImDrawFlags_Closed, 1.5f);
  }
  if (tool_ == Tool::Brush && !overUi) dl->AddCircle(mouse, brushRadiusPx_, orange, 48, 1.5f);
  if (tool_ == Tool::Sphere && hover_ && !overUi) {
    // Projected radius of the ball at the hover point.
    ImVec2 c;
    if (toScreen(vp, *hover_, c)) {
      const float depth = std::max(glm::dot(*hover_ - camera_.position(), camera_.forward()), 1e-4f);
      const float r = sphereRadius_ * scene_.focusRadius * camera_.focalPixels(static_cast<float>(winH)) / depth;
      dl->AddCircle(c, r, orange, 48, 1.5f);
      dl->AddCircleFilled(c, 2.5f, orange);
    }
  }

  // Transform gizmo.
  if (tool_ == Tool::Transform && selectedCount_ > 0 && !pivotDirty_) {
    const glm::vec3 center = gizmoPivot_ + gizmoMove_;
    const float depth = std::max(glm::dot(center - camera_.position(), camera_.forward()), 1e-4f);
    const float L = 90.0f * depth / camera_.focalPixels(static_cast<float>(winH));
    ImVec2 c;
    if (toScreen(vp, center, c)) {
      const ImU32 axisColor[3] = {IM_COL32(230, 70, 70, 255), IM_COL32(90, 210, 90, 255), IM_COL32(80, 130, 240, 255)};
      const int hot = gizmoActive_ >= 0 ? gizmoActive_ : gizmoHot_;
      constexpr int kSeg = 48;
      std::vector<ImVec2> ring;
      for (int s = 0; s <= kSeg; ++s) {
        const float t = 6.2831853f * s / kSeg;
        ImVec2 q;
        if (toScreen(vp, center + 0.8f * L * glm::vec3(std::cos(t), 0.0f, std::sin(t)), q)) ring.push_back(q);
      }
      dl->AddPolyline(ring.data(), static_cast<int>(ring.size()), hot == 4 ? IM_COL32(255, 255, 120, 255) : IM_COL32(230, 200, 60, 200),
                      0, hot == 4 ? 3.0f : 1.5f);
      for (int a = 0; a < 3; ++a) {
        glm::vec3 e(0.0f);
        e[a] = 1.0f;
        ImVec2 tip;
        if (!toScreen(vp, center + e * L, tip)) continue;
        dl->AddLine(c, tip, hot == a ? IM_COL32(255, 255, 255, 255) : axisColor[a], hot == a ? 4.0f : 2.5f);
        dl->AddCircleFilled(tip, 5.0f, axisColor[a]);
      }
      dl->AddRect(ImVec2(c.x - 7, c.y - 7), ImVec2(c.x + 7, c.y + 7), hot == 3 ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 200), 0.0f, 0, 2.0f);
    }
  }

  // Measurements (finished and the one being drawn), in scene coordinates.
  const glm::dvec3 up = glm::dvec3(glm::inverse(glm::mat3(modelMatrix())) * glm::vec3(0, 1, 0));
  const ImU32 yellow = IM_COL32(255, 220, 40, 255);
  auto drawMeasurement = [&](const std::vector<glm::dvec3>& pts, MeasurementRecord::Kind kind, const char* name, bool draft) {
    std::vector<ImVec2> sp;
    for (const glm::dvec3& p : pts) {
      ImVec2 s;
      if (!toScreen(svp, glm::vec3(p), s)) return;
      sp.push_back(s);
    }
    const ImU32 col = draft ? IM_COL32(255, 255, 255, 230) : yellow;
    if (kind == MeasurementRecord::Kind::Area && sp.size() >= 3) dl->AddConvexPolyFilled(sp.data(), static_cast<int>(sp.size()), IM_COL32(255, 220, 40, 40));
    if (sp.size() >= 2)
      dl->AddPolyline(sp.data(), static_cast<int>(sp.size()), col, kind == MeasurementRecord::Kind::Area ? ImDrawFlags_Closed : 0, 2.0f);
    for (const ImVec2& s : sp) dl->AddCircleFilled(s, 4.5f, col);
    const size_t need = kind == MeasurementRecord::Kind::Area ? 3 : 2;
    if (pts.size() >= need) {
      MeasurementRecord r;
      r.kind = kind;
      r.points = pts;
      char buf[128];
      std::snprintf(buf, sizeof(buf), tr("%s%s%.3f %s%s"), name, name[0] ? ": " : "", measurementRealValue(r, project_.unitScale, up),
                    project_.unit.c_str(), kind == MeasurementRecord::Kind::Area ? "^2" : "");
      ImVec2 at(0, 0);
      for (const ImVec2& s : sp) at = ImVec2(at.x + s.x / sp.size(), at.y + s.y / sp.size());
      dl->AddRectFilled(ImVec2(at.x + 6, at.y - 2), ImVec2(at.x + 12 + ImGui::CalcTextSize(buf).x, at.y + 16), IM_COL32(0, 0, 0, 160), 3.0f);
      dl->AddText(ImVec2(at.x + 9, at.y), col, buf);
    }
  };
  for (const MeasurementRecord& m : project_.measurements) drawMeasurement(m.points, m.kind, m.name.c_str(), false);
  if (!measureDraft_.empty()) {
    std::vector<glm::dvec3> pts = measureDraft_;
    drawMeasurement(pts, measureKind_, "", true);
    ImVec2 last;
    if (toScreen(svp, glm::vec3(pts.back()), last) && !overUi) dl->AddLine(last, mouse, IM_COL32(255, 255, 255, 120), 1.0f);
  }

  // Notes (pins).
  for (size_t i = 0; i < project_.annotations.size(); ++i) {
    const Annotation& a = project_.annotations[i];
    ImVec2 s;
    if (!toScreen(svp, a.position, s)) continue;
    const bool sel = static_cast<int>(i) == editAnnotation_;
    dl->AddLine(s, ImVec2(s.x, s.y - 18), IM_COL32(255, 255, 255, 220), 1.5f);
    dl->AddCircleFilled(ImVec2(s.x, s.y - 22), 6.0f, sel ? IM_COL32(255, 160, 40, 255) : IM_COL32(60, 170, 255, 255));
    const ImVec2 tp(s.x + 10, s.y - 30);
    dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 1), ImVec2(tp.x + ImGui::CalcTextSize(a.title.c_str()).x + 3, tp.y + 15), IM_COL32(0, 0, 0, 160), 3.0f);
    dl->AddText(tp, IM_COL32(255, 255, 255, 255), a.title.c_str());
    if (!overUi && std::abs(mouse.x - s.x) < 10 && std::abs(mouse.y - (s.y - 22)) < 10 && !a.text.empty())
      ImGui::SetTooltip(tr("%s\n%s"), a.title.c_str(), a.text.c_str());
  }
}

// ------------------------------------------------------------------ panels
void App::drawToolPanel() {
  if (!ImGui::CollapsingHeader(trId("Tools"), ImGuiTreeNodeFlags_DefaultOpen)) return;
  ImGui::TextWrapped("%s: %s", tr(kTools[static_cast<int>(tool_)].name), tr(kTools[static_cast<int>(tool_)].help));

  // ---- selection actions
  ImGui::SeparatorText(tr("Selection"));
  ImGui::Text(tr("%zu selected, %zu hidden, %zu total"), selectedCount_, hiddenCount_, scene_.size());
  if (ImGui::Button(trId("All"))) setSelection(selectAll(scene_), SelectOp::Replace, "Select all");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Ctrl+A"));
  ImGui::SameLine();
  if (ImGui::Button(trId("None"))) setSelection(SplatMask(scene_.size(), 0), SelectOp::Replace, "Select none");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Ctrl+D"));
  ImGui::SameLine();
  if (ImGui::Button(trId("Invert"))) {
    SplatMask inv = selected_;
    invert(inv);
    setSelection(inv, SelectOp::Replace, "Invert");
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Ctrl+I"));
  ImGui::SameLine();
  ImGui::BeginDisabled(selectedCount_ == 0);
  if (ImGui::Button(trId("Delete"))) deleteSelection();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Delete key"));
  ImGui::SameLine();
  if (ImGui::Button(trId("Hide"))) hideSelection();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("H"));
  ImGui::SameLine();
  if (ImGui::Button(trId("Isolate"))) isolateSelection();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("I: hide everything else"));
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(hiddenCount_ == 0);
  if (ImGui::Button(trId("Show all"))) unhideAll();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Alt+H"));
  ImGui::EndDisabled();
  ImGui::BeginDisabled(selectedCount_ == 0);
  if (ImGui::Button(trId("Grow"))) growOrShrink(true);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("= : add one layer of neighbours"));
  ImGui::SameLine();
  if (ImGui::Button(trId("Shrink"))) growOrShrink(false);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("- : remove the boundary layer"));
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::Checkbox(trId("Visible only (V)"), &visibleOnly_);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", tr("Rect, Lasso and Brush skip splats hidden behind a nearer surface.\nOff: select through everything (e.g. floaters behind a wall)."));
  ImGui::ColorEdit3(trId("Highlight"), &splatSettings_.highlight.x, ImGuiColorEditFlags_NoInputs);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120);
  ImGui::SliderFloat("##hl", &splatSettings_.highlight.w, 0.1f, 1.0f, "mix %.2f");

  // ---- per-tool settings
  if (tool_ == Tool::Brush) ImGui::SliderFloat(trId("Brush radius (px)"), &brushRadiusPx_, 4.0f, 400.0f, "%.0f");
  if (tool_ == Tool::Sphere) ImGui::SliderFloat(trId("Ball radius"), &sphereRadius_, 0.002f, 1.0f, "%.3f x scene", ImGuiSliderFlags_Logarithmic);
  if (tool_ == Tool::Flood) {
    ImGui::SliderFloat(trId("Max gap"), &floodGap_, 1.0f, 30.0f, "%.1f x spacing", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Larger: jumps over holes, may leak into touching objects"));
    ImGui::SliderFloat(trId("Colour tolerance"), &floodColor_, 0.02f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("1.0 = ignore colour (geometry only)"));
    ImGui::SliderFloat(trId("Max radius"), &floodRadius_, 0.0f, 1.0f, floodRadius_ > 0.0f ? "%.2f x scene" : "-", ImGuiSliderFlags_None);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Only splats this close to the click (0 = no limit): stops leaks into the room"));
    if (editTreeValid_) ImGui::TextDisabled(tr("typical spacing %.4f scene units"), spacing_);
  }

  // ---- auto clean
  ImGui::SeparatorText(tr("Auto-clean floaters"));
  ImGui::SetNextItemWidth(140);
  ImGui::SliderFloat(trId("Strictness"), &floaterRatio_, 1.0f, 5.0f, "%.1f sigma");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lower = more splats flagged. A splat is flagged when its 16 nearest\n"
                                                "neighbours are farther than mean + this x std of the scene."));
  if (!colmap_.images.empty()) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt(trId("Min views"), &floaterViews_, 1, 10);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Also flag splats seen by fewer training cameras than this"));
  }
  ImGui::BeginDisabled(cleanJob_.valid() || scene_.empty());
  if (ImGui::Button(cleanJob_.valid() ? trId("Searching...") : trId("Find floaters (select for review)"))) autoCleanFloaters();
  ImGui::EndDisabled();
  if (cleanJob_.valid()) {
    ImGui::SameLine();
    if (ImGui::Button(trId("Cancel"))) {
      abandonedCleans_.push_back(std::move(cleanJob_));  // finishes in the background, result dropped
      status_ = tr("Auto-clean cancelled");
    }
  }

  // ---- transform
  if (ImGui::TreeNode(trId("Transform selection"))) {
    ImGui::BeginDisabled(selectedCount_ == 0);
    ImGui::DragFloat3(trId("Move"), numMove_, 0.002f * std::max(scene_.focusRadius, 1e-3f));
    ImGui::DragFloat3(trId("Rotate (deg)"), numRotate_, 0.25f, -360.0f, 360.0f);
    ImGui::DragFloat(trId("Scale"), &numScale_, 0.002f, 0.01f, 100.0f);
    if (ImGui::Button(trId("Apply"))) {
      finishLiveEdit(true);
      if (pivotDirty_) gizmoPivot_ = toDisplay(selectionCentroid(scene_, selected_));
      const glm::vec3 pv = gizmoPivot_;
      glm::mat4 R(1.0f);
      R = glm::rotate(R, glm::radians(numRotate_[1]), glm::vec3(0, 1, 0));
      R = glm::rotate(R, glm::radians(numRotate_[0]), glm::vec3(1, 0, 0));
      R = glm::rotate(R, glm::radians(numRotate_[2]), glm::vec3(0, 0, 1));
      const glm::mat4 T = glm::translate(glm::mat4(1.0f), pv + glm::vec3(numMove_[0], numMove_[1], numMove_[2])) * R *
                          glm::scale(glm::mat4(1.0f), glm::vec3(numScale_)) * glm::translate(glm::mat4(1.0f), -pv);
      const glm::mat4 M = modelMatrix();
      const glm::mat4 sceneT = glm::inverse(M) * T * M;
      history_.modify(scene_, selected_, "Transform", [&](GaussianScene& s, const SplatMask& m) { transformSplats(s, m, sceneT); });
      livePreviewUpload(true);
      pivotDirty_ = true;
      numMove_[0] = numMove_[1] = numMove_[2] = 0.0f;
      numRotate_[0] = numRotate_[1] = numRotate_[2] = 0.0f;
      numScale_ = 1.0f;
      status_ = tr("Transform applied (Ctrl+Z to undo)");
    }
    ImGui::SameLine();
    if (ImGui::Button(trId("Duplicate"))) {
      // Appends a copy of the selected splats and selects the copy (move it away with the gizmo).
      finishLiveEdit(true);
      const size_t n0 = scene_.size();
      const size_t added = history_.duplicate(scene_, selected_, "Duplicate");
      SplatMask copy(scene_.size(), 0);
      std::fill(copy.begin() + static_cast<std::ptrdiff_t>(n0), copy.end(), 1);
      uploadScene();  // resizes selected_ / hidden_
      selected_ = copy;
      selectionChanged();
      char buf[200];
      std::snprintf(buf, sizeof(buf), tr("Duplicated %zu splats: the copy is selected (Transform tool to move it)"), added);
      status_ = buf;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Copy the selected splats (undoable)"));
    ImGui::EndDisabled();
    ImGui::TreePop();
  }

  // ---- colour
  if (ImGui::TreeNode(trId("Colour (selection, or all when nothing is selected)"))) {
    bool changed = ImGui::SliderFloat(trId("Brightness"), &colorAdjust_.brightness, -0.5f, 0.5f, "%+.3f");
    changed |= ImGui::SliderFloat(trId("Contrast"), &colorAdjust_.contrast, 0.0f, 2.0f, "%.3f");
    changed |= ImGui::SliderFloat(trId("Saturation"), &colorAdjust_.saturation, 0.0f, 2.0f, "%.3f");
    changed |= ImGui::SliderFloat(trId("Temperature"), &colorAdjust_.temperature, -0.3f, 0.3f, "%+.3f");
    if (changed) {
      const SplatMask target = selectedCount_ ? selected_ : SplatMask(scene_.size(), 1);
      if (!history_.liveActive()) history_.begin(scene_, target, "Colour");
      history_.restore(scene_);
      adjustColor(scene_, target, colorAdjust_);
      livePreviewUpload(false);
    }
    ImGui::BeginDisabled(!history_.liveActive());
    if (ImGui::Button(trId("Apply colour"))) finishLiveEdit(true);
    ImGui::SameLine();
    if (ImGui::Button(trId("Cancel"))) finishLiveEdit(false);
    ImGui::EndDisabled();
    ImGui::TreePop();
  }

  // ---- scale & level
  if (ImGui::TreeNode(trId("Scale & level"))) {
    ImGui::TextWrapped("%s", tr("Real size: measure a known length (Measure > Distance), pick it here and type its real length."));
    std::vector<int> distances;
    for (size_t i = 0; i < project_.measurements.size(); ++i)
      if (project_.measurements[i].kind == MeasurementRecord::Kind::Distance) distances.push_back(static_cast<int>(i));
    if (scaleSource_ >= static_cast<int>(project_.measurements.size())) scaleSource_ = -1;
    const char* preview = scaleSource_ >= 0 ? project_.measurements[static_cast<size_t>(scaleSource_)].name.c_str() : "(choose)";
    if (ImGui::BeginCombo(trId("Reference"), preview)) {
      for (int i : distances)
        if (ImGui::Selectable(project_.measurements[static_cast<size_t>(i)].name.c_str(), i == scaleSource_)) scaleSource_ = i;
      ImGui::EndCombo();
    }
    ImGui::InputFloat(trId("Real length (m)"), &realLength_, 0.01f, 0.1f, "%.4f");
    ImGui::BeginDisabled(scaleSource_ < 0 || realLength_ <= 0.0f);
    if (ImGui::Button(trId("Set scale"))) {
      const MeasurementRecord& r = project_.measurements[static_cast<size_t>(scaleSource_)];
      project_.unitScale = scaleForKnownLength(r.points.front(), r.points.back(), realLength_);
      project_.unit = "m";
      char buf[128];
      std::snprintf(buf, sizeof(buf), tr("Scale set: 1 scene unit = %.5f m"), project_.unitScale);
      status_ = buf;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(trId("Reset scale"))) {
      project_.unitScale = 1.0;
      project_.unit = "units";
    }
    ImGui::Text(tr("1 scene unit = %.5f %s"), project_.unitScale, project_.unit.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("%s", tr("Level: select part of the floor (Brush / Flood), then press Level."));
    if (ImGui::Button(trId("Level from selection"))) levelFromSelection();
    ImGui::SameLine();
    if (ImGui::Button(trId("Reset level"))) {
      level_ = glm::mat4(1.0f);
      project_.level[0] = 1;
      project_.level[1] = project_.level[2] = project_.level[3] = 0;
    }
    ImGui::TreePop();
  }

  // ---- measurements
  if (ImGui::TreeNode(trId("Measurements"))) {
    int kind = static_cast<int>(measureKind_);
    const char* kinds[] = {tr("Distance"), tr("Polyline"), tr("Area"), tr("Height")};
    if (ImGui::Combo(trId("Type"), &kind, kinds, 4)) {
      measureKind_ = static_cast<MeasurementRecord::Kind>(kind);
      measureDraft_.clear();
    }
    if (tool_ != Tool::Measure && ImGui::Button(trId("Start measuring (M)"))) setTool(Tool::Measure);
    if (!measureDraft_.empty()) {
      ImGui::Text(tr("%zu point(s)"), measureDraft_.size());
      ImGui::SameLine();
      if (ImGui::Button(trId("Finish (Enter)"))) finishMeasurement();
      ImGui::SameLine();
      if (ImGui::Button(trId("Cancel (Esc)"))) measureDraft_.clear();
    }
    const glm::dvec3 up = glm::dvec3(glm::inverse(glm::mat3(modelMatrix())) * glm::vec3(0, 1, 0));
    int remove = -1;
    for (size_t i = 0; i < project_.measurements.size(); ++i) {
      const MeasurementRecord& m = project_.measurements[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::Text("%-12s %10.4f %s%s", m.name.c_str(), measurementRealValue(m, project_.unitScale, up), project_.unit.c_str(),
                  m.kind == MeasurementRecord::Kind::Area ? "^2" : "");
      ImGui::SameLine();
      if (ImGui::SmallButton(trId("x"))) remove = static_cast<int>(i);
      ImGui::PopID();
    }
    if (remove >= 0) project_.measurements.erase(project_.measurements.begin() + remove);
    ImGui::BeginDisabled(project_.measurements.empty());
    if (ImGui::Button(trId("Copy CSV"))) ImGui::SetClipboardText(measurementsToCsv(project_.measurements, project_.unit, project_.unitScale).c_str());
    ImGui::SameLine();
    if (ImGui::Button(trId("Clear all"))) project_.measurements.clear();
    ImGui::EndDisabled();
    ImGui::TreePop();
  }
}

void App::drawProjectPanel() {
  if (!ImGui::CollapsingHeader(trId("Project, notes & views"))) return;
  // ---- notes
  ImGui::SeparatorText(tr("Notes (tool 9)"));
  int remove = -1;
  for (size_t i = 0; i < project_.annotations.size(); ++i) {
    Annotation& a = project_.annotations[i];
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(a.title.c_str(), editAnnotation_ == static_cast<int>(i), 0, ImVec2(180, 0))) {
      editAnnotation_ = static_cast<int>(i);
      std::snprintf(noteTitle_, sizeof(noteTitle_), tr("%s"), a.title.c_str());
      std::snprintf(noteText_, sizeof(noteText_), tr("%s"), a.text.c_str());
      if (a.bookmark >= 0 && a.bookmark < static_cast<int>(project_.bookmarks.size()))
        goToBookmark(project_.bookmarks[static_cast<size_t>(a.bookmark)]);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(trId("x"))) remove = static_cast<int>(i);
    ImGui::PopID();
  }
  if (remove >= 0) {
    project_.annotations.erase(project_.annotations.begin() + remove);
    editAnnotation_ = -1;
  }
  if (editAnnotation_ >= 0 && editAnnotation_ < static_cast<int>(project_.annotations.size())) {
    Annotation& a = project_.annotations[static_cast<size_t>(editAnnotation_)];
    if (ImGui::InputText(trId("Title"), noteTitle_, sizeof(noteTitle_))) a.title = noteTitle_;
    if (ImGui::InputTextMultiline(trId("Text"), noteText_, sizeof(noteText_), ImVec2(-1, 60))) a.text = noteText_;
  }
  // ---- views
  ImGui::SeparatorText(tr("Saved views"));
  int removeView = -1;
  for (size_t i = 0; i < project_.bookmarks.size(); ++i) {
    ImGui::PushID(1000 + static_cast<int>(i));
    if (ImGui::Button(project_.bookmarks[i].name.c_str())) goToBookmark(project_.bookmarks[i]);
    ImGui::SameLine();
    if (ImGui::SmallButton(trId("x"))) removeView = static_cast<int>(i);
    ImGui::PopID();
  }
  if (removeView >= 0) {
    project_.bookmarks.erase(project_.bookmarks.begin() + removeView);
    for (Annotation& a : project_.annotations) {  // keep note -> view links valid
      if (a.bookmark == removeView) a.bookmark = -1;
      else if (a.bookmark > removeView) --a.bookmark;
    }
  }
  if (ImGui::Button(trId("Save current view"))) project_.bookmarks.push_back(currentView(std::string(tr("Saved view")) + " " + std::to_string(project_.bookmarks.size() + 1)));
  // ---- project file
  ImGui::SeparatorText(tr("Project file (.gsvproj)"));
  if (projectPath_[0] == 0) std::snprintf(projectPath_, sizeof(projectPath_), tr("%s"), defaultProjectPath().c_str());
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##proj", projectPath_, sizeof(projectPath_));
  if (ImGui::Button(trId("Save project (Ctrl+S)"))) saveProjectTo(projectPath_);
  ImGui::SameLine();
  if (ImGui::Button(trId("Open project"))) openProject(projectPath_);
  ImGui::TextDisabled("%s", tr("Keeps scene/LAS paths, scale, level, measurements, notes, views.\n"
                      "Measurements are also written as <name>.measurements.csv."));
}

}  // namespace gsv
