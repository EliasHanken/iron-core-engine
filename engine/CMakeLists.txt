add_library(ironcore STATIC
  core/engine_placeholder.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ironcore PUBLIC glfw glad_gl_core_33 stb_image)
