# Core 4 — The core as a C library

## Purpose
One stable binary interface for every application layer: Unity/Unreal plugins, Python tools, C#,
Android (JNI), servers rendering thumbnails without a display.

## Files
- `include/gsv/gsv.h` — the whole API, C99, opaque handles. `GSV_CAPI_VERSION` = 1.
- `src/capi/gsv_capi.cpp` — wrappers; every function catches exceptions and sets a thread-local
  message (`gsv_last_error`).
- Target `gsplat_capi` (SHARED): `libgsplat_capi.so` / `gsplat_capi.dll`. Hidden visibility; on Linux
  `--exclude-libs,ALL` hides zstd/GLFW; on MinGW static libstdc++/libgcc/winpthread, only
  `__declspec(dllexport)` symbols exported. Exports: 38 functions, all `gsv_*`.
- `tests/capi/test_capi.c` — C99 `-pedantic`, target `gsplat_capi_test DIR`, ctest `capi_test`, CI step.

## API groups
| Group | Functions |
|---|---|
| library | `gsv_capi_version`, `gsv_version`, `gsv_last_error`, `gsv_frame_identity`, `gsv_set_shader_dir` |
| scenes | `gsv_scene_load` (.ply/.splat/.spz), `gsv_scene_create_demo`, `gsv_scene_save_ply`, `_size`, `_sh_degree`, `_bounds`, `_focus`, `_free` |
| clouds | `gsv_cloud_load_las`, `gsv_cloud_save_las`, `gsv_cloud_create(origin, xyz, rgb, n)`, `_size`, `_origin`, `_positions`, `_free` |
| geometry | `gsv_estimate_similarity`, `gsv_estimate_similarity_ransac`, `gsv_icp_default_options`, `gsv_icp`, `gsv_fit_plane` |
| LOD | `gsv_lod_build`, `_node_count`, `_cut`, `_save`, `_load`, `_free` |
| render | `gsv_renderer_create(w, h)`, `_set_scene`, `_set_cloud`, `_gpu_name`, `gsv_render(cam, rgb, depth)`, `gsv_pick`, `_free` |

## Conventions (also in the header)
- `gsv_frame`: p' = s·R·p + t, **R row-major**. Status: `GSV_OK`, `GSV_ERROR`, `GSV_INVALID_ARGUMENT`.
- `gsv_icp(source, target, initial)` works in world coordinates including each cloud's origin:
  source points go through `initial` in double, ICP runs in the target's local float frame, the
  result is T(origin)·Δ·T(−origin)·initial.
- `gsv_fit_plane` returns the plane in world coordinates (d shifted by the origin).
- Renderer: one per process (one headless context), calls on the creating thread. Shaders: next to
  the library (`<libdir>/shaders`) else the compiled-in source tree, or `gsv_set_shader_dir`.
  `gsv_render` draws the cloud (opaque) then splats with depth test and per-pixel depth.

## Measured (22/11/2026, container)
- C test: all checks pass, also under ASan (leak check on); ICP recovers (−0.0500, 0, −0.0301) for a
  5 cm / 3 cm shift; offscreen render on llvmpipe: image mean 74.8, 11 011 / 19 200 px with depth;
  point wall in front: centre depth 2.302 vs 2.289 expected.
- Library size: 0.93 MB (Linux, Release).

## Next
- Python package (cffi) and a Unity native plugin sample; Windows DLL to be built and tested on the
  user's machine (⏳).
