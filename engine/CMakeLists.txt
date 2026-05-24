add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Platform.cpp
  core/Application.cpp
  net/NetTransport.cpp
  net/backends/mock/MockTransport.cpp
  net/backends/gns/GnsTransport.cpp
  scene/Mesh.cpp
  scene/Camera.cpp
  scene/FirstPersonController.cpp
  scene/FreeFlyCamera.cpp
  ui/BitmapFont.cpp
  ui/BuiltinFont.cpp
  ui/Hud.cpp
  physics/Rope.cpp
  render/backends/opengl/GLShader.cpp
  render/backends/opengl/GLMesh.cpp
  render/backends/opengl/GLTexture.cpp
  render/backends/opengl/GLCubemap.cpp
  render/backends/opengl/GLSkybox.cpp
  render/backends/opengl/OpenGLRenderer.cpp
  render/backends/opengl/GLDebugLines.cpp
  render/backends/opengl/GLHud.cpp
  render/backends/opengl/GLShadowMap.cpp
  render/backends/opengl/GLReflectionTarget.cpp
  render/ReflectionPlane.cpp
  render/TextureLoader.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ironcore PUBLIC
  glfw
  glad_gl_core_33
  stb_image
  GameNetworkingSockets::GameNetworkingSockets)
