// Implementation of include/gsv/gsv.h: thin C wrappers over the C++ core. Every entry point
// catches exceptions (none may cross the C boundary) and reports failures via gsv_last_error().
// windows.h first (NOMINMAX: no min/max macros; it defines APIENTRY before the GL headers do).
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include "gsv/gsv.h"

#include "core/geo_frame.h"
#include "core/kdtree.h"
#include "core/picking.h"
#include "core/registration.h"
#include "gl/framebuffer.h"
#include "gl/headless_context.h"
#include "gl/shader.h"
#include "render/cloud_renderer.h"
#include "render/splat_renderer.h"
#include "scene/demo_scene.h"
#include "scene/las_io.h"
#include "scene/lod.h"
#include "scene/ply_io.h"
#include "scene/scene_io.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

struct gsv_scene {
  gsv::GaussianScene scene;
};
struct gsv_cloud {
  gsv::PointCloud cloud;
};
struct gsv_lod {
  gsv::LodTree tree;
};
struct gsv_renderer {
  int width = 0, height = 0;
  gsv::SplatRenderer splats;
  gsv::CloudRenderer cloud;
  bool haveScene = false, haveCloud = false;
  float pointSize = 2.0f;
  gsv::Framebuffer target;
  gsv::DepthImage lastDepth;
  gsv::PickCamera lastCamera;
  std::string gpuName;
};

namespace {

thread_local std::string tLastError;

gsv_status fail(const std::string& message, gsv_status code = GSV_ERROR) {
  tLastError = message;
  return code;
}
gsv_status ok() {
  tLastError.clear();
  return GSV_OK;
}

template <typename F>
gsv_status guarded(F&& f) {
  try {
    return f();
  } catch (const std::exception& e) {
    return fail(std::string("exception: ") + e.what());
  } catch (...) {
    return fail("unknown exception");
  }
}

gsv::GeoFrame toFrame(const gsv_frame& f) {
  gsv::GeoFrame g;
  g.scale = f.scale;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) g.rotation[c][r] = f.rotation[3 * r + c];  // glm: [column][row]
  g.translation = {f.translation[0], f.translation[1], f.translation[2]};
  return g;
}
void fromFrame(const gsv::GeoFrame& g, gsv_frame& f) {
  f.scale = g.scale;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) f.rotation[3 * r + c] = g.rotation[c][r];
  for (int k = 0; k < 3; ++k) f.translation[k] = g.translation[k];
}

std::vector<glm::dvec3> toPoints(const double* xyz, size_t n) {
  std::vector<glm::dvec3> p(n);
  for (size_t i = 0; i < n; ++i) p[i] = {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]};
  return p;
}

// Directory of this shared library (shaders are shipped next to it).
std::string libraryDirectory() {
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&libraryDirectory), &module))
    return {};
  wchar_t buf[4096];
  const DWORD n = GetModuleFileNameW(module, buf, 4096);
  if (n == 0 || n >= 4096) return {};
  return gsv::pathToUtf8(std::filesystem::path(std::wstring(buf, n)).parent_path());
#elif defined(__linux__) || defined(__APPLE__)
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(&libraryDirectory), &info) && info.dli_fname)
    return gsv::pathToUtf8(std::filesystem::path(info.dli_fname).parent_path());
  return {};
#else
  return {};
#endif
}

bool gShaderDirSet = false;
void ensureShaderDirectory() {
  if (gShaderDirSet) return;
  const std::string lib = libraryDirectory();
  if (!lib.empty() && std::filesystem::exists(gsv::pathFromUtf8(lib + "/shaders/splat.vert")))
    gsv::setShaderDirectory(lib + "/shaders");
  gShaderDirSet = true;  // else: the compiled-in source directory (GSV_SHADER_DIR)
}

}  // namespace

extern "C" {

int gsv_capi_version(void) { return GSV_CAPI_VERSION; }
const char* gsv_version(void) { return GSV_VERSION; }
const char* gsv_last_error(void) { return tLastError.c_str(); }

void gsv_frame_identity(gsv_frame* out) {
  if (out) fromFrame(gsv::GeoFrame::identity(), *out);
}

void gsv_set_shader_dir(const char* path) {
  if (!path) return;
  gsv::setShaderDirectory(path);
  gShaderDirSet = true;
}

// ---------------------------------------------------------------- scenes
gsv_status gsv_scene_load(const char* path, gsv_scene** out) {
  if (!path || !out) return fail("null argument", GSV_INVALID_ARGUMENT);
  *out = nullptr;
  return guarded([&] {
    gsv::LoadResult r = gsv::loadScene(gsv::pathFromUtf8(path));
    if (!r.ok) return fail(r.error);
    *out = new gsv_scene{std::move(r.scene)};
    return ok();
  });
}

gsv_scene* gsv_scene_create_demo(uint32_t count, uint32_t seed) {
  try {
    return new gsv_scene{gsv::makeDemoScene(count, seed)};
  } catch (...) {
    fail("out of memory");
    return nullptr;
  }
}

gsv_status gsv_scene_save_ply(const gsv_scene* s, const char* path) {
  if (!s || !path) return fail("null argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    std::string err;
    return gsv::savePly(gsv::pathFromUtf8(path), s->scene, err) ? ok() : fail(err);
  });
}

size_t gsv_scene_size(const gsv_scene* s) { return s ? s->scene.size() : 0; }
int gsv_scene_sh_degree(const gsv_scene* s) { return s ? s->scene.shDegree : 0; }

void gsv_scene_bounds(const gsv_scene* s, float mn[3], float mx[3]) {
  if (!s || !mn || !mx) return;
  for (int k = 0; k < 3; ++k) {
    mn[k] = s->scene.boundsMin[k];
    mx[k] = s->scene.boundsMax[k];
  }
}

void gsv_scene_focus(const gsv_scene* s, float c[3], float* r) {
  if (!s) return;
  if (c)
    for (int k = 0; k < 3; ++k) c[k] = s->scene.focusCenter[k];
  if (r) *r = s->scene.focusRadius;
}

void gsv_scene_free(gsv_scene* s) { delete s; }

// ---------------------------------------------------------------- clouds
gsv_status gsv_cloud_load_las(const char* path, gsv_cloud** out) {
  if (!path || !out) return fail("null argument", GSV_INVALID_ARGUMENT);
  *out = nullptr;
  return guarded([&] {
    auto c = std::make_unique<gsv_cloud>();
    std::string err;
    if (!gsv::readLas(path, c->cloud, err)) return fail(err);
    *out = c.release();
    return ok();
  });
}

gsv_status gsv_cloud_save_las(const gsv_cloud* c, const char* path, double scale) {
  if (!c || !path || !(scale > 0.0)) return fail("invalid argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    gsv::LasWriteOptions o;
    o.scale = scale;
    o.pointFormat = c->cloud.hasColor() ? 3 : 0;
    std::string err;
    return gsv::writeLas(path, c->cloud, o, err) ? ok() : fail(err);
  });
}

gsv_cloud* gsv_cloud_create(const double origin[3], const float* xyz, const uint8_t* rgb, size_t count) {
  if (!origin || (!xyz && count)) {
    fail("null argument", GSV_INVALID_ARGUMENT);
    return nullptr;
  }
  try {
    auto c = std::make_unique<gsv_cloud>();
    c->cloud.origin = {origin[0], origin[1], origin[2]};
    c->cloud.position.resize(count);
    std::memcpy(c->cloud.position.data(), xyz, count * sizeof(glm::vec3));
    if (rgb) {
      c->cloud.color.resize(count);
      std::memcpy(c->cloud.color.data(), rgb, count * 3);
    }
    c->cloud.computeBounds();
    return c.release();
  } catch (...) {
    fail("out of memory");
    return nullptr;
  }
}

size_t gsv_cloud_size(const gsv_cloud* c) { return c ? c->cloud.size() : 0; }
void gsv_cloud_origin(const gsv_cloud* c, double o[3]) {
  if (c && o)
    for (int k = 0; k < 3; ++k) o[k] = c->cloud.origin[k];
}
const float* gsv_cloud_positions(const gsv_cloud* c) {
  return c && !c->cloud.empty() ? &c->cloud.position[0].x : nullptr;
}
void gsv_cloud_free(gsv_cloud* c) { delete c; }

// ---------------------------------------------------------------- geometry
gsv_status gsv_estimate_similarity(const double* src, const double* dst, size_t n, int withScale, gsv_frame* out) {
  if (!src || !dst || !out) return fail("null argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    gsv::GeoFrame f;
    if (!gsv::estimateSimilarity(toPoints(src, n), toPoints(dst, n), withScale != 0, f))
      return fail("need at least 3 non-collinear point pairs");
    fromFrame(f, *out);
    return ok();
  });
}

gsv_status gsv_estimate_similarity_ransac(const double* src, const double* dst, size_t n, int withScale, double thr,
                                          gsv_frame* out, size_t* inliers) {
  if (!src || !dst || !out || !(thr > 0.0)) return fail("invalid argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    const gsv::RansacResult r = gsv::estimateSimilarityRansac(toPoints(src, n), toPoints(dst, n), withScale != 0, thr);
    if (!r.ok) return fail("no consistent transform found");
    fromFrame(r.transform, *out);
    if (inliers) *inliers = r.inliers.size();
    return ok();
  });
}

void gsv_icp_default_options(gsv_icp_options* o) {
  if (!o) return;
  o->max_iterations = 50;
  o->max_distance = 0.5;
  o->normal_neighbours = 30;
}

gsv_status gsv_icp(const gsv_cloud* source, const gsv_cloud* target, const gsv_frame* initial,
                   const gsv_icp_options* options, gsv_frame* out, double* rms, int* iterations) {
  if (!source || !target || !out) return fail("null argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    gsv_icp_options o;
    gsv_icp_default_options(&o);
    if (options) o = *options;
    const gsv::GeoFrame init = initial ? toFrame(*initial) : gsv::GeoFrame::identity();
    const gsv::PointCloud& S = source->cloud;
    const gsv::PointCloud& T = target->cloud;
    if (S.size() < 6 || T.size() < 6) return fail("ICP needs at least 6 points per cloud");
    // Work in the target's local float frame: source points go through `initial` in double.
    std::vector<glm::vec3> src(S.size());
    for (size_t i = 0; i < S.size(); ++i) src[i] = glm::vec3(init.apply(S.world(i)) - T.origin);
    gsv::KdTree tree;
    tree.build(T.position);
    const glm::vec3 nanView(std::numeric_limits<float>::quiet_NaN());
    const std::vector<glm::vec3> normals = gsv::estimateNormals(T.position, tree, std::max(3, o.normal_neighbours), nanView);
    gsv::IcpOptions io;
    io.maxIterations = std::max(1, o.max_iterations);
    io.maxCorrespondenceDistance = o.max_distance > 0.0 ? o.max_distance : 0.5;
    const gsv::IcpResult r = gsv::icpPointToPlane(src, T.position, normals, tree, gsv::GeoFrame::identity(), io);
    if (r.pairs < 6) return fail("ICP found too few correspondences (clouds too far apart?)");
    // world result = T(origin) * delta * T(-origin) * initial
    const gsv::GeoFrame result = gsv::GeoFrame::fromTranslation(T.origin) * r.transform *
                                 gsv::GeoFrame::fromTranslation(-T.origin) * init;
    fromFrame(result, *out);
    if (rms) *rms = r.rms;
    if (iterations) *iterations = r.iterations;
    return ok();
  });
}

gsv_status gsv_fit_plane(const gsv_cloud* c, double threshold, double plane[4], size_t* inliers) {
  if (!c || !plane || !(threshold > 0.0)) return fail("invalid argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    const gsv::PlaneFit f = gsv::fitPlaneRansac(c->cloud.position, threshold);
    if (!f.ok) return fail("no plane found");
    // local plane n.p + d = 0 with p = world - origin  =>  world: n.w + (d - n.origin) = 0
    for (int k = 0; k < 3; ++k) plane[k] = f.plane.normal[k];
    plane[3] = f.plane.d - glm::dot(f.plane.normal, c->cloud.origin);
    if (inliers) *inliers = f.inliers.size();
    return ok();
  });
}

// ---------------------------------------------------------------- LOD
gsv_status gsv_lod_build(const gsv_scene* s, uint32_t capacity, gsv_lod** out) {
  if (!s || !out) return fail("null argument", GSV_INVALID_ARGUMENT);
  *out = nullptr;
  return guarded([&] {
    gsv::LodBuildOptions o;
    if (capacity >= 64) o.leafCapacity = o.nodeCapacity = capacity;
    auto l = std::make_unique<gsv_lod>();
    l->tree = gsv::buildLod(s->scene, o);
    if (l->tree.empty()) return fail("empty scene");
    *out = l.release();
    return ok();
  });
}

size_t gsv_lod_node_count(const gsv_lod* l) { return l ? l->tree.nodes.size() : 0; }

gsv_status gsv_lod_cut(const gsv_lod* l, const float eye[3], float focal, float pixelError, uint64_t budget,
                       gsv_scene** out) {
  if (!l || !eye || !out) return fail("null argument", GSV_INVALID_ARGUMENT);
  *out = nullptr;
  return guarded([&] {
    gsv::LodCutParams p;
    p.cameraPosition = {eye[0], eye[1], eye[2]};
    p.focalPixels = focal;
    p.pixelError = pixelError;
    p.splatBudget = budget;
    *out = new gsv_scene{gsv::gatherCut(l->tree, gsv::selectCut(l->tree, p))};
    return ok();
  });
}

gsv_status gsv_lod_save(const gsv_lod* l, const char* path) {
  if (!l || !path) return fail("null argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    std::string err;
    return gsv::saveLod(path, l->tree, err) ? ok() : fail(err);
  });
}

gsv_status gsv_lod_load(const char* path, gsv_lod** out) {
  if (!path || !out) return fail("null argument", GSV_INVALID_ARGUMENT);
  *out = nullptr;
  return guarded([&] {
    auto l = std::make_unique<gsv_lod>();
    std::string err;
    if (!gsv::loadLod(path, l->tree, err)) return fail(err);
    *out = l.release();
    return ok();
  });
}

void gsv_lod_free(gsv_lod* l) { delete l; }

// ---------------------------------------------------------------- rendering
gsv_status gsv_renderer_create(int width, int height, gsv_renderer** out) {
  if (!out || width <= 0 || height <= 0 || width > 16384 || height > 16384)
    return fail("invalid argument", GSV_INVALID_ARGUMENT);
  *out = nullptr;
  return guarded([&] {
    std::string reason;
    if (!gsv::createHeadlessGlContext(reason)) return fail("no OpenGL 4.3 context: " + reason);
    ensureShaderDirectory();
    auto r = std::make_unique<gsv_renderer>();
    r->width = width;
    r->height = height;
    std::string log;
    if (!r->splats.init(log) || !r->cloud.init(log)) return fail("shader setup failed: " + log);
    if (!r->target.create(width, height, gsv::Framebuffer::Format::RGBA16F, {true, true}))
      return fail("framebuffer creation failed");
    r->gpuName = gsv::glRendererName();
    *out = r.release();
    return ok();
  });
}

gsv_status gsv_renderer_set_scene(gsv_renderer* r, const gsv_scene* s) {
  if (!r) return fail("null argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    r->haveScene = s && !s->scene.empty();
    if (r->haveScene) r->splats.upload(s->scene);
    return ok();
  });
}

gsv_status gsv_renderer_set_cloud(gsv_renderer* r, const gsv_cloud* c, float pointSize) {
  if (!r) return fail("null argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    r->haveCloud = c && !c->cloud.empty();
    if (r->haveCloud) r->cloud.upload(c->cloud);
    r->pointSize = pointSize > 0.0f ? pointSize : 2.0f;
    return ok();
  });
}

const char* gsv_renderer_gpu_name(const gsv_renderer* r) { return r ? r->gpuName.c_str() : ""; }

gsv_status gsv_render(gsv_renderer* r, const gsv_camera* cam, uint8_t* rgb, float* depth) {
  if (!r || !cam || !rgb) return fail("null argument", GSV_INVALID_ARGUMENT);
  return guarded([&] {
    gsv::RenderView v;
    const glm::vec3 eye(cam->eye[0], cam->eye[1], cam->eye[2]);
    const glm::vec3 target(cam->target[0], cam->target[1], cam->target[2]);
    const glm::vec3 up(cam->up[0], cam->up[1], cam->up[2]);
    if (glm::length(target - eye) <= 0.0f || !(cam->fov_y > 0.0f) || !(cam->near_plane > 0.0f) ||
        !(cam->far_plane > cam->near_plane))
      return fail("invalid camera", GSV_INVALID_ARGUMENT);
    v.view = glm::lookAt(eye, target, up);
    v.viewport = {r->width, r->height};
    v.projection = glm::perspective(cam->fov_y, v.aspect(), cam->near_plane, cam->far_plane);
    v.cameraPosition = eye;
    v.fovY = cam->fov_y;
    v.nearPlane = cam->near_plane;
    r->target.clear(glm::vec3(0.0f));
    if (r->haveCloud) {
      gsv::CloudSettings cs;
      cs.pointSize = r->pointSize;
      r->cloud.draw(v, gsv::GeoFrame::identity(), glm::dvec3(0.0), cs);
    }
    if (r->haveScene) {
      gsv::SplatSettings ss;
      ss.depthTest = r->haveCloud;
      ss.depthOutput = true;
      r->splats.render(v, ss);
    }
    const gsv::Image img = gsv::readFramebuffer(r->target.id(), r->width, r->height);
    std::memcpy(rgb, img.rgb.data(), img.rgb.size());
    r->lastDepth = gsv::readDepthOutput(r->target);
    r->lastCamera = {v.view, v.focalPixels(), v.principalPixels()};
    if (depth) std::memcpy(depth, r->lastDepth.depth.data(), r->lastDepth.depth.size() * sizeof(float));
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok();
  });
}

gsv_status gsv_pick(const gsv_renderer* r, int x, int y, float xyz[3]) {
  if (!r || !xyz) return fail("null argument", GSV_INVALID_ARGUMENT);
  const auto p = gsv::pickPoint(r->lastDepth, x, y, r->lastCamera);
  if (!p) return fail("nothing under this pixel");
  for (int k = 0; k < 3; ++k) xyz[k] = (*p)[k];
  return ok();
}

void gsv_renderer_free(gsv_renderer* r) { delete r; }

}  // extern "C"
