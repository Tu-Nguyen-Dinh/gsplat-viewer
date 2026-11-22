# Core 2 — Level of detail for splat scenes

## Purpose
Draw scenes larger than the GPU budget: far regions are replaced by merged proxy splats, near regions
keep the originals, the total never exceeds a splat budget.

## Files and API (`scene/lod.h`)
- `LodTree { GaussianScene splats; vector<LodNode> nodes; leafSplatCount; depth }` — `splats` holds the
  original splats (Morton order, indices [0, leafSplatCount)) followed by all inner-node proxies.
- `LodNode { center, radius (sphere enclosing 3σ extents), firstChild, childCount (0 = leaf), splatBegin,
  splatCount, level }` — children of a node are consecutive; nodes[0] is the root.
- `buildLod(scene, LodBuildOptions{leafCapacity = 4096, nodeCapacity = 4096})`.
- `selectCut(tree, LodCutParams{cameraPosition (scene coords), focalPixels, pixelError, splatBudget})`
  → `LodCut{nodes, splatCount, budgetLimited}`; `gatherCut(tree, cut)` → GaussianScene to upload.
- `mergeSplats(scene, indices, out)`, `splatCovariance(scene, i)` (exposed for tests).
- `saveLod/loadLod(path)` — `.gsvlod` v1: magic "GSVLOD1\0", header (shDegree, restPerChannel, depth,
  leafCount), bounds/focus, then length-prefixed arrays (nodes, position, logScale, rotation,
  opacityLogit, shDC, shRest). Every count is bounded by the file size; ranges are validated.

## Algorithm
1. Morton codes (21 bits/axis) of the original positions; stable sort.
2. Recursive split of a code range by the next 3 bits while > leafCapacity; a range in one octant
   descends without adding a level. Child slots are allocated as one block before recursing.
3. Inner node proxies: the children's splats concatenated; if more than nodeCapacity, consecutive
   (Morton-compact) groups of ⌈n/capacity⌉ are merged by moment matching:
   wᵢ = αᵢ·det(Σᵢ)^⅓, μ = Σwμᵢ/W, Σ = Σw(Σᵢ + (μᵢ−μ)(μᵢ−μ)ᵀ)/W, SH = weighted mean,
   α = clamp(Σαᵢ·det(Σᵢ)^⅓ / det(Σ)^⅓, 1/255, 0.99). Σ → scale/rotation by eigen-decomposition.
4. Cut: max-heap on projected radius r·f/(d − r); refine the largest until all ≤ pixelError or the
   next refinement would exceed the budget (greedy).

## Viewer integration (`app/layers_ui.cpp`)
`--lod N` or the Layers panel: tree built synchronously at enable time; when the eye moves > 5 % of
its distance to the focus, `selectCut + gatherCut` run on `std::async`, the result is uploaded on the
GL thread. Edits and new scenes disable LOD and drop the tree.

## Tests (measured 22/11/2026)
| Test | Result |
|---|---|
| `lod_merge_preserves_moments` | mean error 4.7e-8, covariance rel. error 4.3e-7 |
| `lod_tree_structure` (100 k) | 175 nodes, 138 leaves, depth 5, 153 401 splats with proxies, 109 ms; each splat in one leaf; nested spheres |
| `lod_cut_budget_and_distance` (60 k, budget 40 k, 8 px) | 916 → 30 084 → 39 703 splats from 200 to 12 radii; budget respected; unlimited budget = original scene |
| `lod_file_round_trip_and_corruption` | bit-exact round trip; truncation/corruption rejected, no crash (ASan) |
| `gpu_lod_cut_image_close_to_full` (120 k → 39 k) | **43.1 dB** vs full; dropping 2 of 3 splats: 37.4 dB |

## Limits / next steps
- Not out-of-core yet: the whole tree is in RAM and every cut re-uploads all chosen splats (a hitch
  of a few hundred ms at millions of splats). Next: per-node GPU chunks with an LRU cache and partial
  updates; streaming `.gsvlod` by node.
- Proxy opacity is a heuristic; quality vs. a trained hierarchy (Kerbl et al. 2024) is not measured.
- Thresholds of ⏳ ER 4.13 (FPS of a 3 M scene on the RTX 3060) need the user's machine.
