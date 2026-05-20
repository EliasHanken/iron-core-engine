cmake_minimum_required(VERSION 3.20)
project(IronCoreEngine LANGUAGES CXX)

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
)

# --- glad: OpenGL 3.3 core function loader ---
FetchContent_Declare(glad
  GIT_REPOSITORY https://github.com/Dav1dde/glad.git
  GIT_TAG v2.0.6
  SOURCE_SUBDIR cmake
)

FetchContent_MakeAvailable(glfw glad)
glad_add_library(glad_gl_core_33 REPRODUCIBLE API gl:core=3.3)

add_subdirectory(third_party)

add_subdirectory(engine)
add_subdirectory(games/01-spinning-cube)
add_subdirectory(tests)
