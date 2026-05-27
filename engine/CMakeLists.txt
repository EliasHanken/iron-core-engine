add_library(ironcore STATIC
  core/Log.cpp
  core/Window.cpp
  core/Input.cpp
  core/Platform.cpp
  core/Application.cpp
  core/NetArgs.cpp
  net/NetTransport.cpp
  net/MessageRegistry.cpp
  net/PeerManager.cpp
  net/ClockSync.cpp
  net/LagCompensator.cpp
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
  physics/PhysicsWorld.cpp
  physics/Ragdoll.cpp
  physics/CharacterController.cpp
  asset/GltfLoader.cpp
  game/Collision.cpp
  render/RendererFactory.cpp
  render/ReflectionPlane.cpp
  render/TextureLoader.cpp
  debug/GizmoRegistry.cpp
)

target_include_directories(ironcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ironcore PUBLIC
  glfw
  stb_image
  GameNetworkingSockets::GameNetworkingSockets)

target_link_libraries(ironcore PRIVATE
  Jolt::Jolt)

# tinygltf is header-only; just expose its include directory privately.
target_include_directories(ironcore PRIVATE ${TINYGLTF_INCLUDE_DIRS})

if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    target_sources(ironcore PRIVATE
      render/backends/vulkan/VulkanRenderer.cpp
      render/backends/vulkan/VkContext.cpp
      render/backends/vulkan/VkSwapchain.cpp
      render/backends/vulkan/VkFrameRing.cpp
      render/backends/vulkan/VkShader.cpp
      render/backends/vulkan/VkPipeline.cpp
      render/backends/vulkan/VkMesh.cpp
      render/backends/vulkan/VkSkinnedMesh.cpp
      render/backends/vulkan/VkTexture.cpp
      render/backends/vulkan/VkComputePipeline.cpp
      render/backends/vulkan/VkParticleSystem.cpp
      render/backends/vulkan/VkDebugLines.cpp
      render/backends/vulkan/VkHud.cpp
      render/backends/vulkan/VkShadowMap.cpp
      render/backends/vulkan/VkCubemap.cpp
      render/backends/vulkan/VkSkybox.cpp
      render/backends/vulkan/VkReflectionTarget.cpp
    )
    target_compile_definitions(ironcore PUBLIC IRON_RENDER_BACKEND_VULKAN)
    target_link_libraries(ironcore PUBLIC
      Vulkan::Vulkan
      GPUOpen::VulkanMemoryAllocator
      glslang::glslang
      glslang::SPIRV
      glslang::glslang-default-resource-limits)
else()
    target_sources(ironcore PRIVATE
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
    )
    target_compile_definitions(ironcore PUBLIC IRON_RENDER_BACKEND_OPENGL)
    target_link_libraries(ironcore PUBLIC glad_gl_core_33)
endif()
