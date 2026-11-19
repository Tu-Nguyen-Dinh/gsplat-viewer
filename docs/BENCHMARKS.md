# Benchmarks

Machine: NVIDIA GeForce RTX 3060 (driver 610.88), 12 CPU threads, Windows 11.
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
