# M14 — Vulkan shadow map

**Status:** design approved 2026-05-26.

**Direction context:** Vulkan parity track. M14 is the first multi-pass
Vulkan feature. It also restructures the per-frame flow from
"record-as-you-go" to "queue-and-replay", because rendering the scene
twice (once for shadow depth, once for the lit pass that samples that
depth) requires the engine to know all draws before either pass begins.
After M14, the directional sun casts soft 3×3-PCF shadows, matching the
OpenGL backend feature.

## Goals

1. Restructure VulkanRenderer's frame flow so that `submit()` queues
   draws into a `sceneDraws_` vector instead of recording them
   immediately; `particles.render()`, `flushDebugLines`, and (already)
   `drawHud` also defer; `endFrame` orchestrates the entire multi-pass
   sequence.
2. Add a `VkShadowMap` subsystem (depth image + framebuffer + depth-only
   render pass + depth-only graphics pipeline + sampler) that records
   the shadow pass before the scene pass.
3. Extend `LitUbo` from 224 to 288 bytes — appends `lightViewProj` and
   repurposes the previously-padding `materialParams.w` for the shadow
   bias.
4. Grow the lit-pass descriptor set layout from 4 bindings to 5 (adds
   the shadow sampler at binding 4).
5. Update spinning-cube + net-shooter Vulkan shaders to compute
   `vLightSpacePos` (vertex) and sample the shadow map with 3×3 PCF
   (fragment).
6. Implement `VulkanRenderer::setShadowBounds` (was a stub since M9).
7. Compute `pendingLightViewProj_` in `beginFrame` from the sun
   direction + shadow center/radius, matching the OpenGL
   `computeLightViewProj` math.

## Scope

### In

- `engine/render/backends/vulkan/VkShadowMap.h/.cpp` — new subsystem.
- `LitUbo` extension to 288 bytes (lightViewProj + shadowBias in
  materialParams.w).
- Descriptor set layout grown from 4 to 5 bindings (`VkShader.cpp`).
- Descriptor pool sampler capacity bumped from `3 *
  kMaxDescriptorSetsPerFrame` to `4 * kMaxDescriptorSetsPerFrame`
  (`VkFrameRing.cpp`).
- `VulkanRenderer::submit()` queues into `sceneDraws_` instead of
  recording.
- `VkParticleSystem::render` enqueues a deferred callback via a new
  `VulkanRenderer::enqueueDeferredScenePass` API; `VulkanRenderer::endFrame`
  drains the callback queue inside the scene pass. `VkDebugLines` is
  deferred via VulkanRenderer's own pending state (it's already a
  renderer-owned subsystem).
- `VulkanRenderer::setShadowBounds` real implementation.
- `VulkanRenderer::endFrame` orchestrates: shadow pass → image barrier
  → scene pass → external subsystems → end + submit + present.
- Spinning-cube + net-shooter Vulkan shaders rewritten with
  `vLightSpacePos` + `shadowFactor()` PCF.
- Net-shooter startup warning updated.

### Out (deferred to later milestones)

- Point lights (M15).
- Fog (M15).
- Cubemap skybox + cubemap reflection (M16).
- Planar reflection (M17).
- Per-axis UV scale (`Vec2 uvScale`) — backlog from M13.
- Shadow map resolution config — hardcoded 2048×2048 in M14; can be
  exposed later if a game needs lower memory or higher detail.
- Cascaded shadow maps — sphere-bounded ortho is fine for arena-scale
  scenes.
- VSM / ESM / EVSM — 3×3 PCF is good enough for parity with OpenGL.

## Architecture

### Frame-flow restructure

The defer-and-replay model becomes the canonical Vulkan frame flow.
Each external subsystem captures the state it needs at call time and
records into the active command buffer only when called by `endFrame`.

```
beginFrame:
    acquire image (handle skipFrame_ on OUT_OF_DATE_KHR)
    begin cmd buffer (no render pass yet)
    pendingClear_, pendingView_, pendingProjection_ ← args
    pendingSunDir_, pendingSunColor_, pendingAmbient_ ← args
    pendingCameraPos_ ← extractCameraPos(view)
    pendingLightViewProj_ ← computeLightViewProj(pendingSunDir_,
                                                 pendingShadowCenter_,
                                                 pendingShadowRadius_)
    sceneDraws_.clear()
    pendingParticleEnabled_ = false
    debugLines_.queue is already cleared at flushDebugLines call time

submit(call):
    sceneDraws_.push_back(call)

particles.render(view, projection):
    renderer_->enqueueDeferredScenePass(
        [this, view, projection](VkCommandBuffer cb) {
            recordRender(cb, view, projection);  // existing M10 logic
        });

flushDebugLines(view, projection):
    pendingDebugView_ = view
    pendingDebugProj_ = projection
    pendingDebugFlush_ = true

drawHud(batch, fbW, fbH):
    pendingHudBatch_ = batch
    pendingHudW_ = fbW, pendingHudH_ = fbH
    pendingHudValid_ = true

endFrame:
    if skipFrame_: frames_.advance(); return

    cb = current cmd buffer

    // === Pass 1: shadow ===
    shadowMap_.record(cb, ctx.device(), frames_, meshes_,
                     pendingLightViewProj_, sceneDraws_)
        // internally: begin shadow render pass, set viewport=2048,
        //             bind depth-only pipeline, replay sceneDraws_
        //             with depth-only descriptor, end render pass,
        //             insert image barrier → SHADER_READ_ONLY_OPTIMAL

    // === Pass 2: scene ===
    begin scene render pass (existing scenePass)
    setSceneViewport (Y-flipped, swapchain size)

    for call in sceneDraws_:
        existing M13 submit logic — bind scene pipeline, allocate
        descriptor set, write 5 descriptors (UBO + 3 textures +
        shadow sampler), bind buffers, drawIndexed

    for fn in deferredScenePass_: fn(cb)
    deferredScenePass_.clear()

    if pendingDebugFlush_:
        debugLines_.record(cb, device, frames_, pendingDebugView_,
                          pendingDebugProj_)
        pendingDebugFlush_ = false

    if pendingHudValid_:
        hud_.record(cb, device, frames_, textures_, pendingHudBatch_,
                    pendingHudW_, pendingHudH_)
        pendingHudValid_ = false

    end scene render pass
    end cmd buffer
    submit + present
```

### `VkShadowMap`

New files. Owns the shadow image, framebuffer, render pass, depth-only
pipeline, sampler.

```cpp
class VkShadowMap {
public:
    static constexpr int kResolution = 2048;
    // VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE means out-of-bounds samples
    // read as 1.0 (fully lit), matching the OpenGL CLAMP_TO_BORDER
    // + white border setup.

    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // Records the entire shadow pass into the active command buffer.
    // Begins the depth-only render pass, replays sceneDraws via the
    // depth-only pipeline, ends the render pass, and inserts the
    // image-layout barrier so the lit pass can sample.
    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                VkMeshStore& meshes,
                const Mat4& lightViewProj,
                const std::vector<DrawCall>& draws);

    VkImageView depthView() const { return depthView_; }
    VkSampler   sampler()   const { return sampler_; }

private:
    struct LightUbo { float lightModelViewProj[16]; };  // per-draw

    bool ok_ = false;
    VkImage         depthImage_  = VK_NULL_HANDLE;
    VmaAllocation   depthAlloc_  = VK_NULL_HANDLE;
    VkImageView     depthView_   = VK_NULL_HANDLE;
    VkFramebuffer   framebuffer_ = VK_NULL_HANDLE;
    VkRenderPass    renderPass_  = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline    pipeline_    = VK_NULL_HANDLE;
    VkSampler       sampler_     = VK_NULL_HANDLE;
};
```

Depth-only vertex shader (inline GLSL 450):

```glsl
#version 450
layout(location = 0) in vec3 aPos;
// Other vertex attrs declared but unused — the shared Vertex format
// has 4 attributes; we read only aPos.
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LightUbo {
    mat4 lightModelViewProj;
} u;

void main() {
    gl_Position = u.lightModelViewProj * vec4(aPos, 1.0);
}
```

No fragment shader (depth-only pass).

Render pass: single depth attachment with format D32_SFLOAT,
loadOp=CLEAR, storeOp=STORE, initialLayout=UNDEFINED,
finalLayout=SHADER_READ_ONLY_OPTIMAL. One subpass, no color
attachments, no input attachments. External→0 dependency on
EARLY_FRAGMENT_TESTS_BIT.

Pipeline: depth test on, depth write on, depth compare LESS, no color
attachments (`pColorBlendState->attachmentCount = 0`), cull back-face
(prevent peter-panning by drawing back-faces into the shadow map),
front-face counter-clockwise, dynamic viewport+scissor.

Sampler: NEAREST filter, CLAMP_TO_BORDER address mode XYZ, border
color `VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE`.

### `LitUbo` extension (288 bytes)

```cpp
struct LitUbo {
    Mat4 mvp;             // 64
    Mat4 model;           // 64
    Mat4 lightViewProj;   // 64  M14 — for shadow map sampling
    Vec4 sunDir;          // 16
    Vec4 sunColor;        // 16
    Vec4 ambient;         // 16
    Vec4 emissive;        // 16
    Vec4 cameraPos;       // 16
    Vec4 materialParams;  // 16  x=uvScale, y=specPower, z=reflectivity,
                          //     w=shadowBias  ← M14 repurposes padding
};
static_assert(sizeof(LitUbo) == 288, "LitUbo std140 layout");
```

`materialParams.w` is per-DRAW but `shadowBias` is per-FRAME; we still
upload it per draw (cheap, simpler than a second UBO binding). The
value is a small constant like `0.002f`, computed in `submit` from a
new `pendingShadowBias_` private field (default 0.002f).

### Descriptor set layout: 5 bindings

| Binding | Type | Stage | Purpose |
|---|---|---|---|
| 0 | UNIFORM_BUFFER | VS+FS | LitUbo |
| 1 | COMBINED_IMAGE_SAMPLER | FS | Diffuse |
| 2 | COMBINED_IMAGE_SAMPLER | FS | Normal |
| 3 | COMBINED_IMAGE_SAMPLER | FS | Specular |
| 4 | COMBINED_IMAGE_SAMPLER | FS | Shadow (NEW) |

In `VkShader.cpp` the hardcoded `bindings[]` array grows from 4 to 5
entries; `bindingCount = 5`. In `VkFrameRing.cpp`'s pool sizes:
`{COMBINED_IMAGE_SAMPLER, 4 * kMaxDescriptorSetsPerFrame}`
(= 512 / frame).

### `VulkanRenderer::submit` (now a replay function)

The current M13 submit body becomes a private helper
`recordSceneDraw(cb, call)` invoked by endFrame's scene-pass loop. The
public `submit` does only:

```cpp
void VulkanRenderer::submit(const DrawCall& call) {
    if (skipFrame_) return;
    sceneDraws_.push_back(call);
}
```

`recordSceneDraw` differs from M13 only in the additional shadow
sampler write (writes[4]) and the larger LitUbo population
(`ubo.lightViewProj = pendingLightViewProj_;`,
`ubo.materialParams.w = pendingShadowBias_;`).

### `VulkanRenderer::setShadowBounds`

Drops the `warnOnce` stub. Stores center + radius in
`pendingShadowCenter_` / `pendingShadowRadius_`. `beginFrame` computes
`pendingLightViewProj_` from these (matching the OpenGL math —
lookAt from `center - dir * 2r`, orthographic
`[-r, r]² × [r/2, 3.5r]`).

A new private helper `computeLightViewProj(...)` lives in
`VulkanRenderer.cpp`'s anonymous namespace alongside
`extractCameraPos`.

### Shader updates (spinning-cube + net-shooter, identical)

Vertex shader gains the LitUbo `lightViewProj` field and outputs
`vLightSpacePos` at location 4:

```glsl
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    vTangent = mat3(u.model) * aTangent;
    vUV = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
```

Fragment shader gains the shadow sampler at binding 4 and a
`shadowFactor()` helper:

```glsl
#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vLightSpacePos;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp; mat4 model; mat4 lightViewProj;
    vec4 sunDir; vec4 sunColor; vec4 ambient; vec4 emissive;
    vec4 cameraPos; vec4 materialParams;
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;
layout(set = 0, binding = 4) uniform sampler2D uShadowMap;

float shadowFactor(vec4 lightSpacePos, float bias) {
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    // Vulkan clip-space Z is already [0,1]; only remap XY.
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (proj.z > 1.0) return 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = texture(uShadowMap, uv + vec2(x, y) * texel).r;
            sum += (proj.z - bias > stored) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

void main() {
    float uvScale   = u.materialParams.x;
    float specPower = u.materialParams.y;
    float bias      = u.materialParams.w;
    vec2 uv = vUV * uvScale;

    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 tangentNormal = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
    vec3 perturbedN = normalize(TBN * tangentNormal);

    vec3 L = -normalize(u.sunDir.xyz);
    vec3 V = normalize(u.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float diffuse  = max(dot(perturbedN, L), 0.0);
    float spec     = pow(max(dot(perturbedN, H), 0.0), specPower);
    float specMask = texture(uSpecularMap, uv).r;
    float shadow   = shadowFactor(vLightSpacePos, bias);

    vec3 lighting = u.sunColor.xyz * (diffuse * shadow + spec * specMask * shadow)
                  + u.ambient.xyz;
    vec3 diff = texture(uDiffuse, uv).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;
    outColor = vec4(lit, 1.0);
}
```

Both games share these shaders identically.

### Net-shooter startup warning update

```cpp
iron::Log::warn("net-shooter Vulkan path: sun + ambient + emissive "
                "+ normal/spec + shadow map (Blinn-Phong, 3x3 PCF) lit. "
                "Still missing point lights, fog, cubemap reflections. "
                "Full parity ships in future milestones.");
```

## Data flow per frame

```
1. game.beginFrame(...)
   └── VulkanRenderer stores pending state, computes
       pendingCameraPos_ and pendingLightViewProj_; clears sceneDraws_
2. game.submit(call) × N
   └── sceneDraws_.push_back(call)
3. game.particles.tick(dt) + particles.render(view, proj)
   └── stores pendingParticleView_/Proj_; pendingParticleEnabled_=true
4. game.drawLine × M + flushDebugLines(view, proj)
   └── stores pendingDebugView_/Proj_; pendingDebugFlush_=true
5. game.drawHud(batch, w, h)
   └── stores pendingHudBatch_ etc.; pendingHudValid_=true
6. game.endFrame()
   ├── shadowMap_.record(cb, ctx.device(), frames_, meshes_,
   │                     pendingLightViewProj_, sceneDraws_)
   │       internally: render pass with depth-only pipeline + barrier
   ├── begin scene render pass + viewport
   ├── for call in sceneDraws_: recordSceneDraw(cb, call)
   ├── particles.recordRender(cb, ...) if enabled
   ├── debugLines.record(cb, ...) if pending
   ├── hud.record(cb, ...) if pending
   └── end render pass + submit + present
```

## Error handling

- `setShadowBounds` not called before `beginFrame`: default center
  `{0,0,0}` and radius `20.0f` give a reasonable fallback that covers
  spinning-cube + small scenes. (Game must call it for larger scenes;
  the warning is in the docs, not at runtime.)
- Shadow image creation failure: aborts via VK_CHECK in `init`. The
  M9 pattern stands — Vulkan errors are fatal.
- Per-draw `LightUbo` allocation overflow: `frames_.allocateUbo`
  asserts. The shadow pass allocates one LightUbo per draw; at 64
  bytes/UBO and 256-byte alignment, 256 KB / 256 = 1024 draws/frame
  budget. Net-shooter has ~40. Spinning-cube has 1. Headroom is
  3 orders of magnitude.

## Testing

### Unit tests

No new unit tests. Pipeline + shader work.

A small future addition could be `test_compute_light_view_proj.cpp`
that exercises the math helper, but for M14 we'll rely on the visual
smoke check.

### Smoke tests (manual)

After implementation:

- **Spinning-cube on Vulkan**: cube casts a shadow on itself (only
  visible because the cube has facets — top face lit, bottom face
  dark, plus self-shadowing on overhangs). Subtle but verifiable.
- **Net-shooter on Vulkan**: walls cast shadows onto the floor and
  onto each other. Player cubes cast shadows. Strong sun angle (the
  scene's existing `sun.direction = {-0.4, -1.0, -0.3}` gives clear
  diagonal shadows).
- **Particle-storm on Vulkan**: unchanged (its render path doesn't
  touch the new shadow flow).
- All three games on OpenGL: unchanged.
- Vulkan validation layers run clean (no warnings about descriptor
  set count, image layouts, render pass dependencies).

### CI

35/35 tests pass on both backends.

## Risks

- **Frame-flow restructure is bigger than the visual feature.** This
  is the most impactful change of M14 — touches submit, particles,
  debug-lines, hud, endFrame. Mitigation: each subsystem's record
  path already exists; we're calling it from a different place. The
  shapes are familiar.
- **VkParticleSystem split between tick (compute) and render (defer).**
  The compute dispatch in `tick(dt)` stays where it is; only the
  `render(view, proj)` call changes — now wraps the existing
  recording logic in a `std::function` and registers it with
  VulkanRenderer's deferred queue. Verify during implementation that
  `tick` doesn't internally call `render`.
- **Shadow image layout transitions.** Must be DEPTH_ATTACHMENT_OPTIMAL
  during pass 1 (handled by render-pass attachment finalLayout +
  subpass dependency) and SHADER_READ_ONLY_OPTIMAL during pass 2
  (final layout of pass 1 = SHADER_READ_ONLY_OPTIMAL). No explicit
  `vkCmdPipelineBarrier` needed if we structure the render pass
  dependencies correctly. Validation layers will catch mistakes.
- **Peter-panning vs shadow acne tradeoff.** `bias = 0.002f` and
  cull-back-face during shadow pass is the OpenGL combination. We
  match it. If real-world tweaking proves it wrong on D32_SFLOAT
  precision, adjust during smoke testing.
- **Spinning-cube `setShadowBounds` call.** The game doesn't currently
  call it (no shadow on Vulkan to bound). With M14 the default
  bounds (center=0, radius=20) work for the cube. Net-shooter calls
  `setShadowBounds({0,0,0}, 30.0f)` — verified in the existing
  source.
- **Vulkan negative-height viewport interaction with shadow UVs.**
  Negative-height viewport affects clip-Y → framebuffer-Y but NOT
  shadow texture sampling (we're reading depth from a normal-Y
  framebuffer that the shadow pass writes). The shadow render pass
  uses standard (non-flipped) viewport because we're rendering INTO
  the shadow image, and the lit shader's `proj.xy * 0.5 + 0.5`
  matches Vulkan's UV space. Should be safe.

## File / module changes

### New files

- `engine/render/backends/vulkan/VkShadowMap.h`
- `engine/render/backends/vulkan/VkShadowMap.cpp`

### Modified files

- `engine/render/backends/vulkan/VulkanRenderer.h` — adds
  `pendingShadowCenter_`, `pendingShadowRadius_`, `pendingShadowBias_`,
  `pendingLightViewProj_`,
  `pendingDebugView_/Proj_/Flush_`, `pendingHudBatch_/W_/H_/Valid_`,
  `sceneDraws_`, `shadowMap_`, `deferredScenePass_`
  (vector of `std::function<void(VkCommandBuffer)>`). Declares
  `recordSceneDraw(cb, call)` and public
  `enqueueDeferredScenePass(std::function<void(VkCommandBuffer)>)`.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — extends
  `LitUbo` to 288 bytes; adds `computeLightViewProj` helper; replaces
  `submit` body with queue-only; rewrites `endFrame` as the
  orchestrator; implements `setShadowBounds`; existing M13
  submit-body becomes `recordSceneDraw` with shadow-binding added;
  particles/debugLines/hud calls become deferred.
- `engine/render/backends/vulkan/VkShader.cpp` — descriptor set
  layout grows from 4 to 5 bindings.
- `engine/render/backends/vulkan/VkFrameRing.cpp` — COMBINED_IMAGE_SAMPLER
  pool size bumped to `4 * kMaxDescriptorSetsPerFrame`.
- `engine/render/backends/vulkan/VkParticleSystem.cpp` — `render(view,
  proj)` becomes a thin wrapper that captures view+proj into a
  `std::function` and registers it via
  `renderer_->enqueueDeferredScenePass(...)`. A new private helper
  `recordRender(cb, view, proj)` holds the existing M10 body. The
  compute dispatch in `tick(dt)` is unchanged.
- `engine/render/backends/vulkan/VkDebugLines.h/.cpp` — no API
  changes. `VulkanRenderer::flushDebugLines` just changes WHERE it
  calls `debugLines_.record(...)` (now from endFrame, not inline).
- `engine/CMakeLists.txt` — register `VkShadowMap.cpp` under the
  Vulkan source list.
- `games/01-spinning-cube/main.cpp` — Vulkan shaders rewritten;
  add `renderer.setShadowBounds({0,0,0}, 5.0f)` after createRenderer.
- `games/07-net-shooter/main.cpp` — Vulkan shaders rewritten;
  warning string updated.
- `docs/engine/rhi-abstraction.md` — appended M14 section.

## Open questions

None blocking.
