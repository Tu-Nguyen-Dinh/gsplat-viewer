#include "scene/scene_io.h"

#include "scene/ply_io.h"
#include "scene/spz_io.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace gsv {

namespace {

constexpr size_t kSplatRecord = 32;

template <typename T>
bool allFinite(const T& v) {
  for (int i = 0; i < T::length(); ++i)
    if (!std::isfinite(v[i])) return false;
  return true;
}

uint8_t toU8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f))); }

}  // namespace

bool loadSplat(const std::filesystem::path& path, GaussianScene& out, std::string& error) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    error = "Cannot open file";
    return false;
  }
  const auto bytes = static_cast<size_t>(f.tellg());
  if (bytes == 0 || bytes % kSplatRecord != 0) {
    error = "Invalid .splat file: size is not a multiple of 32 bytes";
    return false;
  }
  std::vector<uint8_t> data(bytes);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(bytes));

  const size_t n = bytes / kSplatRecord;
  GaussianScene s;
  s.position.resize(n);
  s.logScale.resize(n);
  s.rotation.resize(n);
  s.opacityLogit.resize(n);
  s.shDC.resize(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* r = data.data() + i * kSplatRecord;
    float f6[6];
    std::memcpy(f6, r, sizeof(f6));
    s.position[i] = {f6[0], f6[1], f6[2]};
    // Scale is linear in .splat; our scene stores log(scale) like the PLY format.
    for (int k = 0; k < 3; ++k) s.logScale[i][k] = std::log(std::max(f6[3 + k], 1e-8f));
    const uint8_t* rgba = r + 24;
    s.shDC[i] = (glm::vec3(rgba[0], rgba[1], rgba[2]) / 255.0f - 0.5f) / kShC0;
    s.opacityLogit[i] = logit(std::clamp(rgba[3] / 255.0f, 1e-4f, 1.0f - 1e-4f));
    const uint8_t* q = r + 28;  // [0,255] -> [-1,1]
    s.rotation[i] = glm::vec4(q[0], q[1], q[2], q[3]) / 255.0f * 2.0f - 1.0f;
  }
  out = std::move(s);
  return true;
}

bool saveSplat(const std::filesystem::path& path, const GaussianScene& s, std::string& error) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    error = "Cannot create file";
    return false;
  }
  for (size_t i = 0; i < s.size(); ++i) {
    uint8_t r[kSplatRecord];
    const glm::vec3 scale = glm::exp(s.logScale[i]);
    const float f6[6] = {s.position[i].x, s.position[i].y, s.position[i].z, scale.x, scale.y, scale.z};
    std::memcpy(r, f6, sizeof(f6));
    const glm::vec3 c = baseColor(s, i) * 255.0f;
    r[24] = toU8(c.r);
    r[25] = toU8(c.g);
    r[26] = toU8(c.b);
    r[27] = toU8(sigmoid(s.opacityLogit[i]) * 255.0f);
    const glm::vec4 q = glm::normalize(s.rotation[i]);
    for (int k = 0; k < 4; ++k) r[28 + k] = toU8((q[k] * 0.5f + 0.5f) * 255.0f);
    f.write(reinterpret_cast<const char*>(r), sizeof(r));
  }
  if (!f) {
    error = "Write failed";
    return false;
  }
  return true;
}

size_t removeInvalidSplats(GaussianScene& s) {
  const size_t n = s.size();
  const size_t restPerSplat = static_cast<size_t>(s.shRestPerChannel) * 3;
  size_t w = 0;
  for (size_t i = 0; i < n; ++i) {
    const bool ok = allFinite(s.position[i]) && allFinite(s.logScale[i]) && allFinite(s.rotation[i]) &&
                    std::isfinite(s.opacityLogit[i]) && allFinite(s.shDC[i]) && glm::length(s.rotation[i]) > 1e-8f;
    if (!ok) continue;
    if (w != i) {  // compact in place, keeping the order
      s.position[w] = s.position[i];
      s.logScale[w] = s.logScale[i];
      s.rotation[w] = s.rotation[i];
      s.opacityLogit[w] = s.opacityLogit[i];
      s.shDC[w] = s.shDC[i];
      if (restPerSplat)
        std::copy_n(s.shRest.begin() + static_cast<std::ptrdiff_t>(i * restPerSplat), restPerSplat,
                    s.shRest.begin() + static_cast<std::ptrdiff_t>(w * restPerSplat));
    }
    ++w;
  }
  s.position.resize(w);
  s.logScale.resize(w);
  s.rotation.resize(w);
  s.opacityLogit.resize(w);
  s.shDC.resize(w);
  s.shRest.resize(w * restPerSplat);
  return n - w;
}

LoadResult loadScene(const std::filesystem::path& path) {
  const auto t0 = std::chrono::steady_clock::now();
  LoadResult r;
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (ext == ".ply") {
    r.ok = loadPly(path, r.scene, r.error);
  } else if (ext == ".splat") {
    r.ok = loadSplat(path, r.scene, r.error);
  } else if (ext == ".spz") {
    r.ok = loadSpz(path, r.scene, r.error);
  } else {
    r.error = "Unsupported file type '" + ext + "' (use .ply, .splat or .spz; convert others with splat-transform)";
  }
  if (r.ok) {
    r.removedInvalid = removeInvalidSplats(r.scene);
    if (r.scene.empty()) {
      r.ok = false;
      r.error = "File contains no valid splats";
    } else {
      computeRobustBounds(r.scene);
      const std::u8string name = path.filename().u8string();
      r.scene.sourceName.assign(name.begin(), name.end());
    }
  }
  r.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return r;
}

std::future<LoadResult> loadSceneAsync(const std::filesystem::path& path) {
  return std::async(std::launch::async, [path] { return loadScene(path); });
}

}  // namespace gsv
