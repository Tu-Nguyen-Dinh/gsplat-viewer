#include "render/cloud_renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gsv {

namespace {
struct CloudVertex {      // 20 bytes
  glm::vec3 position;
  uint8_t rgba[4];
  uint16_t intensity;
  uint16_t classification;
};
static_assert(sizeof(CloudVertex) == 20, "packed vertex");
}  // namespace

CloudRenderer::~CloudRenderer() {
  if (vbo_) glDeleteBuffers(1, &vbo_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
}

bool CloudRenderer::init(std::string& log) {
  if (!program_.loadGraphics("cloud.vert", "cloud.frag", log)) return false;
  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(CloudVertex),
                        reinterpret_cast<const void*>(offsetof(CloudVertex, position)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(CloudVertex),
                        reinterpret_cast<const void*>(offsetof(CloudVertex, rgba)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_UNSIGNED_SHORT, GL_FALSE, sizeof(CloudVertex),
                        reinterpret_cast<const void*>(offsetof(CloudVertex, intensity)));
  glBindVertexArray(0);
  return true;
}

void CloudRenderer::upload(const PointCloud& cloud) {
  std::vector<CloudVertex> verts(cloud.size());
  const bool color = cloud.hasColor();
  const bool intensity = cloud.intensity.size() == cloud.size();
  const bool cls = cloud.classification.size() == cloud.size();
  for (size_t i = 0; i < cloud.size(); ++i) {
    CloudVertex& v = verts[i];
    v.position = cloud.position[i];
    const glm::u8vec3 c = color ? cloud.color[i] : glm::u8vec3(200);
    v.rgba[0] = c.r;
    v.rgba[1] = c.g;
    v.rgba[2] = c.b;
    v.rgba[3] = 255;
    v.intensity = intensity ? cloud.intensity[i] : 0;
    v.classification = cls ? cloud.classification[i] : 0;
  }
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(CloudVertex)), verts.data(),
               GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  count_ = static_cast<GLsizei>(verts.size());
  hasColor_ = color;
  localMin_ = cloud.localMin;
  localMax_ = cloud.localMax;
  if (localMax_ == localMin_ && count_ > 0) {  // bounds not computed by the caller
    localMin_ = localMax_ = cloud.position[0];
    for (const glm::vec3& p : cloud.position) {
      localMin_ = glm::min(localMin_, p);
      localMax_ = glm::max(localMax_, p);
    }
  }
}

void CloudRenderer::draw(const RenderView& view, const GeoFrame& layerToWorld, const glm::dvec3& renderOrigin,
                         const CloudSettings& s) {
  if (!program_.valid() || count_ == 0) return;
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
#if !defined(GSV_GLES)
  glEnable(GL_PROGRAM_POINT_SIZE);  // always on in OpenGL ES
#endif
  glDisable(GL_BLEND);
  CloudColorMode mode = s.colorMode;
  if (mode == CloudColorMode::Rgb && !hasColor_) mode = CloudColorMode::Height;
  const int axis = s.heightAxis == 1 ? 1 : 2;
  const float lo = localMin_[axis], hi = localMax_[axis];
  program_.use();
  program_.set("uModelView", view.view * layerToWorld.localMatrix(renderOrigin));
  program_.set("uProjection", view.projection);
  program_.set("uPointSize", s.pointSize);
  program_.set("uWorldSize", s.worldSize);
  program_.set("uFocal", view.focalPixels().y);
  program_.set("uColorMode", static_cast<int>(mode));
  program_.set("uHeightRange", glm::vec2(lo, 1.0f / std::max(hi - lo, 1e-6f)));
  program_.set("uHeightAxis", axis);
  program_.set("uRoundPoints", s.roundPoints ? 1 : 0);
  glBindVertexArray(vao_);
  glDrawArrays(GL_POINTS, 0, count_);
  glBindVertexArray(0);
  glDisable(GL_DEPTH_TEST);
}

}  // namespace gsv
