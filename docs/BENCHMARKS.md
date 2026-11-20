# Benchmarks

Machine: NVIDIA GeForce RTX 3060 (driver 610.88), Intel Core i5-12400F (6 cores / 12 threads), Windows 11.
Release build (MinGW GCC 15.2, `-O3`). Offscreen render target **1920×1080**, VSync off.
Each configuration: 30 warm-up frames, then 240 frames orbiting 360° around the scene
(the sort order changes every frame). Times are averages of `GL_TIME_ELAPSED` queries.

Reproduce: `build/gsplat_viewer.exe scene.ply --bench --size 1920x1080 --frames 240 --csv docs/benchmarks.csv`
(raw data: [benchmarks.csv](benchmarks.csv)).

## Final configuration (GPU radix sort, SH stored as fp16, oriented quads)

| Splats | Preprocess | Sort | Draw | GPU total | Frame | FPS |
|---:|---:|---:|---:|---:|---:|---:|
| 300 K | 0.35 ms | 0.89 ms | 1.42 ms | 2.66 ms | 2.75 ms | 364 |
| 1 M | 1.00 ms | 2.47 ms | 4.83 ms | 8.29 ms | 8.39 ms | 119 |
| 3 M | 2.83 ms | 7.06 ms | 14.13 ms | 24.02 ms | 24.10 ms | 41 |
| 1 M, 16-bit depth keys | 1.01 ms | 1.25 ms | 4.86 ms | 7.12 ms | 7.21 ms | 139 |
| 3 M, 16-bit depth keys | 2.78 ms | 3.57 ms | 14.35 ms | 20.71 ms | 20.73 ms | 48 |

## Effect of each optimization (1 M splats)

| Optimization | Before | After | Gain |
|---|---|---|---|
| Sort on the GPU instead of CPU radix (incl. depth keys) | 13.79 ms (CPU) | 2.47 ms (GPU) | 5.6× |
| CPU radix instead of `std::sort` (1 M random keys) | 60.6 ms | 11.6 ms | 5.3× |
| SH bands 1–3 stored as fp16 instead of fp32 (preprocess) | 4.09 ms | 1.00 ms | 4.1× |
| 16-bit instead of 32-bit depth keys (4 radix passes instead of 8) | 2.47 ms | 1.25 ms | 2.0× |
| Oriented quads instead of squares: rasterized fragments | 292.9 M | 132.3 M | 2.2× fewer |
| Oriented quads instead of squares: draw time | 5.69 ms | 4.83 ms | 1.18× |
| Parallel scene precompute (1 M covariances, 12 threads) | 160.5 ms | 25.6 ms | 6.3× |

## Correctness of the optimized paths

| Check | Result |
|---|---|
| GPU vs CPU reference renderer (RGBA16F target) | 54.4 dB PSNR |
| GPU vs CPU reference renderer (RGBA8, like the window) | 51.6 dB |
| GPU radix sort with 16-bit keys vs reference | 53.9 dB (32-bit keys: 53.9 dB) |
| SH fp16 vs fp32 color error per splat | ≤ 2.1·10⁻⁴ (image PSNR unchanged: 54.4 dB) |
| vs PlayCanvas splat-transform (independent WebGPU renderer) | **40.6 dB** — `tools/compare_with_splat_transform.ps1` |

## Scene loading (1 M splats, 248-byte PLY records)

| Step | Thread | Time |
|---|---|---|
| Parse + validate PLY | worker | 365 ms |
| Precompute + GPU upload (splat renderer) | main | 131 ms |

## Blending precision vs speed (v1.1, 20/11/2026)

Splats are blended back to front into a render target. An 8-bit target rounds after every
blend, so faint splats (α·|c − C| below half a step) vanish. Real captured scenes have hundreds of
such layers per pixel. `--blend` / the "Blending" combo choose the target; the window then gets a single blit.

| Target | Real scene: PSNR on 24 held-out photos (Brush: 30.54 dB) | Synthetic vs CPU reference | truck 1080p draw | truck FPS (32-bit keys) | truck FPS (16-bit keys) |
|---|---|---|---|---|---|
| RGBA8 (v1) | 29.91 dB (−0.62) | 51.6 dB | 4.64 ms | 85 | 111 |
| RGB10A2 | 30.23 dB (−0.31) | 53.5 dB | 6.71 ms | 73 | 92 |
| RGBA16F (default) | **30.26 dB (−0.28)** | 54.4 dB | 6.74 ms | 73 | 92 |

RGB10A2 has the same 32 bits per pixel as RGBA8 but blends just as slowly as RGBA16F: the cost is
not memory bandwidth but the blend rate (the RTX 3060 blends RGBA8 at full rate, other formats at
about half). Real scene: `scenes/phong` (phone video, 232 553 splats, `gsplat_viewer phong.ply --eval --blend X`).

## Camera framing on load (v1.1)

`computeFocus` (scene_bounds.cpp) frames the densest region instead of the 2–98 % per-axis bounds:
truck opens on the truck (≈ 40 % of the frame) without `--camera-pos`; cost on 2.5 M splats: 33 ms
(200k-sample histogram; a full sort took 370 ms).

## Task 2.6: sort keys, quad size, benchmark noise (v1.1, 21/11/2026)

truck.splat, 1920×1080, orbit, frame = **median** of 240 frame intervals (was the mean):

| Config | sort | draw | FPS, RGBA16F blend (default) | FPS, RGBA8 blend | Real-scene PSNR (phong, 24 test views) |
|---|---|---|---|---|---|
| 32-bit float keys (8 passes) | 6.0–6.3 ms | 6.5 / 4.5 ms | 69–74 | 84–87 | 30.26 dB |
| **24-bit float keys (6 passes, default)** | 4.5–4.8 ms | 6.5 / 4.7 ms | **74–78** | **96** | **30.26 dB** |
| 16-bit linear keys (4 passes) | 3.0 ms | 6.9 / 4.8 ms | 88–93 | 109 | 29.80 dB (−0.46) |

- **24-bit keys** = the top 24 bits of the float depth: no depth range, no clamping, relative
  precision 2⁻¹⁵. Same PSNR as 32-bit on the real scene; image 70 dB from the 32-bit one on the
  synthetic test. 16-bit linear keys lose 0.46 dB on a captured room, so they stay optional.
- **Opacity-aware quads**: a splat is drawn out to σ·√(2 ln(255·α)) (≤ 3σ) instead of 3σ, where its
  alpha falls below the 1/255 discard threshold. Rasterized fragments 245.4 M → 179.8 M (−27 %),
  blended fragments unchanged (151.6 M → 151.5 M), test PSNR vs reference unchanged (54.39 dB).
- **"16-bit keys slower than 32-bit" (v1.0 baseline)** did not reproduce in 10+ runs: 16-bit is
  always fastest. The v1.0 frame time was total / frames, so one hitch outside the renderer
  (seen once: 21 ms frame for 13 ms of GPU work) inflated a whole configuration. The benchmark now
  reports the median interval; repeated runs agree within ±1 FPS.

## Task 2.4: compressed GPU format (v1.1, 21/11/2026)

Static data per splat 64 → **24 bytes** (SH bands 1–3 excluded): 16-bit positions inside the bounds
of 256-splat chunks (splats sorted along a Morton curve so chunks are compact), fp16 log-scale,
opacity and SH DC, smallest-three quaternion in 32 bits. The covariance is rebuilt in the
preprocess shader. The per-frame projected buffer went 64 → 32 bytes (center + conic fp32,
color + opacity fp16, ellipse axes rebuilt in the vertex shader).

truck.splat (2 541 226 splats, SH degree 0), 1920×1080:

| | fp32 (`--no-compress`) | compressed (default) |
|---|---|---|
| GPU buffers (v1.0: 339.3 MB, 133 B/splat) | 252.0 MB | **155.4 MB (64 B/splat)** |
| preprocess | 0.80–0.87 ms | **0.51–0.54 ms** |
| FPS, default config (24-bit keys, RGBA16F) | 81–82 | **84–86** |
| scene upload | 446–450 ms | 520–566 ms (Morton sort 60 ms + packing 180 ms) |

Image error of the compressed format vs fp32 (same renderer, 1920×1080): truck 56.2 dB, truck
inside 56.7 dB, captured room (SH degree 3) 57.3 dB, demo 1 M 59.9 dB. Held-out photos of the
room: 30.25 dB compressed vs 30.26 dB fp32.

## Task 2.5: .spz reader (v1.1, 21/11/2026)

Versions 2-3 (one gzip stream) with an in-house DEFLATE/gzip decoder (RFC 1951/1952, CRC-32 checked).
Files converted from PLY by PlayCanvas splat-transform 3.11.2 (`--spz-version 3`):

| Scene | .spz size | load (parse) | render vs the source file (same renderer, 1280×720) |
|---|---|---|---|
| truck (2.54 M, SH 0) | 43.1 MB (.splat: 81.3 MB) | 709 ms (.splat: 465 ms) | 36.3 dB |
| room "phong" (233 K, SH 3) | 6.0 MB (.ply: 54.9 MB) | 205 ms | 36.6 dB (37.4 dB at a second view) |

The loss is the format's own quantization (8-bit color and alpha, log-scale steps of 1/16, SH in
4-5 bits): splat-transform's own renderer gives 38.8 dB between its PLY and SPZ renders of the room.
Our decoder matches splat-transform's decoder value for value (positions, scales, SH identical,
quaternions within 1e-7); the only difference is deliberate: alpha 255 decodes to a finite logit
(splat-transform writes +inf, which our loader would drop as invalid).

Version 4 (zstd streams, splat-transform's default) with the vendored zstd 1.5.7 decoder: truck
42.4 MB, parse 497 ms (v3 with our inflate: 701 ms); decodes to the same values as v3 of the same
scene (test), render 65.5 dB from the v3 file.

## Regression found before v1.1.0: SH loads and L1 thrashing (21/11/2026)

The release benchmark showed the preprocess pass of SH-degree-3 scenes at 4.7 ms for 1 M splats
(v1.0: 0.96 ms), in both the compressed and the fp32 paths, while degree-0 scenes had become faster.
Bisected with worktree builds and runtime shader swaps (same camera as v1.0):

| Build / experiment | preprocess, 1 M splats, SH fp16 |
|---|---|
| v1.0.1 (`1d556ef`) | 0.96 ms |
| Phase 1 (`ab48eeb`) | 0.97 ms |
| v1.1 viewer work (`b1e9507`) | 1.18 ms |
| compressed format (`b780e28`) | 4.7 ms |
| ... v1.0 C++ with the new shaders | 4.7 ms (so: shader side) |
| ... new shader without the decoder branch / old gaussian.glsl / padded 64-byte records | 4.7 ms each |
| **SH stored coefficient-major (fix)** | **0.53 ms** (fp32 SH: 0.78 ms, was 3.96 ms in v1.0) |

The SH coefficients were stored per splat (23 words, 92 bytes apart). Every load of a warp touched
32 different cache lines and the pass only ran fast while L1 kept those lines for the next 22
loads. The new shader used fewer registers, so more warps ran at once and evicted each other's
lines. Storing word w of splat k at `[w * count + k]` makes each warp load 128 contiguous bytes:
no dependence on L1 at all, and faster than v1.0 in both precisions.
