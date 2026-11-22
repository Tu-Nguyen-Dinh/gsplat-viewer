# Core 6 — User experience: shell, safety, navigation, export

Phase 6 makes the viewer usable without reading documentation and without losing work. Most of it
lives in the viewer (`src/app/*_ui.cpp`); the testable pieces are in `core/` and `scene/`.

## Files
| File | What |
|---|---|
| `core/settings.{h,cpp}` | `Settings` JSON in `settingsDirectory()` (`%APPDATA%\gsplat-viewer`, `$XDG_CONFIG_HOME` / `~/.config`, override `GSV_SETTINGS_DIR`); `writeFileAtomic` (temp + rename); recent files (12, dedupe with `samePath`: case-insensitive and `\`=`/` on Windows); Flip Y per file; `currentProcessId()` |
| `core/i18n.{h,cpp}`, `core/i18n_vi.inc` | `tr(english)` text, `trId(english)` widget label `"tiếng Việt###english"` (stable ImGui ID across languages); `setLanguage`, `systemLanguage`, `languageFromSetting("auto"/"en"/"vi")` |
| `core/json.{h,cpp}` | the defensive parser (moved from project_file): `parseJson(text, out, err, maxDepth)`, `jsonQuote`, `jsonNumber` |
| `scene/edit_history` | + `revision()` (dirty tracking), `setSpill(dir, threshold)` (big steps to disk) |
| `scene/edit_ops` | + `keepVisible`, `growSelection`, `shrinkSelection`, `selectConnectedWithin` |
| `render/splat_renderer` | + `SplatSettings::previewActive / previewAffine` (gizmo preview on the GPU) |
| `app/shell_ui.cpp` | menu bar, status bar, welcome screen + thumbnails, shortcut sheet (F1), 5-step tour, fonts / DPI, Open/Save dialogs |
| `app/file_dialog.{h,cpp}` | native dialogs (Windows `GetOpenFileNameW` / `GetSaveFileNameW`, comdlg32); elsewhere a path prompt |
| `app/session_ui.cpp` | unsaved-changes prompt, background PLY save, autosave + recovery, History panel, cancel |
| `app/nav_ui.cpp` | camera animation, quick views, compass, frame selection, auto quality (`renderMainView`) |
| `app/export_ui.cpp` | picture (1×/2×/4K), video along saved views (PNG → MP4 via ffmpeg), HTML report |
| `tools/i18n_extract.py` | lists UI strings without a translation (also scans `kTools`, `kShortcuts`, `kSteps`, `kTitles`, `kHistoryLabels`) |

## Design decisions (and why)
- **English text is the key** (`tr("Delete selection")`). No ID tables to keep in sync; a missing translation
  shows English. Widget labels use `trId` → `"Xoá vùng chọn###Delete selection"`: ImGui hashes only the part
  after `###`, so switching language at runtime keeps open headers, focus and window state.
- **Format strings are translated too** (`snprintf(buf, n, tr("%zu selected"), k)`): the test
  `ux_i18n_formats_match` checks that every translation has the same printf specifiers in the same order (a
  missing `%s` would read garbage). Tooltips without arguments go through `"%s"` (never a translated format).
- **Font**: the built-in ImGui font is ASCII. `setupFonts()` loads a system font (Segoe UI / Arial / Tahoma on
  Windows; Noto / DejaVu / Liberation on Linux) with `GetGlyphRangesVietnamese()`, size 16 px × UI scale. No
  font found → ASCII font and English forced. `ux_i18n_glyphs_in_font_ranges` checks every Vietnamese code
  point is in those ranges. The atlas is rebuilt between frames when the scale changes.
- **Command line beats settings** (`Options::sizeGiven / shGiven / backgroundGiven`). Screenshot / benchmark /
  turntable / evaluate runs (`interactive_ == false`) never read nor write the settings.
- **Dirty tracking**: scene = `EditHistory::revision()` at the last save / load (a counter: undoing back to the
  saved state still counts as changed, which is safe); project = its JSON at the last save / open (small).
  `markProjectSaved()` at startup: an empty project is not "unsaved" (bug found: the command-line scene waited
  behind the prompt).
- **Unsaved prompt** (`requestDiscard(action)`): close button (`glfwSetWindowCloseCallback` → cancel close),
  File > Exit, opening a scene / project, demo scene. LAS layers do not replace work, so no prompt.
- **Autosave** every `settings.autosaveSeconds` (120) when something changed since the last autosave:
  `recovery/session.json`, `recovery/project.gsvproj` (absolute paths), `recovery/scene.ply` written on a worker
  thread from a copy (`.part` then rename). A clean exit deletes the folder, so finding it at startup means a
  crash. No prompt when there is nothing worth recovering (0 edits, empty project). Recovery loads a copy of the
  autosave, then restores the original file's name, COLMAP cameras and Flip Y, and stays "unsaved".
- **Cancelling**: a `std::future` from `std::async` blocks in its destructor, so cancelled loads / auto-cleans
  are moved to `abandoned*_` and dropped when they finish.
- **Gizmo preview on the GPU**: preprocess applies `uPreviewAffine` (scene space) to splats with selection bit 0:
  μ' = Aμ, Σ' = AΣAᵀ, and evaluates the SH at dir' = Rᵀ·dir instead of rotating the coefficients (exact:
  c'(d) = c(Rᵀd)). The data changes once, on release (one undo step). Points view keeps the CPU path.
- **Auto quality**: `renderMainView` renders into `lowRes_` at `renderScale_` and blits with linear filtering while
  the view changed in the last 0.25 s. Control: frame time > 24 ms → ×0.85 (floor 0.35), < 19 ms → ×1.08 (the
  vsync wait makes ~16.7 ms "fast enough").
- **Visible only**: one expected-depth image (half resolution) per Rect / Lasso release or Brush stroke;
  `keepVisible` compares each centre's view depth z with the FARTHEST depth in the 3×3 pixels around it,
  hidden if z > zs·1.03; pixels with coverage < 0.5 count as see-through.
- **Undo on disk**: steps whose data ≥ 32 MB write before/after records to `history/<pid>/step_N.bin`; memory
  keeps the indices (4 B/splat). Undo / redo read the file; a missing file makes undo refuse (scene unchanged).
- **Export** renders with `frame()` into an offscreen target without UI and with the selection highlight off;
  the video renders one frame per loop iteration (progress, Cancel), encodes with
  `ffmpeg -c:v libx264 -preset veryfast -crf 20 -pix_fmt yuv420p` when ffmpeg is on the PATH.

## Tests (23/11/2026, container: 120 CPU + 22 GPU, ASan + UBSan clean)
| ER | Test | Result |
|---|---|---|
| 6.7 | `ux_settings_round_trip_and_defaults` | round trip; damaged / hostile values → defaults per field; 1000-deep JSON rejected |
| 6.7 | `ux_settings_file_and_recent_list` | first start = defaults; 12 recent, dedupe, move to front; atomic (no .tmp left); truncated file → defaults |
| 6.7 | `ux_i18n_formats_match`, `ux_i18n_glyphs_in_font_ranges`, `ux_i18n_lookup_and_ids` | 406 strings: specifiers identical, all glyphs in the font ranges, `###` IDs stable and cached |
| 6.13 | `ux_keep_visible` | front plane 210/210 kept, 152/152 hidden splats removed, 210/210 unhidden far splats kept |
| 6.14 | `gpu_transform_preview_fp32` / `_compressed` | vs CPU transform: centre 0.0002 px, conic 3e-6, colour 0.001 (compressed: 0.026 px, codec error) |
| 6.15 | `ux_grow_shrink_radius` | exact layer counts; radius limit 11 of 40 |
| 6.16 | `ux_history_spill_to_disk` | 40 KB instead of 2.4 MB in memory; undo / redo bit-exact; missing file → undo refused |

Interactive (Xvfb + llvmpipe + XTest, phong scene): welcome screen → Vietnamese → demo → tour; recent
thumbnail → open; edit → autosave → `kill -9` → restart → Recover (212 863 splats, original name, flipped) → Exit →
prompt → Don't save → recovery deleted; History jump; compass top view; gizmo drag (blurred low-res preview while
dragging, sharp after) → Ctrl+Z; Rect visible-only 55 167 vs 56 748; report with 3 pictures; video 90 frames →
H.264 1920×1080.

## Pitfalls found
- **Lost key taps at low FPS**: fixed in Phase 5 with sticky keys. At 1–2 FPS (software GL) a quick click right
  after a mouse move sometimes did not press an ImGui button; logging showed ImGui received press + release
  (trickled over two frames, as in a standalone repro that works). Not reproducible at normal frame rates;
  auto quality keeps interaction fast. Noted, not fixed.
- **Project "dirty" at startup** (empty JSON baseline) blocked the command-line scene behind the unsaved prompt.
- **Recovery ordering**: the Flip Y / COLMAP lookups ran with the recovery file's path; the original name must be
  restored before them.
- **"View" means two things**: menu "Hiển thị" vs a saved view "Góc nhìn": separate keys ("Saved view").
- **windows.h after GLFW** in one file redefines `APIENTRY`: keep Win32 calls in files without GL headers
  (`core/settings.cpp`).
- `\n` inside translations must stay an escape in the `.inc` (a real newline breaks the C string).
- The tour's tooltip-anchored steps need the panel's right edge: the toolbar is centred in the free area right of
  the panel (`panelRight_`), otherwise the panel covered it.

## Known limits / next
- 6.11 touch input and 6.12 installer: not started (device / signing decisions).
- Not measured on the user's machine yet: FPS while dragging / orbiting 2.5 M splats, export times.
- First LOD build and the visible-only depth pass still run on the UI thread (one frame / a few hundred ms).
- Flood fill: no normal-based split yet.
