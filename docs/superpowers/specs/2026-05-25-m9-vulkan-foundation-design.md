# M9 — Vulkan backend foundation design

**Date:** 2026-05-25
**Milestone:** M9
**Status:** Design approved; implementation plan to follow.

## Goal

Stand up a parallel `engine/render/backends/vulkan/` directory alongside the
existing OpenGL backend, implementing enough of `iron::Renderer` to run
`games/01-spinning-cube` identically on Vulkan. OpenGL stays the default
backend. Subsequent milestones (M10+) port the remaining render features
(shadows, cubemap/skybox, reflections, HUD, debug-lines) and other games one
by one. Once parity, M-something flips the default and starts deleting the
OpenGL backend.

## Scope decisions (locked during brainstorm)

| Decision                  | Choice                                                                          |
| ------------------------- | ------------------------------------------------------------------------------- |
| Milestone scope           | Foundation + spinning-cube parity (NOT shadows/cubemap/HUD/etc.)                |
| Backend selection         | CMake flag `-DIRON_RENDER_BACKEND=opengl\|vulkan` (default `opengl`); only the chosen backend compiles + links |
| File layout               | Mirror OpenGL backend's per-concern split                                       |
| Shader compilation        | Runtime GLSL→SPIR-V via glslang. Existing game GLSL strings unchanged.          |
| Dependencies              | vcpkg manifest — `vulkan-headers`, `vulkan-loader`, `glslang`, `vulkan-memory-allocator` |
| Validation layers         | Enabled in Debug only; messages routed through `iron::Log::error`               |
| Frames in flight          | 2                                                                                |
| Color format / depth      | `B8G8R8A8_SRGB` (fallback first-available); depth `D32_SFLOAT`                  |
| Present mode              | FIFO (vsync); optional MAILBOX upgrade if available                              |
| Stubbed Renderer methods  | cubemap/skybox, shadow bounds, reflection plane, debug lines, HUD                |

## Architecture

The abstract `iron::Renderer` interface (`engine/render/Renderer.h`) stays
unchanged. It already returns opaque handles + accepts GLSL strings, so it
fits Vulkan without breaking OpenGL.

A new `RendererFactory::create(Window&)` returns a `unique_ptr<Renderer>` whose
concrete type is decided at build time by the `IRON_RENDER_BACKEND_*` define.
Game code never names a concrete renderer; it calls the factory.

### File layout

```
engine/render/
├── Renderer.h                           (UNCHANGED)
├── RendererFactory.h/.cpp               (NEW — picks backend by define)
└── backends/
    ├── opengl/                          (UNCHANGED)
    └── vulkan/                          (NEW)
        ├── VulkanRenderer.h/.cpp        (implements iron::Renderer)
        ├── VkContext.h/.cpp             (instance, device, queues, VMA)
        ├── VkSwapchain.h/.cpp           (surface, swapchain, depth, recreate)
        ├── VkFrameRing.h/.cpp           (2 frames: cmd pool/buf, semaphores, fence)
        ├── VkMesh.h/.cpp                (vertex + index buffers via VMA)
        ├── VkTexture.h/.cpp             (sampled image upload, sampler)
        ├── VkShader.h/.cpp              (glslang compile, set layout, pipeline layout)
        ├── VkPipeline.h/.cpp            (graphics pipeline, render pass, framebuffers)
        └── VkUtils.h                    (VK_CHECK, format helpers)

engine/CMakeLists.txt                    (toggles backend sources + defines)
vcpkg.json                               (adds vulkan deps)

tests/
├── test_glsl_to_spirv.cpp               (NEW — 2 cases)
└── test_renderer_factory.cpp            (NEW — 1 case per backend, conditional)
```

### Dependency direction

- `engine/render/Renderer.h` includes no backend headers.
- `engine/render/RendererFactory.*` includes `Renderer.h` plus exactly one
  backend header (selected by build define).
- `engine/render/backends/vulkan/*` includes Vulkan headers + VMA + glslang
  freely. Does NOT include any OpenGL headers.
- Games include `Renderer.h` + `RendererFactory.h` and never reach into a
  backend directly.

### CMake wiring

In `engine/CMakeLists.txt`:

```cmake
set(IRON_RENDER_BACKEND "opengl" CACHE STRING "Render backend: opengl or vulkan")
set_property(CACHE IRON_RENDER_BACKEND PROPERTY STRINGS opengl vulkan)

if (IRON_RENDER_BACKEND STREQUAL "vulkan")
    target_sources(ironcore PRIVATE
        render/RendererFactory.cpp
        render/backends/vulkan/VulkanRenderer.cpp
        render/backends/vulkan/VkContext.cpp
        render/backends/vulkan/VkSwapchain.cpp
        render/backends/vulkan/VkFrameRing.cpp
        render/backends/vulkan/VkMesh.cpp
        render/backends/vulkan/VkTexture.cpp
        render/backends/vulkan/VkShader.cpp
        render/backends/vulkan/VkPipeline.cpp)
    target_compile_definitions(ironcore PUBLIC IRON_RENDER_BACKEND_VULKAN)
    target_link_libraries(ironcore PUBLIC
        Vulkan::Vulkan
        glslang::glslang
        glslang::SPIRV
        GPUOpen::VulkanMemoryAllocator)
else()
    target_sources(ironcore PRIVATE
        render/RendererFactory.cpp
        render/backends/opengl/OpenGLRenderer.cpp
        # ... existing opengl sources unchanged ...
    )
    target_compile_definitions(ironcore PUBLIC IRON_RENDER_BACKEND_OPENGL)
endif()
```

`vcpkg.json`:

```json
{
  "name": "iron-core-engine",
  "version-string": "0.0.0",
  "dependencies": [
    "gamenetworkingsockets",
    "vulkan-headers",
    "vulkan-loader",
    "glslang",
    "vulkan-memory-allocator"
  ]
}
```

vcpkg fetches and builds the new packages; the GitHub Actions binary cache
absorbs them on subsequent CI runs. No external SDK install required by any
contributor.

## Components

### `VkContext` (per-app, lives until shutdown)

Owns:
- `VkInstance` (Debug: `VK_EXT_debug_utils` + `VK_LAYER_KHRONOS_validation`)
- `VkSurfaceKHR` (via `glfwCreateWindowSurface`)
- `VkPhysicalDevice` (prefer discrete; require graphics+present queue family +
  `VK_KHR_swapchain` extension; bail with `Log::error` if no device qualifies)
- `VkDevice` + a single combined graphics-and-present `VkQueue`
- `VmaAllocator` (VMA owns all buffer + image memory)
- `VkDebugUtilsMessengerEXT` (Debug only; routes messages to `iron::Log::error`)

Init failure surface: any step failing returns false from `VkContext::init`,
which `VulkanRenderer::init` propagates as a false return. Same contract as
`OpenGLRenderer::init`.

### `VkSwapchain` (recreatable on window resize)

Owns the `VkSwapchainKHR`, its `VkImage`s + `VkImageView`s, plus a single
shared depth `VkImage` + view (D32_SFLOAT) for the foundation pass.

Surface format negotiation: prefer `B8G8R8A8_SRGB` + `SRGB_NONLINEAR_KHR`,
fall back to the first available. Present mode: prefer `MAILBOX_KHR`, fall
back to `FIFO_KHR` (universally supported).

Resize: a glfw framebuffer-size callback sets a `pendingResize_` flag on the
`VulkanRenderer`. The next `beginFrame` calls `vkDeviceWaitIdle()` then
`VkSwapchain::recreate(ctx, w, h)` (destroys image views + depth, recreates
all three). Framebuffers in `VkPipeline` are also rebuilt.

### `VkFrameRing` (2 frames in flight)

Each `Frame`:
- `VkCommandPool` + one primary `VkCommandBuffer`
- `VkSemaphore imageAvailable` (GPU↔GPU)
- `VkSemaphore renderFinished` (GPU↔GPU)
- `VkFence inFlight` (CPU↔GPU; `beginFrame` waits + resets)
- `VkDescriptorPool` (sized for **128 sets / 128 UBOs / 128 samplers per frame** —
  comfortable headroom over spinning-cube's 1 draw; grow when a future
  milestone surfaces a real ceiling)
- One growing `VkBuffer` (HOST_VISIBLE | HOST_COHERENT) used as a linear
  per-frame UBO allocator; cursor resets at `beginFrame`

`current() → Frame&` + `advance()` are the only public methods beyond
`init/destroy`.

### `VkMesh`, `VkTexture`, `VkShader` stores

Each store keeps an `std::unordered_map<Handle, ResourceStruct>` plus a
monotonic `nextHandle_` counter. `Renderer::createMesh/createTexture/
createShader` enter resources; the maps are queried during draw recording;
`destroyAll(ctx)` runs in `VulkanRenderer::~VulkanRenderer`.

**Mesh upload:** VMA `vmaCreateBuffer` with `VMA_MEMORY_USAGE_AUTO` +
`HOST_ACCESS_SEQUENTIAL_WRITE_BIT`. Foundation = host-visible; staging-to-
device-local is a future optimization. `updateMesh` reuses the existing
buffer if the new size fits; otherwise destroys + recreates.

**Texture upload:** create staging buffer (host-visible) → memcpy RGBA →
create device-local `VkImage` (`SAMPLED_BIT | TRANSFER_DST_BIT`) →
one-shot command buffer: transition to `TRANSFER_DST_OPTIMAL`,
`vkCmdCopyBufferToImage`, transition to `SHADER_READ_ONLY_OPTIMAL` →
submit + wait → free staging. Per-store shared `VkSampler` (linear filter,
repeat) is sufficient for foundation.

**Shader compile:** feed each GLSL string to `glslang::TShader` with
`EShClientVulkan` semantics → SPIR-V `std::vector<uint32_t>` →
`vkCreateShaderModule`. Errors logged with `glslang::getInfoLog()` and
return `kInvalidHandle`.

Foundation hard-codes the descriptor set layout that `games/01-spinning-cube`
needs:
- `set=0 binding=0`: uniform buffer, contains `mat4 mvp`.
- `set=0 binding=1`: combined image sampler (the cube's base color texture).

This is **deliberately not generic** in M9. Later milestones extend the
layout. SPIR-V reflection (via spirv-cross) is a future upgrade if we want
auto-detected uniforms.

### `VkPipeline` (per-shader graphics pipeline + render pass)

One `VkPipeline` per shader. Foundation render pass: one color attachment
(loadOp CLEAR, storeOp STORE, finalLayout PRESENT_SRC_KHR) + one depth
attachment (loadOp CLEAR, storeOp DONT_CARE). One subpass.

Pipeline state:
- Vertex input: `MeshData::Vertex` layout bound at binding 0
- Input assembly: TRIANGLE_LIST
- Viewport + scissor: dynamic (`VK_DYNAMIC_STATE_VIEWPORT` /
  `_SCISSOR`) so resize doesn't rebuild the pipeline
- Rasterization: cullMode BACK, frontFace COUNTER_CLOCKWISE
- Depth-stencil: test + write, compareOp LESS
- Multisample: 1 sample (no MSAA)
- Color blend: opaque

One `VkFramebuffer` per swapchain image (color attachment = that image;
depth attachment = the shared depth view).

## Data flow

### Per-frame (`VulkanRenderer::beginFrame` … `endFrame`)

```
beginFrame(clearColor, sun, lights, fog, view, projection)
    if pendingResize: vkDeviceWaitIdle + swapchain.recreate + framebuffers rebuild
    wait + reset current frame's inFlight fence
    reset current frame's command pool + descriptor pool
    reset current frame's UBO sub-allocator cursor
    vkAcquireNextImageKHR → imageIndex
        on VK_ERROR_OUT_OF_DATE_KHR: set pendingResize, skip frame
    vkBeginCommandBuffer + vkCmdBeginRenderPass(framebuffers[imageIndex],
                                                clearValues={color, depth=1})
    vkCmdSetViewport + vkCmdSetScissor
    (CPU buffers `currentFrame.clearColor`, etc; the only state needed
     during draws is `mvp` per draw, which `submit()` computes)

submit(drawCall)
    Append drawCall to frame.queuedDraws.

endFrame()
    for draw in queuedDraws:
        vkCmdBindPipeline(pipeline for draw.shader)
        Allocate descriptor set from frame's pool against shader.setLayout.
        Write MVP UBO (projection * view * draw.model) into the frame UBO
            sub-allocator; write the descriptor (binding=0 → UBO,
            binding=1 → sampler for draw.material.texture).
        vkCmdBindDescriptorSets, vkCmdBindVertexBuffers, vkCmdBindIndexBuffer
        vkCmdDrawIndexed(indexCount)
    vkCmdEndRenderPass + vkEndCommandBuffer
    vkQueueSubmit(graphicsQueue, cmdBuf,
                  wait imageAvailable @ COLOR_ATTACHMENT_OUTPUT_BIT,
                  signal renderFinished, fence inFlight)
    vkQueuePresentKHR(presentQueue, wait renderFinished, swapchain, imageIndex)
        on VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR: set pendingResize
    frameRing.advance()
```

### Stubbed methods

For the 🟡 methods (cubemap, skybox, shadow bounds, reflection plane, debug
lines, HUD), the implementations have the same call-once-warn shape so that
filling them in later doesn't change the surrounding code:

```cpp
void VulkanRenderer::setSkybox(CubemapHandle) override {
    static bool warned = false;
    if (!warned) {
        Log::warn("Vulkan: setSkybox not implemented yet (stub)");
        warned = true;
    }
}
```

A game that needs a stubbed feature will visibly miss that feature when run
under the Vulkan backend, which is correct behavior: each future milestone
fills in one stub at a time.

## Error handling

`VK_CHECK(call)` macro: on non-`VK_SUCCESS`, log
`"VK_CHECK failed: <call> at <file>:<line> → <result_to_string>"` via
`iron::Log::error`. Most call sites either return false (init failures) or
log+continue (per-frame errors that aren't fatal).

Init failures: `VulkanRenderer::init` returns false, same as
`OpenGLRenderer::init`. Game code already checks this return.

Per-frame failures:
- `vkAcquireNextImageKHR` returning `VK_ERROR_OUT_OF_DATE_KHR` → set
  `pendingResize_`, skip the frame, recover on next `beginFrame`.
- `vkQueuePresentKHR` returning the same → same path.
- Any other unexpected result → `Log::error` and continue (game keeps
  running but may render garbage; validation layers will already have
  surfaced the root cause).

Validation layer messages are routed through the debug-utils messenger
(Debug only). The messenger callback formats the message + severity and
calls `iron::Log::error` for ERROR severity, `iron::Log::warn` for WARNING.
None are fatal in Debug builds; they're diagnostic. In Release builds
validation layers are not loaded.

## Testing

### Layer A — pure logic (headless C++, CTest harness)

- `tests/test_glsl_to_spirv.cpp` — feed trivial GLSL strings to a thin
  `iron::vulkan::compileGlsl(stage, src) → std::vector<uint32_t>` helper
  extracted from `VkShader.cpp`. Assert the output starts with SPIR-V magic
  `0x07230203` and is non-empty. 2 cases (vertex + fragment).
- `tests/test_renderer_factory.cpp` — assert
  `RendererFactory::create(window)` returns a non-null `unique_ptr<Renderer>`
  whose `dynamic_cast<VulkanRenderer*>` (or `OpenGLRenderer*`) is non-null,
  conditionally compiled with `#ifdef IRON_RENDER_BACKEND_VULKAN`. 1 case
  per backend.

The factory test is the **only** test that requires a window. We use a
hidden GLFW window (`GLFW_VISIBLE = GLFW_FALSE`) so the test is headless.
If `init()` fails (no GPU on the CI runner), the test logs the failure and
skips rather than fails — Vulkan-on-CI is enabled when the runner can
provide a working ICD.

Bringing CTest total from 31 → 33.

### Layer B — manual smoke (PR description checklist)

1. Build with `-DIRON_RENDER_BACKEND=vulkan`. Build is clean, links against
   `vulkan-loader`, `glslang`, `VMA`.
2. Run `games/01-spinning-cube`. Window opens, textured cube spins, no
   validation messages logged to stderr.
3. Re-build with `-DIRON_RENDER_BACKEND=opengl` (the default). Same game
   runs identically — no regression on the OpenGL path.
4. Resize the spinning-cube window during play. Vulkan swapchain recreates
   cleanly, cube keeps spinning, no validation errors.

The validation-layer pass is the actual correctness signal. A silent
validation run = sound foundation.

### Out of scope for testing in M9

- Performance benchmarks (no meaningful comparison at 1-draw-call scale)
- Other games (net-cubes, Strandbound, showcase, etc.) — they use features
  that are 🟡 stubs in M9. Each future milestone smoke-tests its own
  newly-ported game.

## Risks / open questions

- **CI Vulkan availability.** GitHub Actions Windows runners may or may
  not provide a working Vulkan ICD. The `test_renderer_factory` Vulkan
  case is written to skip rather than fail if `init()` returns false on
  CI (no GPU / no ICD), so CI stays green. The factory test still
  validates the OpenGL path. Manual smoke tests on local hardware are
  the real correctness signal for Vulkan in M9.
- **Future descriptor layout extension.** Hard-coded binding 0 = UBO,
  binding 1 = sampler. When lit pass / shadows land, we'll add more
  bindings. Decision: hand-code each shader's layout until SPIR-V
  reflection is worth integrating (probably M12+).
- **Single shared sampler.** Linear filter, repeat wrap. Becomes a
  fork-point when textures need different filtering (e.g., shadow map
  comparison sampler). Per-texture sampler is the obvious extension.

## Out of scope (deferred to future milestones)

- Shadow map render pass (depends on M10+ — shadow milestone)
- Cubemap upload, skybox draw (M10+ — atmosphere/skybox milestone)
- Planar reflection pass + reflective material (M11+ — reflection milestone)
- Debug-lines pipeline (M11+ — when first Vulkan game needs it)
- HUD overlay pipeline (M11+ — same)
- Multi-pass coordination (when shadows + reflections both land)
- Per-resource destroy API
- SPIR-V reflection for auto-discovering uniforms
- MSAA
- Compute shaders (Vulkan-only milestone; not yet planned)
- Ray-tracing extensions (future)
- Backend hot-swap at runtime (deliberately not built; CMake-time only)
