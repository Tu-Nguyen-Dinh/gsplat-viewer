# Core 1 — Scene foundation: real coordinates, layers, depth

## Purpose
Put data from different sources (splats from COLMAP, LiDAR in survey coordinates, later meshes) in
one world, render them together with correct occlusion, and expose per-pixel depth for picking.

## Files and API
### `core/geo_frame.h` (no OpenGL)
- `struct GeoFrame { double scale; dmat3 rotation; dvec3 translation; }` — p' = s·R·p + t.
  `apply`, `applyVector`, `operator*` (a*b = a∘b), `inverse`, `matrix()` (dmat4),
  `localMatrix(origin)` → float mat4 mapping layer-local → (world − origin), subtraction in double,
  `orthonormalize()`, factories `identity`, `fromTranslation`, `fromQuaternion(q, t, s)`.
- `toLocal(world, origin)`, `fromLocal(local, origin)`, `rotationAngleBetween(Ra, Rb)`.
### `scene/point_cloud.h`
- `PointCloud { dvec3 origin; vector<vec3> position (offsets); vector<u8vec3> color; vector<uint16_t>
  intensity; vector<uint8_t> classification; vector<vec3> normal; localMin/localMax; }`
  — `world(i)`, `computeBounds()`, `rebase(newOrigin)` (exact), `hasColor()`. Empty vector = absent.
### `scene/layers.h`
- `Layer { id, name, LayerKind kind (Splats|Points), GeoFrame frame (layer → world), visible,
  shared_ptr<GaussianScene> splats, shared_ptr<PointCloud> points }`.
- `LayerStack`: `addSplats`, `addPoints` (return unique id, never reused), `remove`, `find`,
  `worldBounds(visibleOnly)` (splats: robust bounds; points: full extent; 8 corners through the frame),
  `suggestRenderOrigin()` (centre of all bounds rounded to metres).
- The viewer does not use LayerStack yet (it has one splat scene + one cloud); applications should.
### `render/cloud_renderer.h`
- `CloudRenderer::init/upload(PointCloud)/draw(view, layerToWorld, renderOrigin, CloudSettings)`.
  Vertex = 20 B (vec3 + rgba8 + u16 intensity + u16 class). Depth test LESS + depth write, no blend.
  `CloudSettings { pointSize px | worldSize m, roundPoints, colorMode Rgb|Intensity|Classification|Height, heightAxis }`.
  RGB falls back to Height when the cloud has no color. Also writes the depth output (z, coverage 1).
### `gl/framebuffer.h`
- `Framebuffer::create(w, h, Format, FramebufferAttachments{depthOutput, depthBuffer})`.
  depthOutput = COLOR_ATTACHMENT1 RG32F (RG16F on ES); depthBuffer = DEPTH_COMPONENT32F.
  `clear(background)` clears color, depth output (0) and depth (1). `readDepthOutput(fb)` → `DepthImage`
  (in `core/image.h`: depth, coverage, rows top-down).
### Splat renderer additions (`SplatSettings`)
- `depthTest`: splats test (LEQUAL, no write) against the bound depth buffer. The vertex shader now
  writes a real NDC z from the splat centre depth (`uProjZ` = P[2][2], P[3][2]), clamped to [-1, 1].
- `depthOutput`: per-pixel depth. Preprocess writes `depthGrad[i]` (binding 8, vec2/splat, allocated
  on first use); the fragment shader outputs `z = depth + g·offset` clamped to [0.5, 2]× centre depth.
  Off → the depth output still receives each splat's centre depth (cheaper, less exact).

## Maths
- Expected depth: blending (z·α, α) with the same "over" operator gives Σwᵢzᵢ and 1−T; depth = r/g.
- Per-pixel depth: for a 3D Gaussian, depth along a pixel ray is (linearized) Gaussian with mean
  z + g·Δu, g = Σuu⁻¹·Cov(z,u), Cov(z,u) = −e_zᵀ S Jᵀ (S = camera-space covariance, J = projection
  Jacobian). **Σuu must be the undilated covariance**: using the dilated conic (anti-aliasing
  +0.3 px²) biased picks by 0.55 % on a floor; undilated → 0.059 %.
- Precision: float spacing at 1.2·10⁶ m is 0.125 m; offsets around a nearby origin keep < 1 mm.

**Do not measure with the expected depth.** It is the mean depth of everything in the pixel, wrong at edges
and behind floaters (up to 34 cm at a table edge, see CORE3). Picking for measurement uses the 3D ray median
(`core/ray_pick.h`); the depth output stays for opaque layers, depth-of-field, fog, and as the input of
future median-depth rasterization.

## Invariants
- Draw opaque layers first (they fill the depth buffer), then splats with `depthTest`.
- Splats never write depth. Clouds always write it.
- `localMatrix(origin)` must be built from the double frame every frame; never accumulate float matrices.

## Tests (measured 22/11/2026, llvmpipe)
| Test | Result |
|---|---|
| `geo_frame_compose_and_inverse` | compose/inverse ≤ 1e-9 relative |
| `geo_frame_local_float_keeps_millimetres` | float absolute 0.0313 m vs relative-to-origin 9.9e-6 m |
| `point_cloud_rebase_is_exact`, `layer_stack_bounds_and_origin` | pass |
| `gpu_expected_depth_matches_reference` | 13 394 px: median rel. error 2.4e-6, p99 2.9e-5 |
| `gpu_opaque_points_occlude_splats` | wall in front 100 % red; behind 36 %; no depth test 36 % |
| `gpu_pick_on_splat_plane` | 47 picks, worst 0.059 % of distance (ER < 0.5 %) |

## Pitfalls
- GCC rejects a nested struct with default member initializers as a default argument inside its own
  class ("could not convert {}"): `FramebufferAttachments` is declared outside `Framebuffer`.
- **Never keep the depth output attached to the window's scene target.** Blending a second RG32F
  target at full screen doubles the cost of every fragment: phong, 2560×1440, RTX 3060: draw
  **36.6 ms** with it vs **5.8 ms** without (measured 22/11/2026; llvmpipe cannot show this). The
  viewer attaches only a depth buffer while a cloud is shown, and `pickMeasurePoint` renders one
  frame into its own target with the depth output when the user clicks.
- ES: vertex shaders use 3 SSBOs (projected, order, depthGrad); ES 3.1 allows 0 vertex SSBOs in
  theory, Adreno exposes 24. Checked on Mesa GLES; still ⏳ on the S23 Ultra.
