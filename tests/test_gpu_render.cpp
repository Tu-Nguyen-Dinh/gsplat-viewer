// Image tests: the GPU renderer must match the CPU reference renderer.
#include "core/camera.h"
#include "core/image.h"
#include "gl/framebuffer.h"
#include "render/reference_renderer.h"
#include "render/splat_renderer.h"
#include "scene/demo_scene.h"
#include "test_framework.h"

#include <filesystem>

using namespace gsv;

namespace {

constexpr int kW = 320, kH = 240;
const glm::vec3 kBackground(0.07f, 0.08f, 0.1f);

RenderView makeView(const GaussianScene& scene, float yaw, float distanceScale) {
  OrbitCamera cam;
  cam.frameBounds(scene.boundsMin, scene.boundsMax);
  cam.yaw = yaw;
  cam.distance *= distanceScale;
  RenderView v;
  v.view = cam.view();
  v.viewport = {kW, kH};
  v.projection = cam.projection(v.aspect());
  v.cameraPosition = cam.position();
  v.fovY = cam.fovY;
  v.nearPlane = cam.nearPlane;
  return v;
}

Image renderGpu(SplatRenderer& r, const RenderView& view, const SplatSettings& s, Framebuffer::Format format) {
  Framebuffer fb;
  fb.create(kW, kH, format);
  fb.bind();
  glClearColor(kBackground.r, kBackground.g, kBackground.b, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  r.render(view, s);
  Image img = readFramebuffer(fb.id(), kW, kH);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return img;
}

ReferenceOptions referenceFor(const SplatSettings& s) {
  ReferenceOptions o;
  o.shDegree = s.shDegree;
  o.splatScale = s.splatScale;
  o.orientedQuads = s.orientedQuads;
  o.cullOutsideViewport = s.cullOutsideViewport;
  o.background = kBackground;
  return o;
}

void saveForInspection(const char* name, const Image& img) {
  std::string error;
  const auto dir = std::filesystem::temp_directory_path() / "gsv_test_images";
  std::filesystem::create_directories(dir);
  savePng(dir / name, img, error);
}

}  // namespace

GPU_TEST_CASE(gpu_render_matches_cpu_reference) {
  const GaussianScene scene = makeDemoScene(20000, 21);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  renderer.upload(scene);

  Image previousRef;
  for (float yaw : {0.6f, 2.5f}) {
    const RenderView view = makeView(scene, yaw, 0.8f);
    SplatSettings s;
    s.sort = SortMode::CpuRadix;
    const Image ref = renderReference(scene, view, referenceFor(s));
    const Image gpu16 = renderGpu(renderer, view, s, Framebuffer::Format::RGBA16F);
    const Image gpu8 = renderGpu(renderer, view, s, Framebuffer::Format::RGBA8);
    const double p16 = psnr(ref, gpu16), p8 = psnr(ref, gpu8);
    std::printf("    yaw %.1f: PSNR vs reference  RGBA16F %.2f dB (max diff %d)  RGBA8 %.2f dB (max diff %d)\n", yaw,
                p16, maxAbsDifference(ref, gpu16), p8, maxAbsDifference(ref, gpu8));
    CHECK(p16 > 40.0);
    CHECK(p8 > 33.0);
    if (!previousRef.rgb.empty()) CHECK(psnr(previousRef, ref) < 25.0);  // the two views really differ
    previousRef = ref;
    saveForInspection(yaw < 1.0f ? "reference_a.png" : "reference_b.png", ref);
    saveForInspection(yaw < 1.0f ? "gpu_rgba16f_a.png" : "gpu_rgba16f_b.png", gpu16);
  }
}

GPU_TEST_CASE(gpu_square_quads_match_reference) {
  const GaussianScene scene = makeDemoScene(10000, 4);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  renderer.upload(scene);
  const RenderView view = makeView(scene, 1.2f, 1.0f);
  SplatSettings s;
  s.orientedQuads = false;
  const double p = psnr(renderReference(scene, view, referenceFor(s)),
                        renderGpu(renderer, view, s, Framebuffer::Format::RGBA16F));
  std::printf("    square quads: PSNR %.1f dB\n", p);
  CHECK(p > 40.0);
}

GPU_TEST_CASE(unsorted_render_is_detected) {
  // "Test the test": without sorting the image must be clearly different,
  // otherwise the comparison above could not detect ordering bugs.
  const GaussianScene scene = makeDemoScene(20000, 21);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  renderer.upload(scene);
  const RenderView view = makeView(scene, 0.6f, 0.8f);
  SplatSettings s;
  s.sort = SortMode::None;
  const Image unsorted = renderGpu(renderer, view, s, Framebuffer::Format::RGBA16F);
  const double p = psnr(renderReference(scene, view, referenceFor(s)), unsorted);
  std::printf("    unsorted GPU vs sorted reference: PSNR %.1f dB\n", p);
  saveForInspection("gpu_unsorted.png", unsorted);
  CHECK(p < 30.0);
}
