// Offline modes of the viewer:
//   --bench      renders an orbit with several renderer configurations and reports averaged
//                per-stage GPU times, frame time and fragment counts
//   --turntable  saves the frames of an orbit as PNG files
#include "app/app.h"

#include "gl/framebuffer.h"
#include "gl/gl_loader.h"
#include "scene/ply_io.h"

#include <filesystem>

#include <GLFW/glfw3.h>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace gsv {

namespace {

struct BenchConfig {
  const char* name;
  SplatSettings settings;
  bool halfSh;
};

std::vector<BenchConfig> makeConfigs() {
  SplatSettings base;  // GPU sort, 32-bit keys, oriented quads, SH degree 3
  std::vector<BenchConfig> c;
  SplatSettings s = base;
  s.sort = SortMode::CpuRadix;
  c.push_back({"cpu radix sort", s, true});
  c.push_back({"gpu sort (SH fp32)", base, false});
  c.push_back({"gpu sort (SH fp16)", base, true});
  s = base;
  s.depthKeyBits = 16;
  c.push_back({"gpu sort 16-bit keys", s, true});
  s.depthKeyBits = 24;
  c.push_back({"gpu sort 24-bit keys", s, true});
  s = base;
  s.orientedQuads = false;
  c.push_back({"square quads", s, true});
  s = base;
  s.shDegree = 0;
  c.push_back({"SH degree 0", s, true});
  return c;
}

}  // namespace

int App::runBenchmark(const Options& o) {
  glfwSwapInterval(0);  // never wait for the display: measure the real cost
  const std::vector<BenchConfig> configs = makeConfigs();
  const int frames = o.benchFrames;
  const float baseYaw = camera_.yaw;
  GLuint samplesQuery = 0;
  glGenQueries(1, &samplesQuery);

  std::string csv = "scene,splats,width,height,config,preprocess_ms,sort_ms,draw_ms,gpu_total_ms,frame_ms,fps,"
                    "raster_fragments_m,blended_fragments_m,upload_ms\n";
  std::printf("\nBenchmark: %s | %zu splats | %dx%d | %d frames per config (orbit 360 deg), VSync off\n",
              scene_.sourceName.c_str(), scene_.size(), o.width, o.height, frames);
  std::printf("%-22s %9s %9s %9s %9s %9s %7s %10s %10s\n", "config", "preproc", "sort", "draw", "GPU sum", "frame",
              "FPS", "raster(M)", "blend(M)");

  double uploadMs = splats_.stats().uploadSceneMs;
  for (const BenchConfig& cfg : configs) {
    if (cfg.halfSh != splats_.halfPrecisionSh()) {  // SH precision is decided at upload time
      splats_.setHalfPrecisionSh(cfg.halfSh);
      splats_.upload(scene_);
      uploadMs = splats_.stats().uploadSceneMs;
    }
    // Render into an offscreen target of exactly the requested size: the OS may make the
    // window smaller than asked (e.g. 1920x1080 does not fit next to the taskbar).
    const int fbW = o.width, fbH = o.height;
    Framebuffer target;
    target.create(fbW, fbH, targetFormat());
    int winW, winH;
    glfwGetFramebufferSize(window_, &winW, &winH);

    auto renderFrame = [&](float yaw, const SplatSettings& s) {
      camera_.yaw = yaw;
      target.bind();
      glClearColor(background_.r, background_.g, background_.b, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      splats_.render(makeRenderView(fbW, fbH), s);
    };
    auto present = [&] {  // show progress in the window (scaled copy, outside the timed passes)
      glBindFramebuffer(GL_READ_FRAMEBUFFER, target.id());
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
      glBlitFramebuffer(0, 0, fbW, fbH, 0, 0, winW, winH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glfwSwapBuffers(window_);
    };

    // Fragment counts at the start pose: every rasterized fragment vs fragments that survive discard.
    auto countSamples = [&](const SplatSettings& s) {
      glBeginQuery(GL_SAMPLES_PASSED, samplesQuery);
      renderFrame(baseYaw, s);
      glEndQuery(GL_SAMPLES_PASSED);
      GLuint64 n = 0;
      glGetQueryObjectui64v(samplesQuery, GL_QUERY_RESULT, &n);
      return static_cast<double>(n) * 1e-6;
    };
    SplatSettings coverage = cfg.settings;
    coverage.countCoverage = true;
    const double rasterM = countSamples(coverage);
    const double blendedM = countSamples(cfg.settings);

    for (int i = 0; i < 30; ++i) {  // warm-up: caches, clocks, buffer allocations
      renderFrame(baseYaw + 0.01f * i, cfg.settings);
      present();
    }
    glFinish();
    splats_.resetAverages();
    // Frame time = MEDIAN of the per-frame intervals, not total / frames: a single hitch outside the
    // renderer (another process, a GPU clock change, the compositor) used to add several ms to the
    // mean of a whole configuration and made 16-bit keys look slower than 32-bit ones.
    double cpuSortSum = 0.0;
    std::vector<double> intervals;
    intervals.reserve(static_cast<size_t>(frames));
    auto last = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
      renderFrame(baseYaw + glm::two_pi<float>() * static_cast<float>(f) / static_cast<float>(frames), cfg.settings);
      cpuSortSum += splats_.stats().cpuSortMs;
      present();
      glfwPollEvents();
      const auto now = std::chrono::steady_clock::now();
      intervals.push_back(std::chrono::duration<double, std::milli>(now - last).count());
      last = now;
    }
    glFinish();
    std::nth_element(intervals.begin(), intervals.begin() + static_cast<std::ptrdiff_t>(intervals.size() / 2),
                     intervals.end());
    const double frameMs = intervals[intervals.size() / 2];
    for (int i = 0; i < 6; ++i) renderFrame(baseYaw, cfg.settings);  // flush pending timer queries
    glFinish();

    const SplatRenderer::Averages a = splats_.averages();
    const double sortMs = cfg.settings.sort == SortMode::GpuRadix ? a.gpuSortMs : cpuSortSum / frames;
    const double gpuSum = a.preprocessMs + (cfg.settings.sort == SortMode::GpuRadix ? a.gpuSortMs : 0.0) + a.drawMs;
    std::printf("%-22s %7.3fms %7.3fms %7.3fms %7.3fms %7.3fms %7.0f %10.1f %10.1f%s\n", cfg.name, a.preprocessMs,
                sortMs, a.drawMs, gpuSum, frameMs, 1000.0 / frameMs, rasterM, blendedM,
                cfg.settings.sort == SortMode::GpuRadix ? "" : "  (sort on CPU)");
    char line[512];
    std::snprintf(line, sizeof(line), "\"%s\",%zu,%d,%d,\"%s\",%.4f,%.4f,%.4f,%.4f,%.4f,%.1f,%.2f,%.2f,%.1f\n",
                  scene_.sourceName.c_str(), scene_.size(), fbW, fbH, cfg.name, a.preprocessMs, sortMs, a.drawMs,
                  gpuSum, frameMs, 1000.0 / frameMs, rasterM, blendedM, uploadMs);
    csv += line;
  }
  std::printf("Scene upload (precompute + buffers): %.1f ms with SH fp16 | GPU buffers %.1f MB\n", uploadMs,
              static_cast<double>(splats_.stats().gpuMemoryBytes) / (1024.0 * 1024.0));
  glDeleteQueries(1, &samplesQuery);

  if (!o.benchCsv.empty()) {
    const bool exists = std::ifstream(pathFromUtf8(o.benchCsv)).good();
    std::ofstream out(pathFromUtf8(o.benchCsv), std::ios::app);
    out << (exists ? csv.substr(csv.find('\n') + 1) : csv);  // header only once
    std::printf("Appended results to %s\n", o.benchCsv.c_str());
  }
  return 0;
}

int App::runTurntable(const Options& o) {
  // Offscreen frames of a 360 degree orbit, saved as PNG (turn them into a GIF/video with ffmpeg).
  std::error_code ec;
  std::filesystem::create_directories(pathFromUtf8(o.turntableDir), ec);
  Framebuffer target;
  target.create(o.width, o.height, targetFormat());
  const float baseYaw = camera_.yaw;
  for (int f = 0; f < o.benchFrames; ++f) {
    camera_.yaw = baseYaw + glm::two_pi<float>() * static_cast<float>(f) / static_cast<float>(o.benchFrames);
    target.bind();
    glClearColor(background_.r, background_.g, background_.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    splats_.render(makeRenderView(o.width, o.height), splatSettings_);
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%04d.png", f);
    std::string error;
    if (!savePng(pathFromUtf8(o.turntableDir) / name, readFramebuffer(target.id(), o.width, o.height), error)) {
      std::fprintf(stderr, "Cannot write frame: %s\n", error.c_str());
      return 1;
    }
  }
  std::printf("Wrote %d frames to %s\n", o.benchFrames, o.turntableDir.c_str());
  return 0;
}

namespace {
// Catmull-Rom spline through p1..p2 (p0 and p3 shape the tangents), t in [0, 1].
glm::vec3 catmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t) {
  const float t2 = t * t, t3 = t2 * t;
  return 0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                 (3.0f * p1 - p0 - 3.0f * p2 + p3) * t3);
}
}  // namespace

int App::runFlythrough(const Options& o) {
  // A smooth flight along the path the phone took: every k-th training camera (photos are in
  // video order) is a key frame, positions and view directions follow Catmull-Rom splines through
  // them. Hand-held jitter averages out between key frames.
  if (colmap_.images.size() < 2) {
    std::fprintf(stderr, "--flythrough needs the training cameras (COLMAP model next to the scene)\n");
    return 1;
  }
  const glm::mat3 r(orientation_);
  std::vector<glm::vec3> eyes, dirs;
  const size_t step = std::max<size_t>(1, colmap_.images.size() / 16);
  for (size_t i = 0; i < colmap_.images.size(); i += step) {
    eyes.push_back(r * glm::vec3(colmap_.images[i].center()));
    dirs.push_back(glm::normalize(r * glm::vec3(colmap_.images[i].forwardInWorld())));
  }
  goToCamera(0);  // field of view of the photos
  std::error_code ec;
  std::filesystem::create_directories(pathFromUtf8(o.flythroughDir), ec);
  Framebuffer target;
  target.create(o.width, o.height, targetFormat());
  const int keys = static_cast<int>(eyes.size());
  auto at = [&](const std::vector<glm::vec3>& v, int k) { return v[static_cast<size_t>(std::clamp(k, 0, keys - 1))]; };
  for (int f = 0; f < o.benchFrames; ++f) {
    const float u = static_cast<float>(f) / static_cast<float>(std::max(1, o.benchFrames - 1)) * static_cast<float>(keys - 1);
    const int k = std::min(static_cast<int>(u), keys - 2);
    const float t = u - static_cast<float>(k);
    const glm::vec3 eye = catmullRom(at(eyes, k - 1), at(eyes, k), at(eyes, k + 1), at(eyes, k + 2), t);
    const glm::vec3 dir = glm::normalize(catmullRom(at(dirs, k - 1), at(dirs, k), at(dirs, k + 1), at(dirs, k + 2), t));
    camera_.lookAt(eye, eye + dir * std::max(scene_.focusRadius, 1e-3f));
    target.bind();
    glClearColor(background_.r, background_.g, background_.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    splats_.render(makeRenderView(o.width, o.height), splatSettings_);
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%04d.png", f);
    std::string error;
    if (!savePng(pathFromUtf8(o.flythroughDir) / name, readFramebuffer(target.id(), o.width, o.height), error)) {
      std::fprintf(stderr, "Cannot write frame: %s\n", error.c_str());
      return 1;
    }
  }
  std::printf("Wrote %d frames along %d key cameras to %s\n", o.benchFrames, keys, o.flythroughDir.c_str());
  return 0;
}

}  // namespace gsv
