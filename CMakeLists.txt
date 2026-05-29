cmake_minimum_required(VERSION 3.20)
project(IronCoreEngine LANGUAGES C CXX)

# Force every target (including FetchContent dependencies) onto the dynamic CRT
# so they all agree — avoids LNK4098 (LIBCMT vs MSVCRT mismatch).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
endif()

enable_testing()

include(FetchContent)

# --- GLFW: windowing + input ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE
)

# --- glad: OpenGL 3.3 core function loader ---
FetchContent_Declare(glad
  GIT_REPOSITORY https://github.com/Dav1dde/glad.git
  GIT_TAG v2.0.6
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR cmake
)

FetchContent_MakeAvailable(glfw glad)
glad_add_library(glad_gl_core_33 REPRODUCIBLE API gl:core=3.3)

# glad's cmake/CMakeLists.txt declares cmake_minimum_required(3.12), which
# predates CMP0091; the generated vcxproj writes an empty <RuntimeLibrary/>
# tag that blocks both CMAKE_MSVC_RUNTIME_LIBRARY and MSVC_RUNTIME_LIBRARY
# from taking effect.  Patch the target directly with explicit CL flags so
# glad is compiled with the dynamic CRT, matching every other target.
if(MSVC)
  target_compile_options(glad_gl_core_33 PRIVATE
    $<$<CONFIG:Debug>:/MDd>
    $<$<NOT:$<CONFIG:Debug>>:/MD>
  )
endif()

# --- GameNetworkingSockets: UDP transport with reliable channels, encryption,
#     congestion control. Brought in via vcpkg manifest mode; see
#     docs/engine/build-setup.md for the one-time vcpkg toolchain setup.
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(Jolt CONFIG REQUIRED)

# tinygltf is header-only on vcpkg and does not ship a CMake config package.
# Locate its include dir via find_path; engine/CMakeLists.txt adds it to the
# ironcore target as a PRIVATE include directory (only GltfLoader.cpp needs it).
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h" REQUIRED)

set(IRON_RENDER_BACKEND "vulkan" CACHE STRING
    "Render backend: vulkan (default) or opengl (deprecated, frozen)")
set_property(CACHE IRON_RENDER_BACKEND PROPERTY STRINGS vulkan opengl)

if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(glslang CONFIG REQUIRED)
    message(STATUS "Iron: building with Vulkan backend (default)")
else()
    message(STATUS "Iron: building with OpenGL backend (deprecated)")
endif()

add_subdirectory(third_party)

add_subdirectory(engine)
add_subdirectory(games/01-spinning-cube)
add_subdirectory(games/02-strandbound)
add_subdirectory(games/03-showcase)
add_subdirectory(games/04-net-pingpong)
add_subdirectory(games/05-net-cubes)
add_subdirectory(games/06-net-tag)
add_subdirectory(games/07-net-shooter)
add_subdirectory(games/08-particle-storm)
add_subdirectory(games/09-physics-playground)
add_subdirectory(games/10-gltf-viewer)
add_subdirectory(games/11-sandbox)
add_subdirectory(tests)
