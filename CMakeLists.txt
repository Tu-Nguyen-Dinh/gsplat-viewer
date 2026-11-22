cmake_minimum_required(VERSION 3.24)

# Some dependencies declare very old minimum versions; CMake 4 needs this to configure them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(gsplat_viewer VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GSV_BUILD_TESTS "Build the unit / GPU test executable" ON)

# ---------------------------------------------------------------------------
# Dependencies (downloaded at configure time, no package manager needed)
# ---------------------------------------------------------------------------
# The core is also linked into a shared library (C API), so every static library is built as PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glm   GIT_REPOSITORY https://github.com/g-truc/glm.git  GIT_TAG 1.0.1   GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE)

if(ANDROID)
  # Android (NDK, tools/build_android.ps1): OpenGL ES 3.2 through EGL, no GLFW; ImGui uses its
  # Android input backend and the OpenGL ES 3 renderer.
  FetchContent_MakeAvailable(glm imgui)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
  target_link_libraries(imgui PUBLIC android log GLESv3 EGL)
else()
  FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(glfw glm imgui)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_link_libraries(imgui PUBLIC glfw)
endif()
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# zstd decoder (third_party/zstd, v1.5.7, BSD): only used to read .spz version 4.
file(GLOB GSV_ZSTD_SOURCES CONFIGURE_DEPENDS
  third_party/zstd/lib/common/*.c third_party/zstd/lib/decompress/*.c)
add_library(zstd_decoder STATIC ${GSV_ZSTD_SOURCES})
target_include_directories(zstd_decoder PUBLIC third_party/zstd/lib)
# Same defines as zstd's own build; no assembly fast path (portable C on every platform).
target_compile_definitions(zstd_decoder PRIVATE ZSTD_DISABLE_ASM XXH_NAMESPACE=ZSTD_ ZSTD_LEGACY_SUPPORT=0)
if(NOT MSVC)
  target_compile_options(zstd_decoder PRIVATE -w)  # third-party code: not held to our warning flags
endif()

# Warnings for our own targets only.
function(gsv_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# gsplat_core: everything that is not UI (math, scene I/O, OpenGL, renderers).
# Shared by the viewer and the tests.
# ---------------------------------------------------------------------------
add_library(gsplat_core STATIC
  src/core/camera.cpp
  src/core/gaussian_math.cpp
  src/core/image.cpp
  src/core/sorting.cpp
  src/core/platform.cpp
  src/core/frame_select.cpp
  src/core/inflate.cpp
  src/core/net.cpp
  src/core/geo_frame.cpp
  src/core/kdtree.cpp
  src/core/registration.cpp
  src/core/picking.cpp
  src/core/ray_pick.cpp
  src/gl/gl_loader.cpp
  src/gl/shader.cpp
  src/gl/buffer.cpp
  src/gl/framebuffer.cpp
  src/gl/gpu_timer.cpp
  src/gl/headless_context.cpp
  src/scene/ply_io.cpp
  src/scene/demo_scene.cpp
  src/scene/scene_io.cpp
  src/scene/colmap_io.cpp
  src/scene/scene_bounds.cpp
  src/scene/scene_edit.cpp
  src/scene/splat_codec.cpp
  src/scene/spz_io.cpp
  src/scene/layers.cpp
  src/scene/las_io.cpp
  src/scene/lod.cpp
  src/core/json.cpp
  src/core/settings.cpp
  src/core/i18n.cpp
  src/scene/edit_ops.cpp
  src/scene/edit_history.cpp
  src/scene/project_file.cpp
  src/render/point_renderer.cpp
  src/render/splat_renderer.cpp
  src/render/reference_renderer.cpp
  src/render/gpu_radix_sort.cpp
  src/render/cloud_renderer.cpp)

# third_party/: Khronos GL headers and stb_image_write.h (single-file, vendored).
target_include_directories(gsplat_core PUBLIC src third_party/khronos PRIVATE third_party/stb)
find_package(Threads REQUIRED)  # std::thread / std::async (needs -pthread on Linux)
target_link_libraries(gsplat_core PUBLIC glm::glm Threads::Threads ${CMAKE_DL_LIBS} PRIVATE zstd_decoder)
if(ANDROID)
  # OpenGL ES 3.2: libGLESv3 exports the functions, EGL provides the context.
  target_link_libraries(gsplat_core PUBLIC GLESv3 EGL log)
  target_compile_definitions(gsplat_core PUBLIC GSV_GLES)
else()
  target_link_libraries(gsplat_core PUBLIC glfw)
endif()
if(WIN32)
  target_link_libraries(gsplat_core PUBLIC shell32 ws2_32)  # CommandLineToArgvW (UTF-8 command line), sockets
endif()
target_compile_definitions(gsplat_core PUBLIC
  GLFW_INCLUDE_NONE
  GLM_ENABLE_EXPERIMENTAL
  # Shaders are read from the source tree so they can be edited and hot-reloaded (F5).
  GSV_VERSION="${PROJECT_VERSION}"
  GSV_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")
if(ANDROID)  # gsplat_tests_cpu runs on the phone with the data pushed next to it (adb)
  target_compile_definitions(gsplat_core PUBLIC GSV_TEST_DATA_DIR="/data/local/tmp/gsplat/tests/data")
else()
  target_compile_definitions(gsplat_core PUBLIC GSV_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
endif()
gsv_warnings(gsplat_core)

if(ANDROID)
  # The Android app (NativeActivity, android/) replaces the desktop viewer, tools and tests.
  add_subdirectory(android)
  return()
endif()

# ---------------------------------------------------------------------------
# Viewer application
# ---------------------------------------------------------------------------
add_executable(gsplat_viewer
  src/main.cpp
  src/app/app.cpp
  src/app/ui.cpp
  src/app/options.cpp
  src/app/layers_ui.cpp
  src/app/benchmark.cpp
  src/app/evaluate.cpp
  src/app/edit_ui.cpp
  src/app/tools_ui.cpp
  src/app/file_dialog.cpp
  src/app/shell_ui.cpp
  src/app/session_ui.cpp
  src/app/nav_ui.cpp
  src/app/export_ui.cpp)
target_link_libraries(gsplat_viewer PRIVATE gsplat_core imgui)
if(WIN32)
  target_link_libraries(gsplat_viewer PRIVATE comdlg32)  # native Open / Save dialogs
endif()
gsv_warnings(gsplat_viewer)

if(MINGW)
  # Single self-contained .exe: no libstdc++/libgcc DLLs needed at runtime.
  target_link_options(gsplat_viewer PRIVATE -static)
endif()

# ---------------------------------------------------------------------------
# Capture tools (phone video -> frames for Structure-from-Motion)
# ---------------------------------------------------------------------------
add_executable(gsplat_prep src/tools/prep_main.cpp)
target_link_libraries(gsplat_prep PRIVATE gsplat_core)
gsv_warnings(gsplat_prep)

# Writes the shaders with #includes expanded (desktop or ES dialect) for offline checking.
add_executable(gsplat_shaderdump src/tools/shaderdump_main.cpp)
target_link_libraries(gsplat_shaderdump PRIVATE gsplat_core)
gsv_warnings(gsplat_shaderdump)

# PC side of the phone capture workflow: upload page, training queue, scene downloads.
add_executable(gsplat_server src/tools/server_main.cpp)
target_link_libraries(gsplat_server PRIVATE gsplat_core)
gsv_warnings(gsplat_server)
if(MINGW)
  target_link_options(gsplat_prep PRIVATE -static)
  target_link_options(gsplat_server PRIVATE -static)
endif()

# ---------------------------------------------------------------------------
# Tests: `ctest --test-dir build` (GPU tests open a hidden OpenGL window)
# ---------------------------------------------------------------------------
# C API (Core 4): include/gsv/gsv.h, shared library gsplat_capi
# ---------------------------------------------------------------------------
add_library(gsplat_capi SHARED src/capi/gsv_capi.cpp)
target_include_directories(gsplat_capi PUBLIC include)
target_link_libraries(gsplat_capi PRIVATE gsplat_core)
target_compile_definitions(gsplat_capi PRIVATE GSV_CAPI_BUILD)
set_target_properties(gsplat_capi PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
gsv_warnings(gsplat_capi)
if(MINGW)
  # Self-contained DLL; only the __declspec(dllexport) gsv_* functions are exported.
  target_link_options(gsplat_capi PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
elseif(UNIX AND NOT APPLE)
  # Hide the symbols of the static libraries linked in (zstd, GLFW): only gsv_* is the ABI.
  target_link_options(gsplat_capi PRIVATE -Wl,--exclude-libs,ALL)
endif()

# ---------------------------------------------------------------------------
if(GSV_BUILD_TESTS)
  enable_testing()
  add_executable(gsplat_tests
    tests/test_main.cpp
    tests/gl_test_context.cpp
    tests/test_math.cpp
    tests/test_scene_io.cpp
    tests/test_paths.cpp
    tests/test_frame_select.cpp
    tests/test_colmap.cpp
    tests/test_image_metrics.cpp
    tests/test_scene_bounds.cpp
    tests/test_camera.cpp
    tests/test_scene_edit.cpp
    tests/test_splat_codec.cpp
    tests/test_inflate.cpp
    tests/test_spz.cpp
    tests/test_net.cpp
    tests/test_sorting.cpp
    tests/test_gpu_projection.cpp
    tests/test_gpu_render.cpp
    tests/test_gpu_sort.cpp
    tests/test_geo_frame.cpp
    tests/test_gpu_depth.cpp
    tests/test_las.cpp
    tests/test_geometry.cpp
    tests/test_lod.cpp
    tests/test_edit_ops.cpp
    tests/test_ux.cpp)
  target_link_libraries(gsplat_tests PRIVATE gsplat_core)
  target_include_directories(gsplat_tests PRIVATE tests)
  gsv_warnings(gsplat_tests)
  if(MINGW)
    target_link_options(gsplat_tests PRIVATE -static)
  endif()
  add_test(NAME cpu_tests COMMAND gsplat_tests --cpu-only)
  add_test(NAME gpu_tests COMMAND gsplat_tests --gpu-only)

  # The C API exercised from plain C99.
  enable_language(C)
  add_executable(gsplat_capi_test tests/capi/test_capi.c)
  set_target_properties(gsplat_capi_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(gsplat_capi_test PRIVATE gsplat_capi)
  if(NOT MSVC)
    target_compile_options(gsplat_capi_test PRIVATE -Wall -Wextra -pedantic)
    target_link_libraries(gsplat_capi_test PRIVATE m)
  endif()
  add_test(NAME capi_test COMMAND gsplat_capi_test ${CMAKE_BINARY_DIR})
endif()
