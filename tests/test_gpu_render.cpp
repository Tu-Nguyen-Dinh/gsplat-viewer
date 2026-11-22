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

// Same scene, splats in a random (seeded) order.
GaussianScene shuffled(const GaussianScene& in, uint32_t seed) {
  std::vector<size_t> perm(in.size());
  for (size_t i = 0; i < perm.size(); ++i) perm[i] = i;
  uint32_t state = seed;
  for (size_t i = perm.size(); i > 1; --i) {
    state = state * 1664525u + 1013904223u;
    std::swap(perm[i - 1], perm[state % i]);
  }
  GaussianScene out = in;
  const size_t rest = static_cast<size_t>(in.shRestPerChannel) * 3;
  for (size_t i = 0; i < perm.size(); ++i) {
    const size_t j = perm[i];
    out.position[i] = in.position[j];
    out.logScale[i] = in.logScale[j];
    out.rotation[i] = in.rotation[j];
    out.opacityLogit[i] = in.opacityLogit[j];
    out.shDC[i] = in.shDC[j];
    for (size_t k = 0; k < rest; ++k) out.shRest[i * rest + k] = in.shRest[j * rest + k];
  }
  return out;
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
    const Image gpu10 = renderGpu(renderer, view, s, Framebuffer::Format::RGB10A2);
    const double p16 = psnr(ref, gpu16), p8 = psnr(ref, gpu8), p10 = psnr(ref, gpu10);
    std::printf("    yaw %.1f: PSNR vs reference  RGBA16F %.2f dB (max diff %d)  RGB10A2 %.2f dB  RGBA8 %.2f dB (max diff %d)\n",
                yaw, p16, maxAbsDifference(ref, gpu16), p10, p8, maxAbsDifference(ref, gpu8));
    CHECK(p16 > 40.0);
    CHECK(p8 > 33.0);
    if (test::softwareRenderer()) {
      // llvmpipe blends 8-bit targets without the per-blend rounding loss of real GPUs.
      std::printf("    (software renderer: 8-bit vs 10/16-bit precision ordering not asserted)\n");
    } else {
      CHECK(p10 > p8);  // blending precision: 10 bits beats 8 bits
      CHECK(p16 > p8);
    }
    if (!previousRef.rgb.empty()) CHECK(psnr(previousRef, ref) < 25.0);  // the two views really differ
    previousRef = ref;
    saveForInspection(yaw < 1.0f ? "reference_a.png" : "reference_b.png", ref);
    saveForInspection(yaw < 1.0f ? "gpu_rgba16f_a.png" : "gpu_rgba16f_b.png", gpu16);
  }
}

GPU_TEST_CASE(gpu_sorted_on_gpu_matches_reference) {
  const GaussianScene scene = makeDemoScene(30000, 8);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  renderer.upload(scene);
  const RenderView view = makeView(scene, 4.0f, 0.9f);
  SplatSettings s;
  s.sort = SortMode::GpuRadix;
  const Image ref = renderReference(scene, view, referenceFor(s));
  const Image img32 = renderGpu(renderer, view, s, Framebuffer::Format::RGBA16F);
  const double p32 = psnr(ref, img32);
  s.depthKeyBits = 24;
  const Image img24 = renderGpu(renderer, view, s, Framebuffer::Format::RGBA16F);
  const double p24 = psnr(ref, img24);
  s.depthKeyBits = 16;
  const double p16 = psnr(ref, renderGpu(renderer, view, s, Framebuffer::Format::RGBA16F));
  s.shDegree = 0;  // without view-dependent color the image must differ (SH is really used)
  const double pNoSh = psnr(ref, renderGpu(renderer, view, s, Framebuffer::Format::RGBA16F));
  std::printf("    GPU sort: 32-bit keys %.2f dB, 24-bit %.2f dB (vs 32-bit %.2f dB), 16-bit %.2f dB, SH degree 0 %.2f dB\n",
              p32, p24, psnr(img32, img24), p16, pNoSh);
  CHECK(p32 > 40.0);
  // 24-bit float keys: practically the 32-bit image (measured ~70 dB apart, ~0.1 dB vs reference).
  CHECK(p24 > p32 - 0.3);
  CHECK(psnr(img32, img24) > 60.0);
  CHECK(p16 > 35.0);
  CHECK(pNoSh < 35.0);
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
  // The splats are shuffled and uploaded uncompressed (the compressed format reorders them along a
  // Morton curve): the unsorted visible-list order then has no relation to depth on every driver.
  // A driver whose atomics preserve index order (llvmpipe) would otherwise draw the spatially
  // coherent Morton order, which composites almost correctly (41 dB measured).
  const GaussianScene scene = shuffled(makeDemoScene(20000, 21), 1234u);
  SplatRenderer renderer;
  std::string log;
  CHECK(renderer.init(log));
  renderer.setCompressed(false);
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
