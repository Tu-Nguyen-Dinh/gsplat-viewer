#pragma once
// Minimal self-registering test framework (no external dependency).
//
//   TEST_CASE(my_test) { CHECK(1 + 1 == 2); CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9); }
//   GPU_TEST_CASE(...) runs only when an OpenGL 4.3 context could be created.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace test {

struct Case {
  const char* name;
  bool gpu;
  void (*fn)();
};

std::vector<Case>& registry();
void reportFailure(const char* file, int line, const std::string& message);

struct Registrar {
  Registrar(const char* name, bool gpu, void (*fn)()) { registry().push_back({name, gpu, fn}); }
};

}  // namespace test

#define TEST_CASE(name)                                              \
  static void name();                                                \
  static const test::Registrar name##_registrar(#name, false, name); \
  static void name()

#define GPU_TEST_CASE(name)                                         \
  static void name();                                               \
  static const test::Registrar name##_registrar(#name, true, name); \
  static void name()

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) test::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

// Like CHECK, but stops the test: use it for preconditions the rest of the test relies on
// (e.g. "the file loaded"), so a failure is reported instead of crashing on an empty vector.
#define REQUIRE(cond)                                                  \
  do {                                                                 \
    if (!(cond)) {                                                     \
      test::reportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");   \
      return;                                                          \
    }                                                                  \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                      \
  do {                                                                                             \
    const double gsvA_ = static_cast<double>(a), gsvB_ = static_cast<double>(b);                   \
    if (!(std::fabs(gsvA_ - gsvB_) <= static_cast<double>(tol)))                                   \
      test::reportFailure(__FILE__, __LINE__,                                                      \
                          std::string("CHECK_NEAR(" #a ", " #b ")  ") + std::to_string(gsvA_) +    \
                              " vs " + std::to_string(gsvB_) + "  tol " + std::to_string(tol));    \
  } while (0)
