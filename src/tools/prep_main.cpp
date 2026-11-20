// gsplat_prep: turns a phone video into a set of sharp frames for Structure-from-Motion.
//
//   gsplat_prep video.mp4 [--out DIR] [--fps 10] [--target 200] [--analysis-width 480]
//
// Pass 1: ffmpeg decodes the video at --fps into small grayscale PGM frames on a pipe (no temporary
//         files); each frame gets a sharpness score (variance of the Laplacian).
// Pass 2: the sharpest frame of every window of N = candidates / target frames is kept (whole
//         windows that are blurred, e.g. while shaking the phone, are dropped) and ffmpeg writes
//         only those frames at full resolution to DIR/images/.
// DIR/frames.csv lists every candidate frame with its score and whether it was kept.
#include "core/frame_select.h"
#include "core/platform.h"
#include "core/utf8_path.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gsv;
namespace fs = std::filesystem;

namespace {

struct PrepOptions {
  std::string video;
  std::string out;  // default: the video's folder
  std::string ffmpeg = "ffmpeg";
  double fps = 10.0;
  int target = 200;
  int analysisWidth = 480;
  double minRelative = 0.35;
};

int usage() {
  std::fprintf(stderr,
               "gsplat_prep " GSV_VERSION "\n\n"
               "Usage: gsplat_prep video.mp4 [options]\n"
               "  --out DIR             output folder (default: the video's folder); writes DIR/images/ + DIR/frames.csv\n"
               "  --fps F               candidate frames per second of video (default 10)\n"
               "  --target N            number of frames to keep (default 200)\n"
               "  --analysis-width W    width of the grayscale frames used for scoring (default 480)\n"
               "  --min-relative R      drop windows whose best frame scores < R x median (default 0.35)\n"
               "  --ffmpeg PATH         ffmpeg executable (default: ffmpeg on PATH)\n");
  return 2;
}

bool parse(int argc, char** argv, PrepOptions& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (a == "--out" && (v = value())) o.out = v;
    else if (a == "--fps" && (v = value())) o.fps = std::atof(v);
    else if (a == "--target" && (v = value())) o.target = std::atoi(v);
    else if (a == "--analysis-width" && (v = value())) o.analysisWidth = std::atoi(v);
    else if (a == "--min-relative" && (v = value())) o.minRelative = std::atof(v);
    else if (a == "--ffmpeg" && (v = value())) o.ffmpeg = v;
    else if (!a.empty() && a[0] != '-' && o.video.empty()) o.video = a;
    else return false;
  }
  return !o.video.empty() && o.fps > 0 && o.target > 0 && o.analysisWidth >= 16;
}

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  Utf8Argv args(argc, argv);  // UTF-8 even for non-ASCII paths on Windows
  PrepOptions o;
  if (!parse(args.argc(), args.argv(), o)) return usage();

  const fs::path video = pathFromUtf8(o.video);
  if (!fs::is_regular_file(video)) {
    std::fprintf(stderr, "Video not found: %s\n", o.video.c_str());
    return 1;
  }
  const fs::path outDir = o.out.empty() ? video.parent_path() : pathFromUtf8(o.out);
  const fs::path imagesDir = outDir / "images";
  std::error_code ec;
  fs::remove_all(imagesDir, ec);  // stale frames from a previous run would confuse COLMAP
  fs::create_directories(imagesDir, ec);
  if (ec) {
    std::fprintf(stderr, "Cannot create %s: %s\n", pathToUtf8(imagesDir).c_str(), ec.message().c_str());
    return 1;
  }

  char fpsText[32];
  std::snprintf(fpsText, sizeof fpsText, "%g", o.fps);
  const std::string ffmpeg = quoteArgument(o.ffmpeg) + " -hide_banner -loglevel error -nostdin -i " +
                             quoteArgument(pathToUtf8(video));

  // ---- Pass 1: score every candidate frame --------------------------------------------------
  const auto t0 = std::chrono::steady_clock::now();
  const std::string scoreCmd = ffmpeg + " -vf \"fps=" + fpsText + ",scale=" + std::to_string(o.analysisWidth) +
                               ":-2,format=gray\" -f image2pipe -vcodec pgm -";
  std::FILE* pipe = openProcessRead(scoreCmd);
  if (!pipe) {
    std::fprintf(stderr, "Cannot start ffmpeg (%s)\n", o.ffmpeg.c_str());
    return 1;
  }
  std::vector<double> scores;
  GrayImage frame;
  bool streamError = false;
  while (readPgmFrame(pipe, frame, streamError)) {
    scores.push_back(laplacianVariance(frame));
    if (scores.size() % 100 == 0) std::printf("\r  scored %zu frames", scores.size());
  }
  const int ffmpegExit = closeProcess(pipe);
  std::printf("\r  scored %zu frames (%dx%d) in %.1f s\n", scores.size(), frame.width, frame.height, secondsSince(t0));
  if (streamError || ffmpegExit != 0 || scores.empty()) {
    std::fprintf(stderr, "ffmpeg failed while decoding (exit %d%s)\n", ffmpegExit,
                 streamError ? ", malformed frame stream" : "");
    return 1;
  }

  // ---- Selection ------------------------------------------------------------------------------
  const int window = std::max(1, static_cast<int>(std::lround(static_cast<double>(scores.size()) / o.target)));
  const FrameSelection sel = selectSharpestFrames(scores, window, o.minRelative);
  std::printf("  window %d -> kept %zu frames, dropped %d blurred windows (median score %.1f)\n", window,
              sel.selected.size(), sel.rejectedBlurry, sel.medianScore);
  if (sel.selected.empty()) {
    std::fprintf(stderr, "No usable frames\n");
    return 1;
  }

  {
    std::ofstream csv(outDir / "frames.csv");
    csv << "candidate,time_s,sharpness,selected\n";
    size_t next = 0;
    for (size_t i = 0; i < scores.size(); ++i) {
      const bool kept = next < sel.selected.size() && sel.selected[next] == static_cast<int>(i);
      if (kept) ++next;
      csv << i << ',' << i / o.fps << ',' << scores[i] << ',' << (kept ? 1 : 0) << '\n';
    }
  }

  // ---- Pass 2: write only the selected frames at full resolution -----------------------------
  // select= uses n = frame index after the fps filter, i.e. the same numbering as pass 1.
  const auto t1 = std::chrono::steady_clock::now();
  std::string expr;
  for (int i : sel.selected) expr += (expr.empty() ? "" : "+") + std::string("eq(n\\,") + std::to_string(i) + ")";
  const std::string writeCmd = ffmpeg + " -vf \"fps=" + fpsText + ",select='" + expr +
                               "'\" -fps_mode passthrough -qscale:v 2 " +
                               quoteArgument(pathToUtf8(imagesDir / "frame_%04d.jpg"));
  const int writeExit = runProcess(writeCmd);
  size_t written = 0;
  for (const auto& e : fs::directory_iterator(imagesDir, ec))
    if (e.path().extension() == ".jpg") ++written;
  std::printf("  wrote %zu images to %s in %.1f s\n", written, pathToUtf8(imagesDir).c_str(), secondsSince(t1));
  if (writeExit != 0 || written != sel.selected.size()) {
    std::fprintf(stderr, "ffmpeg failed while writing frames (exit %d, %zu of %zu written)\n", writeExit, written,
                 sel.selected.size());
    return 1;
  }
  std::printf("Done in %.1f s\n", secondsSince(t0));
  return 0;
}
