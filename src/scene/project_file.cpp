#include "scene/project_file.h"

#include "core/json.h"
#include "core/utf8_path.h"
#include "scene/edit_ops.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace gsv {

namespace {

std::string vec3(const glm::dvec3& v) { return "[" + jsonNumber(v.x) + "," + jsonNumber(v.y) + "," + jsonNumber(v.z) + "]"; }

bool readVec3(const Json* j, glm::dvec3& out) {
  if (!j || j->type != Json::Type::Array || j->a.size() != 3) return false;
  for (int k = 0; k < 3; ++k) {
    if (j->a[static_cast<size_t>(k)].type != Json::Type::Number) return false;
    out[k] = j->a[static_cast<size_t>(k)].n;
  }
  return true;
}
std::string readString(const Json* j) { return j && j->type == Json::Type::String ? j->s : std::string(); }

}  // namespace

const char* measurementKindName(MeasurementRecord::Kind k) {
  switch (k) {
    case MeasurementRecord::Kind::Distance: return "distance";
    case MeasurementRecord::Kind::Polyline: return "polyline";
    case MeasurementRecord::Kind::Area: return "area";
    case MeasurementRecord::Kind::Height: return "height";
  }
  return "distance";
}

double measurementValue(const MeasurementRecord& m, const glm::dvec3& up) {
  switch (m.kind) {
    case MeasurementRecord::Kind::Area: return polygonArea(m.points);
    case MeasurementRecord::Kind::Height:
      return m.points.size() >= 2 ? glm::dot(m.points[1] - m.points[0], glm::normalize(up)) : 0.0;
    default: return polylineLength(m.points);
  }
}

double measurementRealValue(const MeasurementRecord& m, double unitScale, const glm::dvec3& up) {
  const double v = measurementValue(m, up);
  return m.kind == MeasurementRecord::Kind::Area ? v * unitScale * unitScale : v * unitScale;
}

std::string projectToJson(const Project& p) {
  std::ostringstream o;
  o << "{\n  \"format\": \"gsplat-viewer project\",\n  \"version\": " << p.version << ",\n";
  o << "  \"scene\": " << jsonQuote(p.scenePath) << ",\n  \"las\": " << jsonQuote(p.lasPath) << ",\n";
  if (p.hasLasTransform) {
    o << "  \"lasTransform\": [";
    for (int k = 0; k < 8; ++k) o << (k ? "," : "") << jsonNumber(p.lasTransform[k]);
    o << "],\n";
  }
  o << "  \"unit\": " << jsonQuote(p.unit) << ",\n  \"unitScale\": " << jsonNumber(p.unitScale) << ",\n";
  o << "  \"level\": [" << jsonNumber(p.level[0]) << "," << jsonNumber(p.level[1]) << "," << jsonNumber(p.level[2]) << "," << jsonNumber(p.level[3])
    << "],\n  \"bookmarks\": [";
  for (size_t i = 0; i < p.bookmarks.size(); ++i) {
    const ViewBookmark& b = p.bookmarks[i];
    o << (i ? "," : "") << "\n    {\"name\": " << jsonQuote(b.name) << ", \"eye\": " << vec3(glm::dvec3(b.eye))
      << ", \"target\": " << vec3(glm::dvec3(b.target)) << ", \"fovY\": " << jsonNumber(b.fovY) << "}";
  }
  o << "\n  ],\n  \"annotations\": [";
  for (size_t i = 0; i < p.annotations.size(); ++i) {
    const Annotation& a = p.annotations[i];
    o << (i ? "," : "") << "\n    {\"title\": " << jsonQuote(a.title) << ", \"text\": " << jsonQuote(a.text)
      << ", \"position\": " << vec3(glm::dvec3(a.position)) << ", \"bookmark\": " << a.bookmark << "}";
  }
  o << "\n  ],\n  \"measurements\": [";
  for (size_t i = 0; i < p.measurements.size(); ++i) {
    const MeasurementRecord& m = p.measurements[i];
    o << (i ? "," : "") << "\n    {\"name\": " << jsonQuote(m.name) << ", \"kind\": " << jsonQuote(measurementKindName(m.kind))
      << ", \"points\": [";
    for (size_t k = 0; k < m.points.size(); ++k) o << (k ? "," : "") << vec3(m.points[k]);
    o << "]}";
  }
  o << "\n  ]\n}\n";
  return o.str();
}

bool projectFromJson(const std::string& text, Project& out, std::string& error) {
  out = Project{};
  if (text.size() > (size_t(64) << 20)) {
    error = "project file larger than 64 MB";
    return false;
  }
  Json root;
  if (!parseJson(text, root, error)) return false;
  if (root.type != Json::Type::Object) {
    error = "project file is not a JSON object";
    return false;
  }
  if (readString(root.get("format")) != "gsplat-viewer project") {
    error = "not a gsplat-viewer project (missing \"format\")";
    return false;
  }
  if (const Json* v = root.get("version"); v && v->type == Json::Type::Number) out.version = static_cast<int>(v->n);
  if (out.version > 1) {
    error = "project version " + std::to_string(out.version) + " is newer than this viewer";
    return false;
  }
  out.scenePath = readString(root.get("scene"));
  out.lasPath = readString(root.get("las"));
  out.unit = root.get("unit") ? readString(root.get("unit")) : "units";
  if (const Json* u = root.get("unitScale")) {
    if (u->type != Json::Type::Number || !(u->n > 0.0)) {
      error = "unitScale must be a positive number";
      return false;
    }
    out.unitScale = u->n;
  }
  if (const Json* l = root.get("level")) {
    double q[4];
    bool ok = l->type == Json::Type::Array && l->a.size() == 4;
    for (size_t k = 0; ok && k < 4; ++k) {
      ok = l->a[k].type == Json::Type::Number;
      if (ok) q[k] = l->a[k].n;
    }
    const double n = ok ? std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]) : 0.0;
    if (!ok || n < 1e-9) {
      error = "level must be a quaternion [w, x, y, z]";
      return false;
    }
    for (int k = 0; k < 4; ++k) out.level[k] = q[k] / n;
  }
  if (const Json* t = root.get("lasTransform")) {
    if (t->type != Json::Type::Array || t->a.size() != 8) {
      error = "lasTransform must have 8 numbers";
      return false;
    }
    for (int k = 0; k < 8; ++k) {
      if (t->a[static_cast<size_t>(k)].type != Json::Type::Number) {
        error = "lasTransform must have 8 numbers";
        return false;
      }
      out.lasTransform[k] = t->a[static_cast<size_t>(k)].n;
    }
    out.hasLasTransform = true;
  }
  auto arrayOf = [&](const char* key) -> const std::vector<Json>* {
    const Json* j = root.get(key);
    if (!j) return nullptr;
    if (j->type != Json::Type::Array) {
      error = std::string(key) + " must be an array";
      return reinterpret_cast<const std::vector<Json>*>(1);
    }
    return &j->a;
  };
  const auto* bms = arrayOf("bookmarks");
  if (bms == reinterpret_cast<const std::vector<Json>*>(1)) return false;
  if (bms)
    for (const Json& b : *bms) {
      ViewBookmark v;
      glm::dvec3 e, t;
      if (b.type != Json::Type::Object || !readVec3(b.get("eye"), e) || !readVec3(b.get("target"), t)) {
        error = "bookmark needs eye and target";
        return false;
      }
      v.name = readString(b.get("name"));
      v.eye = glm::vec3(e);
      v.target = glm::vec3(t);
      if (const Json* f = b.get("fovY"); f && f->type == Json::Type::Number) v.fovY = static_cast<float>(f->n);
      out.bookmarks.push_back(v);
    }
  const auto* ans = arrayOf("annotations");
  if (ans == reinterpret_cast<const std::vector<Json>*>(1)) return false;
  if (ans)
    for (const Json& a : *ans) {
      Annotation v;
      glm::dvec3 p;
      if (a.type != Json::Type::Object || !readVec3(a.get("position"), p)) {
        error = "annotation needs a position";
        return false;
      }
      v.position = glm::vec3(p);
      v.title = readString(a.get("title"));
      v.text = readString(a.get("text"));
      if (const Json* b = a.get("bookmark"); b && b->type == Json::Type::Number) v.bookmark = static_cast<int>(b->n);
      if (v.bookmark >= static_cast<int>(out.bookmarks.size()) || v.bookmark < -1) v.bookmark = -1;
      out.annotations.push_back(v);
    }
  const auto* ms = arrayOf("measurements");
  if (ms == reinterpret_cast<const std::vector<Json>*>(1)) return false;
  if (ms)
    for (const Json& m : *ms) {
      MeasurementRecord r;
      const Json* pts = m.type == Json::Type::Object ? m.get("points") : nullptr;
      if (!pts || pts->type != Json::Type::Array) {
        error = "measurement needs points";
        return false;
      }
      for (const Json& p : pts->a) {
        glm::dvec3 v;
        if (!readVec3(&p, v)) {
          error = "measurement point must be [x, y, z]";
          return false;
        }
        r.points.push_back(v);
      }
      r.name = readString(m.get("name"));
      const std::string k = readString(m.get("kind"));
      r.kind = k == "polyline" ? MeasurementRecord::Kind::Polyline
               : k == "area"   ? MeasurementRecord::Kind::Area
               : k == "height" ? MeasurementRecord::Kind::Height
                               : MeasurementRecord::Kind::Distance;
      out.measurements.push_back(std::move(r));
    }
  return true;
}

bool saveProject(const std::string& path, const Project& p, std::string& error) {
  std::ofstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) {
    error = "cannot write " + path;
    return false;
  }
  f << projectToJson(p);
  if (!f) {
    error = "write error on " + path;
    return false;
  }
  return true;
}

bool loadProject(const std::string& path, Project& out, std::string& error) {
  std::ifstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  return projectFromJson(ss.str(), out, error);
}

std::string measurementsToCsv(const std::vector<MeasurementRecord>& ms, const std::string& unit, double unitScale) {
  std::ostringstream o;
  o << "name,kind,value,unit,points\n";
  for (const MeasurementRecord& m : ms) {
    std::string name = m.name;
    for (char& c : name)
      if (c == ',' || c == '\n') c = ' ';
    const std::string u = m.kind == MeasurementRecord::Kind::Area ? unit + "^2" : unit;
    o << name << "," << measurementKindName(m.kind) << "," << jsonNumber(measurementRealValue(m, unitScale)) << "," << u << ",";
    for (size_t k = 0; k < m.points.size(); ++k)
      o << (k ? ";" : "") << jsonNumber(m.points[k].x) << " " << jsonNumber(m.points[k].y) << " " << jsonNumber(m.points[k].z);
    o << "\n";
  }
  return o.str();
}

}  // namespace gsv
