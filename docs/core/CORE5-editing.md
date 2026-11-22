# Core 5 — Editing: selection, transforms, history, project file

What every application gets: select splats on screen or in 3D, move / turn / scale / recolour them
without breaking their look, undo and redo any number of steps, and keep the user's measurements,
notes and views in a project file. The viewer's interactive tools (`app/tools_ui.cpp`) are a thin
layer over this.

## Files and API
### `scene/edit_ops.h` (no OpenGL)
Masks are `SplatMask` (`std::vector<uint8_t>`, one byte per splat, scene order).
- **Set operations**: `combineSelection(mask, next, SelectOp::{Replace, Add, Subtract, Intersect})`,
  `selectAll`, `selectionCentroid` (scene coordinates).
- **Screen selection** (window pixels, origin top-left; `mvp` = projection·view·model):
  `selectScreenRect`, `selectScreenPolygon` (even-odd, concave OK, bounding box pre-test),
  `selectScreenCircle`, `pointInPolygon`. A splat is in when its **centre** projects inside and is in
  front of the camera: splats hidden behind others are selected too (like SuperSplat).
- **3D selection**: `selectSphere(scene, model, centre, radius)` (display space),
  `nearestSplat(scene, p, minOpacity)`, `selectConnected(scene, seed, maxGap, colorTolerance,
  minOpacity, maxCount, tree*)` — region growing (see pitfalls), `typicalSpacing(scene, tree)` = median
  nearest-neighbour distance of opaque splats, the unit for gap defaults.
- **Floaters**: `selectOutliers(scene, k = 16, stdRatio = 2.5)` — statistical outlier removal: mean
  distance to the k nearest neighbours > mean + stdRatio·σ of the scene. `selectUnseen(scene,
  CameraView[], minViews)` — centre seen by fewer than minViews training cameras (in front, inside the
  image with y down, closer than maxDistance).
- **Transform**: `transformSplats(scene, mask, affine)` — affine = rotation · uniform scale · translation
  (no shear). s = cbrt(det M₃), R = M₃/s; position ← M·p; quaternion ← quat(R)·q (stored w,x,y,z);
  logScale += log s; SH bands 1–3 rotated with `shRotation(R)` / `rotateShRest`.
- **SH rotation**: `ShRotation{band1 3×3, band2 5×5, band3 7×7}` (row-major, coefficient order of
  `evalSphericalHarmonics`). Built by least squares, per band: with Ya[s][k] = Y_k(d_s) and
  Yb[s][k] = Y_k(Rᵀ d_s) on 64 Fibonacci directions, X = (YaᵀYa)⁻¹ YaᵀYb and c' = X c. Exact (the
  rotated band is in the span of the band), no Wigner-D formulas to get wrong.
- **Colour**: `adjustColor(scene, mask, ColorAdjust{brightness, contrast, saturation, temperature})`
  on the base colour c = 0.5 + C0·dc; contrast and saturation also scale the view-dependent bands, so
  saturation 0 is grey from every direction. `isNeutral()` → exact no-op.
- **Scale / level helpers**: `scaleForKnownLength(a, b, real)`, `rotationBetween(from, to)` (handles
  opposite vectors).
- **Measuring**: `polylineLength`, `polygonArea` (Newell normal, computed relative to the first vertex).

### `scene/edit_history.h`
- `EditHistory(byteBudget = 768 MB)`: commands store only what they change.
  `modify(scene, mask, label, op)` (before/after of the masked splats), `remove(scene, mask, label)`
  (removed splats + indices; order of the others kept), `duplicate(scene, mask, label)` (copies appended).
- `undo` / `redo` restore bit-exactly; a new command drops the redo branch; the oldest commands are
  dropped above the budget (at least one kept). `undoLabel`, `redoLabel`, `bytes`, `size`, `cursor`.
- **Live edits** (gizmo drag, colour slider): `begin(scene, mask, label)` saves the masked splats,
  each preview does `restore()` and re-applies the **total** change, `commit()` records one command,
  `cancel()` restores. No drift from accumulating small steps.

### `scene/project_file.h` — `.gsvproj` (JSON, UTF-8)
- `Project{version, scenePath, lasPath, lasTransform[8], unit, unitScale, level[4] (w,x,y,z),
  bookmarks[], annotations[], measurements[]}`. **All positions are scene coordinates**; scale and level
  are properties, never baked into the splats — undoing a splat edit can never leave a measurement out
  of step with the scene.
- `projectToJson` / `projectFromJson` (hand-written parser: depth ≤ 64, ≤ 64 MB, `\u` escapes,
  unknown keys ignored, every malformed input rejected with a message), `saveProject`, `loadProject`.
- `measurementValue(m, up)` (scene units; height along `up`), `measurementRealValue(m, unitScale, up)`
  (area × s²), `measurementsToCsv(m, unit, unitScale)`.

### GPU: edit flags (`render/splat_renderer.h`, `shaders/splat_preprocess.comp`)
- `SplatRenderer::setSelectionFlags(sceneFlags)` — one byte per splat in **scene order**, bit 0 selected
  (colour mixed with `SplatSettings::highlight` rgb by a), bit 1 hidden (culled like a transparent splat).
  Re-ordered on the CPU with `gpuToScene()` (Morton order for the compressed format), packed 4 per uint,
  SSBO binding 9 (`BINDING_FLAGS`). `upload()` clears the flags; a wrong size is ignored.

### Viewer (`app/tools_ui.cpp`)
Tools 1–9: Navigate, Rect, Lasso, Brush (screen circle), Sphere (3D ball on the picked surface), Flood,
Transform (gizmo: axis arrows, view-plane square, Y ring; one undo step per drag), Measure (distance /
polyline / area / height), Note (pin + its view). Shift add, Alt subtract, Shift+Alt intersect; with a
tool the right button orbits and the middle button pans. Keys: Delete, H / Alt+H hide / show, I isolate,
Ctrl+A/D/I all / none / invert, Ctrl+Z / Ctrl+Y (Ctrl+Shift+Z), Ctrl+S project, [ ] brush radius, Enter
finish, Backspace remove point, Esc cancel → Navigate. Auto-clean runs on a worker thread and only
**selects** candidates for review. Scale (from a Distance measurement + real length) and Level (plane fit
of the selected floor → `level_` in the model matrix) are non-destructive project properties.

## Tests (measured 22/11/2026, container: Mesa llvmpipe, 2 cores)
| ER | Test | Result |
|---|---|---|
| 5.1 | `edit5_screen_selection` | rect 6, circle 5, concave polygon and triangle exact; splat behind the camera never selected |
| 5.2 | `edit5_select_ops_truth_table` | 4 ops × 4 cases exact |
| 5.3 | `edit5_brush_and_connected` | sphere brush: 0 floor splats; flood: 2402 / 2402 of the box, 0 others |
| 5.3 | `edit5_connected_noisy_colours` (σ 0.08 colour noise, ¼ transparent) | 2348 / 2352 (99.8 %), 0 floor |
| 5.4 | `edit5_sh_rotation_exact` (5 rotations × 200 dirs × degrees 1–3) | max colour error **2.7e-7** (ER ≤ 1e-5) |
| 5.4 | `edit5_transform_splats` (R + s 2.5 + t) | position < 1e-5, Σ' = M Σ Mᵀ relative < 1e-4 |
| 5.5 | `edit5_history_bit_exact` (6 mixed ops, 2000 splats, SH 3) | every undo / redo state bit-exact; redo branch dropped; budget keeps ≥ 1 |
| 5.5 | `edit5_history_live_edit`, `edit5_history_duplicate` | 20 previews = 1 command = direct apply (bit-exact); cancel exact; duplicate undo / redo exact |
| 5.6 | `edit5_floater_detection` (200 floaters ≥ 0.3 from surfaces, 26 278 surface splats) | **200 / 200** found, **0** false positives (ER ≥ 90 % / ≤ 1 %); 131 ms |
| 5.7 | `edit5_color_adjust` | neutral bit-exact; saturation 0 → luma, view-dependent colour grey (< 1e-5) |
| 5.8 | `edit5_scale_and_level` | known length exact; level residual ≤ 0.01° |
| 5.9 | `edit5_measurements` | polyline 7 exact; 3 × 4 rectangle at (5e5, 1.2e6, 30) tilted: 12 ± 1e-9 |
| 5.10 | `edit5_project_file` | round trip (UTF-8 path, quotes, newlines, scale, level, CSV); 12 malformed inputs + 10 000 nested `[` rejected |
| 5.11 | `gpu_selection_flags_fp32`, `_compressed` | highlight colour within fp16 (4e-3); hidden = culled count exact; clear restores |
| — | ASan + UBSan | all `edit5_*` clean |

Interactive check (container, Xvfb + llvmpipe, phong scene 232 553 splats, X11 driver): Flood on the printer
863 splats in one click (+ Shift-click front → 1435); gizmo Y drag, ring turn, Ctrl+Z / Ctrl+Y; Rect on the chair
→ Delete → Ctrl+Z; Measure distance; Note + Ctrl+S → `.gsvproj` + `.measurements.csv`. Frame times on the
user's RTX 3060: not measured yet (PLAN Phase 5 ⏳).

## Pitfalls found
- **Flood fill on real 3DGS colours** (phong printer): comparing each splat with the neighbour that reached
  it selected **9 splats**; a fixed gap of 1 % of the scene radius selected 14. Single splat colours are noisy
  (neighbours of one dark seed ranged 0.18–0.54) because the look comes from overlapping splats. Fix: compare
  with the region's running mean colour, gap in multiples of the measured `typicalSpacing` (default 5×), let
  transparent splats join without spreading. Tried and rejected: ellipsoid-overlap adjacency
  (dist ≤ k(σᵢ+σⱼ)) — the few large splats bridge everything (87 k–228 k splats, whole room).
- **`evalSphericalHarmonics` adds 0.5 and clamps at 0**: it cannot be used to sample the SH basis (negative
  basis values are clipped). `edit_ops.cpp` has its own basis with the same constants and signs.
- **Polygon area of georeferenced points**: Σ pᵢ × pᵢ₊₁ with coordinates ~1e6 lost 3.3e-5 of a 12 m² area
  (catastrophic cancellation). Computed relative to the first vertex: exact to 1e-9.
- **Quick key taps lost at low FPS** (1.5 FPS in software GL; heavy scenes on weak GPUs): polling with
  `glfwGetKey` missed press+release between two frames. `GLFW_STICKY_KEYS` / `GLFW_STICKY_MOUSE_BUTTONS`; read
  a modifier once per frame (sticky state is cleared by the first read).
- **Image y convention in `selectUnseen`**: camera y is up, image y is down: py = cy − fy·y/z.
- Tests: `0.01f * 20 != 0.2f` — compare with the same float expression; grid points exactly on a polygon edge
  are ambiguous — keep a margin.

## Known limits (→ PLAN Phase 6)
- Screen selection takes hidden-behind splats (no "visible only" option yet: needs the depth / ID buffer).
- Drag preview re-uploads the whole scene (throttled to 60 ms): fine at 0.2 M splats, to be measured at 2.5 M;
  a GPU-side transform of the selected splats would make it free.
- Undo stores whole splats (240 B per splat at SH 3: 56 B attributes + 180 B SH + 4 B index): deleting 2 M splats costs ~480 MB of history.
- Flood fill is a heuristic: objects touching surfaces of similar colour need Shift-click / Brush to finish.
