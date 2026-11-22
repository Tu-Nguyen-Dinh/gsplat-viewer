# How gsplat-viewer renders a Gaussian

This note follows one Gaussian from the file to the pixels. Code references point to the C++
implementation (`src/core/gaussian_math.cpp`); the GLSL in `shaders/include/` is a line-by-line copy.

## 1. Parameters and activations

Each Gaussian stores a mean **μ**, log-scales **s**, a quaternion **q** = (w, x, y, z), an opacity
logit *o* and spherical-harmonics coefficients. They are optimized unconstrained during training and
activated when used:

| Quantity | Stored | Used |
|---|---|---|
| scale | **s** | exp(**s**) > 0 |
| opacity | *o* | σ(*o*) = 1 / (1 + e^−o) ∈ (0, 1) |
| rotation | **q** | R(**q** / ‖**q**‖) |

These depend only on the scene, so they are computed once at upload (in parallel), not per frame.

## 2. 3D covariance

Transforming the unit Gaussian by M = R S gives covariance

  **Σ = M Mᵀ = R S Sᵀ Rᵀ**,  S = diag(exp **s**)

which is positive semi-definite for any R, S. Invariants used in tests: trace Σ = Σ sᵢ²,
det Σ = (s₁ s₂ s₃)².

## 3. Projection (EWA splatting)

1. Camera space: **t** = V **μ** (affine), Σ_view = W Σ Wᵀ with W the rotation part of V.
2. The pinhole projection u = f_x t_x / z, v = f_y t_y / z with z = −t_z is not affine;
   linearize it at **t** with its Jacobian

   J = | f_x/z   0      f_x t_x / z² |
       | 0       f_y/z  f_y t_y / z² |

   (t_x/z and t_y/z are clamped to 1.3 × tan(fov/2) so splats far outside the view stay sane).
3. Screen-space covariance: **Σ′ = J W Σ Wᵀ Jᵀ + 0.3 I** (pixels²). The 0.3 is a low-pass filter
   that keeps sub-pixel splats from aliasing and guarantees det Σ′ > 0.

f = (H/2) / tan(fovY/2) is the focal length in pixels.

## 4. From Σ′ to an ellipse

For Σ′ = [[a, b], [b, c]]:

- eigenvalues λ₁,₂ = m ± √(m² − det), m = (a + c)/2
- major axis **v₁** = normalize(b, λ₁ − a), minor axis **v₂** ⊥ **v₁**
- the 3σ ellipse has half-axes 3√λ₁ **v₁** and 3√λ₂ **v₂**; the drawn quad is aligned with them
  (2.2× fewer fragments than an axis-aligned square of radius 3√λ₁)
- the fragment shader uses the **conic** Σ′⁻¹ = [[c, −b], [−b, a]] / det:

  α(**d**) = min(0.99, σ(o) · exp(−½ **d**ᵀ Σ′⁻¹ **d**)),  discarded if α < 1/255

## 5. View-dependent color

color(**d**) = max(0, 0.5 + Σ_{l≤3} Σ_m k_lm Y_lm(**d**)), with **d** the unit direction from the
camera to the Gaussian in the scene's own coordinate frame (the camera is transformed by the inverse
model matrix). Degree 3 means 16 coefficients per channel; bands 1–3 are stored as float16.

## 6. Compositing

Sorted back to front, each splat is blended with premultiplied "over":
C ← α c + (1 − α) C (`glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`), which equals the front-to-back sum

  C = Σᵢ cᵢ αᵢ Πⱼ<ᵢ (1 − αⱼ).

Sorting is per splat (by the depth of its center), the same approximation as the reference
implementation.

## 7. Sorting

Keys: for positive floats the IEEE-754 bit pattern is monotonic, so key = 0xFFFFFFFF − bits(depth)
sorts far to near in ascending integer order (optionally 16-bit: depth quantized in the bounding-sphere
range).

GPU LSD radix sort, per 4-bit digit:
1. per-block (1024 keys) histogram, stored digit-major: `hist[digit · blocks + block]`;
2. exclusive scan of that array (two-level Blelloch) → global start of every (digit, block);
3. scatter: destination = block offset + count in earlier rounds + rank among earlier threads of the
   round (Hillis–Steele scan of one-hot flags, two 16-bit counters packed per `uint`).

Every term preserves input order, so each pass is stable — the requirement for LSD radix sort.

## Per-pixel depth of a splat (Core 1)

In camera space a splat is a Gaussian N(t, S). The linearized projection maps an offset Δt to a
screen offset Δu = J Δt, with J the 2×3 Jacobian of the EWA projection, and to a depth change
Δz = −e_zᵀ Δt. (u, z) are then jointly Gaussian, and the expected depth along the ray through the
pixel at offset Δu from the centre is the conditional mean

    E[z | u] = z₀ + g·Δu,   g = Σ_uu⁻¹ Cov(z, u),   Σ_uu = J S Jᵀ,   Cov(z, u) = −e_zᵀ S Jᵀ.

Σ_uu must be the covariance **without** the 0.3 px² anti-aliasing dilation (the dilation blurs the
footprint, not the surface). The fragment shader blends (z·α, α) like the color, so the second render
target holds Σ wᵢ zᵢ and 1 − T: the expected depth of the composite is their ratio.

## Merging Gaussians (Core 2, LOD)

Weights wᵢ = αᵢ det(Σᵢ)^⅓. The merged Gaussian keeps the first two moments of the weighted mixture:

    μ = Σ wᵢ μᵢ / W,      Σ = Σ wᵢ (Σᵢ + (μᵢ − μ)(μᵢ − μ)ᵀ) / W.

Scale and rotation come back from the eigen-decomposition Σ = R diag(s²) Rᵀ.
