#pragma once

#include <string>

namespace gsv {

// Directory that contains the running executable (UTF-8, forward slashes), or "" if unknown.
// Used to find files shipped next to the .exe (e.g. shaders/) regardless of the working directory.
std::string executableDirectory();

}  // namespace gsv
