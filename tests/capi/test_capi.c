/* Core 4: the C API used from plain C99 (compiled with -std=c99 -pedantic). Exits non-zero on failure. */
#include "gsv/gsv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define EXPECT(cond)                                                          \
  do {                                                                        \
    if (!(cond)) {                                                            \
      printf("  FAIL line %d: %s  (last error: %s)\n", __LINE__, #cond, gsv_last_error()); \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

static void rotate_z(double angle, const double* p, double* out) {
  double c = cos(angle), s = sin(angle);
  out[0] = c * p[0] - s * p[1];
  out[1] = s * p[0] + c * p[1];
  out[2] = p[2];
}

int main(int argc, char** argv) {
  const char* tmp = argc > 1 ? argv[1] : ".";
  char path[1024];
  printf("gsv %s, C API v%d\n", gsv_version(), gsv_capi_version());
  EXPECT(gsv_capi_version() == GSV_CAPI_VERSION);

  /* --- errors are reported, not crashes */
  {
    gsv_scene* s = NULL;
    EXPECT(gsv_scene_load("/definitely/missing.ply", &s) == GSV_ERROR && s == NULL);
    EXPECT(strlen(gsv_last_error()) > 0);
    EXPECT(gsv_scene_load(NULL, &s) == GSV_INVALID_ARGUMENT);
  }

  /* --- scenes and LOD */
  {
    gsv_scene* s = gsv_scene_create_demo(20000, 3);
    gsv_lod* lod = NULL;
    gsv_scene* cut = NULL;
    float mn[3], mx[3], eye[3];
    EXPECT(s != NULL && gsv_scene_size(s) == 20000);
    gsv_scene_bounds(s, mn, mx);
    EXPECT(mx[0] > mn[0]);
    EXPECT(gsv_lod_build(s, 1000, &lod) == GSV_OK && gsv_lod_node_count(lod) > 4);
    eye[0] = 0.0f; eye[1] = 0.0f; eye[2] = 50.0f * (mx[2] - mn[2]);
    EXPECT(gsv_lod_cut(lod, eye, 1000.0f, 2.0f, 5000, &cut) == GSV_OK);
    EXPECT(gsv_scene_size(cut) > 0 && gsv_scene_size(cut) <= 5000);
    snprintf(path, sizeof path, "%s/capi_test.gsvlod", tmp);
    EXPECT(gsv_lod_save(lod, path) == GSV_OK);
    gsv_lod_free(lod);
    lod = NULL;
    EXPECT(gsv_lod_load(path, &lod) == GSV_OK && gsv_lod_node_count(lod) > 4);
    remove(path);
    snprintf(path, sizeof path, "%s/capi_test.ply", tmp);
    EXPECT(gsv_scene_save_ply(cut, path) == GSV_OK);
    gsv_scene_free(cut);
    cut = NULL;
    EXPECT(gsv_scene_load(path, &cut) == GSV_OK && gsv_scene_size(cut) > 0);
    remove(path);
    gsv_scene_free(cut);
    gsv_lod_free(lod);
    gsv_scene_free(s);
  }

  /* --- similarity from point pairs */
  {
    double src[3 * 10], dst[3 * 10];
    gsv_frame f;
    int i;
    for (i = 0; i < 10; ++i) {
      double p[3], q[3];
      p[0] = i * 0.7; p[1] = (i * i) % 7 - 3.0; p[2] = (i % 3) * 1.5;
      rotate_z(0.5, p, q);
      memcpy(src + 3 * i, p, sizeof p);
      dst[3 * i] = 2.0 * q[0] + 600000.0;
      dst[3 * i + 1] = 2.0 * q[1] + 1200000.0;
      dst[3 * i + 2] = 2.0 * q[2] + 10.0;
    }
    EXPECT(gsv_estimate_similarity(src, dst, 10, 1, &f) == GSV_OK);
    EXPECT(fabs(f.scale - 2.0) < 1e-9);
    EXPECT(fabs(f.rotation[0] - cos(0.5)) < 1e-9 && fabs(f.rotation[3] - sin(0.5)) < 1e-9);  /* row-major */
    EXPECT(fabs(f.translation[0] - 600000.0) < 1e-6);
    EXPECT(gsv_estimate_similarity(src, dst, 2, 1, &f) == GSV_ERROR);
  }

  /* --- point clouds: LAS round trip in survey coordinates, plane fit, ICP */
  {
    enum { N = 4000 };
    static float xyz[3 * N], moved[3 * N];
    double origin[3] = {585000.0, 1186000.0, 5.0};
    gsv_cloud *a, *b, *back = NULL;
    double plane[4], rms = 0.0, o2[3];
    size_t inliers = 0;
    gsv_frame init, out;
    int i, iterations = 0;
    unsigned seed = 1;
    for (i = 0; i < N; ++i) {  /* an L-shaped corner: floor + two walls */
      float u, v;
      seed = seed * 1664525u + 1013904223u; u = (seed >> 8) / 16777216.0f;
      seed = seed * 1664525u + 1013904223u; v = (seed >> 8) / 16777216.0f;
      if (i % 3 == 0) { xyz[3 * i] = 4 * u; xyz[3 * i + 1] = 3 * v; xyz[3 * i + 2] = 0; }
      else if (i % 3 == 1) { xyz[3 * i] = 4 * u; xyz[3 * i + 1] = 0; xyz[3 * i + 2] = 2.5f * v; }
      else { xyz[3 * i] = 0; xyz[3 * i + 1] = 3 * v; xyz[3 * i + 2] = 2.5f * u; }
      moved[3 * i] = xyz[3 * i] + 0.05f;  /* second scan shifted by 5 cm in x and 3 cm in z */
      moved[3 * i + 1] = xyz[3 * i + 1];
      moved[3 * i + 2] = xyz[3 * i + 2] + 0.03f;
    }
    a = gsv_cloud_create(origin, xyz, NULL, N);
    b = gsv_cloud_create(origin, moved, NULL, N);
    EXPECT(a && b && gsv_cloud_size(a) == N);
    snprintf(path, sizeof path, "%s/capi_test.las", tmp);
    EXPECT(gsv_cloud_save_las(a, path, 0.001) == GSV_OK);
    EXPECT(gsv_cloud_load_las(path, &back) == GSV_OK && gsv_cloud_size(back) == N);
    gsv_cloud_origin(back, o2);
    EXPECT(fabs(o2[0] - 585000.0) < 100.0 && fabs(o2[1] - 1186000.0) < 100.0);
    remove(path);
    EXPECT(gsv_fit_plane(a, 0.01, plane, &inliers) == GSV_OK);
    EXPECT(inliers >= N / 3 - 10);
    gsv_frame_identity(&init);
    EXPECT(gsv_icp(b, a, &init, NULL, &out, &rms, &iterations) == GSV_OK);
    printf("  ICP: %d iterations, rms %.5f, translation (%.4f, %.4f, %.4f)\n", iterations, rms, out.translation[0],
           out.translation[1], out.translation[2]);
    EXPECT(fabs(out.translation[0] + 0.05) < 2e-3 && fabs(out.translation[2] + 0.03) < 2e-3);
    gsv_cloud_free(a);
    gsv_cloud_free(b);
    gsv_cloud_free(back);
  }

  /* --- offscreen rendering + picking (skipped without an OpenGL 4.3 context) */
  {
    gsv_renderer* r = NULL;
    if (gsv_renderer_create(160, 120, &r) != GSV_OK) {
      printf("  rendering skipped: %s\n", gsv_last_error());
    } else {
      gsv_scene* s = gsv_scene_create_demo(20000, 5);
      unsigned char* rgb = (unsigned char*)malloc(160 * 120 * 3);
      float* depth = (float*)malloc(160 * 120 * sizeof(float));
      gsv_camera cam;
      float c[3], radius, xyz3[3];
      long sum = 0;
      int i, covered = 0;
      printf("  GPU: %s\n", gsv_renderer_gpu_name(r));
      gsv_scene_focus(s, c, &radius);
      memcpy(cam.target, c, sizeof c);
      cam.eye[0] = c[0]; cam.eye[1] = c[1] + 0.3f * radius; cam.eye[2] = c[2] + 2.5f * radius;
      cam.up[0] = 0.0f; cam.up[1] = 1.0f; cam.up[2] = 0.0f;
      cam.fov_y = 0.9f; cam.near_plane = 0.01f; cam.far_plane = 1000.0f;
      EXPECT(gsv_renderer_set_scene(r, s) == GSV_OK);
      EXPECT(gsv_render(r, &cam, rgb, depth) == GSV_OK);
      for (i = 0; i < 160 * 120 * 3; ++i) sum += rgb[i];
      for (i = 0; i < 160 * 120; ++i) covered += depth[i] > 0.0f;
      printf("  image mean %.1f, %d pixels with depth\n", sum / (160.0 * 120.0 * 3.0), covered);
      EXPECT(sum > 160 * 120 * 3 * 5);   /* not black */
      EXPECT(covered > 160 * 120 / 10);
      EXPECT(gsv_pick(r, 80, 60, xyz3) == GSV_OK);
      EXPECT(gsv_pick(r, -1, 60, xyz3) == GSV_ERROR);
      /* an opaque cloud wall in front of the scene hides it: depth at the centre becomes the wall's */
      {
        enum { M = 120 * 120 };
        static float wall[3 * M];
        double origin0[3] = {0.0, 0.0, 0.0};
        float wallZ = c[2] + 1.2f * radius, centreDepth;
        gsv_cloud* w;
        int k;
        for (k = 0; k < M; ++k) {
          wall[3 * k] = c[0] + ((k % 120) - 60) * radius / 25.0f;
          wall[3 * k + 1] = c[1] + ((k / 120) - 60) * radius / 25.0f;
          wall[3 * k + 2] = wallZ;
        }
        w = gsv_cloud_create(origin0, wall, NULL, M);
        EXPECT(gsv_renderer_set_cloud(r, w, 8.0f) == GSV_OK);
        EXPECT(gsv_render(r, &cam, rgb, depth) == GSV_OK);
        centreDepth = depth[60 * 160 + 80];
        printf("  with a point wall in front: centre depth %.3f (wall at %.3f)\n", centreDepth, cam.eye[2] - wallZ);
        EXPECT(fabs(centreDepth - (cam.eye[2] - wallZ)) < 0.05f * (cam.eye[2] - wallZ));
        EXPECT(gsv_renderer_set_cloud(r, NULL, 0.0f) == GSV_OK);
        gsv_cloud_free(w);
      }
      cam.near_plane = 0.0f;
      EXPECT(gsv_render(r, &cam, rgb, NULL) == GSV_INVALID_ARGUMENT);
      free(rgb);
      free(depth);
      gsv_scene_free(s);
      gsv_renderer_free(r);
    }
  }

  printf(failures ? "C API test: %d failure(s)\n" : "C API test: all checks passed\n", failures);
  return failures ? 1 : 0;
}
