# M49 Reflection Probes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add static, box-projected local reflection probes so reflective surfaces show their real local surroundings instead of only the global skybox.

**Architecture:** A new `VkSceneCapture` renders the scene's 6 cube faces from a probe position into an HDR (`RGBA16F`) cubemap; the existing `VkIblBaker::bakePrefiltered` + BRDF LUT turn it into a roughness mip-chain. At runtime, each draw selects the nearest probe whose box contains it and substitutes that probe's prefiltered cube into the lit shader's existing binding 11 (no new descriptor binding), with a box-projection correction applied to the reflection vector. Probes are authored as an optional `SceneEntity` component (M42 pattern) and baked on demand from the editor.

**Tech Stack:** C++17, Vulkan (Vulkan-only backend), VMA, glslang, ImGui, CTest. Reuses the M46 IBL infra (`VkIblBaker`, `VkCubemapStore`, `StandardLitShader.h`).

---

## Background: exact code this plan builds on

**`LitUbo`** (`engine/render/backends/vulkan/VulkanRenderer.cpp:313-338`) is currently **960 bytes**, std140, ending with `reflectionViewProj` (Mat4), `reflectionParams` (Vec4), `clipPlane` (Vec4), and `static_assert(sizeof(LitUbo) == 960, ...)`.

**Binding 11 (`uPrefiltered`)** is written in `recordSceneDraw` (`VulkanRenderer.cpp:~707-712`) from the renderer member `pendingPrefiltered_` (falling back to `cubemaps_.blackCubemap()`). `iblEnabled` is `ubo.materialParams2.w = cubemaps_.has(pendingIrradiance_) ? 1.0f : 0.0f;` (`:586`).

**The lit shader IBL block** (`engine/render/StandardLitShader.h:249-266`) computes `R = reflect(-V, N_)` then `prefiltered = textureLod(uPrefiltered, R, roughness*maxMip)`. Box projection inserts between those two lines.

**`VkCubemapStore`** (`engine/render/backends/vulkan/VkCubemap.h`) holds `unordered_map<CubemapHandle, VkCubemapResource>`, a shared sampler, `createHdr(ctx, faceSize, mipLevels)` (STORAGE+SAMPLED RGBA16F cube), `get`, `has`, `blackCubemap`. **No per-handle destroy exists yet.**

**`VkReflectionTarget`** (`engine/render/backends/vulkan/VkReflectionTarget.{h,cpp}`) is the model for `VkSceneCapture`: owns a color image + depth image + render pass (entry+exit subpass deps) + framebuffer, with `init`/`destroy`/`beginPass`/`endPass`/`descriptorImageInfo`, using the negative-viewport-height convention.

**M42 component pattern:** `std::optional<CollisionShape> collision;` on `SceneEntity` (`engine/scene/SceneFormat.h:50-57`); reflection registration in `engine/world/CollisionShape.reflect.cpp`; editor combo in `engine/editor/SceneInspector.cpp:45-84` (the `kOptional[]` table).

**`bakePrefiltered`** signature (`VkIblBaker.h:56`): `CubemapHandle bakePrefiltered(VkContext&, VkCubemapStore&, CubemapHandle envCube, int faceSize, int mipLevels)`.

---

## File structure

**Create:**
- `engine/render/ReflectionProbe.h` — authorable `ReflectionProbeDef`, runtime `GpuReflectionProbe`, and CPU math (box projection, nearest-probe selection, cube-face view matrices). Header-only, no Vulkan types.
- `tests/test_reflection_probe.cpp` — unit tests for the CPU math.
- `engine/render/backends/vulkan/VkSceneCapture.h` / `.cpp` — 6-face scene capture into an HDR cube.

**Modify:**
- `engine/render/backends/vulkan/VkCubemap.h` / `.cpp` — add `destroy(handle)` + `createColorCube(ctx, faceSize)`.
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — `LitUbo` probe fields, per-draw probe selection in `recordSceneDraw`, `setReflectionProbes`, `bakeReflectionProbes`, own a `VkSceneCapture`.
- `engine/render/Renderer.h` — declare the two new public methods (pure virtual + Mock/OpenGL stubs).
- `engine/render/StandardLitShader.h` — `LitUbo` GLSL block probe fields + box-projection in the IBL block.
- `engine/scene/SceneFormat.h` — `std::optional<ReflectionProbeDef> probe;` on `SceneEntity`.
- `engine/render/ReflectionProbe.reflect.cpp` (new) + reflection registry wiring — register `ReflectionProbeDef` fields.
- `engine/editor/SceneInspector.cpp` — add the probe row to `kOptional[]`.
- `engine/editor/` edit-mode draw — green box-gizmo for probes.
- `games/11-sandbox/` — demo room + reflective sphere + probe + "Bake Reflection Probes" button.
- `tests/CMakeLists.txt` (or root) — register `test_reflection_probe`.

---

## Task 1: ReflectionProbe data + CPU math (box projection, selection, face matrices)

**Files:**
- Create: `engine/render/ReflectionProbe.h`
- Test: `tests/test_reflection_probe.cpp`

- [ ] **Step 1: Write the failing tests**

Create `tests/test_reflection_probe.cpp`:

```cpp
#include "render/ReflectionProbe.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace iron;

static bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
static bool approx3(Vec3 a, Vec3 b, float eps = 1e-3f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

static GpuReflectionProbe makeBox(Vec3 center, Vec3 half, CubemapHandle h = 1) {
    return GpuReflectionProbe{ {center.x - half.x, center.y - half.y, center.z - half.z},
                               {center.x + half.x, center.y + half.y, center.z + half.z},
                               center, h };
}

int main() {
    // --- nearest-probe selection ---
    std::vector<GpuReflectionProbe> probes = {
        makeBox({0, 0, 0}, {5, 5, 5}, 10),
        makeBox({20, 0, 0}, {3, 3, 3}, 20),
    };
    // Inside first box -> probe 10.
    assert(nearestProbeContaining(probes, Vec3{1, 1, 1}) == 0);
    // Inside second box -> probe 20.
    assert(nearestProbeContaining(probes, Vec3{20, 0, 0}) == 1);
    // Outside all boxes -> -1 (skybox fallback).
    assert(nearestProbeContaining(probes, Vec3{100, 0, 0}) == -1);
    // Empty list -> -1.
    assert(nearestProbeContaining({}, Vec3{0, 0, 0}) == -1);

    // Overlapping boxes: pick the one whose center is nearest.
    std::vector<GpuReflectionProbe> overlap = {
        makeBox({0, 0, 0}, {10, 10, 10}, 1),
        makeBox({5, 0, 0}, {10, 10, 10}, 2),
    };
    assert(nearestProbeContaining(overlap, Vec3{4, 0, 0}) == 1);  // center {5,..} closer than {0,..}

    // --- box projection ---
    // Probe box centered at origin, half-extent 10. A fragment at origin
    // reflecting straight +X must hit the +X wall at x=10; the corrected
    // direction (hit - center) is +X (unchanged for a centered fragment).
    GpuReflectionProbe p = makeBox({0, 0, 0}, {10, 10, 10});
    Vec3 corrected = boxProjectReflection(Vec3{1, 0, 0}, Vec3{0, 0, 0}, p);
    assert(approx3(normalize(corrected), Vec3{1, 0, 0}));

    // Off-center fragment: at x=-5 reflecting +X hits x=10 -> hitpoint {10,0,0};
    // corrected dir = {10,0,0} - center{0,0,0} = +X still, but a fragment at
    // {0,5,0} reflecting +X hits {10,5,0}; corrected = {10,5,0}-{0,0,0} != +X.
    Vec3 c2 = normalize(boxProjectReflection(Vec3{1, 0, 0}, Vec3{0, 5, 0}, p));
    assert(c2.y > 0.0f);  // points up-and-out, not pure +X

    // --- cube-face view matrices ---
    // Six faces must produce six distinct forward directions matching
    // cubeFaceDirection's center texel (u=v=0).
    for (int face = 0; face < 6; ++face) {
        Mat4 v = cubeFaceView(Vec3{0, 0, 0}, face);
        (void)v;  // smoke: must not crash / produce NaN
    }
    Mat4 vpx = cubeFaceView(Vec3{0, 0, 0}, 0);
    assert(!std::isnan(vpx.m[0]));

    std::printf("test_reflection_probe: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails (header missing)**

Run: `cmake --build build-vk --target test_reflection_probe`
Expected: FAIL — `render/ReflectionProbe.h` not found (and target not yet registered).

- [ ] **Step 3: Write `engine/render/ReflectionProbe.h`**

```cpp
#pragma once

#include "math/Vec.h"
#include "math/Mat4.h"
#include "render/Handles.h"

#include <cstddef>
#include <vector>

namespace iron {

// Authorable probe component (serialized + edited). The owning entity's
// transform supplies the world center; halfExtents define the box bounds.
struct ReflectionProbeDef {
    Vec3  halfExtents{5.0f, 5.0f, 5.0f};
    int   faceSize   = 128;   // cube face resolution (128 default, 256 hero)
    float intensity  = 1.0f;  // scales the probe's contribution (reserved)
};

// Runtime probe handed to the renderer: world-space box + baked prefiltered cube.
struct GpuReflectionProbe {
    Vec3          boxMin;
    Vec3          boxMax;
    Vec3          center;
    CubemapHandle prefiltered = kInvalidHandle;
};

inline bool boxContains(const GpuReflectionProbe& p, Vec3 pos) {
    return pos.x >= p.boxMin.x && pos.x <= p.boxMax.x &&
           pos.y >= p.boxMin.y && pos.y <= p.boxMax.y &&
           pos.z >= p.boxMin.z && pos.z <= p.boxMax.z;
}

// Returns the index of the nearest probe (by center distance) whose box
// contains `pos`, or -1 if none contain it (caller falls back to skybox IBL).
inline int nearestProbeContaining(const std::vector<GpuReflectionProbe>& probes, Vec3 pos) {
    int best = -1;
    float bestDist2 = 0.0f;
    for (std::size_t i = 0; i < probes.size(); ++i) {
        if (!boxContains(probes[i], pos)) continue;
        const Vec3 d{pos.x - probes[i].center.x, pos.y - probes[i].center.y,
                     pos.z - probes[i].center.z};
        const float dist2 = d.x * d.x + d.y * d.y + d.z * d.z;
        if (best == -1 || dist2 < bestDist2) { best = static_cast<int>(i); bestDist2 = dist2; }
    }
    return best;
}

// Box-projected cubemap correction (standard parallax correction). Given a
// reflection direction `R` and the fragment world position, intersect the
// reflection ray with the probe AABB and return the direction from the probe
// center to the hit point. Sampling the cube with this corrected direction
// makes flat surfaces reflect the right local geometry.
inline Vec3 boxProjectReflection(Vec3 R, Vec3 worldPos, const GpuReflectionProbe& p) {
    // Per-axis slab intersection; pick the nearest positive exit along R.
    const Vec3 invR{1.0f / (R.x != 0 ? R.x : 1e-6f),
                    1.0f / (R.y != 0 ? R.y : 1e-6f),
                    1.0f / (R.z != 0 ? R.z : 1e-6f)};
    const Vec3 tMax{(((R.x > 0 ? p.boxMax.x : p.boxMin.x) - worldPos.x) * invR.x),
                    (((R.y > 0 ? p.boxMax.y : p.boxMin.y) - worldPos.y) * invR.y),
                    (((R.z > 0 ? p.boxMax.z : p.boxMin.z) - worldPos.z) * invR.z)};
    const float t = std::min(tMax.x, std::min(tMax.y, tMax.z));
    const Vec3 hit{worldPos.x + R.x * t, worldPos.y + R.y * t, worldPos.z + R.z * t};
    return Vec3{hit.x - p.center.x, hit.y - p.center.y, hit.z - p.center.z};
}

// View matrix looking from `position` down cube face `face` (0..5 =
// +X,-X,+Y,-Y,+Z,-Z), matching ProceduralSky / Ibl.h face convention so the
// captured radiance cube shares orientation with the HDR skybox cubes.
// Forward = cubeFaceDirection(face, 0, 0); up chosen to avoid gimbal at poles.
inline Mat4 cubeFaceView(Vec3 position, int face) {
    Vec3 fwd;
    Vec3 up;
    switch (face) {
        case 0: fwd = { 1,  0,  0}; up = {0, -1,  0}; break;  // +X
        case 1: fwd = {-1,  0,  0}; up = {0, -1,  0}; break;  // -X
        case 2: fwd = { 0,  1,  0}; up = {0,  0,  1}; break;  // +Y
        case 3: fwd = { 0, -1,  0}; up = {0,  0, -1}; break;  // -Y
        case 4: fwd = { 0,  0,  1}; up = {0, -1,  0}; break;  // +Z
        default: fwd = { 0, 0, -1}; up = {0, -1,  0}; break;  // -Z
    }
    const Vec3 target{position.x + fwd.x, position.y + fwd.y, position.z + fwd.z};
    return lookAt(position, target, up);
}

}  // namespace iron
```

NOTE: confirm `lookAt`, `normalize`, and `Mat4::m` exist in `engine/math/` with these signatures; if `normalize(Vec3)` is named differently (e.g. `Vec3::normalized()`), adjust the test + header to match. Add `#include <algorithm>` for `std::min`.

- [ ] **Step 4: Register the test target**

In the tests CMake (`tests/CMakeLists.txt` — match the existing `test_ibl` / `test_pbr` registration style):

```cmake
add_executable(test_reflection_probe test_reflection_probe.cpp)
target_link_libraries(test_reflection_probe PRIVATE ironcore)
add_test(NAME test_reflection_probe COMMAND test_reflection_probe)
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build-vk --target test_reflection_probe && ctest --test-dir build-vk -R test_reflection_probe --output-on-failure`
Expected: PASS — `test_reflection_probe: OK`.

- [ ] **Step 6: Commit**

```bash
git add engine/render/ReflectionProbe.h tests/test_reflection_probe.cpp tests/CMakeLists.txt
git commit -m "M49: reflection-probe data + CPU math (box projection, selection, face views)"
```

---

## Task 2: VkCubemapStore — per-handle destroy + color-renderable cube

**Files:**
- Modify: `engine/render/backends/vulkan/VkCubemap.h`, `engine/render/backends/vulkan/VkCubemap.cpp`

- [ ] **Step 1: Add declarations to `VkCubemap.h`**

Add a `faceViews` field to `VkCubemapResource` (single-layer 2D views for color-attachment rendering; empty for non-render-target cubes):

```cpp
    // Per-face single-layer 2D views (6) for color-attachment rendering.
    // Empty unless created via createColorCube.
    std::vector<VkImageView> faceViews;
```

Add to the `VkCubemapStore` public API:

```cpp
    // Allocates an RGBA16F cube-compatible image (faceSize^2, 6 layers, 1 mip)
    // with COLOR_ATTACHMENT + SAMPLED usage, a cube sampling `view`, and 6
    // single-layer `faceViews` for rendering each face. Left in
    // VK_IMAGE_LAYOUT_UNDEFINED; the render pass transitions it.
    CubemapHandle createColorCube(VkContext& ctx, int faceSize);

    // Destroys one baked cube (image/alloc/views). Safe no-op on kInvalidHandle
    // or an unknown handle. Does NOT destroy the shared sampler or the black fallback.
    void destroy(VkContext& ctx, CubemapHandle h);
```

- [ ] **Step 2: Implement in `VkCubemap.cpp`**

`createColorCube` — mirror `createHdr` but with `usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT`, `mipLevels = 1`, format `VK_FORMAT_R16G16B16A16_SFLOAT`, and after the cube view, create 6 single-layer 2D views:

```cpp
CubemapHandle VkCubemapStore::createColorCube(VkContext& ctx, int faceSize) {
    VkCubemapResource res{};
    res.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    res.width = res.height = static_cast<std::uint32_t>(faceSize);
    res.mipLevels = 1;
    res.sampler = sharedSampler_;

    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = res.format;
    ic.extent = {res.width, res.height, 1};
    ic.mipLevels = 1;
    ic.arrayLayers = 6;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &ic, &ai, &res.image, &res.alloc, nullptr));

    VkImageViewCreateInfo cv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    cv.image = res.image;
    cv.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cv.format = res.format;
    cv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    VK_CHECK(vkCreateImageView(ctx.device(), &cv, nullptr, &res.view));

    res.faceViews.resize(6);
    for (std::uint32_t f = 0; f < 6; ++f) {
        VkImageViewCreateInfo fv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        fv.image = res.image;
        fv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        fv.format = res.format;
        fv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f, 1};
        VK_CHECK(vkCreateImageView(ctx.device(), &fv, nullptr, &res.faceViews[f]));
    }

    const CubemapHandle h = nextHandle_++;
    cubemaps_[h] = res;
    return h;
}

void VkCubemapStore::destroy(VkContext& ctx, CubemapHandle h) {
    if (h == kInvalidHandle) return;
    auto it = cubemaps_.find(h);
    if (it == cubemaps_.end()) return;
    VkCubemapResource& r = it->second;
    for (VkImageView v : r.faceViews) vkDestroyImageView(ctx.device(), v, nullptr);
    for (VkImageView v : r.storageViews) vkDestroyImageView(ctx.device(), v, nullptr);
    if (r.view) vkDestroyImageView(ctx.device(), r.view, nullptr);
    if (r.image) vmaDestroyImage(ctx.allocator(), r.image, r.alloc);
    cubemaps_.erase(it);
}
```

NOTE: match the exact `ctx.allocator()` / `ctx.device()` accessor names used elsewhere in this file. Ensure `destroyAll` also iterates `faceViews` (add the loop alongside the existing `storageViews` cleanup so the new views aren't leaked at shutdown).

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build-vk --target ironcore`
Expected: PASS (no link/compile errors).

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VkCubemap.h engine/render/backends/vulkan/VkCubemap.cpp
git commit -m "M49: VkCubemapStore per-handle destroy + color-renderable cube"
```

---

## Task 3: VkSceneCapture — render the scene's 6 faces into an HDR cube

**Files:**
- Create: `engine/render/backends/vulkan/VkSceneCapture.h`, `engine/render/backends/vulkan/VkSceneCapture.cpp`

This mirrors `VkReflectionTarget` but renders **6 times** into the 6 `faceViews` of a color cube from `VkCubemapStore::createColorCube`. Use a **simplified capture pipeline** (UBO + diffuse texture; sun-lambert + ambient), modeled on the existing reflection pipeline. The skybox is drawn first into each face so areas with no geometry show the sky.

- [ ] **Step 1: Write `VkSceneCapture.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkSceneCapture is Vulkan-only."
#endif

#include "render/Handles.h"
#include "render/DrawCall.h"
#include "math/Vec.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <vector>

namespace iron {

class VkContext;
class VkCubemapStore;
class VkMeshStore;     // match the actual mesh-store type name used by the renderer
class VkSkybox;

// Renders the scene's 6 cube faces from a world position into an RGBA16F cube
// (allocated via VkCubemapStore::createColorCube). Simplified shading: sun +
// ambient + diffuse texture, plus the skybox. Used by reflection-probe baking;
// runs on demand (not per frame). Returns the radiance cube handle.
class VkSceneCapture {
public:
    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // Captures into a NEW color cube and returns its handle. Caller owns the
    // returned handle (typically passes it to bakePrefiltered then destroys it).
    CubemapHandle capture(VkContext& ctx, VkCubemapStore& cubes, VkMeshStore& meshes,
                          VkSkybox& skybox, CubemapHandle skyboxCube,
                          const std::vector<DrawCall>& sceneDraws,
                          Vec3 sunDir, Vec3 sunColor, Vec3 ambient,
                          Vec3 position, int faceSize);

private:
    VkRenderPass          renderPass_   = VK_NULL_HANDLE;  // single color attachment
    VkImage               depthImage_   = VK_NULL_HANDLE;
    VmaAllocation         depthAlloc_   = VK_NULL_HANDLE;
    VkImageView           depthView_    = VK_NULL_HANDLE;
    std::uint32_t         depthSize_    = 0;               // recreated if faceSize grows
    VkDescriptorSetLayout setLayout_    = VK_NULL_HANDLE;  // UBO + diffuse
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          pipeline_     = VK_NULL_HANDLE;   // global-scope: shadows iron::VkPipeline
};

}  // namespace iron
```

NOTE: confirm the real mesh-store class/accessor (the renderer exposes `meshes_`); match its type. `DrawCall.h` path: confirm where `DrawCall` is declared (`engine/render/`).

- [ ] **Step 2: Write `VkSceneCapture.cpp`**

Structure (model `VkReflectionTarget.cpp` for the render pass + the reflection pipeline in `VulkanRenderer.cpp` for the inline GLSL + pipeline creation):

1. **`init`**: create the color-only render pass (1 attachment, RGBA16F, `loadOp=CLEAR`, `finalLayout=SHADER_READ_ONLY_OPTIMAL`, with entry+exit subpass dependencies like `VkReflectionTarget`); a depth image + view (`D32_SFLOAT`, lazily sized in `capture` if `faceSize > depthSize_`); a 2-binding descriptor set layout (UBO at 0, combined-image-sampler at 1); and the simplified pipeline from inline GLSL below. Depth attachment shared across faces.

2. **Inline GLSL** (anonymous namespace, compiled via the same `VkShader::compileGlsl` path the reflection pipeline uses):

```glsl
// vertex
#version 450
layout(location=0) in vec3 inPos;
layout(location=1) in vec3 inNormal;
layout(location=2) in vec2 inUv;
layout(location=3) in vec3 inTangent;
layout(set=0, binding=0) uniform CapUbo {
    mat4 mvp;      // faceViewProj * model (set per face per draw on CPU)
    mat4 model;
    vec4 sunDir;   // xyz
    vec4 sunColor; // xyz
    vec4 ambient;  // xyz
} u;
layout(location=0) out vec3 vN;
layout(location=1) out vec2 vUv;
void main() {
    gl_Position = u.mvp * vec4(inPos, 1.0);
    vN  = mat3(u.model) * inNormal;
    vUv = inUv;
}
```

```glsl
// fragment
#version 450
layout(set=0, binding=1) uniform sampler2D uDiffuse;
layout(location=0) in vec3 vN;
layout(location=1) in vec2 vUv;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform CapUbo {
    mat4 mvp; mat4 model; vec4 sunDir; vec4 sunColor; vec4 ambient;
} u;
void main() {
    vec3 N = normalize(vN);
    float ndl = max(dot(N, -normalize(u.sunDir.xyz)), 0.0);
    vec3 albedo = texture(uDiffuse, vUv).rgb;
    vec3 color = albedo * (u.ambient.xyz + u.sunColor.xyz * ndl);
    outColor = vec4(color, 1.0);
}
```

Define a matching C++ `CapUbo` struct (std140; mat4+mat4+3×vec4 = 176 bytes; `static_assert`).

3. **`capture`**:
   - `CubemapHandle cube = cubes.createColorCube(ctx, faceSize);`
   - (Re)create the depth image if `faceSize > depthSize_`.
   - Begin a one-time command buffer (mirror the pattern `VkIblBaker`/`VkTexture` use for one-shot GPU work — `ctx.beginSingleTimeCommands()` or the existing helper).
   - For `face` in 0..5:
     - Create a transient framebuffer binding `cubes.get(cube).faceViews[face]` + `depthView_` to `renderPass_`.
     - `vkCmdBeginRenderPass` (clear color to a neutral value, depth 1.0), set viewport/scissor to `faceSize` with the **negative-height** convention.
     - Compute `faceVP = perspective(90°, 1.0, near, far) * cubeFaceView(position, face)` (reuse `iron::perspective` + `cubeFaceView` from `ReflectionProbe.h`; match the project's `perspective` signature/clip convention used elsewhere for Vulkan).
     - Draw skybox first (translation-stripped) via `skybox.record(...)` if `skyboxCube` valid — confirm `VkSkybox::record` signature and that it accepts an explicit view/proj; if it reads renderer state, replicate its minimal draw here instead.
     - Bind `pipeline_`; for each `DrawCall` in `sceneDraws` (skip `useReflectionPlane`), allocate a UBO slice + descriptor set, write `CapUbo{ faceVP*model, model, sunDir, sunColor, ambient }` and the diffuse texture (binding 1, fallback to `whiteTexture`), `vkCmdBindVertexBuffers/IndexBuffer`, `vkCmdDrawIndexed`.
     - `vkCmdEndRenderPass`; destroy the transient framebuffer after submit.
   - End + submit + `vkQueueWaitIdle` (this is an on-demand bake, blocking is fine).
   - `return cube;`

NOTE: descriptor allocation for an off-frame bake should use a **dedicated transient descriptor pool** created in `init` (do NOT use `VkFrameRing`'s per-frame pool — bake runs outside the frame loop). Reset it at the start of each `capture`.

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build-vk --target ironcore`
Expected: PASS.

- [ ] **Step 4: Add a shader compile-check test (optional but matches repo convention)**

If the repo has per-shader compile tests (e.g. `test_glsl_to_spirv`), expose `kCaptureVertSrc()`/`kCaptureFragSrc()` and add asserts that they compile. Otherwise skip.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkSceneCapture.h engine/render/backends/vulkan/VkSceneCapture.cpp
git commit -m "M49: VkSceneCapture — 6-face scene capture into an HDR cube"
```

---

## Task 4: LitUbo probe fields + shader box-projection (kept in lockstep)

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp:313-338` (C++ `LitUbo`)
- Modify: `engine/render/StandardLitShader.h` (GLSL `LitUbo` block + IBL block)

The C++ struct and every GLSL `LitUbo` block (there are 3 — vertex, fragment, and any skinned variant; grep `materialParams2` shows them at `:30`, `:78`, `:137`) MUST stay byte-identical.

- [ ] **Step 1: Append probe fields to the C++ `LitUbo`**

In `VulkanRenderer.cpp`, after `clipPlane`:

```cpp
    Vec4 clipPlane;           // 16  M17 — (normal.xyz, -d) for reflection pass; ignored in scene
    Vec4 probeBoxMin;         // 16  M49 — xyz = probe AABB min (world); w unused
    Vec4 probeBoxMax;         // 16  M49 — xyz = probe AABB max (world); w unused
    Vec4 probeCenter;         // 16  M49 — xyz = probe center; w = probeActive (0/1)
};
static_assert(sizeof(LitUbo) == 1008, "LitUbo std140 layout (M49 reflection probes)");
```

- [ ] **Step 2: Append the same fields to every GLSL `LitUbo` block in `StandardLitShader.h`**

In each `uniform LitUbo { ... }` block (the 3 occurrences), append after `clipPlane`:

```glsl
    vec4 probeBoxMin;   // M49
    vec4 probeBoxMax;   // M49
    vec4 probeCenter;   // M49 — w = probeActive
```

- [ ] **Step 3: Insert box-projection into the IBL block**

In `StandardLitShader.h:258-260`, replace:

```glsl
        vec3  R           = reflect(-V, N_);
        float maxMip      = float(textureQueryLevels(uPrefiltered) - 1);
        vec3  prefiltered = textureLod(uPrefiltered, R, roughness * maxMip).rgb;
```

with:

```glsl
        vec3  R           = reflect(-V, N_);
        if (u.probeCenter.w > 0.5) {
            // M49 — box-projected parallax correction toward local geometry.
            vec3 invR = 1.0 / (abs(R) < vec3(1e-6) ? vec3(1e-6) * sign(R + 1e-9) : R);
            vec3 tMax = (mix(u.probeBoxMin.xyz, u.probeBoxMax.xyz, step(0.0, R)) - vWorldPos) * invR;
            float t   = min(min(tMax.x, tMax.y), tMax.z);
            vec3 hit  = vWorldPos + R * t;
            R = hit - u.probeCenter.xyz;
        }
        float maxMip      = float(textureQueryLevels(uPrefiltered) - 1);
        vec3  prefiltered = textureLod(uPrefiltered, R, roughness * maxMip).rgb;
```

(`vWorldPos` is the fragment world position already used by the M16 fallback at `:279`.)

- [ ] **Step 4: Zero the new fields by default in `recordSceneDraw` / `recordSkinnedDraw`**

The default `LitUbo ubo{}` zero-inits the new fields (`probeActive = 0`), so non-probe draws and the reflection mini-UBO path are unaffected. Verify no path sets them yet.

- [ ] **Step 5: Build both the engine and a game to verify shaders still compile**

Run: `cmake --build build-vk --target ironcore 11-sandbox`
Expected: PASS — glslang compiles all three LitUbo variants; runtime UBO size matches.

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/StandardLitShader.h
git commit -m "M49: LitUbo probe fields + shader box-projection (probeActive-gated)"
```

---

## Task 5: Renderer per-draw probe selection + setReflectionProbes API

**Files:**
- Modify: `engine/render/Renderer.h`, `engine/render/backends/vulkan/VulkanRenderer.h`, `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: OpenGL backend + MockRenderer (stub the new pure-virtuals — see [[verify-clean-build-before-ci]])

- [ ] **Step 1: Declare the API on the `Renderer` interface**

In `engine/render/Renderer.h`, add (near the IBL/skybox methods):

```cpp
    // M49 — set the active reflection probes for this scene. Each draw selects
    // the nearest probe whose box contains it; outside all probes, the global
    // skybox IBL is used. Pass an empty span to disable probes.
    virtual void setReflectionProbes(std::span<const GpuReflectionProbe> probes) = 0;

    // M49 — bake (capture + prefilter) every probe. On-demand; blocks. The
    // probe vector is updated in place with the baked prefiltered handles.
    virtual void bakeReflectionProbes(std::vector<GpuReflectionProbe>& probes) = 0;
```

Add `#include "render/ReflectionProbe.h"` and `#include <span>` / `<vector>`.

- [ ] **Step 2: Stub on OpenGL + Mock backends (keep them concrete)**

In the OpenGL renderer and `MockRenderer`, add:

```cpp
    void setReflectionProbes(std::span<const GpuReflectionProbe>) override { /* unsupported on GL */ }
    void bakeReflectionProbes(std::vector<GpuReflectionProbe>&) override {}
```

(Grep all `: public Renderer` / `: public iron::Renderer` subclasses to be sure none is left abstract.)

- [ ] **Step 3: Implement `setReflectionProbes` + selection in VulkanRenderer**

Add member `std::vector<GpuReflectionProbe> pendingProbes_;` to `VulkanRenderer.h`. Implement:

```cpp
void VulkanRenderer::setReflectionProbes(std::span<const GpuReflectionProbe> probes) {
    pendingProbes_.assign(probes.begin(), probes.end());
}
```

In `recordSceneDraw`, after `ubo` is built and before the binding-11 image-info is chosen, select the probe and override:

```cpp
    // M49 — per-draw probe selection. Object position = model translation.
    CubemapHandle drawPrefiltered = pendingPrefiltered_;  // skybox default
    const Vec3 objPos{call.model.m[12], call.model.m[13], call.model.m[14]};
    const int pi = nearestProbeContaining(pendingProbes_, objPos);
    if (pi >= 0 && cubemaps_.has(pendingProbes_[pi].prefiltered)) {
        const GpuReflectionProbe& gp = pendingProbes_[pi];
        drawPrefiltered = gp.prefiltered;
        ubo.probeBoxMin = Vec4{gp.boxMin.x, gp.boxMin.y, gp.boxMin.z, 0.0f};
        ubo.probeBoxMax = Vec4{gp.boxMax.x, gp.boxMax.y, gp.boxMax.z, 0.0f};
        ubo.probeCenter = Vec4{gp.center.x, gp.center.y, gp.center.z, 1.0f};
    }
```

Then change the binding-11 source (`VulkanRenderer.cpp:~707`) from `pendingPrefiltered_` to `drawPrefiltered`:

```cpp
    const CubemapHandle prefiltHandle = cubemaps_.has(drawPrefiltered)
        ? drawPrefiltered : cubemaps_.blackCubemap();
```

NOTE: confirm the `Mat4` translation components are at indices 12/13/14 (column-major) in this codebase's `Mat4`; if row-major, use 3/7/11. Cross-check against how `cameraPos` is extracted elsewhere.

- [ ] **Step 4: Build to verify it compiles (all targets)**

Run: `cmake --build build-vk`
Expected: PASS — no abstract-class errors from the new pure-virtuals.

- [ ] **Step 5: Commit**

```bash
git add engine/render/Renderer.h engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp <opengl + mock files>
git commit -m "M49: per-draw probe selection + setReflectionProbes API (binding-11 swap)"
```

---

## Task 6: Bake orchestration in VulkanRenderer

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`, `engine/render/backends/vulkan/VulkanRenderer.cpp`

- [ ] **Step 1: Own a `VkSceneCapture` + init it**

Add member `VkSceneCapture sceneCapture_;` to `VulkanRenderer.h` (include `VkSceneCapture.h`). In the renderer's init (where `iblBaker_`/`skybox_` are initialized, ~`VulkanRenderer.cpp:114`), call `sceneCapture_.init(context_);` and destroy it in the renderer teardown.

- [ ] **Step 2: Implement `bakeReflectionProbes`**

```cpp
void VulkanRenderer::bakeReflectionProbes(std::vector<GpuReflectionProbe>& probes) {
    constexpr int kMips = 5;
    constexpr int kFaceSize = 128;  // v1 default; per-probe faceSize is deferred
    for (auto& p : probes) {
        const int faceSize = kFaceSize;
        // 1. Capture scene radiance into a color cube.
        CubemapHandle radiance = sceneCapture_.capture(
            context_, cubemaps_, meshes_, skybox_, pendingSkybox_, sceneDraws_,
            pendingSunDir_, pendingSunColor_, pendingAmbient_, p.center, faceSize);
        if (radiance == kInvalidHandle) continue;
        // 2. Prefilter into a roughness mip-chain.
        CubemapHandle prefiltered = iblBaker_.bakePrefiltered(
            context_, cubemaps_, radiance, faceSize, kMips);
        // 3. Free the intermediate + any previous bake (re-bake hygiene).
        cubemaps_.destroy(context_, radiance);
        if (p.prefiltered != kInvalidHandle) cubemaps_.destroy(context_, p.prefiltered);
        p.prefiltered = prefiltered;
    }
}
```

NOTE: `bakeReflectionProbes` reads `sceneDraws_`, so the caller MUST submit the scene's draw calls (via `submit`) **before** calling bake, within a begin/endFrame-less context or right after a `beginFrame`. Confirm `sceneDraws_` is populated by `submit` and not cleared until `endFrame`; if bake must run standalone, document that the game calls `submit` for every object, then `bakeReflectionProbes`, then proceeds. Also confirm `pendingSunDir_`/`pendingSunColor_`/`pendingAmbient_` member names (set in `beginFrame`).

- [ ] **Step 3: Build**

Run: `cmake --build build-vk`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M49: bake orchestration — capture + prefilter + re-bake hygiene"
```

---

## Task 7: Editor authoring — ReflectionProbeDef component + Inspector + gizmo

**Files:**
- Modify: `engine/scene/SceneFormat.h`
- Create: `engine/render/ReflectionProbe.reflect.cpp` (+ wire into the reflection registry entry point, matching `CollisionShape.reflect.cpp`)
- Modify: `engine/editor/SceneInspector.cpp`
- Modify: editor edit-mode draw (the file that draws collider wireframes — grep for the green collider gizmo)

- [ ] **Step 1: Add the optional component to `SceneEntity`**

In `engine/scene/SceneFormat.h:50-57`:

```cpp
    std::optional<CollisionShape> collision;  // M42 — absent = no collider
    std::optional<AudioEmitter>   audio;       // M42 — absent = no emitter
    std::optional<ReflectionProbeDef> probe;   // M49 — absent = no probe
```

Add `#include "render/ReflectionProbe.h"`.

- [ ] **Step 2: Register `ReflectionProbeDef` fields for reflection**

Create `engine/render/ReflectionProbe.reflect.cpp` mirroring `CollisionShape.reflect.cpp`:

```cpp
#include "render/ReflectionProbe.h"
#include "reflection/Reflection.h"

namespace iron {
void registerReflectionProbe(Reflection& r) {
    r.registerType<ReflectionProbeDef>("ReflectionProbeDef")
        .field("halfExtents", &ReflectionProbeDef::halfExtents, {.min = 0.1f})
        .field("faceSize",    &ReflectionProbeDef::faceSize)
        .field("intensity",   &ReflectionProbeDef::intensity, {.min = 0.0f, .max = 4.0f, .slider = true});
}
}  // namespace iron
```

Wire `registerReflectionProbe(r)` into the same place `registerCollisionShape` / `registerAudioEmitter` are called (grep for `registerCollisionShape`).

- [ ] **Step 3: Add the probe row to the Inspector `kOptional[]` table**

In `engine/editor/SceneInspector.cpp:55-67`, add a third entry:

```cpp
        { "ReflectionProbe",
          [](const SceneEntity& s){ return s.probe.has_value(); },
          [](SceneEntity& s){ s.probe.emplace(); },
          [](SceneEntity& s){ s.probe.reset(); },
          [](const Reflection& r, SceneEntity& s){ return renderComponent(r, *s.probe); } },
```

- [ ] **Step 4: Draw a green box gizmo for probes in Edit mode**

In the edit-mode draw loop (where collider wireframes are emitted via `drawLine`/gizmo AABB), for each entity with `e.probe`, draw an AABB centered at `e.transform.position` with half-extents `e.probe->halfExtents`, color green. Reuse the existing AABB debug-line helper.

- [ ] **Step 5: Build the editor + sandbox**

Run: `cmake --build build-vk --target 11-sandbox`
Expected: PASS — Inspector shows "ReflectionProbe" in the Add Component combo; adding one shows halfExtents/faceSize/intensity sliders and a green box in the viewport.

- [ ] **Step 6: Commit**

```bash
git add engine/scene/SceneFormat.h engine/render/ReflectionProbe.reflect.cpp engine/editor/SceneInspector.cpp <reflect registry file> <editor gizmo file>
git commit -m "M49: editor ReflectionProbe component + Inspector row + box gizmo"
```

---

## Task 8: Sandbox demo scene + bake button + visual gate

**Files:**
- Modify: `games/11-sandbox/` (main/sandbox source + assets)

- [ ] **Step 1: Build a reflective test scene**

In the sandbox scene setup, add: a small enclosed "room" (4 walls + floor + ceiling using existing box meshes, distinct CC0 textures per wall so reflections are legible), a **reflective metallic sphere** in the center (metallic≈1, roughness≈0.1 so it mirrors), and a `ReflectionProbeDef` on an entity at the room center with `halfExtents` wrapping the room. Keep the HDR skybox loaded (existing path) so outside-the-box reflection has a visible fallback.

- [ ] **Step 2: Wire the "Bake Reflection Probes" button**

In the editor toolbar/panel, add a button that: gathers `GpuReflectionProbe`s from every entity with a `probe` (center = transform position, box from halfExtents), calls `renderer.bakeReflectionProbes(probes)`, then `renderer.setReflectionProbes(probes)` each frame thereafter. Ensure the scene's draw calls are submitted before baking (per Task 6 Step 2 note).

```cpp
if (ImGui::Button("Bake Reflection Probes")) {
    bakedProbes_.clear();
    for (const SceneEntity& e : scene.entities) {
        if (!e.probe) continue;
        const Vec3 c = e.transform.position;
        const Vec3 h = e.probe->halfExtents;
        bakedProbes_.push_back(GpuReflectionProbe{
            {c.x-h.x, c.y-h.y, c.z-h.z}, {c.x+h.x, c.y+h.y, c.z+h.z}, c, kInvalidHandle});
    }
    renderer.bakeReflectionProbes(bakedProbes_);
}
// every frame, before submit/endFrame:
renderer.setReflectionProbes(bakedProbes_);
```

- [ ] **Step 3: Build + run the visual gate**

Run: `cmake --build build-vk --target 11-sandbox` then run the sandbox.
Expected (visual gate — confirm with user):
- The reflective sphere shows the **room walls** (not just the skybox) after Bake.
- The reflection **parallax-corrects** — moving the camera/sphere within the box keeps wall reflections geometrically anchored.
- Moving the sphere **outside the probe box** degrades to the skybox reflection.
- Re-baking repeatedly does not grow GPU memory / cube count (destroy-on-rebake works).

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox
git commit -m "M49: sandbox reflection-probe demo room + bake button (visual gate)"
```

---

## Self-review notes (spec coverage)

- Static baked capture → Tasks 3, 6. Box-projected parallax → Tasks 1 (CPU), 4 (GLSL). Nearest single + skybox fallback → Tasks 1, 5. Editor authoring (M42 pattern) → Task 7. Leak fix (`destroy`) → Task 2, used in Task 6. Visual gate → Task 8. Unit tests → Task 1. All spec sections covered.
- No new descriptor binding (binding 11 reused) → Task 5 confirms.
- Deferred items (real-time, blending, probe diffuse) appear in no task — correct.

## Risks / verification reminders

- **Math conventions:** verify `Mat4` storage order (column vs row) for the translation extraction (Task 5) and `lookAt`/`perspective`/`normalize` signatures (Tasks 1, 3) against `engine/math/`. These are the most likely break points.
- **`sceneDraws_` lifetime** during bake (Task 6) — confirm it's populated by `submit` and not cleared before bake runs.
- **`VkSkybox::record` signature** (Task 3) — confirm it can render with an explicit per-face view/proj; if it reads renderer state, inline a minimal skybox draw instead.
- **Clean build before CI:** run `cmake --build build-vk` (all targets) and `ctest --test-dir build-vk` after Task 5 and at the end — the new pure-virtuals must not leave any `Renderer` subclass abstract ([[verify-clean-build-before-ci]]).
- **Both backends:** OpenGL is frozen but must still compile; the new methods are stubbed there (Task 5 Step 2).
