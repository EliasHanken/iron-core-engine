# The RHI: a graphics-API-agnostic renderer

The engine should not be welded to OpenGL. The **Render Hardware Interface**
(`iron::Renderer`) is an abstract class — a contract — that game and scene code
talks to. Concrete backends implement it.

## The contract

`Renderer` exposes only what a game needs:

- create resources: `createMesh`, `createTexture`, `loadTexture`,
  `createShader`
- per frame: `beginFrame`, `submit`, `endFrame`
- `setViewport` on resize

Resources are referred to by opaque **handles** (`MeshHandle`, etc.), not raw
GL ids. Game code never sees an OpenGL type.

## Why one backend first

The interface only earns its keep once a real backend implements it. We build
`OpenGLRenderer` first and ship the spinning cube. A second backend (Vulkan, or
a software rasterizer) comes later — and *that* is when the interface gets
truly tested: anything OpenGL-specific that leaked into the interface will show
up as friction. Designing two backends at once would mean debugging two things
before learning the pipeline once.

## Draw submission

A `DrawCall` bundles a mesh, shader, texture, and model matrix. `submit` also
takes the camera's view and projection. The renderer is free to batch or
reorder calls between `beginFrame` and `endFrame`.

Related: [[render-pipeline]], [[transforms-and-projection]]

## Rendering backends

The engine ships two parallel render backends behind the abstract
`iron::Renderer` interface:

- **OpenGL 3.3** (`engine/render/backends/opengl/`) — the default. Full
  feature set: shadow map, cubemap skybox, planar reflection, HUD,
  debug lines.
- **Vulkan 1.3** (`engine/render/backends/vulkan/`) — foundation only as
  of M9. Implements mesh / texture / shader / single render pass.
  Cubemap, skybox, shadow, reflection, debug-lines, and HUD methods log a
  one-time warning and return safely. Subsequent milestones port each
  feature.

Selection is build-time:

```
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl    # default
cmake -S . -B build -DIRON_RENDER_BACKEND=vulkan
```

Only the chosen backend is compiled and linked. Game code calls
`iron::createRenderer(window)` (from `render/RendererFactory.h`); the
factory returns a `unique_ptr<Renderer>` whose concrete type is
selected by the `IRON_RENDER_BACKEND_VULKAN` or `_OPENGL` build define.
Game code never names a concrete renderer directly.

### Vulkan dependencies

vcpkg manifests fetch `vulkan-headers`, `vulkan-loader`, `glslang`, and
`vulkan-memory-allocator`. No external Vulkan SDK install required.
Validation layers are enabled in Debug only (loaded if the
`VK_LAYER_KHRONOS_validation` layer is available in the system Vulkan
loader); their messages are routed to `iron::Log::error` /
`iron::Log::warn` via a debug-utils messenger.

### Shader compilation

Game code passes GLSL strings to `Renderer::createShader(vert, frag)`.
Under Vulkan, those are compiled to SPIR-V at runtime via glslang
(GLSL 450, Vulkan 1.3 client, SPV 1.5 target). Note that GL 3.3 cannot
reliably compile `#version 450` shaders, and the OpenGL backend's
per-uniform style (`glGetUniformLocation`/`glUniformMatrix4fv`) is
incompatible with Vulkan's UBO-block + descriptor-set model — so games
that target both backends keep two parallel shader sources behind an
`#ifdef IRON_RENDER_BACKEND_VULKAN` selector. Spinning-cube is the
reference: see `games/01-spinning-cube/main.cpp`.

### Window setup

`engine/core/Window.cpp` conditionally compiles its GLFW + glad
initialization: OpenGL builds create a GL 3.3 core context and load
glad; Vulkan builds use `GLFW_CLIENT_API = GLFW_NO_API`, skip glad,
and let the renderer create the `VkSurfaceKHR` via
`glfwCreateWindowSurface`. `Window::swapBuffers()` is a no-op under
Vulkan — `vkQueuePresentKHR` inside `Renderer::endFrame` drives
presentation.

### Current Vulkan game support

| Game                  | Vulkan      |
| --------------------- | ----------- |
| 01-spinning-cube      | ✅ supported |
| 02-strandbound        | ❌ requires shadows + cubemap |
| 03-showcase           | ❌ requires shadows + reflections |
| 04-net-pingpong       | ❌ (not a graphical game; could port if useful) |
| 05-net-cubes          | ❌ requires HUD + debug-lines |
| 06-net-tag            | ❌ requires HUD |
| 07-net-shooter        | ❌ requires HUD + debug-lines |

Each game's `CMakeLists.txt` is wrapped in
`if (IRON_RENDER_BACKEND STREQUAL "opengl")` so the Vulkan configure
only attempts to build games that have actually ported. Future
milestones flip the table entries to ✅ one at a time.

### Vulkan backend architecture

The backend mirrors the OpenGL split with one file per concern:

- `VulkanRenderer` — implements `iron::Renderer`; orchestrates the rest.
- `VkContext` — instance, surface, physical+logical device, queues, VMA
  allocator; lives for the app.
- `VkSwapchain` — surface format negotiation (B8G8R8A8_SRGB), present
  mode (MAILBOX→FIFO), color image views + shared D32_SFLOAT depth.
  Recreates on resize.
- `VkFrameRing` — 2 frames in flight; per-frame transient command pool,
  primary command buffer, image-available + render-finished
  semaphores, in-flight fence, descriptor pool, and host-mapped 256 KB
  UBO buffer used as a linear sub-allocator.
- `VkShader` — `compileGlsl(stage, src)` drives glslang → SPIR-V →
  `vkCreateShaderModule`; hardcoded descriptor set layout
  (set=0 binding=0=UBO, binding=1=combined sampler).
- `VkPipeline` — single foundation render pass, graphics-pipeline
  factory caching one `VkPipeline` per `VkShader*`, per-swapchain-image
  framebuffers.
- `VkMesh` — host-visible vertex + index buffers via VMA.
- `VkTexture` — staging-uploaded device-local images + shared linear
  sampler + built-in 1×1 white / flat-normal / no-spec textures.
- `VkUtils.h` — `VK_CHECK(call)` macro that aborts on non-success after
  logging file:line + result.

Per-frame flow in `VulkanRenderer::endFrame`: vkAcquireNextImageKHR
(handles OUT_OF_DATE_KHR by flagging a resize), record into the current
frame's command buffer, allocate a descriptor set + write UBO + sampler
per draw, vkCmdDrawIndexed, vkQueueSubmit (waits on image-available,
signals render-finished, fence), vkQueuePresentKHR.
