add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Platform.cpp
  core/Application.cpp
  scene/Mesh.cpp
  scene/Camera.cpp
  render/backends/opengl/GLShader.cpp
  render/backends/opengl/GLMesh.cpp
  render/backends/opengl/GLTexture.cpp
  render/backends/opengl/OpenGLRenderer.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ironcore PUBLIC glfw glad_gl_core_33 stb_image)
