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

## GPU compute & particle system (Vulkan-only)

The Vulkan backend is the first to expose a GPU-resident particle
system, demonstrating the hardware capability the Vulkan investment
unlocks. The OpenGL backend has no equivalent — by design, since the
demo's CMakeLists.txt is gated on the Vulkan configure.

### Public API: `iron::ParticleSystem`

```cpp
#include "render/ParticleSystem.h"

iron::ParticleSystemConfig cfg;       // sensible defaults: 1M particles
auto particles = iron::createParticleSystem(*renderer, cfg);
if (!particles) { /* init failed */ }

// Per frame:
particles->tick(dt);                 // one compute dispatch
renderer->beginFrame(...);
particles->render(view, projection); // one instanced billboard draw
renderer->endFrame();
```

The header has an `#error` if included under any non-Vulkan backend, so
games meant to support multiple backends must gate the include themselves.

### Implementation

`engine/render/backends/vulkan/VkParticleSystem` owns:

- **SSBO** — one host-visible VMA buffer holding `count` 64-byte
  particles (1M = 64 MB). Initial state seeded with `std::mt19937`
  uniform-in-sphere positions + staggered initial ages so the field
  is saturated on frame 0.
- **Compute pipeline** — curl-noise update shader (analytic value
  noise, no texture lookups). Workgroup size 256.
  `tick(dt)` records a one-shot command pool + buffer, dispatches
  `ceil(count/256)` workgroups, submits, `vkQueueWaitIdle`. Isolated
  from the per-frame command buffer for M10 simplicity.
- **Graphics pipeline** — additive blend (ONE,ONE), depth test ON /
  write OFF, cull NONE, no vertex input. `render(view, projection)`
  allocates one descriptor set from the active frame's pool, writes
  the Camera UBO via the frame ring's per-frame sub-allocator, then
  `vkCmdDraw(6, count, 0, 0)` — 6 verts (billboard quad) × count
  instances, vertex pull from the SSBO via `gl_InstanceIndex`.

### Cross-boundary touchpoints

External Vulkan subsystems (like `VkParticleSystem`) need four accessors
on `iron::VulkanRenderer` that are NOT part of the abstract `Renderer`:

```cpp
VkContext&      context();              // raw Vulkan handles + VMA
VkFrameRing&    frameRing();            // per-frame UBO sub-allocator + descriptor pool
VkCommandBuffer currentCommandBuffer(); // active primary cmd buffer; VK_NULL_HANDLE when frame is skipped
VkRenderPass    scenePass() const;      // the main color+depth render pass
```

`currentCommandBuffer()` returns `VK_NULL_HANDLE` during a skipped
frame (acquire failed, resize pending). External subsystems must
early-out on null instead of recording into an unbegun command buffer.

These are documented as engine-internal in the header. Game code never
calls them.

### `VkFrameRing` pool capacity

The per-frame descriptor pool was sized in M9 for `UNIFORM_BUFFER` +
`COMBINED_IMAGE_SAMPLER` only. M10 added `STORAGE_BUFFER` capacity for
the particle system's render-side SSBO binding. New Vulkan subsystems
that allocate descriptors of new types from the frame pool need to
extend `VkFrameRing::initFrame`'s `sizes[]` array.

### Internal helper: `VkComputePipeline`

`engine/render/backends/vulkan/VkComputePipeline.h` wraps the SPIR-V →
shader module → pipeline layout → compute pipeline boilerplate. Private
to the Vulkan backend; gains a second consumer in a future milestone
(GPU skinning, GPU culling, etc.) at which point we'll consider
promoting it.

### Future work

- Fold the compute dispatch into the per-frame command buffer (drop
  the `vkQueueWaitIdle`).
- Multiple particle systems per scene (currently untested — the
  abstraction allows it but the demo uses one).
- Public `iron::ComputePipeline` engine surface (when a second
  consumer needs it).
- Particle textures, sub-particle physics (collisions, etc.) — out of
  scope for M10.

## HUD + debug-lines + gizmos on Vulkan (M11)

The Vulkan backend gained two render subsystems and one engine-level
debug-visualization layer:

- `VkHud` (`engine/render/backends/vulkan/VkHud.cpp`) — screen-space
  triangle-list pipeline, alpha blend, depth off. Recorded inside the
  scene render pass as the final draw. Each `HudDrawGroup` allocates a
  descriptor set from the active frame pool and gets one `vkCmdDraw`.
- `VkDebugLines` (`engine/render/backends/vulkan/VkDebugLines.cpp`) —
  line-list pipeline, depth test on / write off, no blend. `drawLine`
  queues; `flushDebugLines` records into the active cmd buffer using
  the frame's vertex sub-allocator.
- `VkFrameRing` extended with a per-frame 1 MB host-visible vertex
  sub-allocator (`allocateVertices`) alongside the existing 256 KB UBO
  sub-allocator. Reset at the start of each frame. Sized for ~16 K
  HudVertices or ~31 K LineVertices per frame.
- `iron::GizmoRegistry` (`engine/debug/GizmoRegistry.h`) — backend-
  agnostic retained-mode shape registry. Game code adds named,
  categorized lines / AABBs / spheres with optional timed expiry;
  `tick(dt, renderer)` advances expiries and emits `drawLine` for
  enabled categories. AABBs tessellate into 12 edges; spheres into
  3 great-circle loops of 32 segments each (96 lines). Each shape
  gets a `GizmoId` handle that can be updated in place or removed.
  Master toggle (`enableAll(bool)`) for F3-style on/off.

### Scene-pass record order

Per frame inside the active render pass:

1. Scene geometry (the queue submitted via `Renderer::submit`)
2. Particles (`iron::ParticleSystem::render`, M10)
3. Debug lines (`Renderer::flushDebugLines` → `VkDebugLines::record`)
4. HUD (`Renderer::drawHud` → `VkHud::record`)

HUD is last so overlays sit on top of everything; debug-lines goes
after particles so lines are visible through transparent particle
puffs.

### Net-shooter Vulkan port

`games/07-net-shooter` no longer gates on the OpenGL backend. The
Vulkan path uses an **unlit textured shader** matching the M9
single-mat4-UBO contract (the existing `VulkanRenderer::submit` only
uploads `mat4 mvp` per draw — no lighting uniforms). The game logs a
one-time warning on Vulkan startup. Full lit-shader parity needs a
future milestone to extend `submit` with a richer per-draw UBO
(model/view/projection split, sun direction + color, ambient, point
lights, emissive, etc.).

Lag-comp AABBs and rocket-splash spheres are registered as gizmos in
two categories (`"lagcomp"` and `"splash"`), toggled with F3. The
host adds/updates an AABB per peer in `authStates` each frame and
removes them when peers disconnect. Both host and client add a
0.4-second splash sphere at rocket detonation sites (matching the
existing `ExplosionFx` lifetime).
