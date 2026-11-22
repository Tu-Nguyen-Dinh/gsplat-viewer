/* gsplat-viewer core as a C library (Core 4). See docs/core/CORE4-capi.md.
 *
 * Plain C99: opaque handles, explicit create/free, status codes, no C++ types. Usable from C, C#
 * (P/Invoke), Python (ctypes/cffi), Java/Kotlin (JNI), Unity and Unreal native plugins.
 *
 * Conventions
 *   - Strings are UTF-8. Arrays are tightly packed; positions are x,y,z triples.
 *   - Functions returning gsv_status set a per-thread message readable with gsv_last_error().
 *   - Handles are not thread-safe; use one handle per thread or lock around calls.
 *   - Rendering functions must be called from the thread that created the renderer (OpenGL).
 *   - Coordinates: point clouds keep a double-precision origin and float offsets (survey
 *     coordinates keep millimetres). Frames map source -> destination: p' = s * R * p + t.
 */
#ifndef GSV_GSV_H
#define GSV_GSV_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(GSV_CAPI_BUILD)
#define GSV_API __declspec(dllexport)
#else
#define GSV_API __declspec(dllimport)
#endif
#else
#define GSV_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GSV_CAPI_VERSION 1

typedef enum gsv_status { GSV_OK = 0, GSV_ERROR = 1, GSV_INVALID_ARGUMENT = 2 } gsv_status;

typedef struct gsv_scene gsv_scene;       /* Gaussian splats (.ply, .splat, .spz) */
typedef struct gsv_cloud gsv_cloud;       /* point cloud (LAS 1.2-1.4) */
typedef struct gsv_lod gsv_lod;           /* level-of-detail tree of a scene */
typedef struct gsv_renderer gsv_renderer; /* offscreen renderer with its own OpenGL context */

/* p' = scale * R * p + t, R row-major (rotation[3*row + col]). */
typedef struct gsv_frame {
  double scale;
  double rotation[9];
  double translation[3];
} gsv_frame;

typedef struct gsv_camera {
  float eye[3];
  float target[3];
  float up[3];
  float fov_y;       /* radians */
  float near_plane;
  float far_plane;
} gsv_camera;

/* ---------------------------------------------------------------- library */
GSV_API int gsv_capi_version(void);            /* GSV_CAPI_VERSION of the binary */
GSV_API const char* gsv_version(void);         /* project version, e.g. "1.1.0" */
GSV_API const char* gsv_last_error(void);      /* message of the last failure on this thread ("" if none) */
GSV_API void gsv_frame_identity(gsv_frame* out);
/* Folder with the GLSL shaders (default: next to the library, then the source tree). */
GSV_API void gsv_set_shader_dir(const char* path_utf8);

/* ---------------------------------------------------------------- splat scenes */
GSV_API gsv_status gsv_scene_load(const char* path_utf8, gsv_scene** out);
GSV_API gsv_scene* gsv_scene_create_demo(uint32_t count, uint32_t seed);
GSV_API gsv_status gsv_scene_save_ply(const gsv_scene* scene, const char* path_utf8);
GSV_API size_t gsv_scene_size(const gsv_scene* scene);
GSV_API int gsv_scene_sh_degree(const gsv_scene* scene);
/* Robust bounds (floaters ignored) and the dense subject (centre + radius) the viewer frames. */
GSV_API void gsv_scene_bounds(const gsv_scene* scene, float min_out[3], float max_out[3]);
GSV_API void gsv_scene_focus(const gsv_scene* scene, float center_out[3], float* radius_out);
GSV_API void gsv_scene_free(gsv_scene* scene);

/* ---------------------------------------------------------------- point clouds */
GSV_API gsv_status gsv_cloud_load_las(const char* path_utf8, gsv_cloud** out);
GSV_API gsv_status gsv_cloud_save_las(const gsv_cloud* cloud, const char* path_utf8, double scale);
/* Copies `count` float offsets (x,y,z) around `origin`. rgb may be NULL (else 3 bytes per point). */
GSV_API gsv_cloud* gsv_cloud_create(const double origin[3], const float* xyz, const uint8_t* rgb, size_t count);
GSV_API size_t gsv_cloud_size(const gsv_cloud* cloud);
GSV_API void gsv_cloud_origin(const gsv_cloud* cloud, double origin_out[3]);
GSV_API const float* gsv_cloud_positions(const gsv_cloud* cloud);   /* 3 * size floats, owned by the cloud */
GSV_API void gsv_cloud_free(gsv_cloud* cloud);

/* ---------------------------------------------------------------- geometry */
/* Least squares dst ~ s R src + t from n >= 3 pairs (x,y,z doubles). with_scale = 0: rigid. */
GSV_API gsv_status gsv_estimate_similarity(const double* src_xyz, const double* dst_xyz, size_t n, int with_scale,
                                           gsv_frame* out);
/* Same with outlier pairs (RANSAC); inliers_out may be NULL. */
GSV_API gsv_status gsv_estimate_similarity_ransac(const double* src_xyz, const double* dst_xyz, size_t n,
                                                  int with_scale, double inlier_threshold, gsv_frame* out,
                                                  size_t* inliers_out);
typedef struct gsv_icp_options {
  int max_iterations;         /* 50 */
  double max_distance;        /* metres; pairs farther apart are ignored (0.5) */
  int normal_neighbours;      /* points used for target normals (30) */
} gsv_icp_options;
GSV_API void gsv_icp_default_options(gsv_icp_options* out);
/* Rigid point-to-plane ICP: refines `initial` (source world -> target world). The world frames of the
 * two clouds include their origins. rms_out and iterations_out may be NULL. */
GSV_API gsv_status gsv_icp(const gsv_cloud* source, const gsv_cloud* target, const gsv_frame* initial,
                           const gsv_icp_options* options, gsv_frame* out, double* rms_out, int* iterations_out);
/* Dominant plane (RANSAC + least squares) in world coordinates: a x + b y + c z + d = 0, |(a,b,c)| = 1. */
GSV_API gsv_status gsv_fit_plane(const gsv_cloud* cloud, double threshold, double plane_out[4], size_t* inliers_out);

/* ---------------------------------------------------------------- level of detail */
GSV_API gsv_status gsv_lod_build(const gsv_scene* scene, uint32_t node_capacity, gsv_lod** out);
GSV_API size_t gsv_lod_node_count(const gsv_lod* lod);
/* Splats to draw for a camera at `eye` (scene coordinates): refine until nodes are below
 * pixel_error pixels or the budget is reached. Returns a new scene to free with gsv_scene_free. */
GSV_API gsv_status gsv_lod_cut(const gsv_lod* lod, const float eye[3], float focal_pixels, float pixel_error,
                               uint64_t splat_budget, gsv_scene** out);
GSV_API gsv_status gsv_lod_save(const gsv_lod* lod, const char* path_utf8);
GSV_API gsv_status gsv_lod_load(const char* path_utf8, gsv_lod** out);
GSV_API void gsv_lod_free(gsv_lod* lod);

/* ---------------------------------------------------------------- offscreen rendering */
/* Creates an OpenGL 4.3 context without a window (hidden GLFW window, or EGL surfaceless on Linux
 * servers) and a width x height render target. One renderer per process. */
GSV_API gsv_status gsv_renderer_create(int width, int height, gsv_renderer** out);
/* The renderer keeps its own GPU copy: the scene/cloud may be freed afterwards. NULL clears. */
GSV_API gsv_status gsv_renderer_set_scene(gsv_renderer* r, const gsv_scene* scene);
GSV_API gsv_status gsv_renderer_set_cloud(gsv_renderer* r, const gsv_cloud* cloud, float point_size_pixels);
GSV_API const char* gsv_renderer_gpu_name(const gsv_renderer* r);
/* Renders points (opaque, depth-tested) then splats. Coordinates are relative to the cloud's
 * origin when a cloud is set (splats are assumed to be in that frame), else scene coordinates.
 * rgb_out: width*height*3 bytes, rows top-down. depth_out (may be NULL): width*height floats,
 * expected view depth, 0 where nothing was drawn. */
GSV_API gsv_status gsv_render(gsv_renderer* r, const gsv_camera* camera, uint8_t* rgb_out, float* depth_out);
/* 3D point under pixel (x, y from the top) of the last gsv_render, or GSV_ERROR if empty there. */
GSV_API gsv_status gsv_pick(const gsv_renderer* r, int x, int y, float xyz_out[3]);
GSV_API void gsv_renderer_free(gsv_renderer* r);

#ifdef __cplusplus
}
#endif

#endif /* GSV_GSV_H */
