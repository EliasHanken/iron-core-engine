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

- **Vulkan 1.3** (`engine/render/backends/vulkan/`) — the default and
  the actively-developed backend. Full feature set: shadow map, cubemap
  skybox, cubemap reflection, planar reflection, HUD, debug lines,
  GPU compute particles.
- **OpenGL 3.3** (`engine/render/backends/opengl/`) — **deprecated and
  frozen** as of M17 (2026-05-27). No new features land here. Still
  compiles and runs the demos it shipped with (`02-strandbound`,
  `04-net-pingpong`, `05-net-cubes`, `06-net-tag`). Same feature set
  as Vulkan minus the GPU compute particle system.

Selection is build-time:

```
cmake -S . -B build                                  # Vulkan (default)
cmake -S . -B build -DIRON_RENDER_BACKEND=opengl     # OpenGL (deprecated)
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

## Vulkan lit shader basics (M12)

The Vulkan backend's per-draw UBO grew from a bare `mat4 mvp` to a
`LitUbo` struct (192 bytes, std140-safe):

```cpp
struct LitUbo {
    Mat4 mvp;        // projection * view * model
    Mat4 model;      // for mat3(model) * aNormal in the vertex shader
    Vec4 sunDir;     // xyz direction; w padding
    Vec4 sunColor;   // xyz color; w padding
    Vec4 ambient;    // xyz pre-multiplied ambient color; w padding
    Vec4 emissive;   // xyz from call.material.emissive; w padding
};
```

`VulkanRenderer::beginFrame` stores the directional light + ambient
from its args; `VulkanRenderer::submit` packs everything into
`LitUbo` and uploads via `VkFrameRing::allocateUbo`. The descriptor
set layout (set=0 binding=0 = UBO, binding=1 = combined image
sampler) is unchanged — only the UBO contents grew.

Vulkan shaders in `games/01-spinning-cube/main.cpp` and
`games/07-net-shooter/main.cpp` were rewritten to consume `LitUbo`
and perform Lambertian shading: `dot(N, -sunDir) * sunColor +
ambient`, multiplied by the diffuse texture sample, plus the
per-draw emissive.

### What's still missing on the Vulkan lit path

These all need additional UBO fields and either pipeline state
(LEQUAL depth compare, extra subpasses) or whole new passes
(shadow depth, cubemap, reflection RTT):

- Point lights (16-array with range falloff).
- Exponential distance fog.
- Shadow map sampling (needs the depth-pass port).
- Cubemap skybox + cubemap-based reflection sampling.
- Planar reflection sampling (needs RTT pipeline).
- Normal maps + specular maps + TBN math.
- Per-DrawCall UV scale (`call.material.uvScale`).

Net-shooter's Vulkan-startup warning enumerates the gaps so the user
knows what to expect.

## Vulkan normal + specular maps + UV scale (M13)

The Vulkan lit pass gained TBN-perturbed normals, Blinn-Phong
specular highlights, and per-Material UV tiling.

### LitUbo grew to 224 bytes

```cpp
struct LitUbo {
    Mat4 mvp;
    Mat4 model;
    Vec4 sunDir;
    Vec4 sunColor;
    Vec4 ambient;
    Vec4 emissive;
    Vec4 cameraPos;       // M13 — for Blinn-Phong view vector
    Vec4 materialParams;  // M13 — x=uvScale, y=specPower, z=reflectivity (unused until M16/17)
};
```

`VulkanRenderer::beginFrame` extracts the camera world position from
the view matrix via `-R^T * t`; `submit` packs `materialParams` from
the per-`DrawCall::material` fields.

### Descriptor set layout grew to 4 bindings

| Binding | Type | Stage | Purpose |
|---|---|---|---|
| 0 | UNIFORM_BUFFER | VS+FS | LitUbo |
| 1 | COMBINED_IMAGE_SAMPLER | FS | Diffuse |
| 2 | COMBINED_IMAGE_SAMPLER | FS | Normal map (new) |
| 3 | COMBINED_IMAGE_SAMPLER | FS | Specular map (new) |

`VulkanRenderer::submit` writes all three samplers per draw, with
fallback to the built-in white / flat-normal / no-spec textures
when the Material's handle is invalid. The per-frame descriptor pool
in `VkFrameRing` bumped its COMBINED_IMAGE_SAMPLER capacity from
`kMaxDescriptorSetsPerFrame` to `3 * kMaxDescriptorSetsPerFrame` to
match (= 384 samplers / frame).

### Shader-side TBN + Blinn-Phong

Spinning-cube and net-shooter share the same Vulkan-branch shaders.
The fragment shader builds the TBN basis from `vNormal` + `vTangent`,
samples the normal map RGB → tangent-space normal, multiplies by TBN
to get a world-space perturbed normal. Blinn-Phong: half vector
`H = normalize(L + V)`, specular term `pow(max(dot(perturbedN, H),
0), specPower)` modulated by the spec map's red channel as a mask.

### What's still missing

These need either UBO fields or whole new passes:

- Point lights (16-array with range falloff) — M15.
- Exponential distance fog — M15.
- Shadow map sampling — M14 (multi-pass).
- Cubemap skybox + cubemap-based reflection — M16 (separate pass +
  binding).
- Planar reflection — M17 (RTT pipeline).

After M14-M17 land, the Vulkan backend reaches full parity with the
OpenGL lit pass.

## Vulkan shadow map + frame-flow restructure (M14)

The Vulkan backend's first multi-pass rendering feature, plus a
foundational restructure of the per-frame flow to make multi-pass
rendering practical going forward.

### Frame-flow restructure: defer + replay

`VulkanRenderer::submit` no longer records into the active command
buffer. It queues into a `std::vector<DrawCall> sceneDraws_`.
External Vulkan subsystems (`VkParticleSystem::render`) register
deferred render callbacks via a new
`VulkanRenderer::enqueueDeferredScenePass` API.
`flushDebugLines` and `drawHud` also defer (HUD already did on
OpenGL). `endFrame` orchestrates the entire pass sequence:

```
endFrame:
  1. Shadow pass (VkShadowMap::record)
       — depth-only render pass, replays sceneDraws_, ends with
         image in SHADER_READ_ONLY_OPTIMAL
  2. Scene render pass
       — replays sceneDraws_ via recordSceneDraw (writes 5
         descriptors per draw including binding 4 = shadow sampler)
       — drains deferredScenePass_ callbacks (particles)
       — drains pending debug-lines if any
       — drains pending HUD if any
  3. End cmd buffer + submit + present
```

Deferred callbacks capture pointers by raw `this` — callers must
guarantee the captured object outlives the matching `endFrame()`
call (the queue is cleared each `beginFrame`).

### VkShadowMap subsystem

`engine/render/backends/vulkan/VkShadowMap.cpp` owns a 2048×2048
D32_SFLOAT depth image, a depth-only render pass with two subpass
dependencies (entry + exit) for race-free sampling, a framebuffer,
a depth-only graphics pipeline (back-face cull to reduce
peter-panning, no color attachments), and a sampler with NEAREST
filter + CLAMP_TO_BORDER + `VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE`
(matches the OpenGL "out-of-bounds = lit" convention).

### LitUbo grew to 288 bytes

```cpp
struct LitUbo {
    Mat4 mvp;
    Mat4 model;
    Mat4 lightViewProj;   // M14 — for shadow map sampling
    Vec4 sunDir;
    Vec4 sunColor;
    Vec4 ambient;
    Vec4 emissive;
    Vec4 cameraPos;
    Vec4 materialParams;  // x=uvScale, y=specPower, z=reflectivity,
                          // w=shadowBias (M14 repurposes padding)
};
```

`VulkanRenderer::beginFrame` computes `pendingLightViewProj_` from
`pendingSunDir_ + pendingShadowCenter_ + pendingShadowRadius_` using
the same orthographic-from-sphere math the OpenGL backend uses
(`computeLightViewProj` helper). `VulkanRenderer::setShadowBounds`
(was a stub since M9) now stores the center + radius for the next
frame.

### Descriptor set layout: 5 bindings

| Binding | Type | Stage | Purpose |
|---|---|---|---|
| 0 | UNIFORM_BUFFER | VS+FS | LitUbo |
| 1 | COMBINED_IMAGE_SAMPLER | FS | Diffuse |
| 2 | COMBINED_IMAGE_SAMPLER | FS | Normal |
| 3 | COMBINED_IMAGE_SAMPLER | FS | Specular |
| 4 | COMBINED_IMAGE_SAMPLER | FS | Shadow (M14) |

`VkFrameRing` per-frame descriptor pool budget doubled from 128 to
256 sets/frame to accommodate the shadow+scene draw doubling.
COMBINED_IMAGE_SAMPLER capacity = `4 * kMaxDescriptorSetsPerFrame`
(= 1024 / frame).

### Shader-side PCF sampling

Vertex shader emits `vLightSpacePos = u.lightViewProj * world`.
Fragment shader's `shadowFactor()` helper does 3×3 PCF:
remap clip-space XY to UV via `proj.xy * 0.5 + 0.5` (Vulkan clip-Z
is already [0,1] — no remap needed there, unlike OpenGL), then for
each of 9 texel offsets sample the shadow map and compare to
`proj.z - bias`. Average of 9 samples = soft-edged shadow factor.

### What's still missing

- Point lights (16-array with range falloff) — M15.
- Exponential distance fog — M15.
- Cubemap skybox + cubemap-based reflection — M16.
- Planar reflection — M17.

After M15-M17 land, the Vulkan backend reaches full parity with the
OpenGL lit pass.

## Vulkan point lights + fog (M15)

Plumbing-only milestone — extends the Vulkan lit pass with the
remaining UBO-only features. No new render passes, no new
pipelines, no new descriptor bindings.

### LitUbo grew to 832 bytes

```cpp
struct LitUbo {
    Mat4 mvp;
    Mat4 model;
    Mat4 lightViewProj;
    Vec4 sunDir;
    Vec4 sunColor;
    Vec4 ambient;
    Vec4 emissive;
    Vec4 cameraPos;
    Vec4 materialParams;     // x=uvScale, y=specPower, z=reflectivity, w=shadowBias
    Vec4 fogColor;           // M15 — xyz=color, w=density
    Vec4 lightCounts;        // M15 — x=pointLightCount (as float)
    Vec4 pointPositions[16]; // M15 — xyz=position, w=intensity
    Vec4 pointColors[16];    // M15 — xyz=color, w=range
};
```

Each point light is packed across two parallel array slots so the
std140 stride remains a clean 16 bytes. The `kMaxPointLights = 16`
constant matches the OpenGL backend. Excess lights are silently
dropped at `beginFrame`.

`VulkanRenderer::beginFrame` already accepts `std::span<const
PointLight>` and `const Fog&` as args (existing since M9 — were
silently ignored until now). M15 stores them in `pendingPointLights_`
+ `pendingPointLightCount_` + `pendingFog_`; `recordSceneDraw` packs
them into the UBO.

### Shader-side point light + fog

The fragment shader's existing sun + shadow + ambient lighting is
unchanged. After that block, a `for (int i = 0; i < count; ++i)`
loop adds each point light's contribution: `1 - smoothstep(0, range,
dist)` falloff (no inverse-square singularity), Lambertian +
Blinn-Phong via the perturbed normal from the M13 TBN.

Fog mix happens at the very end:
`finalColor = mix(lit, fogColor.xyz, clamp(1 - exp(-density * d), 0, 1))`
where `d = length(cameraPos - vWorldPos)`. When density = 0,
fogFactor = 0, mix is a no-op.

### Per-draw upload cost

832 bytes per draw rounds to 1024 with 256-byte UBO alignment. At
net-shooter's ~40 draws that's 40 KB per frame, well within the
256 KB per-frame UBO sub-allocator budget.

### What's still missing

- Cubemap skybox + cubemap reflection — M16.
- Planar reflection — M17.

After M16-M17 land, the Vulkan backend reaches full parity with the
OpenGL lit pass.

## Vulkan cubemap skybox + cubemap reflection (M16)

Two cubemap-dependent features bundled because they share the
underlying cubemap storage.

### VkCubemapStore subsystem

`engine/render/backends/vulkan/VkCubemap.cpp` owns cube-compatible
images (6 array layers, `VK_IMAGE_VIEW_TYPE_CUBE`), a shared linear
sampler with CLAMP_TO_EDGE address modes, and a built-in 1×1×6 black
fallback. `createFromFaces(width, height, faces[6])` does a single
staging-buffer upload of all 6 faces with one `vkCmdCopyBufferToImage`
call + layout transitions.

The black fallback is used by the lit pass when no skybox is set:
`mix(lit, blackReflection, reflectivity)` → no contribution, matches
the "no skybox" OpenGL behavior.

### VkSkybox subsystem

`engine/render/backends/vulkan/VkSkybox.cpp` owns a one-time-uploaded
cube mesh (8 vertices, 36 indices) + a graphics pipeline:
- Vertex shader emits `gl_Position = (vp * pos).xyww` — clip-Z = clip-W
  → after perspective divide → NDC-Z = 1.0
- depthCompareOp = `LESS_OR_EQUAL` so z=1 passes against the cleared
  depth of 1.0 (sky fragments pass only where geometry hasn't written
  closer depth)
- Fragment shader samples the cubemap with `vDir = aPos` (the
  vertex position IS the direction from the origin)
- cull = NONE (we're inside the cube)
- depth write = OFF (cube doesn't contribute to depth)

`VulkanRenderer::endFrame` draws the skybox FIRST inside the scene
render pass — before geometry — so the GPU can early-z-reject sky
fragments behind opaque geometry. The translation is stripped from
the view matrix CPU-side so the cube stays centered on the camera.

### createCubemap + setSkybox un-stubbed

`VulkanRenderer::createCubemap` (was a `warnOnce` stub since M9) now
delegates to `VkCubemapStore::createFromFaces`. `setSkybox` stores
the handle in `pendingSkybox_`. `endFrame` skips the skybox draw if
`pendingSkybox_ == kInvalidHandle`.

### Cubemap reflection in the lit shader

The lit pass descriptor set layout grew from 5 to 6 bindings (cubemap
sampler at binding 5). `VulkanRenderer::recordSceneDraw` writes the
active skybox (or black fallback) as the 6th descriptor per draw.
`VkFrameRing` descriptor pool sampler capacity bumped to `5 *
kMaxDescriptorSetsPerFrame` (= 1280 / frame).

The lit fragment shader gains a reflection block guarded by
`materialParams.z` (reflectivity, present in LitUbo since M13):

```glsl
float reflectivity = u.materialParams.z;
if (reflectivity > 0.0) {
    vec3 viewDir = normalize(vWorldPos - u.cameraPos.xyz);
    vec3 reflectDir = reflect(viewDir, perturbedN);
    vec3 reflectColor = texture(uSkyCubemap, reflectDir).rgb;
    lit = mix(lit, reflectColor, reflectivity);
}
```

The branch is uniform across the draw call, so the GPU resolves it
efficiently. When reflectivity = 0 (default for most materials), the
block is skipped entirely.

### What's still missing

- Planar reflection (M17 — RTT pass with mirrored camera + clip plane).

After M17 lands, the Vulkan backend reaches full parity with the
OpenGL lit pass.

## M17 — Vulkan planar reflection (2026-05-27)

Vulkan backend reaches OpenGL parity. `VkReflectionTarget` owns a 1024² RGBA8+D32 RTT with its own render pass and an entry+exit subpass dependency. `VkContext::createDevice` enables `shaderClipDistance` so the reflection vertex shader can use `gl_ClipDistance[0]` to discard geometry on the wrong side of the mirror plane.

A separate shared reflection pipeline (2-binding layout: UBO + diffuse, `CULL_MODE_NONE`) is created once at `VulkanRenderer::init`. The pipeline shares the 928-byte LitUbo buffer with the scene pass; its descriptor layout binds only `LitUbo.model`, `LitUbo.reflectionViewProj`, and `LitUbo.clipPlane`.

The lit pass descriptor set grows from 6 → 7 bindings (binding 6 = reflection RTT). Sampler pool capacity bumped 5× → 6× `kMaxDescriptorSetsPerFrame` in `VkFrameRing`.

Materials with `useReflectionPlane=true` sample binding 6 projectively (`gl_FragCoord.xy / screenSize`) when a reflection plane is active. The M16 cubemap reflection path applies as a fallback when no plane is set.

Demo: `games/03-showcase` builds under both OpenGL and Vulkan via `iron::createRenderer(window)`. Mirror floor at y=-0.1 reflects the rotating sphere + metallic cylinder + sunset skybox.
