# Benchmarks

Machine: NVIDIA GeForce RTX 3060, driver 610.88, Windows 11, Release build (MinGW GCC 15.2).
Resolution 1280×720, VSync off, camera framed on the scene (`F`).
GPU time = `GL_TIME_ELAPSED` around the scene render pass only (UI excluded).

| Date | Scene | Splats | Mode | Sort | GPU render | Sort time | FPS |
|---|---|---:|---|---|---:|---:|---:|
| Day 1 | demo | 300 000 | points | – | 0.13 ms | – | – |
|  |  |  |  |  |  |  |  |

## How to measure

1. `build/gsplat_viewer scene.ply --size 1280x720`, untick VSync, press `F`.
2. Wait a few seconds for the smoothed GPU time to settle, read the panel.
3. Use `splat-transform scene.ply -d 25% scene_25.ply` (and 50%, 75%) to get the same scene at several sizes.
