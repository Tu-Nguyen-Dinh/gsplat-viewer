# Core 3 — Geometry: LiDAR files, alignment, picking

## Files and API
### `scene/las_io.h`
- `readLasHeader(path, LasInfo&, err)`, `readLas(path, PointCloud&, err, LasInfo* = nullptr, stride = 1)`,
  `writeLas(path, cloud, LasWriteOptions{versionMinor 2|4, pointFormat 0-3 | 6-7, scale}, err)`.
- Reads LAS 1.0–1.4, point formats 0–10, any record length (extra bytes skipped). Coordinates
  int·scale + offset rebuilt in double; origin = centre of the header bounds rounded to metres.
  Legacy formats: class = low 5 bits. RGB: 16-bit per spec, 8-bit data detected (max ≤ 255).
- Rejects with a message: missing file, no "LASF", LAZ (bit 7/6 of the format byte), version ≠ 1.x,
  unknown format, record shorter than the format, zero/NaN scale, data offset inside the header,
  announced points beyond the file size, short read.
### `core/kdtree.h`
- `KdTree::build(points)` (references the vector), `nearest`, `knn(q, k)` (sorted), `radius(q, r)`.
  Median split on the widest axis, 16 points per leaf, iterative traversal (stack of 128).
- `estimateNormals(points, tree, k, viewpoint)` — PCA smallest eigenvector, oriented to the viewpoint
  (NaN = no orientation). `symmetricEigen3(m, values↑, vectors)` — cyclic Jacobi.
### `core/registration.h`
- `estimateSimilarity(src, dst, withScale, GeoFrame&)` — Horn 1987 quaternion method (4×4 Jacobi),
  optimal scale Σ b'·Ra' / Σ|a'|²; fails for < 3 or collinear pairs.
- `estimateSimilarityRansac(src, dst, withScale, threshold, iterations = 500, seed)` → inliers, rms.
- `icpPointToPlane(source, target, targetNormals, targetTree, initial, IcpOptions)` — Gauss-Newton on
  (ω, t) linearized about the centroid of the moved source, 6×6 solve with pivoting; the gate shrinks to
  3 × median point-to-point pair distance; stops when |ω| and |t| < 1e-6.
- `fitPlaneRansac(points, threshold)`, `fitPlane(points, subset)`; `Plane{normal, d}`.
### `core/ray_pick.h` — **the pick to use for measuring**
- `raycastSplats(scene, model, origin, dir, RayPickOptions{coverageThreshold 0.5, maxSigma 3})` → `RayHit{point,
  distance, coverage}`: every Gaussian is intersected in 3D at its density maximum along the ray,
  t* = (uᵀΣ⁻¹d)/(dᵀΣ⁻¹d), α = opacity·exp(−½ m²) with m² the Mahalanobis distance there (Σ⁻¹ = R S⁻² Rᵀ, no
  inversion); contributions are composited front to back and the hit is the **median depth** (first point where
  accumulated opacity ≥ 50 %). Exact, independent of the view and of the screen resolution.
- `SplatRayCache` precomputes Σ⁻¹ (72 B/splat) for many rays; the scene overload computes on the fly (no memory).
  `rayProfile(...)` returns the sorted (distance, α) pairs. Cost: one parallel pass over the splats per ray.
- Viewer: `pickMeasurePoint` casts the click's ray (snapping to the nearest hit in a ±2 px ring on a miss) and
  compares with the point cloud's GPU depth (nearest wins).
### `core/picking.h` (depth-image picking: only for opaque layers)
- `pickPoint(DepthImage, x, y (from top), PickCamera{view, focal, principal}, minCoverage = 0.5, window = 1)`
  → world point (relative to the render origin); median of covered depths in a 3×3 window.
- `measure(a, b, up)` → distance, horizontal, signed vertical.

## Tests (measured 22/11/2026)
| Test | Result |
|---|---|
| `las_round_trip_12_format3/0`, `_14_format7` | max error 8.3e-4 m (scale 1 mm) / 4.2e-4 m (0.5 mm); attributes exact |
| `las_stride_and_header`, `las_malformed_files_fail_cleanly` | pass (50 random corruptions, no crash under ASan) |
| `kdtree_matches_brute_force` | 200 queries × kNN 8 + radius identical; build 1 M points 296 ms (container) |
| `normals_of_noisy_plane` (σ 2 mm, k 30) | median 0.59°, p99 1.65° (k 16: median 1.15°) |
| `similarity_closed_form_and_ransac` | exact: s/R/t < 1e-9; 30 % outliers: 70 inliers, scale 2.7e-5, 0.013° |
| `icp_point_to_plane_recovers_offset` (5°, 10 cm, σ 1 mm, two scans) | 5 iterations, 14 957 pairs, 0.0045°, 0.31 mm |
| `plane_ransac_with_outliers` (50 %) | normal 0.002°, 3 012 inliers |
| `ray_median_ignores_floaters_and_background` | median 2.000 m on the 2 m surface (expected depth 2.234 m); scaled model OK |
| `gpu_pick_table_edge_median_vs_expected` | see pitfalls: 0.21 cm worst vs 34.2 cm |

## Pitfalls found
- **Picking with the expected depth is wrong for measuring** (found by the user, 22/11/2026: the ends of a
  measured table slid off its edges when the view changed). The expected depth is the weighted mean of all
  surfaces in the pixel: at an edge it mixes the table with the floor, and faint floaters pull it toward the
  camera. The point still projects onto the clicked pixel, so it looks right until the view changes.
  Regression test `gpu_pick_table_edge_median_vs_expected` (419 edge pixels): expected depth median 5.8 cm,
  p95 25.4 cm, worst 34.2 cm off the surface; 3D ray median 0.07 / 0.15 / 0.21 cm. The earlier floor-only test
  (0.059 %) could not see it: a single plane has no edges and no floaters.
- ICP gate on the point-to-plane RMS rejected almost every pair (two scans never sample the same
  spots: aligned pairs are one point spacing apart) → 107 pairs, 1.2 mm error. Gate on point-to-point
  distances → 14 957 pairs, 0.31 mm.
- Point-to-plane ICP is degenerate on a single plane (6×6 system singular) → returns not converged.
  Needs ≥ 3 non-parallel planes (walls + floor) or a point-to-point fallback.
- LAZ is not decoded (needs LASzip, LGPL): convert first.
