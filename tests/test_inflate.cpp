// Tests for the DEFLATE / gzip decoder (src/core/inflate.*). Fixtures were made with GNU gzip:
// stored blocks (random data), fixed Huffman (a short string), dynamic Huffman (text, -9 and -1).
#include "core/inflate.h"
#include "test_framework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace gsv;

namespace {

std::filesystem::path fixture(const char* name) {
  return std::filesystem::path(GSV_TEST_DATA_DIR) / "inflate" / name;
}

std::vector<uint8_t> readAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Same text as the awk command that made text.txt.gz (not stored in the repository).
std::vector<uint8_t> expectedText() {
  std::string s;
  char line[96];
  for (int i = 0; i < 6000; ++i) {
    std::snprintf(line, sizeof(line), "line %d value %d gaussian splat\n", i, (i * i) % 97);
    s += line;
  }
  return {s.begin(), s.end()};
}

bool gunzipFile(const char* name, std::vector<uint8_t>& out, std::string& error) {
  const std::vector<uint8_t> gz = readAll(fixture(name));
  return gunzip(gz.data(), gz.size(), out, error);
}

}  // namespace

TEST_CASE(inflate_crc32_known_value) {
  const char* s = "123456789";  // the standard CRC-32 check value
  CHECK(crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);
}

TEST_CASE(inflate_gzip_fixtures) {
  std::vector<uint8_t> out;
  std::string error;
  REQUIRE(gunzipFile("text.txt.gz", out, error));       // dynamic Huffman, gzip -9
  CHECK(out == expectedText());
  REQUIRE(gunzipFile("text_fast.txt.gz", out, error));  // dynamic Huffman, gzip -1
  CHECK(out == expectedText());
  REQUIRE(gunzipFile("hello.txt.gz", out, error));      // fixed Huffman + a back reference
  CHECK(out == readAll(fixture("hello.txt")));
  REQUIRE(gunzipFile("random.bin.gz", out, error));     // incompressible: stored blocks
  CHECK(out == readAll(fixture("random.bin")));
  REQUIRE(gunzipFile("empty.bin.gz", out, error));
  CHECK(out.empty());
}

// ER 2.5 (part): damaged input must be reported, never crash or loop.
TEST_CASE(inflate_rejects_damaged_input) {
  const std::vector<uint8_t> gz = readAll(fixture("text.txt.gz"));
  REQUIRE(gz.size() > 1000);
  std::vector<uint8_t> out;
  std::string error;
  for (size_t cut : {size_t(5), size_t(30), gz.size() / 2, gz.size() - 3}) {  // truncated
    CHECK(!gunzip(gz.data(), cut, out, error));
    CHECK(!error.empty());
  }
  int rejected = 0;
  for (size_t at = 20; at < gz.size() - 8; at += gz.size() / 37) {  // one flipped byte each
    std::vector<uint8_t> bad = gz;
    bad[at] ^= 0x5A;
    error.clear();
    if (!gunzip(bad.data(), bad.size(), out, error)) ++rejected;
  }
  CHECK(rejected >= 30);  // the CRC-32 catches what the decoder itself does not
  const uint8_t notGzip[20] = {'P', 'K', 3, 4};
  CHECK(!gunzip(notGzip, sizeof(notGzip), out, error));
}
