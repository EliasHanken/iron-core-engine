# M16 — Vulkan cubemap skybox + cubemap reflection

**Status:** design approved 2026-05-27.

**Direction context:** Vulkan parity track. After M15 (point lights +
fog), M16 brings the remaining environment features that depend on
a cubemap texture: a procedural sky drawn behind the scene + a
reflection term in the lit shader that samples the same cubemap via
`reflect(viewDir, normal)`. Both features share the cubemap storage,
so they bundle naturally into one milestone. After M16, only planar
reflection (M17) remains for Vulkan-OpenGL parity.

## Goals

1. Add a `VkCubemapStore` subsystem (cubemap image upload, sampler,
   built-in 1×1×6 black fallback).
2. Add a `VkSkybox` subsystem (cube mesh + skybox pipeline, drawn
   first inside the scene render pass with the `gl_Position.xyww`
   z=1 trick).
3. Un-stub `VulkanRenderer::createCubemap` and `setSkybox`.
4. `endFrame` draws the skybox (if set) before scene geometry replay.
5. Lit pass descriptor set layout grows from 5 to 6 bindings (adds
   cubemap sampler at binding 5).
6. Spinning-cube + net-shooter Vulkan shaders gain a cubemap sample +
   reflection mix gated on `materialParams.z` (`reflectivity`).
7. Net-shooter startup warning updated.

## Scope

### In

- `engine/render/backends/vulkan/VkCubemap.h/.cpp` — new subsystem.
- `engine/render/backends/vulkan/VkSkybox.h/.cpp` — new subsystem.
- `VulkanRenderer::createCubemap` real implementation.
- `VulkanRenderer::setSkybox` stores the cubemap handle (`pendingSkybox_`).
- `VulkanRenderer::endFrame` draws the skybox inside the scene pass
  before geometry, IF a skybox is set.
- `VkShader.cpp` descriptor set layout grows from 5 to 6 bindings.
- `VkFrameRing.cpp` sampler pool capacity bumps from
  `4 * kMaxDescriptorSetsPerFrame` to `5 *
  kMaxDescriptorSetsPerFrame` (= 1280 / frame).
- `VulkanRenderer::recordSceneDraw` writes a 6th descriptor (cubemap
  sampler) per draw, sourcing from `pendingSkybox_` or the
  `blackCubemap()` fallback.
- Spinning-cube + net-shooter Vulkan fragment shaders gain a
  reflection block guarded by `materialParams.z > 0`.
- Net-shooter startup warning updated.
- `engine/CMakeLists.txt` registers both new `.cpp` files under
  the Vulkan branch.

### Out (deferred)

- Planar reflection (M17 — RTT pass with mirrored camera + clip plane).
- Reflectivity tuning on net-shooter game-side (separate fixup commit
  if visible reflection is desired post-merge).
- HDR / image-based lighting / mipmap-based roughness — not the
  current pipeline.

## Architecture

### `VkCubemapStore`

Mirrors the existing `VkTextureStore` pattern.

```cpp
struct VkCubemapResource {
    VkImage       image   = VK_NULL_HANDLE;
    VmaAllocation alloc   = VK_NULL_HANDLE;
    VkImageView   view    = VK_NULL_HANDLE;
    VkSampler     sampler = VK_NULL_HANDLE;  // shared with store
    std::uint32_t width   = 0;
    std::uint32_t height  = 0;
};

class VkCubemapStore {
public:
    bool init(VkContext& ctx);          // builds shared sampler + black fallback
    void destroyAll(VkContext& ctx);

    CubemapHandle createFromFaces(VkContext& ctx, int width, int height,
                                  const std::array<const unsigned char*, 6>& faces);

    CubemapHandle blackCubemap() const { return black_; }
    const VkCubemapResource& get(CubemapHandle h) const;
    bool has(CubemapHandle h) const;

private:
    void uploadFaces(VkContext& ctx, VkCubemapResource& res,
                     int width, int height,
                     const std::array<const unsigned char*, 6>& faces);

    std::unordered_map<CubemapHandle, VkCubemapResource> cubemaps_;
    CubemapHandle nextHandle_ = 1;
    VkSampler sharedSampler_ = VK_NULL_HANDLE;
    CubemapHandle black_ = kInvalidHandle;
};
```

**Image creation:**
- `VkImageCreateInfo::flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`
- `imageType = VK_IMAGE_TYPE_2D`
- `arrayLayers = 6`
- `format = VK_FORMAT_R8G8B8A8_SRGB` (matches `VkTextureStore`)
- `usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`

**Image view:**
- `viewType = VK_IMAGE_VIEW_TYPE_CUBE`
- `subresourceRange.layerCount = 6`

**Face upload (in `uploadFaces`):**
- One large staging buffer holding all 6 faces concatenated
  (`6 * width * height * 4` bytes)
- One `vkCmdCopyBufferToImage` with 6 `VkBufferImageCopy` regions,
  each targeting `baseArrayLayer = i` for `i = 0..5`
- Layout transitions: UNDEFINED → TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
- Submit + wait idle (one-shot, mirrors `VkTextureStore::uploadRgba`)

**Sampler:**
- `VK_FILTER_LINEAR` for min + mag
- `VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE` for U + V + W (cubemaps
  use seamless sampling on modern GPUs by default — no special flag
  needed in Vulkan)

**Built-in fallback:**
- `blackCubemap()` returns a handle to a 1×1×6 cubemap of all-black
  pixels (RGBA8 zeros). Created during `init()`. Used by the lit
  pass when `pendingSkybox_ == kInvalidHandle`.

### `VkSkybox`

```cpp
class VkSkybox {
public:
    bool init(VkContext& ctx, VkRenderPass scenePass);
    void destroy(VkContext& ctx);

    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                const VkCubemapResource& cubemap,
                const Mat4& view, const Mat4& projection);

private:
    struct SkyUbo { float viewProjection[16]; };  // 64 bytes

    bool ok_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline pipeline_ = VK_NULL_HANDLE;

    // Cube geometry (8 unique positions, 36 indices for 12 triangles).
    VkBuffer        vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation   vertexAlloc_  = VK_NULL_HANDLE;
    VkBuffer        indexBuffer_  = VK_NULL_HANDLE;
    VmaAllocation   indexAlloc_   = VK_NULL_HANDLE;
};
```

**Pipeline state:**
- Topology = `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST`
- Polygon = FILL, cull = NONE (we view the cube from inside)
- Depth test = ON, depth write = OFF, depthCompareOp =
  `VK_COMPARE_OP_LESS_OR_EQUAL` (so `z = 1` passes against the
  scene's depth-cleared 1.0)
- Blend = OFF
- Dynamic viewport + scissor
- Vertex input: 1 binding, 1 attribute (vec3 position at location 0)
- Render pass = `scenePass` (same as lit pass) — no separate pass

**Descriptor set layout:**
- Binding 0: UNIFORM_BUFFER (SkyUbo, vertex stage)
- Binding 1: COMBINED_IMAGE_SAMPLER (samplerCube, fragment stage)

**Shaders (inline GLSL 450):**

Vertex:
```glsl
#version 450
layout(location = 0) in vec3 aPos;

layout(set = 0, binding = 0) uniform SkyUbo {
    mat4 viewProjection;
} u;

layout(location = 0) out vec3 vDir;

void main() {
    vDir = aPos;
    vec4 pos = u.viewProjection * vec4(aPos, 1.0);
    gl_Position = pos.xyww;  // z = w → after perspective divide, z = 1
}
```

Fragment:
```glsl
#version 450
layout(location = 0) in vec3 vDir;
layout(set = 0, binding = 1) uniform samplerCube uSkyCubemap;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(uSkyCubemap, vDir).rgb, 1.0);
}
```

**Cube geometry:** the 8 cube corners are hardcoded in
`VkSkybox::init()` as a one-time vertex buffer (96 bytes — 8 vec3s).
36 indices for 12 triangles. Pre-loaded into device-local memory via
a staging buffer at init time.

**View-matrix translation strip:** the caller (`VulkanRenderer::
endFrame`) computes a translation-stripped view matrix before
calling `record`. This keeps the cube anchored at the camera, so the
sky never appears to move:

```cpp
Mat4 viewNoTranslate = view;
viewNoTranslate.at(0, 3) = 0.0f;
viewNoTranslate.at(1, 3) = 0.0f;
viewNoTranslate.at(2, 3) = 0.0f;
const Mat4 vp = projection * viewNoTranslate;
skybox_.record(cb, device, frames, cubemap, vp /* pre-multiplied */, ...);
```

Actually simpler: the record signature takes the pre-multiplied
`vp` directly, and the caller computes it:

```cpp
void VkSkybox::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                     const VkCubemapResource& cubemap,
                     const Mat4& viewProjection);
```

### `VulkanRenderer` changes

#### New private state

In `VulkanRenderer.h`:

```cpp
#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkSkybox.h"

// ... in private section, alongside textures_ / shadowMap_ / etc.:
VkCubemapStore  cubemaps_;
VkSkybox        skybox_;
CubemapHandle   pendingSkybox_ = kInvalidHandle;
```

#### Lifecycle

- `init()`: call `cubemaps_.init(context_)` after `textures_.init`;
  call `skybox_.init(context_, scenePass())` after
  `shadowMap_.init`.
- Destructor: `skybox_.destroy(context_)` and
  `cubemaps_.destroyAll(context_)` in the appropriate LIFO order
  (skybox before pipelines_; cubemaps after textures, before
  pipelines).

#### Un-stub `createCubemap` and `setSkybox`

Replace the M9 `warnOnce` stubs:

```cpp
CubemapHandle VulkanRenderer::createCubemap(int width, int height,
        const std::array<const unsigned char*, 6>& faces) {
    return cubemaps_.createFromFaces(context_, width, height, faces);
}

void VulkanRenderer::setSkybox(CubemapHandle sky) {
    pendingSkybox_ = sky;
}
```

#### Draw the skybox in `endFrame`

Inside the scene render pass, BEFORE the `for (call : sceneDraws_)`
geometry replay loop:

```cpp
if (cubemaps_.has(pendingSkybox_)) {
    // Translation-stripped view * projection.
    Mat4 v = pendingView_;
    v.at(0, 3) = 0.0f;
    v.at(1, 3) = 0.0f;
    v.at(2, 3) = 0.0f;
    const Mat4 vp = pendingProjection_ * v;
    skybox_.record(cb, context_.device(), frames_,
                  cubemaps_.get(pendingSkybox_), vp);
}
```

Drawing skybox first (instead of last) lets the GPU early-z-reject
sky fragments where geometry is in front. Visually identical to
drawing-last (depth test handles it either way).

#### `recordSceneDraw` writes 6 descriptors

The 5-descriptor block from M14 becomes 6. Add a 4th imgInfo:

```cpp
const CubemapHandle skyHandle = cubemaps_.has(pendingSkybox_)
    ? pendingSkybox_
    : cubemaps_.blackCubemap();
const auto& skyTex = cubemaps_.get(skyHandle);

VkDescriptorImageInfo imgInfos[5]{};
// imgInfos[0..2] = diffuse / normal / spec (unchanged from M13)
// imgInfos[3] = shadow (unchanged from M14)
imgInfos[4].sampler     = skyTex.sampler;
imgInfos[4].imageView   = skyTex.view;
imgInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
```

The `VkWriteDescriptorSet writes[6]{}` array gains a 6th entry at
binding 5 pointing to `imgInfos[4]`. `vkUpdateDescriptorSets` call
gets `..., 6, writes, ...`.

### `VkShader.cpp` descriptor set layout

Grows from 5 to 6 bindings:

```cpp
VkDescriptorSetLayoutBinding bindings[6]{};
// 0 = UBO (VS+FS), 1 = diffuse (FS), 2 = normal (FS),
// 3 = spec (FS), 4 = shadow (FS), 5 = sky cubemap (FS).
bindings[5].binding = 5;
bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
bindings[5].descriptorCount = 1;
bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

dslInfo.bindingCount = 6;
```

### `VkFrameRing.cpp` pool sizes

```cpp
VkDescriptorPoolSize sizes[] = {
    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxDescriptorSetsPerFrame},
    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * kMaxDescriptorSetsPerFrame},  // M16: 5 per lit set
    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kMaxDescriptorSetsPerFrame},
};
```

Plus the skybox descriptor set (1 UBO + 1 sampler = 2 descriptors per
skybox draw) is allocated from the same pool. Worst case per frame:
~40 scene draws × 6 descriptors + 1 skybox × 2 = 242 sampler
descriptors. Comfortable within 1280.

### Shader updates (spinning-cube + net-shooter, identical)

Add the cubemap sampler at binding 5:

```glsl
layout(set = 0, binding = 5) uniform samplerCube uSkyCubemap;
```

Insert the reflection block AFTER the existing `lit = diff * lighting
+ u.emissive.xyz;` line and BEFORE the M15 fog block:

```glsl
// M16 — cubemap reflection (planar reflection comes in M17).
float reflectivity = u.materialParams.z;
if (reflectivity > 0.0) {
    vec3 viewDir = normalize(vWorldPos - u.cameraPos.xyz);
    vec3 reflectDir = reflect(viewDir, perturbedN);
    vec3 reflectColor = texture(uSkyCubemap, reflectDir).rgb;
    lit = mix(lit, reflectColor, reflectivity);
}
```

When `reflectivity == 0.0` (default for most materials), the branch
is skipped efficiently (uniform branch). When > 0, the perturbed
normal from M13's TBN gives accurate reflection direction for normal-
mapped surfaces.

### Net-shooter startup warning update

```cpp
iron::Log::warn("net-shooter Vulkan path: full lit pass (sun + "
                "ambient + emissive + normal/spec + shadow + point "
                "lights + fog + cubemap reflections; Blinn-Phong, "
                "3x3 PCF). Still missing planar reflection. Full "
                "parity ships in M17.");
```

## Data flow per frame

```
1. game.beginFrame(...)
   └── stores pending state (unchanged from M15)
2. game.setSkybox(handle)
   └── (or set once at startup) pendingSkybox_ stored
3. game.submit(call) × N
   └── sceneDraws_.push_back
4. game.endFrame()
   ├── shadow pass (M14)
   ├── scene render pass begin
   │   ├── M16 — draw skybox first (if pendingSkybox_ valid)
   │   ├── for call in sceneDraws_: recordSceneDraw (6 descriptors)
   │   ├── deferred callbacks (particles)
   │   ├── debug-lines if pending
   │   └── HUD if pending
   ├── scene render pass end
   └── submit + present
```

## Error handling

- `createCubemap` with any null face pointer or invalid dims:
  `VkCubemapStore` returns `kInvalidHandle` (mirrors `VkTextureStore`).
- `setSkybox(kInvalidHandle)` is a way to disable the skybox.
  `cubemaps_.has(kInvalidHandle)` returns false → `endFrame` skips
  the skybox draw and `recordSceneDraw` uses the black fallback.
- `pendingSkybox_` stays valid across frames until the game changes
  it or destroys the renderer.

## Testing

### Unit tests

None new. Pipeline + shader work, exercised by smoke testing.

### Smoke tests (manual)

After implementation:

- **Net-shooter on Vulkan**: visible sunset sky behind the arena
  (currently a flat blue clear color). Brick walls + ground + cubes
  still rendered correctly with shadows + lights + fog from
  M12-M15. Cubemap reflection invisible unless a material's
  `reflectivity > 0` (none currently).
- **Net-shooter on OpenGL**: unchanged (was already drawing the
  sunset cubemap via GL backend).
- **Spinning-cube on Vulkan**: unchanged. Spinning-cube doesn't set
  a skybox; the M16 code path skips it.
- **Particle-storm on Vulkan**: unchanged. Particle-storm doesn't
  set a skybox.
- Vulkan validation layers clean.

### CI

35/35 tests pass on both backends.

## Risks

- **Skybox draw order.** Drawing first leverages early-z rejection.
  If for some reason the scene clear color matters more than skybox
  (it shouldn't — skybox replaces it visually), we'd switch to
  drawing last. No correctness risk either way.
- **Cubemap face order.** The engine's `Renderer::createCubemap`
  contract says faces are `+X, -X, +Y, -Y, +Z, -Z` (matches
  Vulkan's `baseArrayLayer = i` order). Net-shooter's existing
  `generateSunsetFace` already uses this order. Confirmed.
- **`gl_Position.xyww` z=1 trick under Vulkan.** Vulkan clip-Z is
  [0, 1] natively. The `xyww` swizzle sets `clip.z = clip.w`, which
  after perspective divide becomes `ndc.z = 1.0`. With
  depthCompareOp = LESS_OR_EQUAL and the scene's depth cleared to
  1.0, the sky fragments pass exactly where geometry hasn't written
  closer depth. Correct.
- **Negative-height Vulkan viewport.** The scene pass uses negative-
  height viewport (M9 fix) for GL-style projection compatibility.
  The skybox uses the SAME viewport (dynamic state inherited from
  the scene viewport set). Direction sampling via `vDir = aPos` is
  view-frame-agnostic — no orientation issue.
- **Descriptor pool capacity.** Scene pass ~40 draws × 6
  descriptors + skybox ~2 descriptors + HUD ~5 + debug-lines ~1 +
  particles ~1 = ~250 descriptors per frame. Within 1280 budget.

## File / module changes

### New files

- `engine/render/backends/vulkan/VkCubemap.h`
- `engine/render/backends/vulkan/VkCubemap.cpp`
- `engine/render/backends/vulkan/VkSkybox.h`
- `engine/render/backends/vulkan/VkSkybox.cpp`

### Modified files

- `engine/render/backends/vulkan/VulkanRenderer.h` — adds
  `VkCubemapStore cubemaps_;`, `VkSkybox skybox_;`,
  `CubemapHandle pendingSkybox_;`.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — un-stubs
  `createCubemap` + `setSkybox`; `init` and destructor lifecycle;
  `endFrame` draws skybox; `recordSceneDraw` writes 6 descriptors.
- `engine/render/backends/vulkan/VkShader.cpp` — descriptor set
  layout 5 → 6 bindings.
- `engine/render/backends/vulkan/VkFrameRing.cpp` — sampler pool
  capacity `4*` → `5*` `kMaxDescriptorSetsPerFrame`.
- `games/01-spinning-cube/main.cpp` — Vulkan shaders gain the
  binding=5 sampler + reflection block.
- `games/07-net-shooter/main.cpp` — same; warning string updated.
- `engine/CMakeLists.txt` — registers `VkCubemap.cpp` + `VkSkybox.cpp`
  under the Vulkan branch.
- `docs/engine/rhi-abstraction.md` — appended M16 section.

## Open questions

None blocking.
