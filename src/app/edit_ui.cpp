// Editing panel: remove floaters and crop a captured scene, then save it as a standard 3DGS PLY.
//
// The selection volume is defined in display space (after the model matrix that levels the
// scene), so a box stays aligned with the floor the user sees even for rotated COLMAP scenes.
// Every edit goes through history_ (scene/edit_history.h): multi-level undo / redo that stores only
// the splats an edit touched.
#include "app/app.h"
#include "core/i18n.h"
#include "core/utf8_path.h"
#include "scene/ply_io.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <cstdio>

namespace gsv {

void App::resetSelection() {
  selection_.center = glm::vec3(modelMatrix() * glm::vec4(scene_.focusCenter, 1.0f));
  selection_.halfExtent = glm::vec3(scene_.focusRadius);
  selection_.radius = scene_.focusRadius;
  selectionDirty_ = true;
}

void App::uploadScene() {
  setLodEnabled(false);  // edits change the scene the LOD tree was built from
  lodTree_ = {};
  points_.upload(scene_);
  splats_.upload(scene_);
  selectionDirty_ = true;
  selectionDirtyForFilter_ = true;
  editTreeValid_ = false;
  // Removing / restoring splats changes the indices: a selection would point at other splats.
  if (selected_.size() != scene_.size()) selected_.assign(scene_.size(), 0);
  if (hidden_.size() != scene_.size()) hidden_.assign(scene_.size(), 0);
  selectionChanged();
}

void App::applyRemoval(const SplatMask& mask, const char* what) {
  const size_t n = countSelected(mask);
  if (n == 0) {
    status_ = std::string(tr(what)) + tr(": nothing selected");
    return;
  }
  const auto t0 = std::chrono::steady_clock::now();
  finishLiveEdit(true);
  history_.remove(scene_, mask, what);
  selected_.clear();  // indices changed
  uploadScene();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  char buf[160];
  std::snprintf(buf, sizeof(buf), tr("%s: removed %zu splats, %zu left (%.0f ms)"), tr(what), n, scene_.size(), ms);
  status_ = buf;
}

void App::applyBatchEdits() {
  const Options& o = options_;
  if (o.editMinOpacity > 0.0f || o.editMaxSize > 0.0f) {
    applyRemoval(selectByFilter(scene_, o.editMinOpacity, o.editMaxSize), "Filter");
    std::printf("%s\n", status_.c_str());
  }
  if (o.hasCropSphere) {
    SelectionShape sphere;
    sphere.type = SelectionShape::Type::Sphere;
    sphere.center = {o.cropSphere[0], o.cropSphere[1], o.cropSphere[2]};
    sphere.radius = o.cropSphere[3];
    SplatMask outside = selectInside(scene_, sphere, modelMatrix());
    invert(outside);
    applyRemoval(outside, "Crop");
    std::printf("%s\n", status_.c_str());
  }
  if (!o.saveEdited.empty()) {
    std::string error;
    if (savePly(pathFromUtf8(o.saveEdited), scene_, error))
      std::printf("Saved %zu splats to %s\n", scene_.size(), o.saveEdited.c_str());
    else
      std::fprintf(stderr, "Save failed: %s\n", error.c_str());
  }
}

void App::undoEdit() {
  finishLiveEdit(true);
  if (!history_.canUndo()) {
    status_ = tr("Nothing to undo");
    return;
  }
  const std::string label = history_.undoLabel();
  history_.undo(scene_);
  uploadScene();
  status_ = std::string(tr("Undo")) + ": " + tr(label.c_str()) + " (" + std::to_string(scene_.size()) + " " + tr("splats") + ")";
}

void App::redoEdit() {
  finishLiveEdit(true);
  if (!history_.canRedo()) {
    status_ = tr("Nothing to redo");
    return;
  }
  const std::string label = history_.redoLabel();
  history_.redo(scene_);
  uploadScene();
  status_ = std::string(tr("Redo")) + ": " + tr(label.c_str()) + " (" + std::to_string(scene_.size()) + " " + tr("splats") + ")";
}

void App::drawEditPanel() {
  if (!ImGui::CollapsingHeader(trId("Crop & filter"))) {
    showSelection_ = false;
    return;
  }
  showSelection_ = true;

  // ---- Selection volume ----------------------------------------------------------
  int type = selection_.type == SelectionShape::Type::Box ? 0 : 1;
  bool changed = ImGui::RadioButton(trId("Box"), &type, 0);
  ImGui::SameLine();
  changed |= ImGui::RadioButton(trId("Sphere"), &type, 1);
  selection_.type = type == 0 ? SelectionShape::Type::Box : SelectionShape::Type::Sphere;
  ImGui::SameLine();
  if (ImGui::Button(trId("Reset to scene"))) resetSelection();
  const float speed = 0.005f * std::max(scene_.focusRadius, 1e-3f);
  changed |= ImGui::DragFloat3(trId("Center"), &selection_.center.x, speed);
  if (selection_.type == SelectionShape::Type::Box)
    changed |= ImGui::DragFloat3(trId("Half size"), &selection_.halfExtent.x, speed, 0.0f, 1e6f);
  else
    changed |= ImGui::DragFloat(trId("Radius"), &selection_.radius, speed, 0.0f, 1e6f);
  if (ImGui::Button(trId("Center on view target"))) {
    selection_.center = camera_.target;
    changed = true;
  }
  if (changed) selectionDirty_ = true;
  if (selectionDirty_) {  // ~5 ms for 2.5 M splats: only when the volume or the scene changed
    selectionCount_ = countSelected(selectInside(scene_, selection_, modelMatrix()));
    selectionDirty_ = false;
  }
  ImGui::Text(tr("%zu splats inside, %zu outside"), selectionCount_, scene_.size() - selectionCount_);

  if (ImGui::Button(trId("Delete inside"))) applyRemoval(selectInside(scene_, selection_, modelMatrix()), "Delete inside");
  ImGui::SameLine();
  if (ImGui::Button(trId("Crop (delete outside)"))) {
    SplatMask m = selectInside(scene_, selection_, modelMatrix());
    invert(m);
    applyRemoval(m, "Crop");
  }

  // ---- Filters --------------------------------------------------------------------
  ImGui::SeparatorText(tr("Filter floaters"));
  ImGui::TextDisabled("%s", tr("Nearly transparent or huge splats (typical floaters)"));
  bool filterChanged = ImGui::SliderFloat(trId("Min opacity"), &filterOpacity_, 0.0f, 0.5f, "%.3f");
  filterChanged |= ImGui::SliderFloat(trId("Max size"), &filterScale_, 0.0f, scene_.focusRadius,
                                      filterScale_ > 0.0f ? "%.3f" : "off");
  if (filterChanged || selectionDirtyForFilter_) {
    filterCount_ = countSelected(selectByFilter(scene_, filterOpacity_, filterScale_));
    selectionDirtyForFilter_ = false;
  }
  char label[96];
  std::snprintf(label, sizeof(label), tr("Delete %zu filtered splats"), filterCount_);
  if (ImGui::Button(label)) applyRemoval(selectByFilter(scene_, filterOpacity_, filterScale_), "Filter");

  // ---- Undo / save ----------------------------------------------------------------
  ImGui::Separator();
  ImGui::BeginDisabled(!history_.canUndo());
  if (ImGui::Button(history_.canUndo() ? ("Undo " + history_.undoLabel()).c_str() : "Undo")) undoEdit();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!history_.canRedo());
  if (ImGui::Button(history_.canRedo() ? ("Redo " + history_.redoLabel()).c_str() : "Redo")) redoEdit();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled(tr("%zu step(s), %.1f MB"), history_.size(), static_cast<double>(history_.bytes()) / 1e6);
  if (savePath_[0] == 0 && !scenePath_.empty()) {
    const std::filesystem::path p = pathFromUtf8(scenePath_);
    const std::string def = pathToUtf8(p.parent_path() / (pathToUtf8(p.stem()) + "_edited.ply"));
    std::snprintf(savePath_, sizeof(savePath_), "%s", def.c_str());
  }
  ImGui::SetNextItemWidth(-60);
  ImGui::InputText("##save", savePath_, sizeof(savePath_));
  ImGui::SameLine();
  ImGui::BeginDisabled(saveJob_.valid());
  if (ImGui::Button(saveJob_.valid() ? trId("Saving...") : trId("Save"))) startSceneSave(savePath_);  // background
  ImGui::EndDisabled();
}

// Wireframe of the selection volume with ImGui's background draw list (like the camera frustums).
void App::drawSelection() {
  if (!showSelection_) return;
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  if (size.x <= 0 || size.y <= 0) return;
  const glm::mat4 viewProj = camera_.projection(size.x / size.y) * camera_.view();  // display space
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const ImU32 color = IM_COL32(255, 170, 40, 230);
  auto line = [&](const glm::vec3& a, const glm::vec3& b) {
    glm::vec4 ca = viewProj * glm::vec4(a, 1.0f), cb = viewProj * glm::vec4(b, 1.0f);
    if (ca.w <= 1e-4f && cb.w <= 1e-4f) return;
    // Clip the segment against the near side (w > epsilon) so lines behind the eye do not flip.
    if (ca.w <= 1e-4f || cb.w <= 1e-4f) {
      const float t = (1e-4f - ca.w) / (cb.w - ca.w);
      (ca.w <= 1e-4f ? ca : cb) = ca + (cb - ca) * t;
    }
    auto toScreen = [&](const glm::vec4& c) {
      return ImVec2((c.x / c.w * 0.5f + 0.5f) * size.x, (0.5f - c.y / c.w * 0.5f) * size.y);
    };
    dl->AddLine(toScreen(ca), toScreen(cb), color, 1.5f);
  };
  const glm::vec3 c = selection_.center;
  if (selection_.type == SelectionShape::Type::Box) {
    const glm::vec3 h = selection_.halfExtent;
    for (int i = 0; i < 8; ++i)
      for (int axis = 0; axis < 3; ++axis) {
        if (i & (1 << axis)) continue;  // each edge once: from the corner with that bit clear
        const glm::vec3 a = c + h * glm::vec3((i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : -1);
        const int j = i | (1 << axis);
        const glm::vec3 b = c + h * glm::vec3((j & 1) ? 1 : -1, (j & 2) ? 1 : -1, (j & 4) ? 1 : -1);
        line(a, b);
      }
  } else {
    constexpr int kSegments = 48;
    for (int plane = 0; plane < 3; ++plane)
      for (int s = 0; s < kSegments; ++s) {
        const float a0 = 6.2831853f * s / kSegments, a1 = 6.2831853f * (s + 1) / kSegments;
        auto point = [&](float a) {
          glm::vec3 p(0.0f);
          p[plane] = std::cos(a) * selection_.radius;
          p[(plane + 1) % 3] = std::sin(a) * selection_.radius;
          return c + p;
        };
        line(point(a0), point(a1));
      }
  }
}

}  // namespace gsv
