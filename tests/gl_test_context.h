#pragma once

#include <string>

namespace test {

// Creates a hidden window with an OpenGL 4.3 core context and loads the GL functions.
// Returns false (with a reason) on machines without a suitable GPU/driver.
bool createGlContext(std::string& reason);
void destroyGlContext();

}  // namespace test
