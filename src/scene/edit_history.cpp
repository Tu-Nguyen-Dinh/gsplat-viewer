#include "scene/edit_history.h"

#include "core/utf8_path.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <algorithm>
#include <cstddef>

namespace gsv {

namespace {
template <typename T>
void writeVec(std::ofstream& f, const std::vector<T>& v) {
  const uint64_t n = v.size();
  f.write(reinterpret_cast<const char*>(&n), sizeof(n));
  if (n) f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(n * sizeof(T)));
}
template <typename T>
bool readVec(std::ifstream& f, std::vector<T>& v) {
  uint64_t n = 0;
  if (!f.read(reinterpret_cast<char*>(&n), sizeof(n)) || n > (uint64_t(1) << 34) / sizeof(T)) return false;
  v.resize(static_cast<size_t>(n));
  return n == 0 || static_cast<bool>(f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * sizeof(T))));
}
}  // namespace

EditHistory::~EditHistory() { clear(); }

void EditHistory::setSpill(const std::string& dir, size_t thresholdBytes) {
  spillDir_ = dir;
  spillThreshold_ = thresholdBytes;
  if (!dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(pathFromUtf8(dir), ec);
  }
}

void EditHistory::spill(Command& c) {
  const size_t data = c.before.bytes() + c.after.bytes();
  if (spillDir_.empty() || data < spillThreshold_) return;
  char name[48];
  std::snprintf(name, sizeof(name), "step_%llu.bin", static_cast<unsigned long long>(++spillSerial_));
  const std::string path = pathToUtf8(pathFromUtf8(spillDir_) / name);
  std::ofstream f(pathFromUtf8(path), std::ios::binary | std::ios::trunc);
  if (!f) return;  // cannot write: keep it in memory
  for (const Records* r : {&c.before, &c.after}) {
    writeVec(f, r->position);
    writeVec(f, r->logScale);
    writeVec(f, r->shDC);
    writeVec(f, r->rotation);
    writeVec(f, r->opacityLogit);
    writeVec(f, r->shRest);
  }
  f.flush();
  if (!f) {
    f.close();
    std::error_code ec;
    std::filesystem::remove(pathFromUtf8(path), ec);
    return;
  }
  c.spillPath = path;
  c.spillBytes = data;
  spilledBytes_ += data;
  c.before = Records{};
  c.after = Records{};
}

void EditHistory::unspill(const Command& c, Records& before, Records& after) const {
  std::ifstream f(pathFromUtf8(c.spillPath), std::ios::binary);
  for (Records* r : {&before, &after}) {
    const bool ok = readVec(f, r->position) && readVec(f, r->logScale) && readVec(f, r->shDC) && readVec(f, r->rotation) &&
                    readVec(f, r->opacityLogit) && readVec(f, r->shRest);
    if (!ok) throw std::runtime_error("undo data file is missing or damaged: " + c.spillPath);
  }
}

void EditHistory::dropFile(Command& c) {
  if (c.spillPath.empty()) return;
  std::error_code ec;
  std::filesystem::remove(pathFromUtf8(c.spillPath), ec);
  spilledBytes_ -= c.spillBytes;
  c.spillPath.clear();
  c.spillBytes = 0;
}

size_t EditHistory::Records::bytes() const {
  return position.size() * sizeof(glm::vec3) * 3 + rotation.size() * sizeof(glm::vec4) +
         opacityLogit.size() * sizeof(float) + shRest.size() * sizeof(float);
}

EditHistory::Records EditHistory::capture(const GaussianScene& s, const std::vector<uint32_t>& idx) {
  Records r;
  const size_t rest = static_cast<size_t>(s.shRestPerChannel) * 3;
  r.position.reserve(idx.size());
  r.logScale.reserve(idx.size());
  r.shDC.reserve(idx.size());
  r.rotation.reserve(idx.size());
  r.opacityLogit.reserve(idx.size());
  r.shRest.reserve(idx.size() * rest);
  for (uint32_t i : idx) {
    r.position.push_back(s.position[i]);
    r.logScale.push_back(s.logScale[i]);
    r.shDC.push_back(s.shDC[i]);
    r.rotation.push_back(s.rotation[i]);
    r.opacityLogit.push_back(s.opacityLogit[i]);
    if (rest) r.shRest.insert(r.shRest.end(), s.shRest.begin() + static_cast<std::ptrdiff_t>(i * rest), s.shRest.begin() + static_cast<std::ptrdiff_t>((i + 1) * rest));
  }
  return r;
}

void EditHistory::write(GaussianScene& s, const std::vector<uint32_t>& idx, const Records& r) {
  const size_t rest = static_cast<size_t>(s.shRestPerChannel) * 3;
  for (size_t k = 0; k < idx.size(); ++k) {
    const uint32_t i = idx[k];
    s.position[i] = r.position[k];
    s.logScale[i] = r.logScale[k];
    s.shDC[i] = r.shDC[k];
    s.rotation[i] = r.rotation[k];
    s.opacityLogit[i] = r.opacityLogit[k];
    if (rest) std::copy(r.shRest.begin() + static_cast<std::ptrdiff_t>(k * rest), r.shRest.begin() + static_cast<std::ptrdiff_t>((k + 1) * rest), s.shRest.begin() + static_cast<std::ptrdiff_t>(i * rest));
  }
}

void EditHistory::removeIndices(GaussianScene& s, const std::vector<uint32_t>& idx) {
  const size_t rest = static_cast<size_t>(s.shRestPerChannel) * 3;
  size_t w = 0, k = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (k < idx.size() && idx[k] == i) {
      ++k;
      continue;
    }
    if (w != i) {
      s.position[w] = s.position[i];
      s.logScale[w] = s.logScale[i];
      s.shDC[w] = s.shDC[i];
      s.rotation[w] = s.rotation[i];
      s.opacityLogit[w] = s.opacityLogit[i];
      if (rest) std::copy(s.shRest.begin() + static_cast<std::ptrdiff_t>(i * rest), s.shRest.begin() + static_cast<std::ptrdiff_t>((i + 1) * rest), s.shRest.begin() + static_cast<std::ptrdiff_t>(w * rest));
    }
    ++w;
  }
  s.position.resize(w);
  s.logScale.resize(w);
  s.shDC.resize(w);
  s.rotation.resize(w);
  s.opacityLogit.resize(w);
  s.shRest.resize(w * rest);
}

void EditHistory::insertIndices(GaussianScene& s, const std::vector<uint32_t>& idx, const Records& r) {
  const size_t rest = static_cast<size_t>(s.shRestPerChannel) * 3;
  const size_t n = s.size() + idx.size();
  GaussianScene out;
  out.shDegree = s.shDegree;
  out.shRestPerChannel = s.shRestPerChannel;
  out.position.resize(n);
  out.logScale.resize(n);
  out.shDC.resize(n);
  out.rotation.resize(n);
  out.opacityLogit.resize(n);
  out.shRest.resize(n * rest);
  size_t src = 0, k = 0;
  for (size_t i = 0; i < n; ++i) {
    if (k < idx.size() && idx[k] == i) {
      out.position[i] = r.position[k];
      out.logScale[i] = r.logScale[k];
      out.shDC[i] = r.shDC[k];
      out.rotation[i] = r.rotation[k];
      out.opacityLogit[i] = r.opacityLogit[k];
      if (rest) std::copy(r.shRest.begin() + static_cast<std::ptrdiff_t>(k * rest), r.shRest.begin() + static_cast<std::ptrdiff_t>((k + 1) * rest), out.shRest.begin() + static_cast<std::ptrdiff_t>(i * rest));
      ++k;
    } else {
      out.position[i] = s.position[src];
      out.logScale[i] = s.logScale[src];
      out.shDC[i] = s.shDC[src];
      out.rotation[i] = s.rotation[src];
      out.opacityLogit[i] = s.opacityLogit[src];
      if (rest) std::copy(s.shRest.begin() + static_cast<std::ptrdiff_t>(src * rest), s.shRest.begin() + static_cast<std::ptrdiff_t>((src + 1) * rest), out.shRest.begin() + static_cast<std::ptrdiff_t>(i * rest));
      ++src;
    }
  }
  s.position = std::move(out.position);
  s.logScale = std::move(out.logScale);
  s.shDC = std::move(out.shDC);
  s.rotation = std::move(out.rotation);
  s.opacityLogit = std::move(out.opacityLogit);
  s.shRest = std::move(out.shRest);
}

void EditHistory::push(Command&& c) {
  // A new command discards the redo branch.
  while (commands_.size() > cursor_) {
    bytes_ -= commands_.back().bytes;
    dropFile(commands_.back());
    commands_.pop_back();
  }
  spill(c);
  c.bytes = c.indices.size() * sizeof(uint32_t) + c.before.bytes() + c.after.bytes();
  bytes_ += c.bytes;
  commands_.push_back(std::move(c));
  cursor_ = commands_.size();
  ++revision_;
  while (bytes_ > budget_ && commands_.size() > 1) {  // forget the oldest, keep at least one
    bytes_ -= commands_.front().bytes;
    dropFile(commands_.front());
    commands_.pop_front();
    --cursor_;
  }
}

void EditHistory::modify(GaussianScene& scene, const SplatMask& mask, const std::string& label,
                         const std::function<void(GaussianScene&, const SplatMask&)>& op) {
  Command c;
  c.kind = Command::Kind::Modify;
  c.label = label;
  for (uint32_t i = 0; i < scene.size() && i < mask.size(); ++i)
    if (mask[i]) c.indices.push_back(i);
  if (c.indices.empty()) return;
  c.before = capture(scene, c.indices);
  op(scene, mask);
  c.after = capture(scene, c.indices);
  push(std::move(c));
}

size_t EditHistory::remove(GaussianScene& scene, const SplatMask& mask, const std::string& label) {
  Command c;
  c.kind = Command::Kind::Remove;
  c.label = label;
  for (uint32_t i = 0; i < scene.size() && i < mask.size(); ++i)
    if (mask[i]) c.indices.push_back(i);
  if (c.indices.empty()) return 0;
  c.before = capture(scene, c.indices);
  removeIndices(scene, c.indices);
  computeRobustBounds(scene);
  const size_t n = c.indices.size();
  push(std::move(c));
  return n;
}

void EditHistory::begin(const GaussianScene& scene, const SplatMask& mask, const std::string& label) {
  Command c;
  c.kind = Command::Kind::Modify;
  c.label = label;
  for (uint32_t i = 0; i < scene.size() && i < mask.size(); ++i)
    if (mask[i]) c.indices.push_back(i);
  c.before = capture(scene, c.indices);
  live_ = std::move(c);
}

void EditHistory::restore(GaussianScene& scene) const {
  if (live_) write(scene, live_->indices, live_->before);
}

bool EditHistory::commit(GaussianScene& scene) {
  if (!live_) return false;
  Command c = std::move(*live_);
  live_.reset();
  if (c.indices.empty()) return false;
  c.after = capture(scene, c.indices);
  push(std::move(c));
  return true;
}

void EditHistory::cancel(GaussianScene& scene) {
  restore(scene);
  live_.reset();
}

size_t EditHistory::duplicate(GaussianScene& scene, const SplatMask& mask, const std::string& label) {
  std::vector<uint32_t> src;
  for (uint32_t i = 0; i < scene.size() && i < mask.size(); ++i)
    if (mask[i]) src.push_back(i);
  if (src.empty()) return 0;
  Command c;
  c.kind = Command::Kind::Add;
  c.label = label;
  c.before = capture(scene, src);
  const size_t n0 = scene.size();
  for (size_t k = 0; k < src.size(); ++k) c.indices.push_back(static_cast<uint32_t>(n0 + k));
  insertIndices(scene, c.indices, c.before);
  computeRobustBounds(scene);
  push(std::move(c));
  return src.size();
}

bool EditHistory::undo(GaussianScene& scene) {
  if (!canUndo()) return false;
  const Command& c = commands_[cursor_ - 1];
  Records fileBefore, fileAfter;
  const Records* before = &c.before;
  if (!c.spillPath.empty()) {
    try {
      unspill(c, fileBefore, fileAfter);
    } catch (const std::exception&) {
      return false;  // the data file is gone: the step cannot be undone (scene unchanged)
    }
    before = &fileBefore;
  }
  --cursor_;
  ++revision_;
  if (c.kind == Command::Kind::Modify) {
    write(scene, c.indices, *before);
  } else if (c.kind == Command::Kind::Remove) {
    insertIndices(scene, c.indices, *before);
    computeRobustBounds(scene);
  } else {
    removeIndices(scene, c.indices);
    computeRobustBounds(scene);
  }
  return true;
}

bool EditHistory::redo(GaussianScene& scene) {
  if (!canRedo()) return false;
  const Command& c = commands_[cursor_];
  Records fileBefore, fileAfter;
  const Records* before = &c.before;
  const Records* after = &c.after;
  if (!c.spillPath.empty() && c.kind != Command::Kind::Remove) {  // redoing a removal needs only indices
    try {
      unspill(c, fileBefore, fileAfter);
    } catch (const std::exception&) {
      return false;
    }
    before = &fileBefore;
    after = &fileAfter;
  }
  ++cursor_;
  ++revision_;
  if (c.kind == Command::Kind::Modify) {
    write(scene, c.indices, *after);
  } else if (c.kind == Command::Kind::Remove) {
    removeIndices(scene, c.indices);
    computeRobustBounds(scene);
  } else {
    insertIndices(scene, c.indices, *before);
    computeRobustBounds(scene);
  }
  return true;
}

void EditHistory::clear() {
  ++revision_;
  for (Command& c : commands_) dropFile(c);
  commands_.clear();
  cursor_ = 0;
  bytes_ = 0;
}

}  // namespace gsv
