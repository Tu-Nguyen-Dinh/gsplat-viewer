#pragma once
// Core 4: an OpenGL 4.3 core context without a visible window, for tests, command-line tools and
// the C API (include/gsv/gsv.h). See docs/core/CORE4-capi.md.
//
//   1. GLFW hidden window (Windows, macOS, Linux with a display);
//   2. Linux fallback: EGL on Mesa's surfaceless platform (no display server at all: CI runners,
//      containers, servers). With llvmpipe this is OpenGL 4.5 on the CPU.
// libEGL is opened at run time (dlopen): no EGL headers or link dependency.
// Desktop only (Android creates its EGL context in android/app_main.cpp).

#include <string>

namespace gsv {

// Creates the context, makes it current on the calling thread and loads the GL functions.
// Returns false with a reason. Only one headless context per process.
bool createHeadlessGlContext(std::string& reason);
void destroyHeadlessGlContext();
bool headlessGlContextActive();
// "llvmpipe (LLVM ...)", "NVIDIA GeForce RTX 3060/PCIe/SSE2", ... (empty without a context).
std::string glRendererName();

}  // namespace gsv
