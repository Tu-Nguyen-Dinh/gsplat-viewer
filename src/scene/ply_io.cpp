#include "scene/ply_io.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace gsv {

namespace {

enum class PlyType { I8, U8, I16, U16, I32, U32, F32, F64 };

bool parseType(const std::string& t, PlyType& out, size_t& size) {
  static const std::unordered_map<std::string, std::pair<PlyType, size_t>> kTypes = {
      {"char", {PlyType::I8, 1}},    {"int8", {PlyType::I8, 1}},     {"uchar", {PlyType::U8, 1}},
      {"uint8", {PlyType::U8, 1}},   {"short", {PlyType::I16, 2}},   {"int16", {PlyType::I16, 2}},
      {"ushort", {PlyType::U16, 2}}, {"uint16", {PlyType::U16, 2}},  {"int", {PlyType::I32, 4}},
      {"int32", {PlyType::I32, 4}},  {"uint", {PlyType::U32, 4}},    {"uint32", {PlyType::U32, 4}},
      {"float", {PlyType::F32, 4}},  {"float32", {PlyType::F32, 4}}, {"double", {PlyType::F64, 8}},
      {"float64", {PlyType::F64, 8}}};
  auto it = kTypes.find(t);
  if (it == kTypes.end()) return false;
  out = it->second.first;
  size = it->second.second;
  return true;
}

struct Property {
  std::string name;
  PlyType type;
  size_t offset;
};

template <typename T>
float readAs(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return static_cast<float>(v);
}

float readValue(const uint8_t* p, PlyType t) {
  switch (t) {
    case PlyType::I8: return readAs<int8_t>(p);
    case PlyType::U8: return readAs<uint8_t>(p);
    case PlyType::I16: return readAs<int16_t>(p);
    case PlyType::U16: return readAs<uint16_t>(p);
    case PlyType::I32: return readAs<int32_t>(p);
    case PlyType::U32: return readAs<uint32_t>(p);
    case PlyType::F32: return readAs<float>(p);
    case PlyType::F64: return readAs<double>(p);
  }
  return 0.0f;
}

}  // namespace

std::filesystem::path pathFromUtf8(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

void computeRobustBounds(GaussianScene& scene) {
  const size_t n = scene.size();
  if (n == 0) return;
  std::vector<float> axis(n);
  for (int a = 0; a < 3; ++a) {
    for (size_t i = 0; i < n; ++i) axis[i] = scene.position[i][a];
    const size_t lo = n / 50, hi = n - 1 - n / 50;
    std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(lo), axis.end());
    scene.boundsMin[a] = axis[lo];
    std::nth_element(axis.begin(), axis.begin() + static_cast<std::ptrdiff_t>(hi), axis.end());
    scene.boundsMax[a] = axis[hi];
  }
}

bool loadPly(const std::filesystem::path& path, GaussianScene& out, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "Cannot open file";
    return false;
  }

  // ---- header ------------------------------------------------------------
  std::string line;
  std::getline(file, line);
  if (line.rfind("ply", 0) != 0) {
    error = "Not a PLY file";
    return false;
  }
  size_t vertexCount = 0, stride = 0;
  bool inVertex = false, sawVertex = false;
  std::vector<Property> props;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ss(line);
    std::string word;
    ss >> word;
    if (word == "format") {
      std::string fmt;
      ss >> fmt;
      if (fmt != "binary_little_endian") {
        error = "Only binary_little_endian PLY is supported (got " + fmt + ")";
        return false;
      }
    } else if (word == "element") {
      std::string name;
      size_t count = 0;
      ss >> name >> count;
      if (name == "vertex") {
        if (sawVertex) {
          error = "Multiple vertex elements";
          return false;
        }
        vertexCount = count;
        inVertex = sawVertex = true;
      } else {
        if (!sawVertex) {
          error = "Element '" + name + "' before 'vertex' is not supported";
          return false;
        }
        inVertex = false;  // elements after the vertices (e.g. faces) are ignored
      }
    } else if (word == "property" && inVertex) {
      std::string type, name;
      ss >> type;
      if (type == "list") {
        error = "List properties on vertices are not supported";
        return false;
      }
      ss >> name;
      PlyType t;
      size_t size;
      if (!parseType(type, t, size)) {
        error = "Unknown property type: " + type;
        return false;
      }
      props.push_back({name, t, stride});
      stride += size;
    } else if (word == "end_header") {
      break;
    }
  }
  if (!sawVertex || vertexCount == 0 || stride == 0) {
    error = "PLY has no vertices";
    return false;
  }

  auto find = [&](const std::string& name) -> const Property* {
    for (const auto& p : props)
      if (p.name == name) return &p;
    return nullptr;
  };
  const Property* px = find("x");
  const Property* py = find("y");
  const Property* pz = find("z");
  if (!px || !py || !pz) {
    error = "PLY is missing x/y/z";
    return false;
  }

  const Property* dc[3] = {find("f_dc_0"), find("f_dc_1"), find("f_dc_2")};
  const Property* rgb[3] = {find("red"), find("green"), find("blue")};
  const Property* scale[3] = {find("scale_0"), find("scale_1"), find("scale_2")};
  const Property* rot[4] = {find("rot_0"), find("rot_1"), find("rot_2"), find("rot_3")};
  const Property* opacity = find("opacity");
  const bool isGaussian = dc[0] && dc[1] && dc[2] && scale[0] && scale[1] && rot[0] && rot[1] && rot[2] &&
                          rot[3] && opacity;

  std::vector<const Property*> rest;
  for (int i = 0;; ++i) {
    const Property* p = find("f_rest_" + std::to_string(i));
    if (!p) break;
    rest.push_back(p);
  }
  const int restPerChannel = static_cast<int>(rest.size() / 3);
  int degree = 0;
  if (isGaussian) {
    if (restPerChannel >= 15) degree = 3;
    else if (restPerChannel >= 8) degree = 2;
    else if (restPerChannel >= 3) degree = 1;
  }

  // ---- body --------------------------------------------------------------
  std::vector<uint8_t> data(vertexCount * stride);
  file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (static_cast<size_t>(file.gcount()) != data.size()) {
    error = "File is truncated (expected " + std::to_string(vertexCount) + " vertices)";
    return false;
  }

  GaussianScene s;
  s.position.resize(vertexCount);
  s.logScale.resize(vertexCount);
  s.rotation.resize(vertexCount);
  s.opacityLogit.resize(vertexCount);
  s.shDC.resize(vertexCount);
  s.shDegree = degree;
  s.shRestPerChannel = degree == 0 ? 0 : restPerChannel;
  s.shRest.resize(vertexCount * static_cast<size_t>(s.shRestPerChannel) * 3);

  const float defaultLogScale = std::log(0.01f);
  for (size_t i = 0; i < vertexCount; ++i) {
    const uint8_t* v = data.data() + i * stride;
    s.position[i] = {readValue(v + px->offset, px->type), readValue(v + py->offset, py->type),
                     readValue(v + pz->offset, pz->type)};
    if (isGaussian) {
      for (int c = 0; c < 3; ++c) s.shDC[i][c] = readValue(v + dc[c]->offset, dc[c]->type);
      for (int c = 0; c < 3; ++c)  // 2DGS has no scale_2: make the splat a flat disc
        s.logScale[i][c] = scale[c] ? readValue(v + scale[c]->offset, scale[c]->type) : -12.0f;
      for (int c = 0; c < 4; ++c) s.rotation[i][c] = readValue(v + rot[c]->offset, rot[c]->type);
      s.opacityLogit[i] = readValue(v + opacity->offset, opacity->type);
      const size_t base = i * static_cast<size_t>(s.shRestPerChannel) * 3;
      for (size_t k = 0; k < static_cast<size_t>(s.shRestPerChannel) * 3; ++k)
        s.shRest[base + k] = readValue(v + rest[k]->offset, rest[k]->type);
    } else {
      glm::vec3 color(0.8f);
      if (rgb[0] && rgb[1] && rgb[2]) {
        for (int c = 0; c < 3; ++c) {
          float val = readValue(v + rgb[c]->offset, rgb[c]->type);
          color[c] = rgb[c]->type == PlyType::U8 ? val / 255.0f : val;
        }
      }
      s.shDC[i] = (color - 0.5f) / kShC0;
      s.logScale[i] = glm::vec3(defaultLogScale);
      s.rotation[i] = {1.0f, 0.0f, 0.0f, 0.0f};
      s.opacityLogit[i] = logit(0.95f);
    }
  }

  computeRobustBounds(s);
  const std::u8string name = path.filename().u8string();
  s.sourceName.assign(name.begin(), name.end());
  out = std::move(s);
  return true;
}

bool savePly(const std::filesystem::path& path, const GaussianScene& s, std::string& error) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    error = "Cannot create file";
    return false;
  }
  const size_t n = s.size();
  const int restCount = s.shRestPerChannel * 3;
  f << "ply\nformat binary_little_endian 1.0\n";
  f << "comment Generated by gsplat-viewer\n";
  f << "element vertex " << n << "\n";
  for (const char* name : {"x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2"})
    f << "property float " << name << "\n";
  for (int k = 0; k < restCount; ++k) f << "property float f_rest_" << k << "\n";
  for (const char* name : {"opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"})
    f << "property float " << name << "\n";
  f << "end_header\n";

  std::vector<float> row(static_cast<size_t>(17 + restCount));
  for (size_t i = 0; i < n; ++i) {
    size_t k = 0;
    for (int c = 0; c < 3; ++c) row[k++] = s.position[i][c];
    for (int c = 0; c < 3; ++c) row[k++] = 0.0f;  // normals: unused by 3DGS
    for (int c = 0; c < 3; ++c) row[k++] = s.shDC[i][c];
    for (int r = 0; r < restCount; ++r) row[k++] = s.shRest[i * static_cast<size_t>(restCount) + static_cast<size_t>(r)];
    row[k++] = s.opacityLogit[i];
    for (int c = 0; c < 3; ++c) row[k++] = s.logScale[i][c];
    for (int c = 0; c < 4; ++c) row[k++] = s.rotation[i][c];
    f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * sizeof(float)));
  }
  if (!f) {
    error = "Write failed";
    return false;
  }
  return true;
}

}  // namespace gsv
