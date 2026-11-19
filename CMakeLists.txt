cmake_minimum_required(VERSION 3.24)

# Some dependencies declare very old minimum versions; CMake 4 needs this to configure them.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(gsplat_viewer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

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

# ---------------------------------------------------------------------------
# Viewer
# ---------------------------------------------------------------------------
add_executable(gsplat_viewer
  src/main.cpp
  src/camera.cpp
  src/gl/gl_loader.cpp
  src/gl/shader.cpp
  src/gl/gpu_timer.cpp
  src/gl/screenshot.cpp
  src/scene/ply_io.cpp
  src/scene/demo_scene.cpp
  src/render/point_renderer.cpp)

# third_party/: Khronos GL headers and stb_image_write.h (single-file, vendored).
target_include_directories(gsplat_viewer PRIVATE src third_party/khronos third_party/stb)
target_link_libraries(gsplat_viewer PRIVATE glfw glm::glm imgui)
target_compile_definitions(gsplat_viewer PRIVATE
  GLFW_INCLUDE_NONE
  GLM_ENABLE_EXPERIMENTAL
  # Shaders are read from the source tree so they can be edited and hot-reloaded (F5).
  GSV_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")

if(MSVC)
  target_compile_options(gsplat_viewer PRIVATE /W4 /utf-8)
else()
  target_compile_options(gsplat_viewer PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
  # Single self-contained .exe: no libstdc++/libgcc DLLs needed at runtime.
  target_link_options(gsplat_viewer PRIVATE -static)
endif()
