#pragma once

#include "scene/gaussian_scene.h"

#include <cstdint>

namespace gsv {

// Procedural test scene (a sphere shell, a torus and a checkered floor) built from
// oriented, flattened Gaussians. Lets the viewer run without downloading any data
// and gives a known-good input for testing the renderer.
// viewDependent: add spherical-harmonics coefficients (degree 3) so colors change with the view.
GaussianScene makeDemoScene(size_t count = 300000, uint32_t seed = 1234, bool viewDependent = true);

}  // namespace gsv
