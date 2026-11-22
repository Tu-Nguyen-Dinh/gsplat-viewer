# Changelog

## v2.0.0 — viewer → editor (23/11/2026)

From a renderer to a tool people can use: a reusable core (survey coordinates, LiDAR, geometry,
LOD, C API), interactive editing on the 3D view, and an interface built for non-experts.
142 tests (120 CPU + 22 GPU, also on Mesa llvmpipe), ASan + UBSan clean. Details per module in
[docs/core/INDEX.md](docs/core/INDEX.md).

### Core library (Phase 4)
- Double-precision Sim(3) frames, relative-to-origin rendering: survey coordinates (10⁶ m) keep
  millimetres (float alone: 3 cm).
- LAS 1.2–1.4 reader/writer, opaque point layers that occlude splats through the depth buffer.
- Per-pixel expected depth from splats; 3D ray / Gaussian picking at the median depth along the ray
  (measured edges stay on the edges when the view changes: worst 0.21 cm instead of 34 cm).
- kd-tree, PCA normals, Horn/Umeyama similarity + RANSAC, point-to-plane ICP (0.31 mm), RANSAC planes.
- Level of detail: Morton octree, moment-matching merges, budgeted cut (a third of the splats at 43 dB).
- C API (`include/gsv/gsv.h`, 38 functions, C99) and headless EGL rendering for tests on servers.

### Editing (Phase 5)
- Rect, lasso, screen brush, 3D sphere brush, one-click Flood selection; add / subtract / intersect.
- Transform gizmo with exact spherical-harmonics rotation; duplicate; colour adjustments on the SH.
- Unlimited undo / redo storing only the changed splats (bit-exact).
- Floater detection (statistical outliers + training-camera visibility), selected for review.
- Distance / polyline / area / height measurements, real scale from a known length, level from the floor,
  notes pinned in 3D, `.gsvproj` project files, CSV.
- GPU selection highlight / hide flags (also for the compressed, Morton-ordered format).

### Usability (Phase 6)
- Menu bar, icon tool bar, status bar, welcome screen with recent files and thumbnails, native dialogs,
  5-step tour, F1 shortcuts; English / Vietnamese; settings that survive a restart; DPI scaling.
- Autosave + crash recovery, unsaved-changes prompts, background saving, history panel, cancellable jobs.
- Smooth camera transitions, axis compass, quick views, frame selection, automatic render scale.
- Gizmo preview on the GPU, "visible only" selection, grow / shrink, undo steps spilled to disk.
- Export: pictures up to 4K, video along saved views (ffmpeg), HTML report.

## Earlier versions
v1.0 – v1.1: the renderer (EWA splatting, GPU radix sort, SH degree 3, compressed 24-byte splats),
phone-video capture pipeline, `.ply` / `.splat` / `.spz` input, Android build. See the
[releases](https://github.com/Tu-Nguyen-Dinh/gsplat-viewer/releases).
