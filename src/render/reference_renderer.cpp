#include "render/reference_renderer.h"

#include "core/gaussian_math.h"
#include "core/sorting.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gsv {

namespace {
struct Splat2D {
  uint32_t key;
  ProjectedGaussian g;
  glm::vec3 color;
  float opacity;
};
}  // namespace

Image renderReference(const GaussianScene& scene, const RenderView& view, const ReferenceOptions& o) {
  ProjectionParams p;
  p.view = view.view * view.model;
  p.viewport = glm::vec2(view.viewport);
  p.focal = view.focalPixels();
  p.tanHalfFov = view.tanHalfFov();
  p.nearPlane = view.nearPlane;
  p.splatScale = o.splatScale;
  p.cullOutsideViewport = o.cullOutsideViewport;
  const glm::vec3 camera = glm::vec3(glm::inverse(view.model) * glm::vec4(view.cameraPosition, 1.0f));
  const int degree = std::min(o.shDegree, scene.shDegree);

  // 1. Project every Gaussian (same code the GPU test compares against).
  std::vector<Splat2D> splats;
  splats.reserve(scene.size());
  for (size_t i = 0; i < scene.size(); ++i) {
    const float opacity = sigmoid(scene.opacityLogit[i]);
    if (opacity < 1.0f / 255.0f) continue;
    const Cov3 cov = covarianceFromScaleRotation(glm::exp(scene.logScale[i]), scene.rotation[i]);
    const ProjectedGaussian g = projectGaussian(scene.position[i], cov, p);
    if (!g.visible) continue;
    const glm::vec3 dir = glm::normalize(scene.position[i] - camera);
    const glm::vec3 color = evalSphericalHarmonics(degree, dir, scene.shDC[i], scene.restOf(i), scene.shRestPerChannel);
    splats.push_back({depthKey32(g.depth), g, glm::min(color, glm::vec3(1.0f)), opacity});
  }

  // 2. Back-to-front order (ascending key). stable_sort keeps file order for equal depths.
  if (o.sorted)
    std::stable_sort(splats.begin(), splats.end(), [](const Splat2D& a, const Splat2D& b) { return a.key < b.key; });

  // 3. Composite in float, rows bottom-up like OpenGL.
  const int W = view.viewport.x, H = view.viewport.y;
  std::vector<glm::vec3> image(static_cast<size_t>(W) * H, o.background);
  for (const Splat2D& s : splats) {
    const ProjectedGaussian& g = s.g;
    // Pixels a GPU quad would touch: the oriented rectangle or the axis-aligned square.
    const glm::vec2 extent = o.orientedQuads ? glm::abs(g.axisMajor) + glm::abs(g.axisMinor) : glm::vec2(g.radius);
    const int x0 = std::max(0, static_cast<int>(std::floor(g.center.x - extent.x)));
    const int x1 = std::min(W - 1, static_cast<int>(std::ceil(g.center.x + extent.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(g.center.y - extent.y)));
    const int y1 = std::min(H - 1, static_cast<int>(std::ceil(g.center.y + extent.y)));
    const float majorLen2 = glm::dot(g.axisMajor, g.axisMajor), minorLen2 = glm::dot(g.axisMinor, g.axisMinor);
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const glm::vec2 d = glm::vec2(x + 0.5f, y + 0.5f) - g.center;  // pixel center, like gl_FragCoord
        if (o.orientedQuads) {
          if (std::abs(glm::dot(d, g.axisMajor)) > majorLen2 || std::abs(glm::dot(d, g.axisMinor)) > minorLen2) continue;
        } else if (std::abs(d.x) > g.radius || std::abs(d.y) > g.radius) {
          continue;
        }
        const float power = -0.5f * (g.conic.x * d.x * d.x + 2.0f * g.conic.y * d.x * d.y + g.conic.z * d.y * d.y);
        if (power > 0.0f) continue;
        const float alpha = std::min(0.99f, s.opacity * std::exp(power));
        if (alpha < 1.0f / 255.0f) continue;
        glm::vec3& c = image[static_cast<size_t>(y) * W + x];
        c = alpha * s.color + (1.0f - alpha) * c;  // "over" operator, back to front
      }
    }
  }

  // 4. Quantize to 8 bits, flip to top-down rows.
  Image out(W, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const glm::vec3 c = glm::clamp(image[static_cast<size_t>(H - 1 - y) * W + x], 0.0f, 1.0f);
      uint8_t* px = out.pixel(x, y);
      for (int k = 0; k < 3; ++k) px[k] = static_cast<uint8_t>(std::lround(c[k] * 255.0f));
    }
  return out;
}

}  // namespace gsv
