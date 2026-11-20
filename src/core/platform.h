#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace gsv {

// Command-line arguments as UTF-8. On Windows `argv` is in the ANSI code page, so a path such as
// "thử nghiệm" arrives as "th? nghi?m"; this re-reads the wide-character command line instead.
// Elsewhere it simply copies argv.
std::vector<std::string> utf8Arguments(int argc, char** argv);

// Keeps the strings alive and exposes them as a classic argc/argv pair for existing parsers.
struct Utf8Argv {
  std::vector<std::string> strings;
  std::vector<char*> pointers;
  Utf8Argv(int argc, char** argv);
  int argc() const { return static_cast<int>(strings.size()); }
  char** argv() { return pointers.data(); }
};

// Directory that contains the running executable (UTF-8, forward slashes), or "" if unknown.
// Used to find files shipped next to the .exe (e.g. shaders/) regardless of the working directory.
std::string executableDirectory();

// Child processes (used by tools that drive ffmpeg). Commands are UTF-8 and run through the
// system shell; on Windows they go through the wide-character API so non-ASCII paths work.
std::string quoteArgument(const std::string& arg);           // "arg", embedded quotes escaped
std::FILE* openProcessRead(const std::string& utf8Command);  // the command's stdout, binary mode
int closeProcess(std::FILE* process);                        // waits; returns the exit code
int runProcess(const std::string& utf8Command);              // waits; returns the exit code

}  // namespace gsv
