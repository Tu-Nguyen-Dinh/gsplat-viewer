# Third-party software

gsplat-viewer is MIT-licensed (see `LICENSE`). The release binary statically links the following
libraries; their licenses permit redistribution in binary form with this notice.

| Library | Version | Use | License |
|---|---|---|---|
| [GLFW](https://www.glfw.org/) | 3.4 | window, OpenGL context, input | zlib/libpng |
| [GLM](https://github.com/g-truc/glm) | 1.0.1 | vector / matrix math | MIT (The Happy Bunny License alternative) |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9b | user interface | MIT |
| [stb_image_write](https://github.com/nothings/stb) | vendored in `third_party/stb` | PNG output | MIT or public domain (Unlicense) |
| [Khronos OpenGL headers](https://github.com/KhronosGroup/OpenGL-Registry) | vendored in `third_party/khronos` | `glcorearb.h`, `khrplatform.h` | MIT-style (Khronos) |
| MinGW-w64 runtime / libstdc++ | GCC 15.2 | C/C++ runtime (static) | GCC Runtime Library Exception / ZPL / public domain |

Test data: `tests/data/minimal.splat` comes from PlayCanvas
[splat-transform](https://github.com/playcanvas/splat-transform) (MIT).
