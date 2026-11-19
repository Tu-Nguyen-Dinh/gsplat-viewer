#pragma once

#include "core/image.h"
#include "render/render_view.h"
#include "scene/gaussian_scene.h"

namespace gsv {

struct ReferenceOptions {
  int shDegree = 3;
  float splatScale = 1.0f;
  bool orientedQuads = true;   // must match the GPU setting: it decides which pixels a splat touches
  bool cullOutsideViewport = true;
  bool sorted = true;          // false = draw in file order (to show what sorting fixes)
  glm::vec3 background{0.0f};
};

// Slow but simple CPU renderer implementing exactly the same algorithm as the GPU path:
// project (gaussian_math), sort back-to-front, and for every pixel inside each splat's
// quad composite alpha * color over the image in float precision.
// It is the "ground truth" the GPU output is compared against in the tests.
Image renderReference(const GaussianScene& scene, const RenderView& view, const ReferenceOptions& options);

}  // namespace gsv
