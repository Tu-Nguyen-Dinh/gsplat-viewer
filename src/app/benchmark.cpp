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
    target.create(fbW, fbH);
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
    double cpuSortSum = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
      renderFrame(baseYaw + glm::two_pi<float>() * static_cast<float>(f) / static_cast<float>(frames), cfg.settings);
      cpuSortSum += splats_.stats().cpuSortMs;
      present();
      glfwPollEvents();
    }
    glFinish();
    const double frameMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
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
    const bool exists = std::ifstream(o.benchCsv).good();
    std::ofstream out(o.benchCsv, std::ios::app);
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
  target.create(o.width, o.height);
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

}  // namespace gsv
