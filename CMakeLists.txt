cmake_minimum_required(VERSION 3.24)

# Some dependencies declare very old minimum versions; CMake 4 needs this to configure them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(gsplat_viewer VERSION 1.1.0 LANGUAGES C CXX)

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
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw  GIT_REPOSITORY https://github.com/glfw/glfw.git   GIT_TAG 3.4     GIT_SHALLOW TRUE)
FetchContent_Declare(glm   GIT_REPOSITORY https://github.com/g-truc/glm.git  GIT_TAG 1.0.1   GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9b GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw glm imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

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
  src/gl/gl_loader.cpp
  src/gl/shader.cpp
  src/gl/buffer.cpp
  src/gl/framebuffer.cpp
  src/gl/gpu_timer.cpp
  src/scene/ply_io.cpp
  src/scene/demo_scene.cpp
  src/scene/scene_io.cpp
  src/scene/colmap_io.cpp
  src/scene/scene_bounds.cpp
  src/scene/scene_edit.cpp
  src/scene/splat_codec.cpp
  src/scene/spz_io.cpp
  src/render/point_renderer.cpp
  src/render/splat_renderer.cpp
  src/render/reference_renderer.cpp
  src/render/gpu_radix_sort.cpp)

# third_party/: Khronos GL headers and stb_image_write.h (single-file, vendored).
target_include_directories(gsplat_core PUBLIC src third_party/khronos PRIVATE third_party/stb)
find_package(Threads REQUIRED)  # std::thread / std::async (needs -pthread on Linux)
target_link_libraries(gsplat_core PUBLIC glfw glm::glm Threads::Threads PRIVATE zstd_decoder)
if(WIN32)
  target_link_libraries(gsplat_core PUBLIC shell32)  # CommandLineToArgvW (UTF-8 command line)
endif()
target_compile_definitions(gsplat_core PUBLIC
  GLFW_INCLUDE_NONE
  GLM_ENABLE_EXPERIMENTAL
  # Shaders are read from the source tree so they can be edited and hot-reloaded (F5).
  GSV_VERSION="${PROJECT_VERSION}"
  GSV_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
  GSV_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data")
gsv_warnings(gsplat_core)

# ---------------------------------------------------------------------------
# Viewer application
# ---------------------------------------------------------------------------
add_executable(gsplat_viewer
  src/main.cpp
  src/app/app.cpp
  src/app/ui.cpp
  src/app/options.cpp
  src/app/benchmark.cpp
  src/app/evaluate.cpp
  src/app/edit_ui.cpp)
target_link_libraries(gsplat_viewer PRIVATE gsplat_core imgui)
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
if(MINGW)
  target_link_options(gsplat_prep PRIVATE -static)
endif()

# ---------------------------------------------------------------------------
# Tests: `ctest --test-dir build` (GPU tests open a hidden OpenGL window)
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
    tests/test_sorting.cpp
    tests/test_gpu_projection.cpp
    tests/test_gpu_render.cpp
    tests/test_gpu_sort.cpp)
  target_link_libraries(gsplat_tests PRIVATE gsplat_core)
  target_include_directories(gsplat_tests PRIVATE tests)
  gsv_warnings(gsplat_tests)
  if(MINGW)
    target_link_options(gsplat_tests PRIVATE -static)
  endif()
  add_test(NAME cpu_tests COMMAND gsplat_tests --cpu-only)
  add_test(NAME gpu_tests COMMAND gsplat_tests --gpu-only)
endif()
