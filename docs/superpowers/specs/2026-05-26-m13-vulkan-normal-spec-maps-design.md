# M13 — Vulkan normal maps + specular maps + UV scale

**Status:** design approved 2026-05-26.

**Direction context:** Vulkan parity track committed (M13-M17). M13
adds the per-material maps the OpenGL lit pass already supports:
normal map (TBN-perturbed surface normal), specular map (mask + power
for Blinn-Phong highlights), plus the `uvScale` material parameter
that controls texture tiling. After M13 the Vulkan-side
spinning-cube + net-shooter walls match the OpenGL versions for the
non-multi-pass parts of lit shading.

## Goals

1. Extend `LitUbo` (M12) with `cameraPos` and `materialParams`
   (uvScale, specPower, reflectivity).
2. Extend the descriptor set layout from 2 bindings to 4 (adds
   normal + specular samplers).
3. Update `VulkanRenderer::submit` to write all three samplers per
   draw (diffuse + normal + spec) with built-in fallbacks
   (`flatNormalTexture`, `noSpecularTexture`).
4. Rewrite the spinning-cube + net-shooter Vulkan shaders to
   compute TBN, sample the normal map, sample the spec map, and add
   Blinn-Phong specular to the existing Lambertian + ambient +
   emissive output.
5. Bump the per-frame descriptor pool's `COMBINED_IMAGE_SAMPLER`
   capacity to match 3 samplers per set.

## Scope

### In

- New `Vec4 cameraPos` and `Vec4 materialParams` on `LitUbo`.
- New `pendingCameraPos_` private state on `VulkanRenderer`; extracted
  from `view` at beginFrame.
- `VkShader.cpp` descriptor set layout grows from 2 to 4 bindings.
- `VkFrameRing.cpp` descriptor pool bumps
  `COMBINED_IMAGE_SAMPLER` size from `kMaxDescriptorSetsPerFrame`
  (=128) to `3 * kMaxDescriptorSetsPerFrame` (=384).
- `VulkanRenderer::submit` writes three image descriptors per
  draw with the appropriate fallback for invalid handles.
- Spinning-cube Vulkan shaders gain TBN + normal sample + specular
  + uvScale-applied UVs.
- Net-shooter Vulkan shaders same.
- Net-shooter startup warning updated.

### Out (deferred)

- Point lights (M15).
- Fog (M15).
- Shadow map sampling (M14).
- Cubemap skybox + cubemap reflection sampling (M16).
- Planar reflection sampling (M17).
- Wiring of actual normal/spec map textures into game materials.
  M13 ships the plumbing; if a game's `material.normalMap` is
  `kInvalidHandle`, the flat-normal fallback means visible output
  is the same as M12. A separate small commit (not part of M13's
  task list) can wire net-shooter's wall material to use an
  existing CC0 normal map if visible improvement on net-shooter
  is wanted.

## Architecture

### Final `LitUbo` (224 bytes)

```cpp
struct LitUbo {
    Mat4 mvp;             // 64
    Mat4 model;           // 64
    Vec4 sunDir;          // 16  xyz direction; w padding
    Vec4 sunColor;        // 16  xyz color; w padding
    Vec4 ambient;         // 16  xyz pre-multiplied ambient; w padding
    Vec4 emissive;        // 16  xyz per-draw emissive; w padding
    Vec4 cameraPos;       // 16  xyz world-space camera; w padding
    Vec4 materialParams;  // 16  x=uvScale, y=specPower, z=reflectivity, w=padding
};
static_assert(sizeof(LitUbo) == 224, "LitUbo layout");
```

Total: 224 bytes. std140-safe (all members are mat4 (64-byte
aligned) or vec4 (16-byte aligned)).

### Camera position extraction

`view` is a column-major rigid transform `[R | t; 0 0 0 1]` where
`R` is 3×3 rotation and `t` is the world-to-view translation. The
inverse maps view-space origin back to world space:

```
cameraWorldPos = -R^T * t
```

`R^T` is the transpose of the upper-left 3×3 of view; `t` is the
fourth column. In our column-major `Mat4` storage (`m[col*4+row]`),
`R[r][c] = view.at(r, c)` for r,c in 0..2; `t[r] = view.at(r, 3)`.

A new helper `Vec3 extractCameraPos(const Mat4& view)` lives in
`engine/render/backends/vulkan/VulkanRenderer.cpp`'s anonymous
namespace alongside `LitUbo`. Called once per `beginFrame`.

### `VulkanRenderer` changes

In `VulkanRenderer.h`, add one private field next to the M12 pending
sun/ambient block:

```cpp
Vec3 pendingCameraPos_ = {0.0f, 0.0f, 0.0f};
```

In `beginFrame`, after the existing M12 sun-light assignments:

```cpp
pendingCameraPos_ = extractCameraPos(view);
```

In `submit`, extend the `LitUbo` population (added to M12's block):

```cpp
ubo.cameraPos = Vec4{pendingCameraPos_.x, pendingCameraPos_.y, pendingCameraPos_.z, 0.0f};
ubo.materialParams = Vec4{
    call.material.uvScale,
    call.material.specPower,
    call.material.reflectivity,
    0.0f
};
```

After the descriptor-set allocate, write three image descriptors
(replacing the current single-image write):

```cpp
// Diffuse — fallback to whiteTexture.
const auto& diffuse = textures_.has(call.material.texture)
    ? textures_.get(call.material.texture)
    : textures_.get(textures_.whiteTexture());

// Normal — fallback to flatNormalTexture.
const auto& normal = textures_.has(call.material.normalMap)
    ? textures_.get(call.material.normalMap)
    : textures_.get(textures_.flatNormalTexture());

// Spec — fallback to noSpecularTexture.
const auto& spec = textures_.has(call.material.specularMap)
    ? textures_.get(call.material.specularMap)
    : textures_.get(textures_.noSpecularTexture());

VkDescriptorImageInfo imgInfos[3];
// ... fill imgInfos[i] with each tex.view + tex.sampler + SHADER_READ_ONLY_OPTIMAL ...

VkWriteDescriptorSet writes[4]{};
// writes[0] = UBO (binding 0, existing)
// writes[1] = diffuse (binding 1)
// writes[2] = normal (binding 2)
// writes[3] = spec (binding 3)
vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
```

### `VkShader.cpp` descriptor set layout

Currently:

```cpp
VkDescriptorSetLayoutBinding bindings[2]{};
// binding 0: UBO, VS+FS
// binding 1: COMBINED_IMAGE_SAMPLER, FS
```

Becomes:

```cpp
VkDescriptorSetLayoutBinding bindings[4]{};
// binding 0: UBO, VS+FS
// binding 1: COMBINED_IMAGE_SAMPLER (diffuse), FS
// binding 2: COMBINED_IMAGE_SAMPLER (normal), FS
// binding 3: COMBINED_IMAGE_SAMPLER (spec), FS

dslInfo.bindingCount = 4;
```

### `VkFrameRing.cpp` pool size

Current pool sizes in `initFrame`:

```cpp
VkDescriptorPoolSize sizes[] = {
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxDescriptorSetsPerFrame},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxDescriptorSetsPerFrame},
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kMaxDescriptorSetsPerFrame},
};
```

Bump the sampler capacity:

```cpp
{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kMaxDescriptorSetsPerFrame},
```

Rationale: each lit-pass descriptor set now writes 3 samplers
(diffuse + normal + spec). The HUD pass writes 1 sampler. The
particle render pass writes 0 samplers (it uses an SSBO). For 128
sets/frame the max sampler usage is `3 * 128 = 384`.

### Shader updates (spinning-cube + net-shooter, identical structure)

Vertex shader (set=0 binding=0 UBO with the new layout; new outputs
vWorldPos + vTangent alongside vUV + vNormal):

```glsl
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;  // x=uvScale, y=specPower, z=reflectivity
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    vTangent = mat3(u.model) * aTangent;
    vUV = aUV;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
```

Fragment shader (3 samplers + TBN math + Blinn-Phong):

```glsl
#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;

void main() {
    float uvScale   = u.materialParams.x;
    float specPower = u.materialParams.y;
    vec2 uv = vUV * uvScale;

    // TBN.
    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);
    vec3 tangentNormal = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
    vec3 perturbedN = normalize(TBN * tangentNormal);

    // Sun light direction (point AT the light, not away).
    vec3 L = -normalize(u.sunDir.xyz);
    vec3 V = normalize(u.cameraPos.xyz - vWorldPos);
    vec3 H = normalize(L + V);

    float diffuse = max(dot(perturbedN, L), 0.0);
    float spec    = pow(max(dot(perturbedN, H), 0.0), specPower);
    float specMask = texture(uSpecularMap, uv).r;

    vec3 lighting = u.sunColor.xyz * (diffuse + spec * specMask) + u.ambient.xyz;
    vec3 diff = texture(uDiffuse, uv).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;
    outColor = vec4(lit, 1.0);
}
```

Both spinning-cube and net-shooter use the SAME Vulkan-branch
shader code. (Spinning-cube doesn't actually have a normal map
assigned, so `flatNormalTexture` falls back and `perturbedN == N`
— the visible output is the same as M12. Net-shooter's wall
materials likewise determine whether normal/spec maps actually
show up.)

### Net-shooter startup warning update

```cpp
iron::Log::warn("net-shooter Vulkan path: sun + ambient + emissive "
                "+ normal/spec maps (Blinn-Phong) lit. Still missing "
                "point lights, fog, shadows, cubemap reflections. "
                "Full parity ships in future milestones.");
```

## Data flow per frame

```
1. game.beginFrame(...)
   └── VulkanRenderer stores pendingView_, pendingProjection_,
       pendingSun*, pendingAmbient_, pendingCameraPos_ = extractCameraPos(view)
2. game.submit(call) × N
   └── VulkanRenderer builds LitUbo (224 bytes)
   └── frames_.allocateUbo writes it
   └── Allocate descriptor set; write 4 descriptors:
       binding 0 = UBO
       binding 1 = diffuse (fallback whiteTexture)
       binding 2 = normal (fallback flatNormalTexture)
       binding 3 = spec (fallback noSpecularTexture)
   └── vkCmdDrawIndexed
3. particles / debug-lines / hud / endFrame as before
```

## Error handling

- The new code follows the existing M10/M11/M12 patterns. No new
  failure modes.
- `extractCameraPos` is pure math; no I/O.
- Fallback textures are application-scoped (built in
  `VkTextureStore::init`); cannot become invalid mid-frame.

## Testing

### Unit tests

No new unit tests. Pipeline + shader work.

### Smoke tests (manual)

After implementation:

- **Spinning-cube on Vulkan**: identical to M12 (no normal/spec
  maps assigned; fallbacks make it look unchanged).
- **Spinning-cube on OpenGL**: unchanged.
- **Net-shooter on Vulkan**: identical to M12 unless wall materials
  also wire `normalMap`/`specularMap`. If they don't, M13 ships
  plumbing-only and a separate small commit (post-M13) wires them.
- **Net-shooter on OpenGL**: unchanged.
- **Particle-storm on Vulkan**: unchanged (it uses its own
  pipelines).
- Vulkan validation layers run clean (no warnings about descriptor
  set layout mismatch — the 4 bindings hardcoded in VkShader.cpp
  must match the 4 writes from submit).

### CI

35/35 tests pass under both `-DIRON_RENDER_BACKEND=opengl` and
`-DIRON_RENDER_BACKEND=vulkan`.

## Risks

- **Game material wiring**: M13 ships the Vulkan engine plumbing
  but most game `DrawCall`s leave `normalMap` / `specularMap` =
  `kInvalidHandle`. Visible change requires assigning maps. Calling
  this out explicitly in the milestone summary; a follow-up commit
  (or separate next milestone) can wire net-shooter walls.
- **TBN math edge cases**: zero-length tangent → division by zero
  in `normalize(vTangent)`. Won't happen with the engine's
  `appendBox` / `appendQuad` / `appendTube` (all generate non-zero
  tangents). Document for any future custom geometry.
- **std140 alignment for `materialParams`**: vec4 is naturally
  16-byte aligned. Packing 3 floats + 1 padding is std140-safe.
- **Camera-pos extraction correctness**: the formula
  `cameraPos = -R^T * t` assumes `view` is a pure rigid transform
  (rotation + translation only, no scale). The engine's view
  matrices come from `lookAt` / first-person controllers which
  are all rigid. Document the assumption in the helper's comment.

## File / module changes

### Modified files

- `engine/render/backends/vulkan/VulkanRenderer.h` — adds
  `pendingCameraPos_` private field.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — extends
  `LitUbo`; adds `extractCameraPos`; updates `beginFrame` and
  `submit`.
- `engine/render/backends/vulkan/VkShader.cpp` — descriptor set
  layout grows from 2 to 4 bindings.
- `engine/render/backends/vulkan/VkFrameRing.cpp` — descriptor pool
  COMBINED_IMAGE_SAMPLER capacity bumped to `3 *
  kMaxDescriptorSetsPerFrame`.
- `games/01-spinning-cube/main.cpp` — Vulkan shaders rewritten.
- `games/07-net-shooter/main.cpp` — Vulkan shaders rewritten;
  warning string updated.
- `docs/engine/rhi-abstraction.md` — appended M13 section.

### New files

None.

## Open questions

None blocking.
