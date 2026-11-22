# Core 0 — Headless OpenGL testing

## Purpose
Every GPU code path can be tested on machines without a display or GPU (CI runners, containers),
using Mesa's software rasterizer llvmpipe (OpenGL 4.5 core on the CPU).

## Files
- `src/gl/headless_context.{h,cpp}` — `createHeadlessGlContext(reason)`, `destroyHeadlessGlContext()`,
  `headlessGlContextActive()`, `glRendererName()`.
- `tests/gl_test_context.cpp` — the test runner's context = the headless context.
- `tests/test_framework.h` — `test::softwareRenderer()`.

## How the context is created
1. GLFW hidden 64×64 window, OpenGL 4.3 core (Windows, macOS, Linux with a display).
2. Linux only, if 1 fails: `dlopen("libEGL.so.1")`, `eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)`,
   `eglCreateContext` with no config (EGL_KHR_no_config_context), 4.3 core, made current with no
   surface. GL functions via `eglGetProcAddress`. No EGL headers, no link dependency.
3. On `GSV_GLES` builds the functions are stubs that return false (Android creates its own EGL context).

## Invariants
- One headless context per process, current on the creating thread.
- The EGL context is intentionally not destroyed at exit (some drivers crash in atexit teardown).

## Driver differences (rule D12)
| Behaviour | Real GPU (RTX 3060) | llvmpipe | Handling |
|---|---|---|---|
| 8-bit blend rounding | loses faint splats (8-bit < 16F) | exact | `gpu_render_matches_cpu_reference` only reports the ordering on software renderers |
| Visible-list order with "unsorted" mode | random (parallel atomics) | index order | `unsorted_render_is_detected` shuffles + uploads uncompressed (Morton order would composite almost correctly: 41 dB) |
| GPU timers | real | ~0 | never use llvmpipe numbers for speed (rule D13) |

## Commands
```bash
# Linux without X11 headers (container): GLFW "null" platform, GPU tests through EGL
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGLFW_BUILD_WAYLAND=OFF -DGLFW_BUILD_X11=OFF
cmake --build build --target gsplat_tests gsplat_capi_test
./build/gsplat_tests            # CPU + GPU
./build/gsplat_capi_test /tmp   # C API
```
The viewer itself still needs `libgl-dev` (imgui_impl_glfw includes `GL/gl.h`); in a container add
`-DCMAKE_CXX_FLAGS=-DGLFW_INCLUDE_NONE` to compile it.

GLSL ES check without the NDK: `gsplat_shaderdump DIR --es`, then compile each file in a Mesa GLES 3.1
EGL context (the same dlopen approach); used in Phase 4 to verify all 11 stages.

## Measured (22/11/2026, container, llvmpipe LLVM 20)
- 17/17 GPU tests, 97/97 CPU tests, also under ASan + UBSan; GPU tests take ~12 s on 2 cores.

## Interactive UI tests (Phase 5)
The viewer itself (GLFW + ImGui) can be driven on a Linux box without a desktop:
1. `Xvfb :99 -screen 0 1280x800x24 &` — a virtual X server; Mesa llvmpipe gives OpenGL 4.5.
2. GLFW needs the Xrandr / Xinerama / XInput2 / Xcursor **headers** at build time (it dlopens the
   libraries). Where the -dev packages are missing (the dev container, no network), use the minimal
   headers in `tools/uitest/xstub/` (real struct layouts) and configure a separate build:
   `cmake -S . -B build-x11 -DGLFW_BUILD_X11=ON -DCMAKE_C_FLAGS=-I$PWD/tools/uitest/xstub
   -DX11_Xrandr_INCLUDE_PATH=$PWD/tools/uitest/xstub -DX11_Xinerama_INCLUDE_PATH=... -DX11_Xcursor_INCLUDE_PATH=...
   -DX11_Xi_INCLUDE_PATH=...` (Xshape / Xkb from /usr/include).
3. `tools/uitest/xctl.c` (build: `gcc xctl.c -lX11 -l:libXtst.so.6`) sends XTest events and grabs the
   screen: `DISPLAY=:99 xctl move X Y down 1 sleep 2000 move ... up 1 key 6 kdown Shift_L click 1 kup Shift_L shot out.ppm`.
At ~1.5 FPS (software GL, 0.23 M splats) leave ≥ 1.5 s between steps of a drag; key taps are safe since
the viewer uses sticky keys. Stop the viewer by PID only.
