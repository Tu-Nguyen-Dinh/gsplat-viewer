#pragma once
// Core 5: interactive editing operations on a Gaussian scene. See docs/core/CORE5-editing.md.
//
// Selections are per-splat masks (SplatMask from scene_edit.h, 1 = selected) combined with
// SelectOp. Screen-space tools test the projected centre of each splat; 3D tools work in scene or
// display coordinates (the viewer's model matrix maps scene -> display). Modifications (transform,
// colour) change the selected splats in place; wrap them in EditHistory (edit_history.h) for undo.
// No OpenGL here.

#include "scene/gaussian_scene.h"
#include "scene/scene_edit.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace gsv {

// ------------------------------------------------------------------ selection
enum class SelectOp : int { Replace = 0, Add = 1, Subtract = 2, Intersect = 3 };
void combineSelection(SplatMask& mask, const SplatMask& next, SelectOp op);
SplatMask selectAll(const GaussianScene& scene);
glm::vec3 selectionCentroid(const GaussianScene& scene, const SplatMask& mask);  // scene coordinates

// Screen-space selection: `mvp` = projection * view * model; pixels with the origin at the TOP-left
// (mouse coordinates), `viewport` in pixels. Splats behind the camera are never selected.
SplatMask selectScreenRect(const GaussianScene& scene, const glm::mat4& mvp, glm::vec2 viewport, glm::vec2 a,
                           glm::vec2 b);
SplatMask selectScreenPolygon(const GaussianScene& scene, const glm::mat4& mvp, glm::vec2 viewport,
                              const std::vector<glm::vec2>& polygon);
SplatMask selectScreenCircle(const GaussianScene& scene, const glm::mat4& mvp, glm::vec2 viewport, glm::vec2 center,
                             float radiusPixels);
bool pointInPolygon(glm::vec2 p, const std::vector<glm::vec2>& polygon);  // even-odd rule

// 3D: splats whose centre lies within `radius` of `center` (display coordinates; model: scene -> display).
SplatMask selectSphere(const GaussianScene& scene, const glm::mat4& model, const glm::vec3& center, float radius);

// Index of the splat whose centre is nearest to `point` (scene coordinates), ignoring nearly
// transparent splats. Returns scene.size() for an empty scene.
uint32_t nearestSplat(const GaussianScene& scene, const glm::vec3& point, float minOpacity = 0.1f);

// Region growing from `seed` ("click to select an object"): neighbours within `maxGap` (scene
// units) whose base colour is within `colorTolerance` (0..1, Euclidean RGB) of the REGION's mean
// colour so far. Single splat colours are noisy in 3DGS (overlapping splats share the look), so
// comparing with the neighbour that reached a splat broke regions into fragments (9 splats on a
// printer in a real scene). Splats below minOpacity join when they pass the colour test but do not
// spread the region further. Stops after maxCount splats. `tree` (built on scene.position) may be
// passed to avoid rebuilding it on every click.
SplatMask selectConnected(const GaussianScene& scene, uint32_t seed, float maxGap, float colorTolerance,
                          float minOpacity = 0.1f, size_t maxCount = 5000000, const class KdTree* tree = nullptr);
// Same, limited to splats within `maxRadius` of the seed (scene units; 0 = no limit). The limit
// stops a region from leaking through a touching surface into the whole room.
SplatMask selectConnectedWithin(const GaussianScene& scene, uint32_t seed, float maxGap, float colorTolerance,
                                float maxRadius, const class KdTree* tree = nullptr);

// One layer more / less: grow adds every splat within `gap` of a selected one (and within
// colorTolerance of the selection's mean colour, 1.0 = any colour); shrink removes the selected
// splats that have an unselected opaque neighbour within `gap` (the boundary layer).
void growSelection(const GaussianScene& scene, const class KdTree& tree, SplatMask& mask, float gap, float colorTolerance = 1.0f);
void shrinkSelection(const GaussianScene& scene, const class KdTree& tree, SplatMask& mask, float gap);

// "Visible only" for screen selections: keeps the splats of `mask` whose centre is not hidden behind
// the rendered surface. `depth` is the expected-depth image of the view (core/image.h, top row
// first, any resolution). A centre at view depth z is hidden when z > zs * (1 + relTolerance), zs =
// the FARTHEST surface depth in the 3x3 pixels around it (lenient at silhouettes). Pixels with
// coverage < 0.5 count as empty (everything visible). Centres outside the image are removed.
void keepVisible(const GaussianScene& scene, const glm::mat4& viewModel, const glm::mat4& projection,
                 const struct DepthImage& depth, SplatMask& mask, float relTolerance = 0.03f);

// Typical spacing of the scene: median distance from a splat to its nearest neighbour, over up to
// `samples` opaque splats (deterministic). Used to scale gap / radius defaults to the capture.
float typicalSpacing(const GaussianScene& scene, const class KdTree& tree, int samples = 4000);

// Statistical outliers: mean distance to the k nearest neighbours above mean + stdRatio * sigma of
// that statistic over the scene (classic SOR filter). Typical floaters are isolated.
SplatMask selectOutliers(const GaussianScene& scene, int k = 16, float stdRatio = 2.5f);

// Splats seen by fewer than `minViews` of the given cameras: centre in front, inside the image and
// closer than maxDistance. Floaters in empty space are usually seen by very few training views.
struct CameraView {
  glm::mat4 view{1.0f};          // scene -> camera (camera looks down -Z)
  glm::vec2 focal{1.0f};         // pixels
  glm::vec2 principal{0.0f};     // pixels, image convention (origin top-left, y down)
  glm::vec2 size{1.0f};          // image size in pixels
  float maxDistance = 1e30f;
};
SplatMask selectUnseen(const GaussianScene& scene, const std::vector<CameraView>& cameras, int minViews);

// ------------------------------------------------------------------ modifications
// Applies `affine` (scene coordinates; rotation + uniform scale + translation, no shear) to the
// selected splats: positions, orientations, scales, and the view-dependent colour (SH bands 1..3
// are rotated with R, so highlights turn with the object).
void transformSplats(GaussianScene& scene, const SplatMask& mask, const glm::mat4& affine);

// Rotation of real SH coefficients: band l (l = 1..3) coefficients c' = M_l * c, with M_l of size
// (2l+1)^2, row-major, for the coefficient order used by evalSphericalHarmonics. Computed by least
// squares on 64 directions (exact: every band is closed under rotation).
struct ShRotation {
  std::array<double, 9> band1{};
  std::array<double, 25> band2{};
  std::array<double, 49> band3{};
};
ShRotation shRotation(const glm::mat3& rotation);
// Rotates the rest coefficients of one splat in place (layout: channel-major, restPerChannel each).
void rotateShRest(float* rest, int restPerChannel, const ShRotation& r);

struct ColorAdjust {
  float brightness = 0.0f;   // added to the base colour (0 = none)
  float contrast = 1.0f;     // around mid-grey 0.5
  float saturation = 1.0f;   // 0 = grey (Rec. 709 luma), 1 = unchanged
  float temperature = 0.0f;  // > 0 warmer (red up, blue down)
  bool isNeutral() const { return brightness == 0.0f && contrast == 1.0f && saturation == 1.0f && temperature == 0.0f; }
};
void adjustColor(GaussianScene& scene, const SplatMask& mask, const ColorAdjust& adjust);

// Scale factor that makes the segment a-b (scene units) `realLength` long.
double scaleForKnownLength(const glm::dvec3& a, const glm::dvec3& b, double realLength);
// Rotation (as a 4x4 matrix) taking unit vector `from` onto `to` (shortest arc).
glm::mat4 rotationBetween(const glm::vec3& from, const glm::vec3& to);

// ------------------------------------------------------------------ measurements
double polylineLength(const std::vector<glm::dvec3>& points);
// Area of a planar (or nearly planar) polygon in 3D: half the length of the Newell normal.
double polygonArea(const std::vector<glm::dvec3>& points);

}  // namespace gsv
