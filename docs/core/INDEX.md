# Core library: index for contributors and for Claude

Read this file and the `COREn-*.md` you need **instead of the source**. Each file lists the public API,
the invariants the code relies on, data layouts, the tests that pin the behaviour (with measured
results) and the known pitfalls. When you change a core module, update its file in the same change
(PLAN.md rule D9).

| Core | What it gives every application | Doc | Main files |
|---|---|---|---|
| 0 | OpenGL tests without a display (EGL surfaceless / Mesa llvmpipe), on CI too | [CORE0-testing.md](CORE0-testing.md) | `src/gl/headless_context.*`, `tests/` |
| 1 | Real-world coordinates (double Sim(3)), multi-layer scenes, point-cloud rendering, expected-depth output, splats occluded by opaque layers | [CORE1-scene.md](CORE1-scene.md) | `core/geo_frame`, `scene/point_cloud`, `scene/layers`, `render/cloud_renderer`, `gl/framebuffer`, `shaders/splat.*`, `shaders/cloud.*` |
| 2 | Level of detail for big splat scenes: octree, moment-matching merge, budgeted cut, `.gsvlod` | [CORE2-lod.md](CORE2-lod.md) | `scene/lod` |
| 3 | Geometry: LAS 1.2–1.4 I/O, kd-tree, normals, Horn/Umeyama Sim(3), RANSAC, point-to-plane ICP, plane fit, picking, measuring | [CORE3-geometry.md](CORE3-geometry.md) | `scene/las_io`, `core/kdtree`, `core/registration`, `core/picking` |
| 4 | The core as a C library (`include/gsv/gsv.h`, `libgsplat_capi`), headless rendering | [CORE4-capi.md](CORE4-capi.md) | `include/gsv/gsv.h`, `src/capi/gsv_capi.cpp`, `tests/capi/test_capi.c` |
| 6 | User experience: menu / status bar / welcome screen, Vietnamese + English, settings, autosave + crash recovery, unsaved prompts, history panel, compass + quick views, auto quality, GPU gizmo preview, visible-only selection, undo on disk, export (picture, MP4, HTML report) | [CORE6-ux.md](CORE6-ux.md) | `core/settings`, `core/i18n`, `core/json`, `app/shell_ui`, `app/session_ui`, `app/nav_ui`, `app/export_ui` |
| 5 | Editing: screen / 3D / flood selection, floater detection, transforms with SH rotation, colour, multi-level undo, `.gsvproj` project file, GPU selection flags | [CORE5-editing.md](CORE5-editing.md) | `scene/edit_ops`, `scene/edit_history`, `scene/project_file`, `render/splat_renderer` (flags), `app/tools_ui.cpp` |

Older, pre-core parts (renderer, sort, codec, file formats) are described in `README.md`,
`docs/MATH.md` and `docs/BENCHMARKS.md`.

## Layering (enforced by review, rule D10)

```
core/    maths, geometry, picking            no OpenGL, no UI       (geo_frame, kdtree, registration, picking, image)
scene/   data + file formats                 no OpenGL              (point_cloud, layers, las_io, lod, ...)
gl/      OpenGL wrappers + headless context
render/  renderers                           OpenGL, no UI          (splat_renderer, cloud_renderer)
capi/    C API over all of the above         no UI
app/     viewer (GLFW + ImGui)               the only UI layer      (layers_ui.cpp Cores 1-3, tools_ui.cpp Core 5)
```

## Status at the end of Phase 6 (23/11/2026)

- 142 C++ tests (120 CPU + 22 GPU) + C API test pass; ASan + UBSan clean; 406 Vietnamese UI strings checked.
- Viewer driven interactively in the container (Xvfb + XTest): welcome, language, tour, recovery after kill -9,
  unsaved prompt, history, compass, GPU gizmo preview, visible-only, report, MP4.
- Pending on the user's machine: Windows build of Phase 6, FPS while moving / dragging, export times.

## Status at the end of Phase 5 (22/11/2026)

- 132 C++ tests (112 CPU + 20 GPU) and the C99 API test pass on Linux with Mesa llvmpipe; `edit5_*` also
  under ASan + UBSan; 11 shaders compile as GLSL ES 3.10.
- The viewer tools were driven interactively in the container (Xvfb + llvmpipe, X11 test driver) on the phong
  scene: Flood, Shift-add, gizmo move / turn, undo / redo, Rect + Delete + undo, Measure, Note, Ctrl+S.
- Not yet verified on the user's machine: Windows build of the new tools, RTX 3060 frame and preview times.

## Status at the end of Phase 4 (22/11/2026)

- 116 C++ tests (98 CPU + 18 GPU) and the C99 API test pass on Linux with Mesa llvmpipe, also under
  ASan + UBSan. All 11 shaders compile as GLSL ES 3.10 (Mesa GLES 3.2 context).
- Not yet verified on the user's machines: Windows/MinGW build, RTX 3060 frame times, Android build
  and S23 Ultra runs, interactive use of the new viewer panel. See PLAN.md Phase 4 (⏳ rows).

## Quick recipes

| I want to… | Use |
|---|---|
| show a LAS scan with a splat scene | `gsplat_viewer scene.ply --las scan.las --las-transform s,qw,qx,qy,qz,tx,ty,tz` (CORE1/3) |
| align two scans | `gsv_icp` (C) or `icpPointToPlane` (C++) after a rough initial frame (CORE3) |
| align COLMAP to metric poses | `estimateSimilarity` / `estimateSimilarityRansac` on camera centres (CORE3) |
| get a 3D point under the mouse (measuring) | `raycastSplats` — 3D ray median depth (CORE3). Not the expected depth: it floats at edges |
| draw a 20 M splat scene on a budget | `buildLod` once, `selectCut` + `gatherCut` when the camera moves (CORE2) |
| use the core from Python/C#/Unity | `libgsplat_capi` + `include/gsv/gsv.h` (CORE4) |
| run GPU tests on a server | just run `gsplat_tests`: no display → EGL surfaceless (CORE0) |
| move / turn / scale splats without breaking their colours | `transformSplats` (rotates SH bands 1–3 exactly) (CORE5) |
| undo anything | `EditHistory::modify / remove / duplicate`; drags: `begin / restore / commit` (CORE5) |
| find floaters | `selectOutliers` (+ `selectUnseen` with COLMAP cameras), then review and delete (CORE5) |
| highlight / hide splats on the GPU | `SplatRenderer::setSelectionFlags` (scene order, bit 0 / bit 1) (CORE5) |
| save measurements and notes | `Project` + `saveProject` (`.gsvproj`, scene coordinates) (CORE5) |
| add a UI string | `tr("English")` / `trId("English")` for widget labels; translation in `core/i18n_vi.inc`; `python3 tools/i18n_extract.py` lists missing ones (CORE6) |
| remember a user preference | a field in `Settings` + JSON in/out (defaults on bad values) (CORE6) |
| an action that could lose work | wrap it in `requestDiscard([...]{ ... })` (CORE6) |
| a long job | `std::async` + poll in the loop; cancel = move the future to an `abandoned*_` list (CORE6) |
| test the viewer UI without a desktop | Xvfb + GLFW with X11, drive it with XTest (CORE0 §interactive) |
