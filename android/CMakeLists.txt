# Android app: one shared library loaded by the system NativeActivity (no Java/Kotlin code).
# Built by tools/build_android.ps1 with the NDK toolchain, then packaged with aapt2.

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_options(native_app_glue PRIVATE -w)

add_library(gsplat_android SHARED app_main.cpp)
target_link_libraries(gsplat_android PRIVATE gsplat_core imgui native_app_glue android EGL GLESv3 log)
# NativeActivity looks up ANativeActivity_onCreate (defined in the glue): keep it exported.
target_link_options(gsplat_android PRIVATE -u ANativeActivity_onCreate)
gsv_warnings(gsplat_android)

# CPU unit tests as a plain arm64 executable, run on the phone over adb:
#   adb push build-android/android/gsplat_tests_cpu /data/local/tmp/gsplat/
#   adb push tests/data /data/local/tmp/gsplat/tests/
#   adb shell "cd /data/local/tmp/gsplat && ./gsplat_tests_cpu --cpu-only"
set(GSV_TESTS ${CMAKE_SOURCE_DIR}/tests)
add_executable(gsplat_tests_cpu
  ${GSV_TESTS}/test_main.cpp
  no_gl_test_context.cpp
  ${GSV_TESTS}/test_math.cpp
  ${GSV_TESTS}/test_scene_io.cpp
  ${GSV_TESTS}/test_paths.cpp
  ${GSV_TESTS}/test_frame_select.cpp
  ${GSV_TESTS}/test_colmap.cpp
  ${GSV_TESTS}/test_image_metrics.cpp
  ${GSV_TESTS}/test_scene_bounds.cpp
  ${GSV_TESTS}/test_camera.cpp
  ${GSV_TESTS}/test_scene_edit.cpp
  ${GSV_TESTS}/test_splat_codec.cpp
  ${GSV_TESTS}/test_inflate.cpp
  ${GSV_TESTS}/test_spz.cpp
  ${GSV_TESTS}/test_net.cpp
  ${GSV_TESTS}/test_sorting.cpp)
target_link_libraries(gsplat_tests_cpu PRIVATE gsplat_core)
target_include_directories(gsplat_tests_cpu PRIVATE ${GSV_TESTS})
gsv_warnings(gsplat_tests_cpu)
