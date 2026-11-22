#pragma once
// Core 5: the viewer's project file ".gsvproj" (JSON, UTF-8). See docs/core/CORE5-editing.md.
// It references the data (scene, LAS) by path and stores what the user added on top:
// unit, view bookmarks, annotations and measurements.
//
// All positions are SCENE coordinates (the coordinates of the splat file). The viewer shows them
// through its model matrix, so they stay attached to the scene when the user levels the view.
// Scale and level are stored as properties, never baked into the splats: undoing a splat edit can
// therefore never leave a measurement out of step with the scene. Unknown keys are ignored (forward compatible);
// malformed files are rejected with a message, never crash (depth- and size-limited parser).

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace gsv {

struct ViewBookmark {
  std::string name;
  glm::vec3 eye{0.0f}, target{0.0f, 0.0f, -1.0f};
  float fovY = 0.8f;
};

struct Annotation {
  std::string title, text;
  glm::vec3 position{0.0f};   // scene coordinates
  int bookmark = -1;          // view to fly to when selected (-1: none)
};

struct MeasurementRecord {
  enum class Kind { Distance, Polyline, Area, Height };
  Kind kind = Kind::Distance;
  std::string name;
  std::vector<glm::dvec3> points;  // scene coordinates
};

struct Project {
  int version = 1;
  std::string scenePath, lasPath;  // UTF-8, relative to the project file or absolute
  bool hasLasTransform = false;
  double lasTransform[8] = {1, 1, 0, 0, 0, 0, 0, 0};  // s, qw, qx, qy, qz, tx, ty, tz
  std::string unit = "units";       // "m" once the scale was set from a known length
  double unitScale = 1.0;           // real length = scene length * unitScale
  double level[4] = {1, 0, 0, 0};   // extra rotation (w, x, y, z) that makes the floor horizontal
  std::vector<ViewBookmark> bookmarks;
  std::vector<Annotation> annotations;
  std::vector<MeasurementRecord> measurements;
};

std::string projectToJson(const Project& p);
bool projectFromJson(const std::string& json, Project& out, std::string& error);
bool saveProject(const std::string& path, const Project& p, std::string& error);
bool loadProject(const std::string& path, Project& out, std::string& error);

const char* measurementKindName(MeasurementRecord::Kind k);
// CSV: name,kind,value,unit,points (x y z;...). Value in real units (scaled by unitScale).
std::string measurementsToCsv(const std::vector<MeasurementRecord>& m, const std::string& unit, double unitScale = 1.0);
// Length / area / height in scene units. Height is measured along `up` (scene coordinates).
double measurementValue(const MeasurementRecord& m, const glm::dvec3& up = glm::dvec3(0, 1, 0));
// Real value: length * unitScale, area * unitScale^2.
double measurementRealValue(const MeasurementRecord& m, double unitScale, const glm::dvec3& up = glm::dvec3(0, 1, 0));

}  // namespace gsv
