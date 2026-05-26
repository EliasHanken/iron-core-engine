# M10 — GPU compute particles design

**Date:** 2026-05-25
**Milestone:** M10
**Status:** Design approved; implementation plan to follow.

## Goal

Ship the engine's first Vulkan-only feature: a GPU-resident
`iron::ParticleSystem` that ticks 1M particles per frame via a compute
shader (curl-noise flow field) and renders them as additively-blended
billboards (instanced draw, vertex pull from the same SSBO). A new
demo `games/08-particle-storm` showcases the system.

This is the first feature where Vulkan moves AHEAD of OpenGL rather
than just catching up. The OpenGL backend gets no particle path at
all — the demo's CMakeLists.txt is gated on `IRON_RENDER_BACKEND=vulkan`,
the same pattern the other M9 games use in the opposite direction.

## Scope decisions (locked during brainstorm)

| Decision               | Choice                                                                          |
| ---------------------- | ------------------------------------------------------------------------------- |
| Public API shape       | High-level `iron::ParticleSystem` only. No public `ComputePipeline` surface yet — internal compute helpers stay private to the Vulkan backend until a second consumer (M11+) needs them. |
| Backend availability   | Vulkan only. `ParticleSystem.h` has a `#error` under OpenGL.                    |
| Demo game              | New `games/08-particle-storm`, CMakeLists wrapped in `if (IRON_RENDER_BACKEND STREQUAL "vulkan")`. |
| Particle count         | 1,000,000 (config-tunable; 64 MB GPU memory budget).                            |
| Render technique       | Instanced billboards via `gl_InstanceIndex` SSBO pull (no vertex buffer).       |
| Simulation             | Analytic curl-noise from value noise (no texture lookups, no preprocess).       |
| Per-particle data      | 64 bytes, std430: `vec4 position+age`, `vec4 velocity+lifetime`, `vec4 colorYoung`, `vec4 colorOld`. |
| Appearance             | Procedural radial-gradient sprite, additive blend, color fades cyan→blue with age, alpha fades near death. |
| Render pipeline state  | Additive blend (`ONE, ONE`), depth test ON / write OFF, cull NONE, no vertex input. |
| Compute dispatch       | Workgroup 256, `ceil(count/256)` workgroups. One dispatch per tick.             |
| Compute submission     | One-shot command buffer + `vkQueueWaitIdle` BEFORE the renderer's `beginFrame`. Simpler sync; revisit if perf bites. |
| Lifecycle              | Continuous flow — respawn at source when `age >= lifetime`. No allocation churn, no GPU-side compaction. |
| Tests                  | `test_curl_noise.cpp` (4 cases, CPU port of the noise functions). CTest 33 → 34. |

## Architecture

### File layout

```
engine/render/
├── ParticleSystem.h                    NEW — public API (Vulkan-only build)
└── backends/vulkan/
    ├── VkParticleSystem.h              NEW
    ├── VkParticleSystem.cpp            NEW — SSBO + compute pipeline + render pipeline,
    │                                          inline GLSL shaders, dispatch + draw logic
    ├── VkComputePipeline.h             NEW — private compute-pipeline wrapper
    └── VkComputePipeline.cpp           NEW

engine/render/backends/vulkan/VulkanRenderer.h/.cpp   MODIFIED
                                       add `VkCommandBuffer currentCommandBuffer()`
                                       so external systems (particles) can record into
                                       the active frame's primary command buffer.

games/08-particle-storm/                NEW
├── main.cpp                            FreeFlyCamera + ParticleSystem + render loop
└── CMakeLists.txt                      gated on Vulkan backend

engine/CMakeLists.txt                   MODIFIED — register 3 new Vulkan sources
CMakeLists.txt                          MODIFIED — `add_subdirectory(games/08-particle-storm)`

tests/test_curl_noise.cpp               NEW — 4 cases
tests/CMakeLists.txt                    MODIFIED
```

### Dependency direction

- `engine/render/ParticleSystem.h` includes math headers + a forward declaration of `iron::Renderer`. Includes no backend headers.
- `engine/render/backends/vulkan/VkParticleSystem.cpp` includes the public header + `VkComputePipeline.h` + `VulkanRenderer.h` (for `currentCommandBuffer()`) + the existing `VkContext`/`VkFrameRing`/`VkShader` helpers.
- The new game `games/08-particle-storm` includes only `render/ParticleSystem.h` and the existing engine surface (`Application`, `FreeFlyCamera`, `Renderer`, `RendererFactory`). Never names a backend type directly.

### `iron::ParticleSystem` public API

```cpp
// engine/render/ParticleSystem.h
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "iron::ParticleSystem requires -DIRON_RENDER_BACKEND=vulkan"
#endif

#include "math/Mat4.h"
#include "math/Vec.h"

#include <cstdint>
#include <memory>

namespace iron {

class Renderer;  // forward

struct ParticleSystemConfig {
    std::uint32_t count        = 1'000'000;
    float         spawnRadius  = 20.0f;
    float         lifetimeMin  = 4.0f;
    float         lifetimeMax  = 8.0f;
    float         noiseScale   = 0.08f;
    float         noiseStrength = 4.0f;
    float         spriteSize   = 0.06f;
    Vec3          colorYoung   = {0.6f, 0.95f, 1.0f};   // bright cyan
    Vec3          colorOld     = {0.05f, 0.10f, 0.3f};  // deep blue
    std::uint32_t seed         = 0xC0FFEE;
};

// GPU-resident particle system. Construct AFTER Renderer is initialised.
class ParticleSystem {
public:
    virtual ~ParticleSystem() = default;

    // Advance one tick (compute dispatch).
    virtual void tick(float dtSec) = 0;

    // Draw all live particles. Must be called BETWEEN
    // Renderer::beginFrame and Renderer::endFrame.
    virtual void render(const Mat4& view, const Mat4& projection) = 0;

    virtual std::uint32_t count() const = 0;
};

std::unique_ptr<ParticleSystem> createParticleSystem(
    Renderer& renderer, const ParticleSystemConfig& cfg);

}  // namespace iron
```

`createParticleSystem` returns nullptr on init failure (so game code can
report a clean error instead of crashing). The Vulkan implementation
constructs `VkParticleSystem` and returns it as a `unique_ptr<ParticleSystem>`.

## Per-particle data layout

64 bytes per particle, std430-friendly:

```glsl
struct Particle {
    vec4 position;   // xyz = position, w = age (seconds)
    vec4 velocity;   // xyz = velocity, w = lifetime (seconds)
    vec4 colorYoung; // start color (currently the same for all particles)
    vec4 colorOld;   // end color
};
// 4 × 16 = 64 bytes
```

At 1M particles: 64 MB GPU memory. Comfortable on any modern GPU.

Per-particle colorYoung/colorOld are strictly redundant for the M10
config (uniform across all particles) but cost zero extra memory due to
std430 alignment of the first two vec4s. Leaves the door open for
future per-particle color variation without changing the SSBO layout.

## Compute shader: curl-noise update

Workgroup size 256. Dispatch `ceil(count / 256)` workgroups → 3907
for 1M particles, ~1ms GPU work on the target hardware.

```glsl
#version 450

layout(local_size_x = 256) in;

struct Particle {
    vec4 position;   // xyz pos, w age
    vec4 velocity;   // xyz vel, w lifetime
    vec4 colorYoung;
    vec4 colorOld;
};

layout(std430, set = 0, binding = 0) buffer ParticleBuffer {
    Particle particles[];
};

layout(set = 0, binding = 1) uniform Sim {
    float dtSec;
    float noiseScale;
    float noiseStrength;
    float spawnRadius;
    float lifetimeMin;
    float lifetimeMax;
    uint  count;
    uint  frameSeed;
} sim;

float hash11(float n) { return fract(sin(n) * 43758.5453); }

float hash31(vec3 p) {
    p = fract(p * vec3(443.897, 441.423, 437.195));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash31(i + vec3(0,0,0));
    float n100 = hash31(i + vec3(1,0,0));
    float n010 = hash31(i + vec3(0,1,0));
    float n110 = hash31(i + vec3(1,1,0));
    float n001 = hash31(i + vec3(0,0,1));
    float n101 = hash31(i + vec3(1,0,1));
    float n011 = hash31(i + vec3(0,1,1));
    float n111 = hash31(i + vec3(1,1,1));
    return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z);
}

vec3 potential(vec3 p) {
    return vec3(
        vnoise(p),
        vnoise(p + vec3(31.42, 17.13, 95.11)),
        vnoise(p + vec3(7.31, 81.97, 49.18)));
}

vec3 curl(vec3 p) {
    const float eps = 0.01;
    vec3 dx = vec3(eps, 0, 0);
    vec3 dy = vec3(0, eps, 0);
    vec3 dz = vec3(0, 0, eps);
    vec3 dPdx = (potential(p + dx) - potential(p - dx)) / (2.0 * eps);
    vec3 dPdy = (potential(p + dy) - potential(p - dy)) / (2.0 * eps);
    vec3 dPdz = (potential(p + dz) - potential(p - dz)) / (2.0 * eps);
    return vec3(
        dPdy.z - dPdz.y,
        dPdz.x - dPdx.z,
        dPdx.y - dPdy.x);
}

void respawn(inout Particle p, uint id) {
    float seed = float(id) * 0.0001 + float(sim.frameSeed) * 0.7919;
    float u = hash11(seed + 1.0);
    float v = hash11(seed + 2.0);
    float w = hash11(seed + 3.0);
    float r = sim.spawnRadius * pow(u, 1.0 / 3.0);
    float theta = 6.2831853 * v;
    float phi   = acos(2.0 * w - 1.0);
    p.position.xyz = vec3(
        r * sin(phi) * cos(theta),
        r * sin(phi) * sin(theta),
        r * cos(phi));
    p.position.w = 0.0;
    p.velocity.xyz = vec3(0.0);
    p.velocity.w = mix(sim.lifetimeMin, sim.lifetimeMax,
                       hash11(seed + 4.0));
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= sim.count) return;

    Particle p = particles[id];
    p.position.w += sim.dtSec;
    if (p.position.w >= p.velocity.w) {
        respawn(p, id);
    } else {
        vec3 v = curl(p.position.xyz * sim.noiseScale) * sim.noiseStrength;
        p.velocity.xyz = v;
        p.position.xyz += v * sim.dtSec;
    }
    particles[id] = p;
}
```

**Particles are quasi-massless** — velocity is SET from the field each
tick (rather than accumulated). Avoids runaway energy and gives the
classic flowy look.

## Render shaders

### Vertex

```glsl
#version 450

struct Particle {
    vec4 position;
    vec4 velocity;
    vec4 colorYoung;
    vec4 colorOld;
};
layout(std430, set = 0, binding = 0) readonly buffer ParticleBuffer {
    Particle particles[];
};

layout(set = 0, binding = 1) uniform Camera {
    mat4 view;
    mat4 projection;
    float spriteSize;
    float _pad0, _pad1, _pad2;
} cam;

const vec2 kCorner[6] = vec2[6](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0)
);

layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec4 vColor;

void main() {
    Particle p = particles[gl_InstanceIndex];
    vec2 corner = kCorner[gl_VertexIndex];
    vCorner = corner;

    float t = clamp(p.position.w / max(p.velocity.w, 0.001), 0.0, 1.0);
    vColor = mix(p.colorYoung, p.colorOld, t);
    float aliveFade = 1.0 - smoothstep(0.85, 1.0, t);
    vColor.a *= aliveFade;

    vec4 viewCenter = cam.view * vec4(p.position.xyz, 1.0);
    viewCenter.xy += corner * cam.spriteSize;
    gl_Position = cam.projection * viewCenter;
}
```

### Fragment

```glsl
#version 450

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    float r2 = dot(vCorner, vCorner);
    if (r2 > 1.0) discard;
    float falloff = (1.0 - r2);
    float intensity = falloff * falloff;
    outColor = vec4(vColor.rgb * intensity, vColor.a * intensity);
}
```

## Render pipeline state

| State                    | Value                                                        |
| ------------------------ | ------------------------------------------------------------ |
| Vertex input             | EMPTY (no bindings, no attributes — pull from SSBO via `gl_InstanceIndex` / `gl_VertexIndex`) |
| Topology                 | TRIANGLE_LIST                                                |
| Cull                     | NONE (billboards always face camera)                         |
| Polygon mode             | FILL                                                         |
| Depth test               | ON                                                           |
| Depth write              | OFF                                                          |
| Depth compare            | LESS                                                         |
| Color blend              | enabled; src=ONE, dst=ONE, op=ADD (premultiplied additive)   |
| Sample count             | 1                                                            |
| Dynamic state            | VIEWPORT + SCISSOR                                           |
| Render pass              | Reuse `pipelines_.renderPass()` (the existing scene render pass) |

A separate `VkPipeline` from the curl-noise compute pipeline and from
the default scene's graphics pipeline. All three created at
`VkParticleSystem` init, destroyed at shutdown.

## Tick + render integration

### Tick (compute dispatch — OUTSIDE the render frame)

```
VkParticleSystem::tick(dtSec):
    1. Build Sim uniform (dtSec, noiseScale, noiseStrength,
       spawnRadius, lifetimeMin, lifetimeMax, count, frameSeed++).
    2. Open a one-shot command buffer.
    3. Bind compute pipeline + descriptor (SSBO + Sim UBO).
    4. vkCmdDispatch(ceil(count/256), 1, 1).
    5. End + submit to graphics queue.
    6. vkQueueWaitIdle (simpler sync; revisit if perf bites).
```

The one-shot UBO for `Sim` is allocated from the particle system's own
small UBO buffer (host-mapped, persistent), not the frame ring's
sub-allocator — `tick` runs OUTSIDE `beginFrame`/`endFrame` and so has
no current frame to attach UBO offsets to.

### Render (inside the render frame)

```
VkParticleSystem::render(view, projection):
    Pre-condition: renderer.beginFrame already called, render pass active.

    1. cb = vulkanRenderer.currentCommandBuffer()
    2. Build Camera uniform; allocate offset in the frame ring's UBO
       sub-allocator (the M9-shipped allocateUbo path).
    3. Allocate descriptor set from frame's pool against render pipeline's
       set layout; write SSBO (binding 0) + Camera UBO (binding 1).
    4. vkCmdBindPipeline(GRAPHICS, particleRenderPipeline)
    5. vkCmdSetViewport + vkCmdSetScissor (re-set; the renderer also
       set them but bind-pipeline doesn't clear dynamic state — this
       is defensive in case future pipeline changes blow away state).
    6. vkCmdBindDescriptorSets
    7. vkCmdDraw(6, count, 0, 0)
```

### New method on `VulkanRenderer`

```cpp
class VulkanRenderer : public Renderer {
    // ... existing ...
public:
    // Returns the current frame's primary command buffer. Only meaningful
    // between beginFrame and endFrame. Used by external systems
    // (currently iron::ParticleSystem) that need to record draws into
    // the same render pass.
    VkCommandBuffer currentCommandBuffer() const;

    // Exposes the frame ring so external systems can allocate per-frame
    // UBO storage that lives until the next time this frame index is
    // reused.
    VkFrameRing& frameRing();
};
```

Game code never calls these — they're for engine subsystems. Documented
that way in the header. The `VkParticleSystem` casts the public
`Renderer&` to `VulkanRenderer&` (safe because the system is
Vulkan-only).

## `VkComputePipeline` helper (private)

```cpp
// engine/render/backends/vulkan/VkComputePipeline.h
namespace iron {
class VkContext;

class VkComputePipeline {
public:
    bool init(VkContext& ctx,
              const std::vector<std::uint32_t>& spirv,
              VkDescriptorSetLayout setLayout);
    void destroy(VkContext& ctx);

    VkPipeline       pipeline()       const { return pipeline_; }
    VkPipelineLayout pipelineLayout() const { return layout_; }

private:
    VkShaderModule   module_   = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
};
}
```

A thin wrapper that takes SPIR-V + a descriptor-set layout and produces
a compute pipeline + matching pipeline layout. Private to the Vulkan
backend; extracted to its own file purely for organization. Will gain a
second consumer when M11+ adds another high-level GPU subsystem.

## Game: `games/08-particle-storm/main.cpp` (sketch)

```cpp
#include "core/Application.h"
#include "core/Log.h"
#include "render/Renderer.h"
#include "render/RendererFactory.h"
#include "render/ParticleSystem.h"
#include "scene/Camera.h"
#include "scene/FreeFlyCamera.h"

int main() {
    iron::Application::Config cfg;
    cfg.title = "Iron Core — Particle Storm";
    cfg.width = 1280; cfg.height = 720;
    iron::Application app(cfg);
    if (!app.valid()) return 1;

    auto renderer = iron::createRenderer(app.window());
    if (!renderer) { iron::Log::error("renderer init failed"); return 1; }

    iron::ParticleSystemConfig psc;  // defaults are fine for the demo
    auto particles = iron::createParticleSystem(*renderer, psc);
    if (!particles) { iron::Log::error("particle system init failed"); return 1; }

    iron::FreeFlyCamera camera;
    camera.setPosition({0.0f, 0.0f, 40.0f});
    iron::Camera cameraOut;
    cameraOut.setAspect(static_cast<float>(app.window().width()) /
                        static_cast<float>(app.window().height()));

    app.setUpdate([&](const iron::FrameTime& t) {
        camera.update(app.input(), t.deltaSeconds);
        particles->tick(t.deltaSeconds);
    });
    app.setRender([&]() {
        const auto view = camera.viewMatrix();
        const auto proj = cameraOut.projectionMatrix();
        // (Skybox-less: just clear to deep blue and draw particles.)
        renderer->beginFrame(/*clear*/ {0.02f, 0.02f, 0.06f},
                             /*sun*/ {}, /*lights*/ {},
                             /*fog*/ {}, view, proj);
        particles->render(view, proj);
        renderer->endFrame();
        app.window().swapBuffers();
    });

    app.run();
    return 0;
}
```

(The actual sketch will be verified against the real `Application` /
`Camera` / `FreeFlyCamera` API during the implementation plan.)

## Error handling

- `createParticleSystem` returns `nullptr` on init failure (SPIR-V
  compile, pipeline create, buffer alloc, descriptor-set-layout
  create). Errors logged via `iron::Log::error`.
- All Vulkan calls inside `VkParticleSystem` go through `VK_CHECK`
  (abort on non-success after logging file:line + result).
- Validation layer messages already route through `iron::Log::error`
  (set up in M9 `VkContext::createDebugMessenger`).

## Testing

### Layer A — headless unit test (4 cases)

`tests/test_curl_noise.cpp` — CPU port of `hash11 / hash31 / vnoise /
potential / curl`. Tests:

1. **Determinism:** `vnoise(p)` returns the same value across two calls
   with the same `p`.
2. **Bounded output:** `vnoise(p)` ∈ `[0, 1]` for a grid of sample
   points; `curl(p)` magnitude bounded by a few times `noiseStrength`.
3. **Reproducibility at named points:** `curl((1.0, 2.0, 3.0))` returns
   a precomputed reference vector (CHECK_NEAR tolerance 1e-4). Catches
   accidental edits to the noise constants.
4. **Curl divergence ≈ 0:** sample div(curl(p)) numerically at a
   handful of points; it should be near zero (true mathematically,
   approximately true at our `eps = 0.01`).

Bringing CTest total 33 → 34.

### Layer B — manual smoke (PR description checklist)

1. Build with `-DIRON_RENDER_BACKEND=vulkan` and target
   `particle-storm`. Clean link.
2. Run the binary.
   - 1M particles visible, swirling organically.
   - Color gradient cyan (young) → deep blue (old) visible.
   - Soft fade near death — no popping.
   - Free-fly camera (WASD + mouse-look) navigates the cloud.
   - Zero Vulkan validation errors logged.
3. Frame time ≤ 16.6 ms at 1M particles on the dev machine (RTX 5080).
4. ESC closes cleanly with no GPU work leaks.

### Out of scope for tests

- Visual regression / reference image diffs (eyeball check is the bar).
- Cross-GPU benchmarking.
- OpenGL parity (none by design — game is Vulkan-only).

## Risks / open questions

- **Compute sync isolation.** Using `vkQueueWaitIdle` after the compute
  dispatch is heavier than necessary. M10 accepts this for simplicity;
  a follow-up milestone can fold the dispatch into the frame command
  buffer with a memory barrier between the SSBO write (compute) and
  read (vertex shader) for tighter perf.
- **Particle data is static-uniform across the system.** colorYoung /
  colorOld are duplicated 1M times in the SSBO. Currently zero
  memory cost (alignment slack), but if we add genuinely per-particle
  data later, dropping the duplicated colors saves 32 MB.
- **No GPU-side compaction.** Dead particles are respawned in place,
  so the dispatch count is constant. This is correct for the "continuous
  flow" model. A different use case (one-shot fireworks, missile
  trails) would want compaction; that's a future system, not this one.
- **`FreeFlyCamera` API.** The game sketch assumes the existing
  `iron::FreeFlyCamera` interface. Verify during implementation.

## Out of scope (deferred to future milestones)

- Public `iron::ComputePipeline` engine surface (add when a second
  consumer needs it — e.g., GPU skinning, GPU culling).
- Particle textures (procedural radial gradient is all the M10 demo
  uses).
- Sub-particle physics (collisions, sticky surfaces, etc.).
- Multiple particle systems per scene (the M10 demo uses one global
  system; the abstraction would already allow multiple, but it's
  untested).
- Vulkan-only post-processing (bloom over the additive output would
  make the particles glow beautifully, but it's a separate compute +
  render pass feature).
- Moving the compute dispatch into the per-frame command buffer
  (perf optimization; M10 takes the simple isolated-submit path).
- GPU skinning, GPU culling, compute-driven indirect draw — all
  future Vulkan-only features that share infrastructure with M10.
