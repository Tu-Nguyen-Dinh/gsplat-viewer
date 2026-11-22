// Core 3: LAS read/write round trips and malformed files.
#include "scene/las_io.h"
#include "test_framework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using namespace gsv;

namespace {
std::string tempPath(const char* name) { return (std::filesystem::temp_directory_path() / name).string(); }

PointCloud surveyCloud(size_t n) {
  PointCloud c;
  c.origin = {585123.0, 1186456.0, 12.0};  // VN-2000 / UTM-like magnitudes
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-250.0f, 250.0f);
  for (size_t i = 0; i < n; ++i) {
    c.position.push_back({u(rng), u(rng), 0.1f * u(rng)});
    c.color.push_back(glm::u8vec3(rng() & 255, rng() & 255, rng() & 255));
    c.intensity.push_back(static_cast<uint16_t>(rng() & 0xFFFF));
    c.classification.push_back(static_cast<uint8_t>(rng() % 10));
  }
  c.computeBounds();
  return c;
}

void checkRoundTrip(const LasWriteOptions& o, bool expectColor) {
  const PointCloud in = surveyCloud(5000);
  const std::string path = tempPath("gsv_test.las");
  std::string err;
  REQUIRE(writeLas(path, in, o, err));
  PointCloud out;
  LasInfo info;
  REQUIRE(readLas(path, out, err, &info));
  CHECK(info.versionMinor == o.versionMinor);
  CHECK(info.pointFormat == o.pointFormat);
  REQUIRE(out.size() == in.size());
  double maxErr = 0.0;
  bool attrsOk = true;
  for (size_t i = 0; i < in.size(); ++i) {
    maxErr = std::max(maxErr, glm::length(out.world(i) - in.world(i)));
    attrsOk = attrsOk && out.intensity[i] == in.intensity[i] && out.classification[i] == in.classification[i];
    if (expectColor) attrsOk = attrsOk && out.color[i] == in.color[i];
  }
  std::printf("    LAS 1.%d format %d: max position error %.2e m\n", o.versionMinor, o.pointFormat, maxErr);
  CHECK(maxErr <= o.scale);       // quantization to the file's scale (+ float offsets)
  CHECK(attrsOk);
  CHECK(out.hasColor() == expectColor);
  std::filesystem::remove(path);
}
}  // namespace

TEST_CASE(las_round_trip_12_format3) { checkRoundTrip({2, 3, 0.001}, true); }
TEST_CASE(las_round_trip_12_format0) { checkRoundTrip({2, 0, 0.001}, false); }
TEST_CASE(las_round_trip_14_format7) { checkRoundTrip({4, 7, 0.0005}, true); }

TEST_CASE(las_stride_and_header) {
  const PointCloud in = surveyCloud(1000);
  const std::string path = tempPath("gsv_stride.las");
  std::string err;
  REQUIRE(writeLas(path, in, {}, err));
  LasInfo info;
  REQUIRE(readLasHeader(path, info, err));
  CHECK(info.pointCount == 1000);
  CHECK(info.generatingSoftware == "gsplat-viewer");
  PointCloud out;
  REQUIRE(readLas(path, out, err, nullptr, 10));
  CHECK(out.size() == 100);
  CHECK(glm::length(out.world(1) - in.world(10)) < 2e-3);
  std::filesystem::remove(path);
}

TEST_CASE(las_malformed_files_fail_cleanly) {
  const PointCloud in = surveyCloud(300);
  const std::string good = tempPath("gsv_good.las"), bad = tempPath("gsv_bad.las");
  std::string err;
  REQUIRE(writeLas(good, in, {}, err));
  std::vector<char> bytes;
  {
    std::ifstream f(good, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(f), {});
  }
  auto tryBytes = [&](const std::vector<char>& b) {
    {
      std::ofstream f(bad, std::ios::binary);
      f.write(b.data(), static_cast<std::streamsize>(b.size()));
    }
    PointCloud out;
    std::string e;
    const bool ok = readLas(bad, out, e);
    return std::make_pair(ok, e);
  };
  // truncated point data
  auto r = tryBytes(std::vector<char>(bytes.begin(), bytes.begin() + static_cast<long>(bytes.size() / 2)));
  CHECK(!r.first && r.second.find("truncated") != std::string::npos);
  // truncated header
  r = tryBytes(std::vector<char>(bytes.begin(), bytes.begin() + 100));
  CHECK(!r.first);
  // wrong signature
  std::vector<char> b = bytes;
  b[0] = 'X';
  CHECK(!tryBytes(b).first);
  // LAZ flag
  b = bytes;
  b[104] = static_cast<char>(b[104] | 0x80);
  r = tryBytes(b);
  CHECK(!r.first && r.second.find("LAZ") != std::string::npos);
  // zero scale
  b = bytes;
  std::fill(b.begin() + 131, b.begin() + 139, 0);
  CHECK(!tryBytes(b).first);
  // record length too short for the format
  b = bytes;
  b[105] = 10;
  b[106] = 0;
  CHECK(!tryBytes(b).first);
  // random bytes after the signature: must not crash
  std::mt19937 rng(3);
  for (int k = 0; k < 50; ++k) {
    b = bytes;
    for (int j = 0; j < 20; ++j) b[4 + rng() % 370] = static_cast<char>(rng());
    tryBytes(b);
  }
  PointCloud out;
  CHECK(!readLas(tempPath("gsv_does_not_exist.las"), out, err));
  std::filesystem::remove(good);
  std::filesystem::remove(bad);
}
