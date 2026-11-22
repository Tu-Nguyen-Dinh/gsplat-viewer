#pragma once
// Core 5: multi-level undo / redo for scene edits. See docs/core/CORE5-editing.md.
//
// Commands store only what they change: for a modification, the before/after values of the
// affected splats; for a removal, the removed splats and their indices. Undo restores them exactly
// (bit-exact). The oldest commands are dropped when the stored bytes exceed the budget.

#include "scene/edit_ops.h"
#include "scene/gaussian_scene.h"
#include "scene/scene_edit.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gsv {

class EditHistory {
 public:
  explicit EditHistory(size_t byteBudget = size_t(768) << 20) : budget_(byteBudget) {}
  ~EditHistory();
  EditHistory(const EditHistory&) = delete;
  EditHistory& operator=(const EditHistory&) = delete;

  // Large commands (deleting millions of splats: ~240 B each) keep only their indices in memory;
  // the splat values go to a file in `dir` and are read back on undo / redo. Off when `dir` is empty.
  void setSpill(const std::string& dir, size_t thresholdBytes = size_t(32) << 20);
  size_t spilledBytes() const { return spilledBytes_; }  // on disk

  // Applies `op` to the selected splats and records it. `op` must only change splats in `mask`
  // (positions, scales, rotations, opacity, colours), never add or remove splats.
  void modify(GaussianScene& scene, const SplatMask& mask, const std::string& label,
              const std::function<void(GaussianScene&, const SplatMask&)>& op);
  // Removes the selected splats (keeping the order of the others) and records it.
  // Returns the number removed. Bounds and focus are recomputed.
  size_t remove(GaussianScene& scene, const SplatMask& mask, const std::string& label);
  // Appends a copy of the selected splats at the end of the scene (Duplicate) and records it.
  // Returns the number added; the copies are the last ones.
  size_t duplicate(GaussianScene& scene, const SplatMask& mask, const std::string& label);

  // Live edit (drag a gizmo, move a colour slider): begin() saves the selected splats, every
  // preview calls restore() and re-applies the TOTAL change to the saved values (no drift from
  // accumulating small steps), commit() records one command, cancel() puts the saved values back.
  void begin(const GaussianScene& scene, const SplatMask& mask, const std::string& label);
  void restore(GaussianScene& scene) const;
  bool commit(GaussianScene& scene);
  void cancel(GaussianScene& scene);
  bool liveActive() const { return live_.has_value(); }

  bool undo(GaussianScene& scene);
  bool redo(GaussianScene& scene);
  bool canUndo() const { return cursor_ > 0; }
  bool canRedo() const { return cursor_ < commands_.size(); }
  std::string undoLabel() const { return canUndo() ? commands_[cursor_ - 1].label : std::string(); }
  std::string redoLabel() const { return canRedo() ? commands_[cursor_].label : std::string(); }
  void clear();
  size_t size() const { return commands_.size(); }
  size_t cursor() const { return cursor_; }  // commands [0, cursor) are applied
  const std::string& label(size_t i) const { return commands_[i].label; }
  size_t bytes() const { return bytes_; }
  // Changes on every applied, undone, redone or dropped command: "is the scene the one I saved?"
  // compares revisions (a counter, never reused; undoing back to a saved state is NOT treated as clean).
  uint64_t revision() const { return revision_; }

 private:
  struct Records {  // values of some splats (structure of arrays)
    std::vector<glm::vec3> position, logScale, shDC;
    std::vector<glm::vec4> rotation;
    std::vector<float> opacityLogit, shRest;
    size_t bytes() const;
  };
  struct Command {
    enum class Kind { Modify, Remove, Add } kind = Kind::Modify;
    std::string label;
    std::vector<uint32_t> indices;  // ascending
    Records before, after;          // Remove / Add: `before` = the removed / added splats
    size_t bytes = 0;               // in memory
    std::string spillPath;          // non-empty: before / after are in this file
    size_t spillBytes = 0;
  };
  void spill(Command& c);
  void unspill(const Command& c, Records& before, Records& after) const;
  void dropFile(Command& c);
  static Records capture(const GaussianScene& s, const std::vector<uint32_t>& idx);
  static void write(GaussianScene& s, const std::vector<uint32_t>& idx, const Records& r);
  static void removeIndices(GaussianScene& s, const std::vector<uint32_t>& idx);
  static void insertIndices(GaussianScene& s, const std::vector<uint32_t>& idx, const Records& r);
  void push(Command&& c);

  std::optional<Command> live_;
  std::deque<Command> commands_;
  size_t cursor_ = 0;
  size_t bytes_ = 0;
  size_t budget_;
  uint64_t revision_ = 0;
  std::string spillDir_;
  size_t spillThreshold_ = 0;
  size_t spilledBytes_ = 0;
  uint64_t spillSerial_ = 0;
};

}  // namespace gsv
