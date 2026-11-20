#include "scene/spz_io.h"

#include "core/inflate.h"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace gsv {

namespace {

constexpr uint32_t kMagic = 0x5053474E;  // "NGSP" little-endian
constexpr float kColorScale = 0.15f;

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// Version 3 rotation: 2 bits = index of the largest component (dropped, positive), then three
// 10-bit fields (sign bit + 9-bit magnitude, scaled by 1/sqrt2), the first one in the high bits.
// Component order in the file is (x, y, z, w).
glm::vec4 smallestThreeXyzw(uint32_t comp) {
  constexpr uint32_t kMask = (1u << 9) - 1u;
  const int largest = static_cast<int>(comp >> 30);
  glm::vec4 q(0.0f);
  float sum = 0.0f;
  for (int j = 3; j >= 0; --j) {
    if (j == largest) continue;
    const uint32_t mag = comp & kMask, negative = (comp >> 9) & 1u;
    comp >>= 10;
    q[j] = 0.70710678f * static_cast<float>(mag) / static_cast<float>(kMask) * (negative ? -1.0f : 1.0f);
    sum += q[j] * q[j];
  }
  q[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
  return q;
}

uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

// Version 4: the 16-byte header is stored uncompressed and followed by a table of contents, then
// every attribute is its own zstd frame:
//   byte 15     number of streams (6: positions, alphas, colors, scales, rotations, SH)
//   bytes 16-19 offset of the table (32)
//   table       per stream: compressed size (u64), uncompressed size (u64)
// The streams hold the same bytes as the version 3 body, so they are decompressed and appended
// to a version 3 header and decoded by the same code.
bool unpackSpzV4(const std::vector<uint8_t>& file, std::vector<uint8_t>& payload, std::string& error) {
  if (file.size() < 32) return error = "truncated header", false;
  const uint32_t version = le32(file.data() + 4);
  if (version != 4) return error = "version " + std::to_string(version) + " is not supported (2, 3 and 4 are)", false;
  const size_t streams = file[15], table = le32(file.data() + 16);
  if (streams == 0 || streams > 16 || table < 16 || table + streams * 16 > file.size())
    return error = "bad stream table", false;
  payload.assign(file.begin(), file.begin() + 16);
  payload[4] = 3;  // decoded as a version 3 body (same attribute encodings)
  size_t offset = table + streams * 16;
  for (size_t k = 0; k < streams; ++k) {
    const uint64_t packed = le64(file.data() + table + k * 16), unpacked = le64(file.data() + table + k * 16 + 8);
    if (packed > file.size() - offset || unpacked > (uint64_t(1) << 32)) return error = "stream " + std::to_string(k) + " out of bounds", false;
    const size_t at = payload.size();
    payload.resize(at + static_cast<size_t>(unpacked));
    const size_t got = ZSTD_decompress(payload.data() + at, static_cast<size_t>(unpacked), file.data() + offset,
                                       static_cast<size_t>(packed));
    if (ZSTD_isError(got)) return error = std::string("zstd: ") + ZSTD_getErrorName(got), false;
    if (got != unpacked) return error = "stream " + std::to_string(k) + " has the wrong size", false;
    offset += static_cast<size_t>(packed);
  }
  return true;
}

}  // namespace

bool decodeSpzPayload(const std::vector<uint8_t>& d, GaussianScene& s, std::string& error) {
  if (d.size() < 16) return error = "SPZ: file too small", false;
  if (le32(d.data()) != kMagic) return error = "SPZ: bad magic (not an .spz file)", false;
  const uint32_t version = le32(d.data() + 4), n = le32(d.data() + 8);
  const int degree = d[12], fractionalBits = d[13];
  if (version < 2 || version > 3)
    return error = "SPZ version " + std::to_string(version) + " is not supported (2, 3 and 4 are)", false;
  if (degree > 3) return error = "SPZ: SH degree " + std::to_string(degree) + " > 3", false;
  if (n == 0) return error = "SPZ: no points", false;
  const int shDim = degree == 0 ? 0 : degree == 1 ? 3 : degree == 2 ? 8 : 15;
  const size_t rotBytes = version == 2 ? 3 : 4;
  const size_t need = 16 + size_t(n) * (9 + 1 + 3 + 3 + rotBytes + size_t(shDim) * 3);
  if (d.size() < need)
    return error = "SPZ: truncated (" + std::to_string(d.size()) + " of " + std::to_string(need) + " bytes)", false;

  s = {};
  s.shDegree = degree;
  s.shRestPerChannel = shDim;
  s.position.resize(n);
  s.opacityLogit.resize(n);
  s.shDC.resize(n);
  s.logScale.resize(n);
  s.rotation.resize(n);
  s.shRest.resize(size_t(n) * size_t(shDim) * 3);

  const uint8_t* p = d.data() + 16;
  const float posScale = 1.0f / static_cast<float>(1 << fractionalBits);
  for (uint32_t i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c, p += 3) {
      int32_t v = int32_t(p[0] | p[1] << 8 | p[2] << 16);
      if (v & 0x800000) v |= int32_t(0xFF000000);  // sign-extend 24 -> 32 bits
      s.position[i][c] = static_cast<float>(v) * posScale;
    }
  for (uint32_t i = 0; i < n; ++i, ++p) {
    // Opacity 0 or 1 has no finite logit: clamp to the representable range of a byte.
    const float a = std::clamp(*p / 255.0f, 0.5f / 255.0f, 254.5f / 255.0f);
    s.opacityLogit[i] = logit(a);
  }
  for (uint32_t i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c, ++p) s.shDC[i][c] = (*p / 255.0f - 0.5f) / kColorScale;
  for (uint32_t i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c, ++p) s.logScale[i][c] = *p / 16.0f - 10.0f;
  for (uint32_t i = 0; i < n; ++i) {
    glm::vec4 xyzw;
    if (version == 2) {
      const glm::vec3 xyz = glm::vec3(p[0], p[1], p[2]) / 127.5f - 1.0f;
      xyzw = glm::vec4(xyz, std::sqrt(std::max(0.0f, 1.0f - glm::dot(xyz, xyz))));
      p += 3;
    } else {
      xyzw = smallestThreeXyzw(le32(p));
      p += 4;
    }
    s.rotation[i] = {xyzw.w, xyzw.x, xyzw.y, xyzw.z};  // ours: (w, x, y, z)
  }
  // File: per point, per coefficient, rgb interleaved. Ours: per point, channel-major.
  for (uint32_t i = 0; i < n; ++i)
    for (int j = 0; j < shDim; ++j)
      for (int c = 0; c < 3; ++c, ++p)
        s.shRest[size_t(i) * size_t(shDim) * 3 + size_t(c) * size_t(shDim) + size_t(j)] = (*p - 128.0f) / 128.0f;
  return true;
}

bool loadSpz(const std::filesystem::path& path, GaussianScene& out, std::string& error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return error = "Cannot open file", false;
  const std::vector<uint8_t> file{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  std::vector<uint8_t> payload;
  if (file.size() >= 8 && le32(file.data()) == kMagic) {  // v4: uncompressed header, zstd streams
    if (!unpackSpzV4(file, payload, error)) return error = "SPZ: " + error, false;
    return decodeSpzPayload(payload, out, error);
  }
  if (!gunzip(file.data(), file.size(), payload, error)) return error = "SPZ: " + error, false;
  return decodeSpzPayload(payload, out, error);
}

}  // namespace gsv
