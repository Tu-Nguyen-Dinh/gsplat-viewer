#pragma once
// Core 3: ASPRS LAS 1.2 / 1.3 / 1.4 point clouds (airborne, UAV, mobile and terrestrial LiDAR).
// See docs/core/CORE3-geometry.md.
//
// Read: point data formats 0..10, any record length (extra bytes are skipped), 8- or 16-bit RGB.
// Coordinates are integers * scale + offset in the file; they are rebuilt in double and stored as
// float offsets around a whole-metre origin near the data (PointCloud), so survey magnitudes keep
// their millimetres. LAZ (compressed LAS) is detected and rejected with a clear message.
// Write: LAS 1.2 format 2/3 or LAS 1.4 format 7, used by the tests and for export.

#include "scene/point_cloud.h"

#include <cstdint>
#include <string>

namespace gsv {

struct LasInfo {
  int versionMajor = 0, versionMinor = 0;
  int pointFormat = -1;
  int recordLength = 0;
  uint64_t pointCount = 0;
  double scale[3] = {0, 0, 0}, offset[3] = {0, 0, 0};
  double minXYZ[3] = {0, 0, 0}, maxXYZ[3] = {0, 0, 0};
  bool hasRgb = false, hasGpsTime = false;
  std::string generatingSoftware;
};

// Reads only the header. Returns false with `error` set on failure.
bool readLasHeader(const std::string& path, LasInfo& info, std::string& error);

// Reads every point (or every `stride`-th point, for previews). Fills color only when the format has
// RGB, intensity and classification always. Returns false with `error` set on any problem
// (missing file, wrong signature, LAZ, truncated data); never crashes on malformed input.
bool readLas(const std::string& path, PointCloud& cloud, std::string& error, LasInfo* info = nullptr,
             uint32_t stride = 1);

struct LasWriteOptions {
  int versionMinor = 2;      // 2 (LAS 1.2) or 4 (LAS 1.4)
  int pointFormat = 3;       // 1.2: 0..3 ; 1.4: 6 or 7
  double scale = 0.001;      // metres per integer unit (1 mm)
};
bool writeLas(const std::string& path, const PointCloud& cloud, const LasWriteOptions& options, std::string& error);

}  // namespace gsv
