// Test runner:  gsplat_tests [--cpu-only | --gpu-only] [name-filter]
#include "gl_test_context.h"
#include "test_framework.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>

namespace test {

namespace {
int gCurrentFailures = 0;
const char* volatile gCurrentTest = nullptr;  // read by the crash handler

// A crash would otherwise end the run silently (CI pipes stdout, so buffered output is lost):
// name the test that was running, then exit with the conventional 128 + signal code.
extern "C" void onCrash(int sig) {
  std::fprintf(stderr, "\n*** CRASH (signal %d) in test: %s\n", sig, gCurrentTest ? gCurrentTest : "<startup>");
  std::_Exit(128 + sig);
}
}  // namespace

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

void reportFailure(const char* file, int line, const std::string& message) {
  ++gCurrentFailures;
  std::printf("    FAIL %s:%d  %s\n", file, line, message.c_str());
}

}  // namespace test

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // every line reaches the log even if a test crashes
  for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) std::signal(sig, test::onCrash);

  bool cpu = true, gpu = true;
  const char* filter = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--cpu-only") == 0) gpu = false;
    else if (std::strcmp(argv[i], "--gpu-only") == 0) cpu = false;
    else filter = argv[i];
  }

  bool haveGl = false;
  std::string reason;
  if (gpu) {
    haveGl = test::createGlContext(reason);
    if (!haveGl) std::printf("GPU tests skipped: %s\n", reason.c_str());
  }

  int passed = 0, failed = 0, skipped = 0;
  for (const test::Case& c : test::registry()) {
    if ((c.gpu && !gpu) || (!c.gpu && !cpu)) continue;
    if (filter && !std::strstr(c.name, filter)) continue;
    if (c.gpu && !haveGl) {
      ++skipped;
      continue;
    }
    test::gCurrentFailures = 0;
    test::gCurrentTest = c.name;
    const auto t0 = std::chrono::steady_clock::now();
    c.fn();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const bool ok = test::gCurrentFailures == 0;
    std::printf("[%s] %-44s %8.1f ms\n", ok ? " OK " : "FAIL", c.name, ms);
    (ok ? passed : failed)++;
  }
  if (haveGl) test::destroyGlContext();
  std::printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
  return failed == 0 ? 0 : 1;
}
