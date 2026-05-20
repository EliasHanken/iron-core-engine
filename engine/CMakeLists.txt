add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ironcore PUBLIC glfw glad_gl_core_33 stb_image)
