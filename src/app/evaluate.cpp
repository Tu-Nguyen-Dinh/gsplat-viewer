// --eval: novel-view quality of a trained scene.
//
// For each held-out photo (never seen during training) the scene is rendered offscreen from that
// photo's exact COLMAP pose and pinhole intrinsics, then compared with the photo (PSNR, SSIM).
// Brush's own render of the same view, if present, is scored with the same code, so the two
// numbers show whether this renderer reproduces what the trainer optimized.
//
// Writes <scene dir>/eval/metrics.csv and side-by-side images (photo | ours | Brush).
#include "app/app.h"
#include "core/platform.h"
#include "core/utf8_path.h"
#include "gl/framebuffer.h"
#include "gl/gl_loader.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace gsv {

namespace {
namespace fs = std::filesystem;

// Decodes any image ffmpeg understands (JPG, PNG) into 8-bit RGB through a PPM pipe.
bool loadImageWithFfmpeg(const fs::path& path, Image& out, std::string& error) {
  const std::string cmd = "ffmpeg -hide_banner -loglevel error -nostdin -i " + quoteArgument(pathToUtf8(path)) +
                          " -frames:v 1 -f image2pipe -vcodec ppm -pix_fmt rgb24 -";
  std::FILE* pipe = openProcessRead(cmd);
  if (!pipe) return error = "cannot run ffmpeg", false;
  bool streamError = false;
  const bool ok = readPpmFrame(pipe, out, streamError);
  const int code = closeProcess(pipe);
  if (!ok || code != 0) return error = "ffmpeg could not decode " + pathToUtf8(path), false;
  return true;
}

// Brush writes renders of its held-out views to <scene>/brush/eval_<step>/<image stem>.png.
// Returns the folder of the last step, or an empty path.
fs::path findBrushEvalDir(const fs::path& sceneDir) {
  fs::path best;
  long bestStep = -1;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(sceneDir / "brush", ec)) {
    const std::string n = e.path().filename().string();
    if (!e.is_directory() || n.rfind("eval_", 0) != 0) continue;
    const long step = std::atol(n.c_str() + 5);
    if (step > bestStep) bestStep = step, best = e.path();
  }
  return best;
}

Image sideBySide(const Image& a, const Image& b, const Image* c) {
  const int n = c ? 3 : 2;
  Image out(a.width * n, a.height);
  for (int y = 0; y < a.height; ++y)
    for (int i = 0; i < n; ++i) {
      const Image& src = i == 0 ? a : i == 1 ? b : *c;
      std::copy_n(src.pixel(0, y), static_cast<size_t>(a.width) * 3, out.pixel(i * a.width, y));
    }
  return out;
}

}  // namespace

int App::runEvaluation(const Options& o) {
  if (colmap_.images.empty()) {
    std::fprintf(stderr, "--eval needs the training cameras: no COLMAP model next to the scene (use --colmap DIR)\n");
    return 1;
  }
  const fs::path sceneDir = pathFromUtf8(o.scene).parent_path();
  const fs::path modelDir = pathFromUtf8(colmapDir_);
  // dataset/sparse/0 -> dataset/images ; sparse -> images
  const fs::path imagesDir =
      (modelDir.filename() == "0" ? modelDir.parent_path().parent_path() : modelDir.parent_path()) / "images";
  const fs::path brushDir = findBrushEvalDir(sceneDir);
  const fs::path outDir = sceneDir / "eval";
  std::error_code ec;
  fs::create_directories(outDir, ec);

  // Held-out views: the ones Brush evaluated, else every N-th photo (Brush's default split).
  std::vector<size_t> test;
  for (size_t i = 0; i < colmap_.images.size(); ++i) {
    const fs::path stem = pathFromUtf8(colmap_.images[i].name).stem();
    const bool inBrush = !brushDir.empty() && fs::exists(brushDir / (stem.string() + ".png"), ec);
    if (brushDir.empty() ? (i % static_cast<size_t>(o.evalEvery) == 0) : inBrush) test.push_back(i);
  }
  std::printf("Evaluating %zu held-out views of %zu (%s)\n", test.size(), colmap_.images.size(),
              brushDir.empty() ? "every N-th photo" : ("Brush split: " + pathToUtf8(brushDir)).c_str());
  if (test.empty()) return 1;

  // Black background: Brush composites its renders over black during evaluation.
  const glm::vec3 savedBackground = background_;
  background_ = glm::vec3(0.0f);
  SplatSettings settings = splatSettings_;
  settings.sort = SortMode::GpuRadix;
  settings.debugView = DebugView::Color;

  std::ofstream csv(outDir / "metrics.csv");
  csv << "image,width,height,psnr_ours,ssim_ours,psnr_brush,ssim_brush,render_ms\n";
  double sumPsnr = 0, sumSsim = 0, sumPsnrB = 0, sumSsimB = 0;
  int count = 0, countB = 0, saved = 0;

  for (size_t idx : test) {
    const ColmapImage& im = colmap_.images[idx];
    const ColmapCamera& cam = *colmap_.camera(im.cameraId);
    const glm::dvec4 k = cam.pinhole();
    Image photo;
    std::string error;
    if (!loadImageWithFfmpeg(imagesDir / pathFromUtf8(im.name), photo, error)) {
      std::fprintf(stderr, "  skip %s: %s\n", im.name.c_str(), error.c_str());
      continue;
    }
    if (photo.width != cam.width || photo.height != cam.height) {
      std::fprintf(stderr, "  skip %s: photo %dx%d but camera %dx%d\n", im.name.c_str(), photo.width, photo.height,
                   cam.width, cam.height);
      continue;
    }

    // The trained splats live in COLMAP world coordinates: no model transform, the photo's pose
    // and intrinsics as they are. COLMAP's pixel origin is the top-left; OpenGL's the bottom-left.
    RenderView view;
    view.model = glm::mat4(1.0f);
    view.view = im.viewMatrixGl();
    view.viewport = {cam.width, cam.height};
    view.focalOverride = glm::vec2(static_cast<float>(k.x), static_cast<float>(k.y));
    view.principal = glm::vec2(static_cast<float>(k.z), static_cast<float>(cam.height - k.w));
    view.fovY = static_cast<float>(2.0 * std::atan(0.5 * cam.height / k.y));
    view.nearPlane = 0.01f;
    view.cameraPosition = glm::vec3(im.center());
    view.projection = glm::perspective(view.fovY, view.aspect(), view.nearPlane, 1000.0f);

    Framebuffer target;
    // Half-float target: hundreds of low-alpha splats blended into 8 bits per channel lose
    // precision at every step; the trainer accumulates in fp32 and quantizes only once.
    target.create(cam.width, cam.height, Framebuffer::Format::RGBA16F);
    target.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const auto t0 = std::chrono::steady_clock::now();
    splats_.render(view, settings);
    const Image ours = readFramebuffer(target.id(), cam.width, cam.height);  // waits for the GPU
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    const double p = psnr(ours, photo), s = ssim(ours, photo);
    sumPsnr += p, sumSsim += s, ++count;

    Image brush;
    double pb = NAN, sb = NAN;
    const fs::path brushFile = brushDir.empty() ? fs::path() : brushDir / (pathFromUtf8(im.name).stem().string() + ".png");
    if (!brushFile.empty() && loadImageWithFfmpeg(brushFile, brush, error) && brush.width == photo.width &&
        brush.height == photo.height) {
      pb = psnr(brush, photo), sb = ssim(brush, photo);
      sumPsnrB += pb, sumSsimB += sb, ++countB;
    }
    csv << im.name << ',' << cam.width << ',' << cam.height << ',' << p << ',' << s << ',' << pb << ',' << sb << ','
        << ms << '\n';
    std::printf("  %-24s ours %6.2f dB  SSIM %.4f", im.name.c_str(), p, s);
    if (!std::isnan(pb)) std::printf("  |  Brush %6.2f dB  SSIM %.4f  (diff %+.2f dB)", pb, sb, p - pb);
    std::printf("\n");

    if (saved < 3) {  // a few photo | ours | Brush strips for the README / debugging
      const Image strip = sideBySide(photo, ours, std::isnan(pb) ? nullptr : &brush);
      savePng(outDir / ("compare_" + pathFromUtf8(im.name).stem().string() + ".png"), strip, error);
      ++saved;
    }
    glfwPollEvents();
  }
  background_ = savedBackground;
  if (count == 0) return 1;

  std::printf("\nMean over %d views: ours %.2f dB, SSIM %.4f", count, sumPsnr / count, sumSsim / count);
  if (countB > 0)
    std::printf(" | Brush %.2f dB, SSIM %.4f | difference %+.2f dB", sumPsnrB / countB, sumSsimB / countB,
                sumPsnr / count - sumPsnrB / countB);
  std::printf("\nWrote %s\n", pathToUtf8(outDir / "metrics.csv").c_str());
  return 0;
}

}  // namespace gsv
