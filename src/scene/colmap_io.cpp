#include "scene/colmap_io.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gsv {

int colmapModelParamCount(int model) {
  // SIMPLE_PINHOLE, PINHOLE, SIMPLE_RADIAL, RADIAL, OPENCV, OPENCV_FISHEYE, FULL_OPENCV, FOV,
  // SIMPLE_RADIAL_FISHEYE, RADIAL_FISHEYE, THIN_PRISM_FISHEYE
  static const int counts[] = {3, 4, 4, 5, 8, 8, 12, 5, 4, 5, 12};
  return model >= 0 && model < static_cast<int>(std::size(counts)) ? counts[model] : -1;
}

namespace {
int modelIdFromName(const std::string& name) {
  static const char* names[] = {"SIMPLE_PINHOLE", "PINHOLE",    "SIMPLE_RADIAL",         "RADIAL",
                                "OPENCV",         "OPENCV_FISHEYE", "FULL_OPENCV",        "FOV",
                                "SIMPLE_RADIAL_FISHEYE", "RADIAL_FISHEYE", "THIN_PRISM_FISHEYE"};
  for (int i = 0; i < static_cast<int>(std::size(names)); ++i)
    if (name == names[i]) return i;
  return -1;
}
}  // namespace

glm::dvec4 ColmapCamera::pinhole() const {
  // Models with a single focal length store (f, cx, cy, ...); the others (fx, fy, cx, cy, ...).
  const bool singleFocal = model == 0 || model == 2 || model == 3 || model == 8 || model == 9;
  if (params.size() < 3) return {0, 0, 0, 0};
  if (singleFocal) return {params[0], params[0], params[1], params[2]};
  if (params.size() < 4) return {0, 0, 0, 0};
  return {params[0], params[1], params[2], params[3]};
}

glm::mat4 ColmapImage::viewMatrixGl() const {
  const glm::dmat3 r = worldToCameraRotation();
  glm::dmat4 m(1.0);
  for (int c = 0; c < 3; ++c)
    for (int row = 0; row < 3; ++row) m[c][row] = r[c][row];
  m[3] = glm::dvec4(translation, 1.0);
  const glm::dmat4 flipYZ = glm::scale(glm::dmat4(1.0), glm::dvec3(1.0, -1.0, -1.0));
  return glm::mat4(flipYZ * m);
}

const ColmapCamera* ColmapModel::camera(uint32_t id) const {
  for (const ColmapCamera& c : cameras)
    if (c.id == id) return &c;
  return nullptr;
}

namespace {

// ---- binary -------------------------------------------------------------------------------------
template <typename T>
bool readRaw(std::istream& in, T& v) {
  return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(T)));
}

bool readCamerasBin(const std::filesystem::path& path, std::vector<ColmapCamera>& out, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  uint64_t count = 0;
  if (!in || !readRaw(in, count) || count > 100000) return error = "cannot read " + path.string(), false;
  for (uint64_t i = 0; i < count; ++i) {
    ColmapCamera c;
    int32_t model = 0;
    uint64_t w = 0, h = 0;
    if (!readRaw(in, c.id) || !readRaw(in, model) || !readRaw(in, w) || !readRaw(in, h))
      return error = "truncated cameras.bin", false;
    const int n = colmapModelParamCount(model);
    if (n < 0) return error = "unsupported COLMAP camera model " + std::to_string(model), false;
    c.model = model;
    c.width = static_cast<int>(w);
    c.height = static_cast<int>(h);
    c.params.resize(static_cast<size_t>(n));
    if (!in.read(reinterpret_cast<char*>(c.params.data()), static_cast<std::streamsize>(n * sizeof(double))))
      return error = "truncated cameras.bin", false;
    out.push_back(std::move(c));
  }
  return true;
}

bool readImagesBin(const std::filesystem::path& path, std::vector<ColmapImage>& out, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  uint64_t count = 0;
  if (!in || !readRaw(in, count) || count > 10'000'000) return error = "cannot read " + path.string(), false;
  for (uint64_t i = 0; i < count; ++i) {
    ColmapImage im;
    double q[4], t[3];
    if (!readRaw(in, im.id) || !in.read(reinterpret_cast<char*>(q), sizeof q) ||
        !in.read(reinterpret_cast<char*>(t), sizeof t) || !readRaw(in, im.cameraId))
      return error = "truncated images.bin", false;
    im.rotation = glm::normalize(glm::dquat(q[0], q[1], q[2], q[3]));
    im.translation = {t[0], t[1], t[2]};
    char ch = 0;
    while (in.get(ch) && ch != '\0') im.name += ch;
    uint64_t points2d = 0;
    if (!in || !readRaw(in, points2d)) return error = "truncated images.bin", false;
    in.seekg(static_cast<std::streamoff>(points2d * (2 * sizeof(double) + sizeof(int64_t))), std::ios::cur);
    if (!in) return error = "truncated images.bin", false;
    out.push_back(std::move(im));
  }
  return true;
}

// ---- text ---------------------------------------------------------------------------------------
bool readCamerasTxt(const std::filesystem::path& path, std::vector<ColmapCamera>& out, std::string& error) {
  if (!std::filesystem::exists(path)) return error = "missing " + path.string(), false;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    ColmapCamera c;
    std::string modelName;
    if (!(s >> c.id >> modelName >> c.width >> c.height)) return error = "bad line in cameras.txt: " + line, false;
    c.model = modelIdFromName(modelName);
    const int n = colmapModelParamCount(c.model);
    if (n < 0) return error = "unsupported COLMAP camera model " + modelName, false;
    c.params.resize(static_cast<size_t>(n));
    for (double& p : c.params)
      if (!(s >> p)) return error = "missing camera parameters: " + line, false;
    out.push_back(std::move(c));
  }
  return true;
}

bool readImagesTxt(const std::filesystem::path& path, std::vector<ColmapImage>& out, std::string& error) {
  if (!std::filesystem::exists(path)) return error = "missing " + path.string(), false;
  // Each image takes two lines: the pose line and the (possibly empty) 2D points line. Comment
  // lines only appear in the header, so skip leading comments and then read pairs.
  std::ifstream in(path);
  std::string line;
  bool expectPose = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && line[0] == '#') continue;
    if (!expectPose) {  // the points line belonging to the previous image
      expectPose = true;
      continue;
    }
    if (line.empty()) continue;
    std::istringstream s(line);
    ColmapImage im;
    double qw, qx, qy, qz;
    if (!(s >> im.id >> qw >> qx >> qy >> qz >> im.translation.x >> im.translation.y >> im.translation.z >>
          im.cameraId >> im.name))
      return error = "bad line in images.txt: " + line, false;
    im.rotation = glm::normalize(glm::dquat(qw, qx, qy, qz));
    out.push_back(std::move(im));
    expectPose = false;
  }
  return true;
}

}  // namespace

bool loadColmapModel(const std::filesystem::path& dir, ColmapModel& out, std::string& error) {
  out = {};
  const bool binary = std::filesystem::exists(dir / "cameras.bin") && std::filesystem::exists(dir / "images.bin");
  const bool ok = binary ? readCamerasBin(dir / "cameras.bin", out.cameras, error) &&
                               readImagesBin(dir / "images.bin", out.images, error)
                         : readCamerasTxt(dir / "cameras.txt", out.cameras, error) &&
                               readImagesTxt(dir / "images.txt", out.images, error);
  if (!ok) return false;
  for (const ColmapImage& im : out.images)
    if (!out.camera(im.cameraId)) return error = "image " + im.name + " refers to a missing camera", false;
  std::sort(out.images.begin(), out.images.end(), [](const ColmapImage& a, const ColmapImage& b) { return a.name < b.name; });
  return true;
}

std::filesystem::path findColmapModelFor(const std::filesystem::path& sceneFile) {
  const std::filesystem::path dir = sceneFile.parent_path();
  for (const auto& candidate : {dir / "dataset" / "sparse" / "0", dir / "sparse" / "0", dir / "sparse"}) {
    std::error_code ec;
    if (std::filesystem::exists(candidate / "images.bin", ec) || std::filesystem::exists(candidate / "images.txt", ec))
      return candidate;
  }
  return {};
}

glm::vec3 averageCameraUp(const ColmapModel& model) {
  glm::dvec3 sum(0.0);
  for (const ColmapImage& im : model.images) sum += im.upInWorld();
  const double len = glm::length(sum);
  return len > 1e-9 ? glm::vec3(sum / len) : glm::vec3(0.0f, 1.0f, 0.0f);
}

}  // namespace gsv
