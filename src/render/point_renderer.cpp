#include "render/point_renderer.h"

#include <cstddef>
#include <vector>

namespace gsv {

namespace {
struct PointVertex {
  glm::vec3 position;
  glm::vec4 color;  // rgb = base color, a = opacity
};
}  // namespace

PointRenderer::~PointRenderer() {
  if (vbo_) glDeleteBuffers(1, &vbo_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
}

bool PointRenderer::init(std::string& log) {
  if (!program_.loadGraphics("points.vert", "points.frag", log)) return false;
  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PointVertex),
                        reinterpret_cast<const void*>(offsetof(PointVertex, position)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(PointVertex),
                        reinterpret_cast<const void*>(offsetof(PointVertex, color)));
  glBindVertexArray(0);
  return true;
}

void PointRenderer::upload(const GaussianScene& scene) {
  std::vector<PointVertex> verts(scene.size());
  for (size_t i = 0; i < scene.size(); ++i) {
    verts[i].position = scene.position[i];
    verts[i].color = glm::vec4(baseColor(scene, i), sigmoid(scene.opacityLogit[i]));
  }
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(PointVertex)), verts.data(),
               GL_STATIC_DRAW);
  count_ = static_cast<GLsizei>(verts.size());
}

void PointRenderer::draw(const Params& p) {
  if (!program_.valid() || count_ == 0) return;
  glEnable(GL_DEPTH_TEST);
#if !defined(GSV_GLES)
  glEnable(GL_PROGRAM_POINT_SIZE);  // always on in OpenGL ES
#endif
  glDisable(GL_BLEND);
  program_.use();
  program_.set("uViewProj", p.projection * p.view);
  program_.set("uModel", p.model);
  program_.set("uPointSize", p.pointSize);
  program_.set("uMinOpacity", p.minOpacity);
  glBindVertexArray(vao_);
  glDrawArrays(GL_POINTS, 0, count_);
  glBindVertexArray(0);
}

}  // namespace gsv
