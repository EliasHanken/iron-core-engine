# M17 — Vulkan Planar Reflection (Design Spec)

**Date:** 2026-05-27
**Milestone:** M17 (final Vulkan parity milestone)
**Status:** Design — awaiting implementation plan

## Goal

Bring planar reflection to the Vulkan backend at parity with the OpenGL backend, then port `games/03-showcase` to Vulkan as the visual validator. After M17 lands, the Vulkan backend reaches full OpenGL feature parity; the CMake default flips to Vulkan and OpenGL is marked deprecated.

## Context

`engine/render/Renderer.h` already declares `setReflectionPlane(normal, d)` and `disableReflectionPlane()`. The OpenGL backend implements both via `GLReflectionTarget` (1024² RGBA8 + 24-bit depth FBO), a mirrored view derived from `reflectionMatrix(plane)` in `engine/render/ReflectionPlane.{h,cpp}`, and `gl_ClipDistance[0]` to clip geometry on the wrong side of the plane. Two demos depend on it: `02-strandbound` (water at y=-3) and `03-showcase` (mirror floor at y=-0.1).

The Vulkan backend currently stubs both methods. No clip-distance device feature is enabled. The lit UBO has 6 descriptor bindings (M16): UBO, diffuse, normal, spec, shadow, cubemap.

## Non-Goals

- Porting `02-strandbound` to Vulkan (separate follow-up).
- Dynamic reflectivity per material at runtime beyond what M16 already supports.
- MSAA reflection RTT (single-sample matches OpenGL).
- Reflection of cubemap sky into the mirror (mirror reflects only world geometry, just like OpenGL).
- Removing OpenGL — that's the M17-follow-up "flip default backend" task, not part of M17.

## Architecture

### 1. `VkReflectionTarget` subsystem

New files: `engine/render/backends/vulkan/VkReflectionTarget.{h,cpp}`.

Mirrors `VkShadowMap` in shape, but with a color attachment in addition to depth:

- **Color image:** `VK_FORMAT_R8G8B8A8_UNORM`, 1024×1024, `USAGE_COLOR_ATTACHMENT_BIT | USAGE_SAMPLED_BIT`, VMA `GPU_ONLY`.
- **Depth image:** `VK_FORMAT_D32_SFLOAT`, 1024×1024, `USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`, VMA `GPU_ONLY`.
- **Render pass:** 2 attachments (color, depth). Color load=CLEAR, store=STORE, finalLayout=`SHADER_READ_ONLY_OPTIMAL`. Depth load=CLEAR, store=DONT_CARE, finalLayout=`DEPTH_STENCIL_ATTACHMENT_OPTIMAL`. Single subpass. **Exit subpass dependency** `0 → EXTERNAL`: `COLOR_ATTACHMENT_OUTPUT → FRAGMENT_SHADER`, `COLOR_ATTACHMENT_WRITE → SHADER_READ` (so the scene pass can sample the color texture safely).
- **Framebuffer:** color + depth views.
- **Sampler:** reuse the renderer's shared sampler (`CLAMP_TO_EDGE`, `LINEAR`), passed in at `init`. No new sampler created.

API:
```cpp
class VkReflectionTarget {
public:
    void init(VkContext& ctx, VmaAllocator allocator, VkSampler sharedSampler);
    void destroy(VkContext& ctx, VmaAllocator allocator);
    VkRenderPass renderPass() const { return renderPass_; }
    VkFramebuffer framebuffer() const { return framebuffer_; }
    VkDescriptorImageInfo descriptorImageInfo() const; // for binding 6
    uint32_t resolution() const { return 1024; }
    void beginPass(VkCommandBuffer cb, const float clearColor[4]) const;
    void endPass(VkCommandBuffer cb) const;
};
```

### 2. Device feature enable: `shaderClipDistance`

In `VkContext::createDevice`, the current `VkDeviceCreateInfo` passes no `pEnabledFeatures` at all. Change:

```cpp
VkPhysicalDeviceFeatures features{};
features.shaderClipDistance = VK_TRUE;
info.pEnabledFeatures = &features;
```

`shaderClipDistance` is core Vulkan 1.0, supported on every desktop GPU. Required for `gl_ClipDistance[0]` to function in SPIR-V.

### 3. LitUbo growth (832 → 928 bytes, +96)

Append three fields after the M16 layout (cubemap reflection block):

```cpp
struct LitUbo {
    // ... M11-M16 fields (832 bytes) ...
    Mat4 reflectionViewProj;   // +64  scene pass: identity; reflection pass: P * (V * mirror)
    Vec4 reflectionParams;     // +16  .x = useReflectionPlane (0/1), .y = screenW, .z = screenH, .w = 0
    Vec4 clipPlane;            // +16  (normal.xyz, -d) for reflection pass; ignored in scene pass
};                             // = 928
```

`reflectionViewProj` is unused in the scene fragment shader (sampling is screen-space via `gl_FragCoord`). It exists in the UBO purely so the reflection pass can reuse the same UBO struct without a separate uniform layout.

### 4. Descriptor set layout: 6 → 7 bindings

| Binding | Type | M16 use | M17 change |
| ------- | ---- | ------- | ---------- |
| 0 | UNIFORM_BUFFER | LitUbo | unchanged |
| 1 | COMBINED_IMAGE_SAMPLER | diffuse | unchanged |
| 2 | COMBINED_IMAGE_SAMPLER | normal | unchanged |
| 3 | COMBINED_IMAGE_SAMPLER | spec | unchanged |
| 4 | COMBINED_IMAGE_SAMPLER | shadow | unchanged |
| 5 | COMBINED_IMAGE_SAMPLER | cubemap | unchanged |
| 6 | COMBINED_IMAGE_SAMPLER | — | **NEW: reflection RTT** |

Pool capacity (`VkFrameRing`): sampler bindings 5× → **6×** `kMaxDescriptorSetsPerFrame`.

`recordSceneDraw` writes 7 descriptors: `imgInfos[6]`, `writes[7]`. When no reflection plane is active, binding 6 is wired to a 1×1 black fallback texture (same pattern as M16's cubemap black fallback) so the descriptor write is always valid.

### 5. Reflection-pass pipeline (separate from scene pipeline)

A single shared reflection pipeline created once at `VulkanRenderer::init()` (not per-shader). Rationale: the reflection pass uses one fixed simple shader regardless of which scene shader the user's `createShader` registered.

**Vertex shader (GLSL 450):**
```glsl
#version 450
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;

layout(set=0, binding=0) uniform U {
    mat4 mvp;
    mat4 model;
    // ... (we only use mvp and clipPlane from the reflection-pass UBO)
} u;
// Match the full LitUbo layout so the same descriptor write works.
// We reference only the fields we need.

layout(location=0) out vec2 vUV;
layout(location=1) out vec3 vNormal;

void main() {
    vec4 worldPos = u.model * vec4(aPos, 1.0);
    vUV = aUV;
    vNormal = mat3(u.model) * aNormal;
    gl_ClipDistance[0] = dot(worldPos.xyz, u.clipPlane.xyz) + u.clipPlane.w;
    gl_Position = u.reflectionViewProj * worldPos;
}
```

**Fragment shader:**
```glsl
#version 450
layout(location=0) in vec2 vUV;
layout(location=1) in vec3 vNormal;
layout(set=0, binding=1) uniform sampler2D uDiffuse;
layout(location=0) out vec4 outColor;
void main() {
    vec3 n = normalize(vNormal);
    // simple lambert with hardcoded sun-ish dir
    float diffuse = max(dot(n, normalize(vec3(0.3, 1.0, 0.2))), 0.0);
    vec4 texel = texture(uDiffuse, vUV);
    outColor = vec4(texel.rgb * (0.3 + 0.7 * diffuse), texel.a);
}
```

Pipeline state:
- Cull mode = `VK_CULL_MODE_NONE` (mirroring inverts winding; cheaper than reversing winding per-pipeline)
- Depth test = LESS, depth write = TRUE
- Color attachment = single RGBA8, blend disabled
- Render pass = `VkReflectionTarget::renderPass()`
- Descriptor layout = lightweight 2-binding (UBO + diffuse) layout, **separate** from scene's 7-binding layout

### 6. Frame flow (`VulkanRenderer::endFrame`)

```
1. Shadow pass        (M14 — unchanged)
2. Reflection pass    (NEW)
   - if reflectionPlane_.has_value():
       - reflectionTarget_.beginPass(cb, sceneClearColor)
       - bind reflectionPipeline_
       - viewport 1024² + scissor 1024²
       - for each call in sceneDraws_:
           - if call.material.useReflectionPlane: continue  // skip the mirror itself
           - upload mini-UBO (model, mirroredViewProj, clipPlane)
           - bind binding-1 diffuse texture (or fallback)
           - vkCmdBindVertexBuffers + vkCmdBindIndexBuffer + vkCmdDrawIndexed
       - reflectionTarget_.endPass(cb)
3. Scene pass         (existing — binding 6 now wired to reflectionTarget_ when plane active, else 1x1 black)
4. Deferred extras    (existing)
5. Debug lines        (existing)
6. HUD                (existing)
```

The reflection pass slots between shadow and scene to match the OpenGL ordering and ensure the color attachment's `SHADER_READ_ONLY_OPTIMAL` layout transition completes before the scene pass samples it.

### 7. Scene fragment shader update

Inside the existing M16 reflectivity branch:

```glsl
vec3 reflectColor;
if (useReflectionPlane > 0.5) {
    vec2 ndc = gl_FragCoord.xy / vec2(screenW, screenH);
    reflectColor = texture(uReflection, ndc).rgb;
    lit = mix(lit, reflectColor, reflectivity);
} else if (reflectivity > 0.0) {
    // existing M16 cubemap path
    vec3 V = normalize(cameraPos - vWorldPos);
    vec3 R = reflect(-V, normalize(vNormal));
    reflectColor = texture(uSkyCubemap, R).rgb;
    lit = mix(lit, reflectColor, reflectivity);
}
```

`useReflectionPlane`, `screenW`, `screenH` come from `reflectionParams`. `uReflection` is `sampler2D` at binding 6.

### 8. Demo port — `games/03-showcase`

- Add a `--backend` arg parser (mirrors `01-spinning-cube` and `07-net-shooter`); default to `vulkan`.
- Inline-embed both scene vertex/fragment shaders as `#version 450` SPIR-V-compatible strings alongside the existing `#version 330` strings, selected by backend.
- Vulkan scene shader picks up the M16 cubemap reflection block AND the new M17 planar reflection block (both already present in the OpenGL shader).
- Visual validator: mirror floor at y=-0.1 reflects the rotating sphere + metallic cylinder + sunset skybox.

## Risks

| Risk | Mitigation |
| ---- | ---------- |
| Reflection RTT square aspect ≠ scene 16:9 | Use the scene's projection unmodified (matches GL). The reflection RTT covers the full mirror surface; sampling is projective via `gl_FragCoord / screenSize`. |
| Triangle winding inversion from mirror | Reflection pipeline uses `CULL_MODE_NONE`. |
| Sampler descriptor pool exhaustion | Bump 5× → 6× `kMaxDescriptorSetsPerFrame` in `VkFrameRing`. |
| `shaderClipDistance` not advertised on a device | Vulkan 1.0 core feature, present on every desktop GPU we target (NVIDIA/AMD/Intel/llvmpipe). No runtime check needed; if absent, pipeline creation fails loudly. |
| Layout transition race shadow → reflection → scene | Each pass owns its own render pass with explicit exit subpass dependency. No manual barriers needed. |
| Mini-UBO for reflection pass differs from scene UBO | Reuse the full 928-byte LitUbo layout for both; the reflection pipeline's descriptor layout is a 2-binding *subset* (UBO + diffuse) of the same buffer. Same buffer slice, different bindings — no extra allocator pressure. |

## Tasks (subagent-friendly chunks)

1. **VkReflectionTarget subsystem** — color+depth RTT, render pass with exit dependency, framebuffer, descriptorImageInfo API. Standalone — no integration yet. Tested via init/destroy lifecycle on the Vulkan device.

2. **Device feature `shaderClipDistance`** + **reflection pipeline** — flip `pEnabledFeatures` in `VkContext::createDevice`. Build the shared reflection pipeline (separate 2-binding descriptor layout, `CULL_MODE_NONE`, reflection render pass). Standalone — pipeline created but not yet recorded into a frame.

3. **Atomic integration** — `setReflectionPlane`/`disableReflectionPlane` un-stubbed; `endFrame` records the reflection pass between shadow and scene; LitUbo grows (832 → 928 bytes); descriptor layout 6 → 7; `recordSceneDraw` writes binding 6; scene fragment shader gets the planar branch; pool capacity bump.

4. **Showcase Vulkan port** + docs — `--backend` arg + inline GLSL 450 shaders + default `--backend vulkan`; append M17 section to `docs/M_VULKAN_PARITY.md` (or wherever M16's writeup landed).

## Verification

- **CI green** on Windows MSVC build.
- **Visual:** `.\build-vk\games\03-showcase\Debug\showcase.exe --backend vulkan` shows the mirror floor reflecting the rotating sphere + cylinder, with the sunset cubemap above. Side-by-side against the OpenGL build should be visually similar (within RTT resolution / shader-simplification differences).
- **Regression:** existing Vulkan demos (`01-spinning-cube`, `07-net-shooter`) continue to render correctly. Net-shooter still shows M15 fog + M16 sunset skybox.

## Follow-ups (NOT in M17)

- Flip CMake default backend to Vulkan; mark OpenGL deprecated in README.
- Port `02-strandbound` to Vulkan.
- Begin physics overhaul (per user roadmap).
