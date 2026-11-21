// Tests for locating files at runtime (shader folder, executable directory, UTF-8 paths).
#include "core/platform.h"
#include "core/utf8_path.h"
#include "gl/shader.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>

using namespace gsv;
namespace fs = std::filesystem;

namespace {
// Creates `<root>/<sub>/splat.vert` (the marker file findShaderDirectory looks for).
void makeShaderDir(const fs::path& dir) {
  fs::create_directories(dir);
  std::ofstream(dir / "splat.vert") << "#version 430\n";
}
}  // namespace

TEST_CASE(utf8_path_roundtrip_keeps_non_ascii) {
  const std::string name = "th\xc6\xb0 m\xe1\xbb\xa5""c/\xe1\xba\xa3nh.ply";  // "thư mục/ảnh.ply"
  CHECK(pathToUtf8(pathFromUtf8(name)) == name);
}

TEST_CASE(executable_directory_contains_the_test_binary) {
  const std::string dir = executableDirectory();
  CHECK(!dir.empty());
  bool found = false;
  for (const auto& e : fs::directory_iterator(pathFromUtf8(dir)))
    found |= e.path().stem().string().rfind("gsplat_tests", 0) == 0;  // gsplat_tests(.exe), gsplat_tests_cpu
  CHECK(found);
}

TEST_CASE(shader_directory_lookup_order) {
  const fs::path root = fs::temp_directory_path() / "gsv_shader_lookup \xc3\xa1";  // space + non-ASCII
  fs::remove_all(root);
  const fs::path exeDir = root / "app";
  const fs::path custom = root / "custom";
  fs::create_directories(exeDir);

#if !defined(__ANDROID__)  // a phone has no source tree to fall back to
  // Nothing next to the exe: falls back to the source tree (GSV_SHADER_DIR has the real shaders).
  CHECK(findShaderDirectory("", pathToUtf8(exeDir)) == GSV_SHADER_DIR);
#endif

  // Packaged layout <exe>/shaders wins over the source tree.
  makeShaderDir(exeDir / "shaders");
  CHECK(findShaderDirectory("", pathToUtf8(exeDir)) == pathToUtf8(exeDir / "shaders"));

  // An explicit --shader-dir wins over everything...
  makeShaderDir(custom);
  CHECK(findShaderDirectory(pathToUtf8(custom), pathToUtf8(exeDir)) == pathToUtf8(custom));
  // ...and is not silently replaced when it is wrong.
  CHECK(findShaderDirectory(pathToUtf8(root / "missing"), pathToUtf8(exeDir)).empty());

  // No candidate has the marker file.
  CHECK(findShaderDirectory("", pathToUtf8(exeDir), "no_such_shader.glsl").empty());

  fs::remove_all(root);
}
