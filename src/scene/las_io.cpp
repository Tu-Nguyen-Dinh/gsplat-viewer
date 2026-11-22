#include "scene/las_io.h"

#include "core/utf8_path.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace gsv {

namespace {

template <typename T>
T rd(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));  // LAS is little-endian, like every platform we build for
  return v;
}
template <typename T>
void wr(std::vector<uint8_t>& out, size_t at, T v) {
  std::memcpy(out.data() + at, &v, sizeof(T));
}

// Byte offsets of the fields that depend on the point format.
struct FormatLayout {
  int minLength = 0;
  int intensity = 12;
  int classification = 15;
  bool legacyClass = true;  // formats 0-5: class in the low 5 bits
  int rgb = -1;
  int gps = -1;
};

bool layoutFor(int format, FormatLayout& l) {
  switch (format) {
    case 0: l = {20, 12, 15, true, -1, -1}; return true;
    case 1: l = {28, 12, 15, true, -1, 20}; return true;
    case 2: l = {26, 12, 15, true, 20, -1}; return true;
    case 3: l = {34, 12, 15, true, 28, 20}; return true;
    case 4: l = {57, 12, 15, true, -1, 20}; return true;
    case 5: l = {63, 12, 15, true, 28, 20}; return true;
    case 6: l = {30, 12, 16, false, -1, 22}; return true;
    case 7: l = {36, 12, 16, false, 30, 22}; return true;
    case 8: l = {38, 12, 16, false, 30, 22}; return true;
    case 9: l = {59, 12, 16, false, -1, 22}; return true;
    case 10: l = {67, 12, 16, false, 30, 22}; return true;
    default: return false;
  }
}

bool parseHeader(const uint8_t* h, size_t available, LasInfo& info, uint32_t& pointOffset, std::string& error) {
  if (available < 227 || std::memcmp(h, "LASF", 4) != 0) {
    error = "not a LAS file (missing LASF signature)";
    return false;
  }
  info.versionMajor = h[24];
  info.versionMinor = h[25];
  char software[33] = {};
  std::memcpy(software, h + 58, 32);
  info.generatingSoftware = software;
  const uint16_t headerSize = rd<uint16_t>(h + 94);
  pointOffset = rd<uint32_t>(h + 96);
  const uint8_t rawFormat = h[104];
  if (rawFormat & 0xC0) {
    error = "LAZ (compressed LAS) is not supported: decompress first, e.g. 'laszip -i in.laz -o out.las'";
    return false;
  }
  info.pointFormat = rawFormat & 0x3F;
  info.recordLength = rd<uint16_t>(h + 105);
  uint64_t count = rd<uint32_t>(h + 107);
  for (int k = 0; k < 3; ++k) {
    info.scale[k] = rd<double>(h + 131 + 8 * k);
    info.offset[k] = rd<double>(h + 155 + 8 * k);
    info.maxXYZ[k] = rd<double>(h + 179 + 16 * k);
    info.minXYZ[k] = rd<double>(h + 187 + 16 * k);
  }
  if (info.versionMajor != 1 || info.versionMinor < 0 || info.versionMinor > 4) {
    error = "unsupported LAS version " + std::to_string(info.versionMajor) + "." + std::to_string(info.versionMinor);
    return false;
  }
  if (info.versionMinor >= 4) {
    if (available < 375 || headerSize < 375) {
      error = "truncated LAS 1.4 header";
      return false;
    }
    const uint64_t count64 = rd<uint64_t>(h + 247);
    if (count64 != 0) count = count64;
  }
  info.pointCount = count;
  FormatLayout l;
  if (!layoutFor(info.pointFormat, l)) {
    error = "unsupported LAS point format " + std::to_string(info.pointFormat);
    return false;
  }
  if (info.recordLength < l.minLength) {
    error = "LAS record length " + std::to_string(info.recordLength) + " shorter than format " +
            std::to_string(info.pointFormat) + " requires";
    return false;
  }
  for (int k = 0; k < 3; ++k)
    if (!(info.scale[k] > 0.0) || !std::isfinite(info.scale[k]) || !std::isfinite(info.offset[k])) {
      error = "invalid LAS scale/offset";
      return false;
    }
  if (pointOffset < headerSize) {
    error = "invalid LAS point data offset";
    return false;
  }
  info.hasRgb = l.rgb >= 0;
  info.hasGpsTime = l.gps >= 0;
  return true;
}

}  // namespace

bool readLasHeader(const std::string& path, LasInfo& info, std::string& error) {
  std::ifstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  uint8_t h[375] = {};
  f.read(reinterpret_cast<char*>(h), sizeof(h));
  uint32_t pointOffset = 0;
  return parseHeader(h, static_cast<size_t>(f.gcount()), info, pointOffset, error);
}

bool readLas(const std::string& path, PointCloud& cloud, std::string& error, LasInfo* infoOut, uint32_t stride) {
  cloud = PointCloud{};
  std::ifstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) {
    error = "cannot open " + path;
    return false;
  }
  f.seekg(0, std::ios::end);
  const uint64_t fileSize = static_cast<uint64_t>(f.tellg());
  f.seekg(0);
  uint8_t h[375] = {};
  f.read(reinterpret_cast<char*>(h), sizeof(h));
  LasInfo info;
  uint32_t pointOffset = 0;
  if (!parseHeader(h, static_cast<size_t>(f.gcount()), info, pointOffset, error)) return false;
  if (infoOut) *infoOut = info;
  const uint64_t rec = static_cast<uint64_t>(info.recordLength);
  if (pointOffset > fileSize || info.pointCount > (fileSize - pointOffset) / rec) {
    error = "truncated LAS file: header announces " + std::to_string(info.pointCount) + " points, file holds " +
            std::to_string(pointOffset <= fileSize ? (fileSize - pointOffset) / rec : 0);
    return false;
  }
  FormatLayout l;
  layoutFor(info.pointFormat, l);
  stride = std::max<uint32_t>(1, stride);

  // Whole-metre origin at the centre of the header bounds: offsets then stay within a few km.
  for (int k = 0; k < 3; ++k) cloud.origin[k] = std::round(0.5 * (info.minXYZ[k] + info.maxXYZ[k]));
  if (!std::isfinite(cloud.origin.x + cloud.origin.y + cloud.origin.z)) cloud.origin = glm::dvec3(0.0);
  const size_t n = static_cast<size_t>((info.pointCount + stride - 1) / stride);
  cloud.reserve(n, info.hasRgb, true, true);
  std::vector<glm::u16vec3> rgb16;
  if (info.hasRgb) rgb16.reserve(n);

  f.seekg(static_cast<std::streamoff>(pointOffset));
  constexpr size_t kChunkPoints = 65536;
  std::vector<uint8_t> buf(kChunkPoints * rec);
  uint64_t index = 0;
  uint16_t maxRgb = 0;
  while (index < info.pointCount) {
    const uint64_t todo = std::min<uint64_t>(kChunkPoints, info.pointCount - index);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(todo * rec));
    if (static_cast<uint64_t>(f.gcount()) != todo * rec) {
      error = "unexpected end of LAS point data";
      cloud = PointCloud{};
      return false;
    }
    for (uint64_t j = 0; j < todo; ++j, ++index) {
      if (index % stride) continue;
      const uint8_t* p = buf.data() + j * rec;
      glm::dvec3 w;
      for (int k = 0; k < 3; ++k) w[k] = rd<int32_t>(p + 4 * k) * info.scale[k] + info.offset[k];
      cloud.position.push_back(glm::vec3(w - cloud.origin));
      cloud.intensity.push_back(rd<uint16_t>(p + l.intensity));
      const uint8_t c = p[l.classification];
      cloud.classification.push_back(l.legacyClass ? static_cast<uint8_t>(c & 0x1F) : c);
      if (l.rgb >= 0) {
        const glm::u16vec3 v(rd<uint16_t>(p + l.rgb), rd<uint16_t>(p + l.rgb + 2), rd<uint16_t>(p + l.rgb + 4));
        maxRgb = std::max({maxRgb, v.r, v.g, v.b});
        rgb16.push_back(v);
      }
    }
  }
  // The spec says 16-bit RGB, but many writers store 8-bit values: detect from the data.
  if (info.hasRgb) {
    const int shift = maxRgb > 255 ? 8 : 0;
    cloud.color.resize(rgb16.size());
    for (size_t i = 0; i < rgb16.size(); ++i) cloud.color[i] = glm::u8vec3(rgb16[i] >> static_cast<uint16_t>(shift));
  }
  cloud.computeBounds();
  cloud.sourceName = path;
  return true;
}

bool writeLas(const std::string& path, const PointCloud& cloud, const LasWriteOptions& o, std::string& error) {
  const bool v14 = o.versionMinor >= 4;
  const int format = o.pointFormat;
  FormatLayout l;
  if (!layoutFor(format, l) || (v14 ? (format != 6 && format != 7) : format > 3)) {
    error = "writeLas supports LAS 1.2 formats 0-3 and LAS 1.4 formats 6-7";
    return false;
  }
  if (cloud.empty()) {
    error = "empty point cloud";
    return false;
  }
  const uint16_t headerSize = v14 ? 375 : 227;
  const uint16_t recLen = static_cast<uint16_t>(l.minLength);
  std::vector<uint8_t> header(headerSize, 0);
  std::memcpy(header.data(), "LASF", 4);
  header[24] = 1;
  header[25] = static_cast<uint8_t>(v14 ? 4 : 2);
  const char* software = "gsplat-viewer";
  std::memcpy(header.data() + 58, software, std::strlen(software));
  wr<uint16_t>(header, 94, headerSize);
  wr<uint32_t>(header, 96, headerSize);
  wr<uint32_t>(header, 100, 0);
  header[104] = static_cast<uint8_t>(format);
  wr<uint16_t>(header, 105, recLen);
  const uint64_t n = cloud.size();
  wr<uint32_t>(header, 107, (!v14 || n <= 0xFFFFFFFFull) ? static_cast<uint32_t>(n) : 0u);
  glm::dvec3 lo = cloud.world(0), hi = lo;
  for (size_t i = 1; i < cloud.size(); ++i) {
    lo = glm::min(lo, cloud.world(i));
    hi = glm::max(hi, cloud.world(i));
  }
  const glm::dvec3 offset = glm::round(0.5 * (lo + hi));
  for (int k = 0; k < 3; ++k) {
    wr<double>(header, 131 + 8 * k, o.scale);
    wr<double>(header, 155 + 8 * k, offset[k]);
    wr<double>(header, 179 + 16 * k, hi[k]);
    wr<double>(header, 187 + 16 * k, lo[k]);
  }
  if (v14) wr<uint64_t>(header, 247, n);
  if (v14) wr<uint16_t>(header, 4 + 2, 0x10);  // global encoding: WKT bit (required for formats 6-10)

  std::ofstream f(pathFromUtf8(path), std::ios::binary);
  if (!f) {
    error = "cannot write " + path;
    return false;
  }
  f.write(reinterpret_cast<const char*>(header.data()), headerSize);
  std::vector<uint8_t> rec(recLen);
  const bool color = cloud.hasColor(), inten = cloud.intensity.size() == n, cls = cloud.classification.size() == n;
  for (size_t i = 0; i < cloud.size(); ++i) {
    std::fill(rec.begin(), rec.end(), 0);
    const glm::dvec3 w = cloud.world(i);
    for (int k = 0; k < 3; ++k) {
      const double q = std::round((w[k] - offset[k]) / o.scale);
      wr<int32_t>(rec, 4 * static_cast<size_t>(k), static_cast<int32_t>(std::clamp(q, -2147483648.0, 2147483647.0)));
    }
    wr<uint16_t>(rec, 12, inten ? cloud.intensity[i] : 0);
    if (v14) {
      rec[14] = 0x11;  // return 1 of 1
      rec[16] = cls ? cloud.classification[i] : 1;
    } else {
      rec[14] = 0x09;  // return 1 of 1
      rec[15] = static_cast<uint8_t>((cls ? cloud.classification[i] : 1) & 0x1F);
    }
    if (l.rgb >= 0) {
      const glm::u8vec3 c = color ? cloud.color[i] : glm::u8vec3(0);
      for (int k = 0; k < 3; ++k) wr<uint16_t>(rec, static_cast<size_t>(l.rgb + 2 * k), static_cast<uint16_t>(c[k] * 257));
    }
    f.write(reinterpret_cast<const char*>(rec.data()), recLen);
  }
  if (!f) {
    error = "write error on " + path;
    return false;
  }
  return true;
}

}  // namespace gsv
