#pragma once

#include "scene/gaussian_scene.h"

#include <cstdint>

// Procedural test scene (a sphere shell, a torus and a checkered floor) built from
// oriented, flattened Gaussians. Lets the viewer run without downloading any data
// and gives a known-good input for testing the renderer.
GaussianScene makeDemoScene(size_t count = 200000, uint32_t seed = 1234);
