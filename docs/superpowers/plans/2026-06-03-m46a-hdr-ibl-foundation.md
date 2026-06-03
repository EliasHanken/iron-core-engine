# M46a — HDR Environment Pipeline (IBL Foundation) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load a Radiance `.hdr` equirectangular environment, convert it to an `RGBA16F` cubemap on the GPU via a compute pass, and let games set it as the skybox — bootstrapping the `IblBaker` compute-to-cubemap infrastructure that M46b/c reuse.

**Architecture:** A new `VkIblBaker` owns an `equirectToCube.comp` compute pipeline. It loads the `.hdr` via `stbi_loadf` into a temporary `R32G32B32A32_SFLOAT` sampled image, dispatches the compute shader writing into the 6 layers of a new `RGBA16F` cube-compatible image (allocated by an extended `VkCubemapStore::createHdr`), and returns a `CubemapHandle` usable with the existing `setSkybox()`. Pure CPU direction math lives in a header-only `Ibl.h`, unit-tested in lockstep with the GLSL.

**Tech Stack:** Vulkan compute (glslang runtime compile), VMA, stb_image (`stbi_loadf`), existing `VkComputePipeline` / `VkCubemapStore` patterns.

---

## Background & conventions (read first)

- This is **Vulkan-only**. OpenGL gets a stub override that returns `kInvalidHandle` (per the engine's vulkan-only direction).
- **Cube face order** is `+X, -X, +Y, -Y, +Z, -Z`. The per-texel direction formulas MUST match `engine/render/ProceduralSky.cpp:34-39` so the HDR skybox orientation is consistent with existing cubemaps. With `u = 2*(x+0.5)/size - 1`, `v = 2*(y+0.5)/size - 1`:
  - face 0 (+X): `( 1, -v, -u)`
  - face 1 (-X): `(-1, -v,  u)`
  - face 2 (+Y): `( u,  1,  v)`
  - face 3 (-Y): `( u, -1, -v)`
  - face 4 (+Z): `( u, -v,  1)`
  - face 5 (-Z): `(-u, -v, -1)`
- **Compute writes cannot target sRGB images** → the cube target is `VK_FORMAT_R16G16B16A16_SFLOAT` (linear, filterable, mandatory storage support).
- **One-shot submit pattern** (transient command pool → record → `vkQueueSubmit` → `vkQueueWaitIdle` → destroy pool) is used throughout the codebase for setup-time GPU work — see `VkCubemap.cpp:144-220`. Reuse it; do not add a persistent queue.
- The reflection-driven `Renderer` interface is in `engine/render/Renderer.h`; the Vulkan impl is `engine/render/backends/vulkan/VulkanRenderer.{h,cpp}`; the OpenGL impl is `engine/render/backends/opengl/OpenGLRenderer.{h,cpp}`.
- Header-only math headers (`Pbr.h`, `Tonemap.h`) live in `engine/render/` and are unit-tested by standalone binaries in `tests/` registered with `iron_add_test(<name> <name>.cpp)`.

## File structure

**Create:**
- `engine/render/Ibl.h` — header-only CPU math: `cubeFaceDirection()`, `directionToEquirectUv()`. Mirrors the GLSL exactly.
- `engine/render/backends/vulkan/VkIblBaker.h` / `.cpp` — equirect→cube compute pass + `.hdr` load. Holds the embedded `equirectToCube.comp` GLSL string.
- `tests/test_ibl.cpp` — unit tests for `Ibl.h` and a compile-check of the embedded compute shader.

**Modify:**
- `engine/render/backends/vulkan/VkCubemap.h` / `.cpp` — add `createHdr()` + extend `VkCubemapResource` (format, mipLevels, per-mip storage views) + destroy them.
- `engine/render/Renderer.h` — add pure-virtual `loadHdrSkybox()`.
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — own a `VkIblBaker`, implement `loadHdrSkybox()`, init/destroy the baker.
- `engine/render/backends/opengl/OpenGLRenderer.h` / `.cpp` — stub `loadHdrSkybox()` returning `kInvalidHandle`.
- `engine/CMakeLists.txt` — add `VkIblBaker.cpp` to the Vulkan backend sources.
- `tests/CMakeLists.txt` — register `test_ibl`.
- `games/11-sandbox/main.cpp` — visual gate: try `loadHdrSkybox()`, fall back to the procedural sunset sky.

---

## Task 1: `Ibl.h` CPU math + `test_ibl`

**Files:**
- Create: `engine/render/Ibl.h`
- Create: `tests/test_ibl.cpp`
- Modify: `tests/CMakeLists.txt` (after line 109, `iron_add_test(test_pbr test_pbr.cpp)`)

- [ ] **Step 1: Write the failing test**

Create `tests/test_ibl.cpp`:

```cpp
// Unit tests for the IBL direction math CPU port (Ibl.h), kept in lockstep
// with the equirectToCube.comp GLSL. Verifies cube-face direction
// reconstruction matches the engine's ProceduralSky face convention and that
// direction<->equirect UV mapping round-trips.
#include "render/Ibl.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using iron::Vec3;
using iron::cubeFaceDirection;
using iron::directionToEquirectUv;

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b;
    return (d < 0 ? -d : d) <= eps;
}

int main() {
    // 1. Face centers (u=v=0) point along the expected principal axes.
    {
        Vec3 px = cubeFaceDirection(0, 0.0f, 0.0f);  // +X
        assert(approx(px.x, 1.0f) && approx(px.y, 0.0f) && approx(px.z, 0.0f));
        Vec3 ny = cubeFaceDirection(3, 0.0f, 0.0f);  // -Y
        assert(approx(ny.x, 0.0f) && approx(ny.y, -1.0f) && approx(ny.z, 0.0f));
        Vec3 pz = cubeFaceDirection(4, 0.0f, 0.0f);  // +Z
        assert(approx(pz.x, 0.0f) && approx(pz.y, 0.0f) && approx(pz.z, 1.0f));
    }

    // 2. All returned directions are unit length.
    {
        for (int face = 0; face < 6; ++face) {
            for (float u : {-1.0f, -0.3f, 0.5f, 1.0f}) {
                for (float v : {-1.0f, 0.2f, 1.0f}) {
                    Vec3 d = cubeFaceDirection(face, u, v);
                    float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    assert(approx(len, 1.0f));
                }
            }
        }
    }

    // 3. Equirect UV: +X (yaw 0) maps to horizontal center, horizon to v=0.5.
    {
        auto uv = directionToEquirectUv(Vec3{1.0f, 0.0f, 0.0f});
        assert(approx(uv.x, 0.5f) && approx(uv.y, 0.5f));
    }

    // 4. Equirect UV: straight up (+Y) maps to v=1.0 (top of the image).
    {
        auto uv = directionToEquirectUv(Vec3{0.0f, 1.0f, 0.0f});
        assert(approx(uv.y, 1.0f));
    }

    std::puts("test_ibl: OK");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails (does not compile — header missing)**

Run: `cmake --build build --target test_ibl`
Expected: FAIL — `render/Ibl.h: No such file or directory` (and the test isn't registered yet).

- [ ] **Step 3: Register the test and create the header**

In `tests/CMakeLists.txt`, add after line 109:

```cmake
iron_add_test(test_ibl test_ibl.cpp)
```

Create `engine/render/Ibl.h`:

```cpp
#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace iron {

// CPU port of the cube-face direction + equirect mapping in
// equirectToCube.comp. Keep these in lockstep with the GLSL.
inline constexpr float kIblPi = 3.14159265358979323846f;

// Reconstructs the world-space direction for a texel on cube `face`
// (0..5 = +X,-X,+Y,-Y,+Z,-Z) at face coordinates u, v in [-1, 1].
// Matches ProceduralSky.cpp's face convention so HDR cubemaps share the
// orientation of LDR ones.
inline Vec3 cubeFaceDirection(int face, float u, float v) {
    Vec3 d{};
    switch (face) {
        case 0: d = { 1.0f, -v,   -u};   break;  // +X
        case 1: d = {-1.0f, -v,    u};   break;  // -X
        case 2: d = { u,    1.0f,  v};   break;  // +Y
        case 3: d = { u,   -1.0f, -v};   break;  // -Y
        case 4: d = { u,   -v,    1.0f}; break;  // +Z
        default: d = {-u,  -v,   -1.0f}; break;  // -Z
    }
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return Vec3{d.x / len, d.y / len, d.z / len};
}

// Maps a unit direction to equirectangular UV in [0, 1]. u wraps yaw
// (atan2(z, x)); v maps pitch via asin(y) so the horizon sits at v = 0.5
// and straight up at v = 1.0.
inline Vec3 directionToEquirectUv(Vec3 dir) {
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    const Vec3 d{dir.x / len, dir.y / len, dir.z / len};
    const float u = std::atan2(d.z, d.x) / (2.0f * kIblPi) + 0.5f;
    const float v = std::asin(std::clamp(d.y, -1.0f, 1.0f)) / kIblPi + 0.5f;
    return Vec3{u, v, 0.0f};  // z unused; reuses Vec3 to avoid a Vec2 dep
}

}  // namespace iron
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_ibl && ctest --test-dir build -R test_ibl --output-on-failure`
Expected: PASS — `test_ibl: OK`.

- [ ] **Step 5: Commit**

```bash
git add engine/render/Ibl.h tests/test_ibl.cpp tests/CMakeLists.txt
git commit -m "M46a: Ibl.h cube-face direction + equirect UV math (CPU port + test)"
```

---

## Task 2: `equirectToCube.comp` shader + compile check

The shader lives as an embedded GLSL string in `VkIblBaker.cpp` (Task 4), but we lock its source and verify it compiles now, before the C++ plumbing, so a GLSL typo surfaces immediately.

**Files:**
- Modify: `tests/test_ibl.cpp` (extend with a compile check — Vulkan builds only)

- [ ] **Step 1: Add the compile-check block to `test_ibl.cpp`**

At the top of `tests/test_ibl.cpp`, after the existing includes, add:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkIblBaker.h"
#endif
```

Then, immediately before `std::puts("test_ibl: OK");` in `main()`, add:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    // 5. The embedded equirect->cube compute shader compiles to SPIR-V.
    {
        const auto spv = iron::compileGlsl(
            VK_SHADER_STAGE_COMPUTE_BIT, iron::kEquirectToCubeComputeSrc());
        assert(!spv.empty());
        assert(spv.front() == 0x07230203u);  // SPIR-V magic
    }
#endif
```

`compileGlsl` takes a `std::string`; the `const char*` returned by `kEquirectToCubeComputeSrc()` converts implicitly. The symbol is declared in `VkIblBaker.h` and defined in Task 4.

- [ ] **Step 2: Confirm it fails to compile (symbol not defined yet)**

Run: `cmake --build build --target test_ibl`
Expected: FAIL — `VkIblBaker.h: No such file` / `kEquirectToCubeComputeSrc not declared`. This is expected; Task 4 defines them. Leave the test as-is and proceed — it will pass at the end of Task 4.

> This task intentionally has no standalone green bar; it front-loads the shader contract. The shader source itself is written in Task 4, Step 3.

---

## Task 3: `VkCubemapStore::createHdr` (RGBA16F cube + storage views)

**Files:**
- Modify: `engine/render/backends/vulkan/VkCubemap.h:20-54`
- Modify: `engine/render/backends/vulkan/VkCubemap.cpp` (extend `destroyAll`, add `createHdr`)

- [ ] **Step 1: Extend the resource struct and class declaration**

In `engine/render/backends/vulkan/VkCubemap.h`, replace the `VkCubemapResource` struct (lines 20-27) with:

```cpp
struct VkCubemapResource {
    VkImage       image     = VK_NULL_HANDLE;
    VmaAllocation alloc     = VK_NULL_HANDLE;
    VkImageView   view      = VK_NULL_HANDLE;  // cube view, for sampling
    VkSampler     sampler   = VK_NULL_HANDLE;  // shared with store
    VkFormat      format    = VK_FORMAT_R8G8B8A8_SRGB;
    std::uint32_t width     = 0;
    std::uint32_t height    = 0;
    std::uint32_t mipLevels = 1;
    // Per-mip 2D-array views (6 layers) for compute imageStore. Empty for
    // sampled-only LDR cubemaps created via createFromFaces.
    std::vector<VkImageView> storageViews;
};
```

Add `#include <vector>` to the header includes (after `#include <array>`).

In the `public:` section of `VkCubemapStore` (after the `createFromFaces` declaration, line 39), add:

```cpp
    // Allocates an RGBA16F cube-compatible image (faceSize x faceSize, 6
    // layers, `mipLevels` mips) with STORAGE+SAMPLED usage. The returned
    // resource has a cube sampling `view` plus one 2D-array `storageViews`
    // entry per mip for compute writes. The image is left in
    // VK_IMAGE_LAYOUT_UNDEFINED; the caller transitions it.
    CubemapHandle createHdr(VkContext& ctx, int faceSize, int mipLevels);
```

- [ ] **Step 2: Destroy the new storage views**

In `engine/render/backends/vulkan/VkCubemap.cpp`, in `destroyAll` (lines 43-48), replace the loop body so storage views are also freed:

```cpp
    for (auto& [h, res] : cubemaps_) {
        for (VkImageView sv : res.storageViews) {
            if (sv) { vkDestroyImageView(ctx.device(), sv, nullptr); }
        }
        if (res.view)  { vkDestroyImageView(ctx.device(), res.view, nullptr); }
        if (res.image) { vmaDestroyImage(ctx.allocator(), res.image, res.alloc); }
    }
```

- [ ] **Step 3: Implement `createHdr`**

In `engine/render/backends/vulkan/VkCubemap.cpp`, add after `createFromFaces` (after line 108):

```cpp
CubemapHandle VkCubemapStore::createHdr(VkContext& ctx, int faceSize, int mipLevels) {
    if (faceSize <= 0 || mipLevels <= 0) return kInvalidHandle;
    if (sharedSampler_ == VK_NULL_HANDLE) return kInvalidHandle;

    VkCubemapResource res{};
    res.width     = static_cast<std::uint32_t>(faceSize);
    res.height    = static_cast<std::uint32_t>(faceSize);
    res.mipLevels = static_cast<std::uint32_t>(mipLevels);
    res.format    = VK_FORMAT_R16G16B16A16_SFLOAT;
    res.sampler   = sharedSampler_;

    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = res.format;
    imgInfo.extent        = {res.width, res.height, 1};
    imgInfo.mipLevels     = res.mipLevels;
    imgInfo.arrayLayers   = 6;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aInfo{};
    aInfo.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &imgInfo, &aInfo,
                            &res.image, &res.alloc, nullptr));

    // Cube view spanning all mips, for sampling.
    VkImageViewCreateInfo cubeView{};
    cubeView.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cubeView.image    = res.image;
    cubeView.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    cubeView.format   = res.format;
    cubeView.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    cubeView.subresourceRange.baseMipLevel   = 0;
    cubeView.subresourceRange.levelCount     = res.mipLevels;
    cubeView.subresourceRange.baseArrayLayer = 0;
    cubeView.subresourceRange.layerCount     = 6;
    VK_CHECK(vkCreateImageView(ctx.device(), &cubeView, nullptr, &res.view));

    // One 2D-array storage view per mip, for compute imageStore.
    res.storageViews.resize(res.mipLevels);
    for (std::uint32_t m = 0; m < res.mipLevels; ++m) {
        VkImageViewCreateInfo sv{};
        sv.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        sv.image    = res.image;
        sv.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        sv.format   = res.format;
        sv.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        sv.subresourceRange.baseMipLevel   = m;
        sv.subresourceRange.levelCount     = 1;
        sv.subresourceRange.baseArrayLayer = 0;
        sv.subresourceRange.layerCount     = 6;
        VK_CHECK(vkCreateImageView(ctx.device(), &sv, nullptr, &res.storageViews[m]));
    }

    const CubemapHandle h = nextHandle_++;
    cubemaps_[h] = res;
    return h;
}
```

- [ ] **Step 4: Build the engine library to verify it compiles**

Run: `cmake --build build --target ironcore`
Expected: PASS (no link/compile errors). There is no standalone unit test for GPU resource creation — correctness is validated by the visual gate in Task 6.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkCubemap.h engine/render/backends/vulkan/VkCubemap.cpp
git commit -m "M46a: VkCubemapStore::createHdr (RGBA16F cube + per-mip storage views)"
```

---

## Task 4: `VkIblBaker` — equirect `.hdr` → cubemap compute pass

**Files:**
- Create: `engine/render/backends/vulkan/VkIblBaker.h`
- Create: `engine/render/backends/vulkan/VkIblBaker.cpp`
- Modify: `engine/CMakeLists.txt` (add `VkIblBaker.cpp` next to the other `backends/vulkan/*.cpp`)

- [ ] **Step 1: Create the header**

Create `engine/render/backends/vulkan/VkIblBaker.h`:

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkIblBaker is Vulkan-only."
#endif

#include "render/Handles.h"
#include "render/backends/vulkan/VkComputePipeline.h"

#include <vulkan/vulkan.h>

#include <string>

namespace iron {

class VkContext;
class VkCubemapStore;

// Returns the embedded equirectangular->cubemap compute shader source.
// Exposed for a compile-check unit test.
const char* kEquirectToCubeComputeSrc();

// Owns the equirect->cube compute pipeline and the .hdr load path. This is
// the shared IBL bake foundation; M46b/c add more compute passes alongside it.
class VkIblBaker {
public:
    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // Loads an equirectangular Radiance .hdr from disk, converts it to an
    // RGBA16F cubemap (faceSize x faceSize per face) stored in `store`, and
    // returns its handle. Returns kInvalidHandle on any failure.
    CubemapHandle equirectFileToCubemap(VkContext& ctx, VkCubemapStore& store,
                                        const std::string& hdrPath, int faceSize);

private:
    VkDescriptorSetLayout setLayout_     = VK_NULL_HANDLE;
    VkSampler             equirectSampler_ = VK_NULL_HANDLE;
    VkComputePipeline     pipeline_;
};

}  // namespace iron
```

- [ ] **Step 2: Create the implementation — init/destroy + shader source**

Create `engine/render/backends/vulkan/VkIblBaker.cpp`:

```cpp
// VkIblBaker.cpp — equirectangular .hdr -> RGBA16F cubemap via a compute
// pass. The shared IBL bake foundation (M46a); irradiance/prefilter passes
// (M46b/c) build on the same compute-to-cubemap pattern.

#include "render/backends/vulkan/VkIblBaker.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <stb_image.h>

#include <cstring>
#include <vector>

namespace iron {

const char* kEquirectToCubeComputeSrc() {
    return R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D uEquirect;
// A cubemap is written through a 2D-array view (one layer per face); imageCube
// is for sampling, not storing. Hence image2DArray here, gid.z = face index.
layout(binding = 1, rgba16f) uniform writeonly image2DArray uOut;

const float kPi = 3.14159265358979323846;

// Cube-face direction, matching ProceduralSky / Ibl.h face convention.
vec3 faceDir(int face, float u, float v) {
    vec3 d;
    if      (face == 0) d = vec3( 1.0, -v,   -u);   // +X
    else if (face == 1) d = vec3(-1.0, -v,    u);   // -X
    else if (face == 2) d = vec3( u,    1.0,  v);   // +Y
    else if (face == 3) d = vec3( u,   -1.0, -v);   // -Y
    else if (face == 4) d = vec3( u,   -v,    1.0); // +Z
    else                d = vec3(-u,   -v,   -1.0); // -Z
    return normalize(d);
}

void main() {
    ivec3 gid = ivec3(gl_GlobalInvocationID);
    ivec2 size = imageSize(uOut).xy;
    if (gid.x >= size.x || gid.y >= size.y) return;

    float u = 2.0 * (float(gid.x) + 0.5) / float(size.x) - 1.0;
    float v = 2.0 * (float(gid.y) + 0.5) / float(size.y) - 1.0;
    vec3 dir = faceDir(gid.z, u, v);

    vec2 uv = vec2(atan(dir.z, dir.x) / (2.0 * kPi) + 0.5,
                   asin(clamp(dir.y, -1.0, 1.0)) / kPi + 0.5);

    vec3 color = textureLod(uEquirect, uv, 0.0).rgb;
    imageStore(uOut, gid, vec4(color, 1.0));
}
)";
}

bool VkIblBaker::init(VkContext& ctx) {
    // Descriptor layout: equirect sampler (0) + cube storage image (1).
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding         = 0;
    b[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding         = 1;
    b[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo slInfo{};
    slInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slInfo.bindingCount = 2;
    slInfo.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &slInfo, nullptr, &setLayout_));

    auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, kEquirectToCubeComputeSrc());
    if (spirv.empty()) {
        Log::error("VkIblBaker: equirect->cube compute compile failed");
        return false;
    }
    if (!pipeline_.init(ctx, spirv, setLayout_)) return false;

    // Nearest sampler for the equirect source (avoids a filterable-format
    // requirement on the temp R32G32B32A32_SFLOAT image).
    VkSamplerCreateInfo sInfo{};
    sInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sInfo.magFilter    = VK_FILTER_NEAREST;
    sInfo.minFilter    = VK_FILTER_NEAREST;
    sInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;       // yaw wraps
    sInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // pitch clamps
    sInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device(), &sInfo, nullptr, &equirectSampler_));
    return true;
}

void VkIblBaker::destroy(VkContext& ctx) {
    pipeline_.destroy(ctx);
    if (equirectSampler_) {
        vkDestroySampler(ctx.device(), equirectSampler_, nullptr);
        equirectSampler_ = VK_NULL_HANDLE;
    }
    if (setLayout_) {
        vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
}
```

- [ ] **Step 3: Implement `equirectFileToCubemap`**

Append to `engine/render/backends/vulkan/VkIblBaker.cpp`, before the closing `}  // namespace iron`:

```cpp
CubemapHandle VkIblBaker::equirectFileToCubemap(
        VkContext& ctx, VkCubemapStore& store,
        const std::string& hdrPath, int faceSize) {
    // 1. Load the equirect .hdr as 4-channel float. No vertical flip: equirect
    //    convention has +Y at the top, which our v-mapping expects.
    stbi_set_flip_vertically_on_load(0);
    int w = 0, h = 0, ch = 0;
    float* pixels = stbi_loadf(hdrPath.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        Log::error("VkIblBaker: failed to load HDR '%s'", hdrPath.c_str());
        return kInvalidHandle;
    }
    const VkDeviceSize srcBytes =
        static_cast<VkDeviceSize>(w) * h * 4 * sizeof(float);

    // 2. Temp equirect image (R32G32B32A32_SFLOAT, sampled).
    VkImage       eqImg   = VK_NULL_HANDLE;
    VmaAllocation eqAlloc = VK_NULL_HANDLE;
    VkImageView   eqView  = VK_NULL_HANDLE;
    {
        VkImageCreateInfo ii{};
        ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType     = VK_IMAGE_TYPE_2D;
        ii.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
        ii.extent        = {static_cast<std::uint32_t>(w),
                            static_cast<std::uint32_t>(h), 1};
        ii.mipLevels     = 1;
        ii.arrayLayers   = 1;
        ii.samples       = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ii.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &ii, &ai, &eqImg, &eqAlloc, nullptr));

        VkImageViewCreateInfo vi{};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = eqImg;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &eqView));
    }

    // 3. Staging buffer for the equirect float data.
    VkBuffer      staging      = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = srcBytes;
        bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &staging, &stagingAlloc, &info));
        std::memcpy(info.pMappedData, pixels, static_cast<std::size_t>(srcBytes));
        vmaFlushAllocation(ctx.allocator(), stagingAlloc, 0, srcBytes);
    }
    stbi_image_free(pixels);

    // 4. Allocate the destination cube (RGBA16F, 1 mip).
    const CubemapHandle handle = store.createHdr(ctx, faceSize, /*mipLevels=*/1);
    if (handle == kInvalidHandle) {
        vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);
        vkDestroyImageView(ctx.device(), eqView, nullptr);
        vmaDestroyImage(ctx.allocator(), eqImg, eqAlloc);
        return kInvalidHandle;
    }
    const VkCubemapResource& cube = store.get(handle);

    // 5. One-shot command buffer: upload equirect, transition layouts,
    //    dispatch, transition cube to shader-read.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pInfo{};
    pInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pInfo.queueFamilyIndex = ctx.graphicsFamily();
    pInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pInfo, nullptr, &pool));

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool        = pool;
    cbInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));

    auto barrier = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS,
                       std::uint32_t layers) {
        VkImageMemoryBarrier mb{};
        mb.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb.oldLayout           = oldL;
        mb.newLayout           = newL;
        mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image               = img;
        mb.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        mb.srcAccessMask       = srcA;
        mb.dstAccessMask       = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &mb);
    };

    // Equirect: UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ.
    barrier(eqImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent      = {static_cast<std::uint32_t>(w),
                            static_cast<std::uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cb, staging, eqImg,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(eqImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);

    // Cube: UNDEFINED -> GENERAL (for imageStore), all 6 layers.
    barrier(cube.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 6);

    // One-shot descriptor pool/set.
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = 1;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets       = 1;
    dpInfo.poolSizeCount = 2;
    dpInfo.pPoolSizes    = sizes;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &dpool));

    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool     = dpool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts        = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsInfo, &set));

    VkDescriptorImageInfo eqInfo{};
    eqInfo.sampler     = equirectSampler_;
    eqInfo.imageView   = eqView;
    eqInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo cubeInfo{};
    cubeInfo.imageView   = cube.storageViews[0];
    cubeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &eqInfo;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &cubeInfo;
    vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_.pipelineLayout(), 0, 1, &set, 0, nullptr);
    const std::uint32_t groups = (static_cast<std::uint32_t>(faceSize) + 7u) / 8u;
    vkCmdDispatch(cb, groups, groups, 6);

    // Cube: GENERAL -> SHADER_READ_ONLY for the lit/skybox passes.
    barrier(cube.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 6);

    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    // Cleanup transient resources.
    vkDestroyDescriptorPool(ctx.device(), dpool, nullptr);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    vkDestroyImageView(ctx.device(), eqView, nullptr);
    vmaDestroyImage(ctx.allocator(), eqImg, eqAlloc);
    vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);

    return handle;
}
```

- [ ] **Step 4: Add to the engine build**

In `engine/CMakeLists.txt`, find the list of `render/backends/vulkan/*.cpp` source entries (e.g. the line referencing `render/backends/vulkan/VkCubemap.cpp`) and add directly after it:

```cmake
    render/backends/vulkan/VkIblBaker.cpp
```

- [ ] **Step 5: Fix the Task 2 compile-check symbol and build the test**

In `tests/test_ibl.cpp`, ensure the compile-check uses the exact symbol `iron::kEquirectToCubeComputeSrc()` (correct the deliberately-mangled name from Task 2 Step 1).

Run: `cmake --build build --target test_ibl && ctest --test-dir build -R test_ibl --output-on-failure`
Expected: PASS — `test_ibl: OK`, including the SPIR-V compile check.

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkIblBaker.h engine/render/backends/vulkan/VkIblBaker.cpp engine/CMakeLists.txt tests/test_ibl.cpp
git commit -m "M46a: VkIblBaker equirect .hdr -> RGBA16F cubemap compute pass"
```

---

## Task 5: `loadHdrSkybox()` renderer API + wire the baker

**Files:**
- Modify: `engine/render/Renderer.h` (after `setSkybox`, ~line 152)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` (member + override)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (init/destroy/impl)
- Modify: `engine/render/backends/opengl/OpenGLRenderer.h` / `.cpp` (stub)

- [ ] **Step 1: Add the interface method**

In `engine/render/Renderer.h`, after the `setSkybox` declaration (line 152), add:

```cpp
    // Loads an equirectangular Radiance .hdr from disk, bakes it into an
    // RGBA16F cubemap, and returns its handle (usable with setSkybox). The
    // path is resolved relative to the working directory. `faceSize` is the
    // per-face resolution of the baked cube. Returns kInvalidHandle on
    // failure (e.g. file not found, or on backends without IBL support).
    virtual CubemapHandle loadHdrSkybox(const std::string& hdrPath,
                                        int faceSize = 512) = 0;
```

Confirm `#include <string>` is already present in `Renderer.h` (it is used by other methods); if not, add it.

- [ ] **Step 2: Declare the Vulkan override + member**

In `engine/render/backends/vulkan/VulkanRenderer.h`, after the `setSkybox` override (line 72), add:

```cpp
    CubemapHandle loadHdrSkybox(const std::string& hdrPath, int faceSize = 512) override;
```

Add the include near the other backend includes at the top of the header:

```cpp
#include "render/backends/vulkan/VkIblBaker.h"
```

Next to the `VkCubemapStore cubemaps_;` member (line 227), add:

```cpp
    VkIblBaker iblBaker_;
```

- [ ] **Step 3: Init and destroy the baker**

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, find where `cubemaps_.init(...)` is called during renderer initialization and add immediately after it:

```cpp
    if (!iblBaker_.init(context_)) {
        Log::error("VulkanRenderer: IBL baker init failed");
        return false;
    }
```

> Use the same surrounding success/failure convention as the adjacent `cubemaps_.init` call (match its `if (!...) return false;` style and whatever `context_` accessor name is used locally — the field may be `context_` or accessed via `context()`).

Find where `cubemaps_.destroyAll(...)` is called during shutdown and add immediately before it (the baker owns a pipeline that must be torn down before the device):

```cpp
    iblBaker_.destroy(context_);
```

- [ ] **Step 4: Implement `loadHdrSkybox`**

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, add the implementation near `setSkybox`'s definition (search for `void VulkanRenderer::setSkybox`):

```cpp
CubemapHandle VulkanRenderer::loadHdrSkybox(const std::string& hdrPath, int faceSize) {
    return iblBaker_.equirectFileToCubemap(context_, cubemaps_, hdrPath, faceSize);
}
```

> If `setSkybox` references the context as `context()` rather than `context_`, match that local convention here.

- [ ] **Step 5: Stub the OpenGL backend**

In `engine/render/backends/opengl/OpenGLRenderer.h`, after its `setSkybox` override, add:

```cpp
    CubemapHandle loadHdrSkybox(const std::string& hdrPath, int faceSize = 512) override;
```

In `engine/render/backends/opengl/OpenGLRenderer.cpp`, add:

```cpp
CubemapHandle OpenGLRenderer::loadHdrSkybox(const std::string& /*hdrPath*/,
                                            int /*faceSize*/) {
    Log::warn("OpenGLRenderer: loadHdrSkybox is Vulkan-only; returning invalid handle");
    return kInvalidHandle;
}
```

Confirm `core/Log.h` is included in `OpenGLRenderer.cpp` (it is widely used); if not, add it.

- [ ] **Step 6: Build everything**

Run: `cmake --build build`
Expected: PASS — both backends compile; the abstract `Renderer` is satisfied by both impls.

- [ ] **Step 7: Commit**

```bash
git add engine/render/Renderer.h engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/backends/opengl/OpenGLRenderer.h engine/render/backends/opengl/OpenGLRenderer.cpp
git commit -m "M46a: loadHdrSkybox renderer API (Vulkan impl + OpenGL stub)"
```

---

## Task 6: Visual gate — HDR skybox in the sandbox

**Files:**
- Add asset: `assets/hdri/symmetrical_garden_02_2k.hdr` (CC0, Poly Haven)
- Modify: `games/11-sandbox/main.cpp:120-127`

- [ ] **Step 1: Acquire a CC0 HDRI**

Download a CC0 equirectangular HDRI (Poly Haven, CC0) into `assets/hdri/`. Concretely:

```bash
mkdir -p assets/hdri
curl -L -o assets/hdri/symmetrical_garden_02_2k.hdr \
  "https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/symmetrical_garden_02_2k.hdr"
```

Verify it downloaded as a real HDR (non-trivial size, ~5-15 MB):

Run: `ls -la assets/hdri/symmetrical_garden_02_2k.hdr`
Expected: a multi-megabyte file (not an HTML error page). If the host is unreachable, substitute any CC0 `.hdr` from Poly Haven and update the path below to match.

- [ ] **Step 2: Wire it into the sandbox with a procedural fallback**

In `games/11-sandbox/main.cpp`, replace the skybox block (lines 120-127) with:

```cpp
    // Skybox: prefer a baked HDR environment (M46a); fall back to the
    // procedural sunset cubemap if the .hdr is missing or the backend
    // lacks IBL support.
    {
        iron::CubemapHandle sky =
            renderer.loadHdrSkybox("assets/hdri/symmetrical_garden_02_2k.hdr", 512);
        if (sky == iron::kInvalidHandle) {
            iron::Log::warn("sandbox: HDR skybox failed; using procedural sunset");
            sky = iron::createSunsetSkybox(renderer);
        }
        if (sky == iron::kInvalidHandle)
            iron::Log::warn("sandbox: sunset skybox failed; sky shows clear color");
        renderer.setSkybox(sky);
    }
```

- [ ] **Step 3: Build and run the sandbox**

Run: `cmake --build build --target sandbox` (use the actual sandbox target name if it differs — check `games/11-sandbox/CMakeLists.txt`), then launch the produced executable from the repo root so `assets/` resolves.

Expected on screen:
- The skybox shows the real HDR garden environment (not the orange sunset gradient), proving the equirect→cube bake ran and the `RGBA16F` cube samples correctly.
- The DamagedHelmet's existing crude reflection now samples the HDR environment.
- No validation-layer errors in the console about layout transitions or descriptor writes.

> This is the M46a acceptance gate. If the sky is black: check the console for the "HDR skybox failed" warning (file path / load failure). If the sky is garbled or seams are obvious: verify the face direction formulas in the shader match `Ibl.h` exactly (Task 4 Step 2).

- [ ] **Step 4: Commit**

```bash
git add games/11-sandbox/main.cpp assets/hdri/symmetrical_garden_02_2k.hdr
git commit -m "M46a: visual gate — sandbox loads a CC0 HDR skybox (procedural fallback)"
```

> If repo policy excludes large binary assets from git, instead add `assets/hdri/` to `.gitignore`, commit only the `main.cpp` change, and document the download command in the sandbox README. Decide based on how existing assets (e.g. `assets/damaged-helmet`) are tracked — match that convention.

---

## Task 7: Full verification + code review

- [ ] **Step 1: Run the whole test suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass (target count is the existing suite + `test_ibl`).

- [ ] **Step 2: Best-practices code review**

Per the engine's always-review rule, review the full diff for this branch before opening the PR:

Run: `git diff main...HEAD`

Check specifically:
- All transient Vulkan resources in `equirectFileToCubemap` are destroyed on every return path (including the early-return when `createHdr` fails).
- No validation-layer warnings during the sandbox run (image layouts, descriptor types, queue family).
- The shader face formulas and `Ibl.h` stay byte-for-byte consistent.
- `vkQueueWaitIdle` setup-time stalls are acceptable here (one-time bake), matching the existing `VkCubemap` upload pattern.

- [ ] **Step 3: Open the PR**

Use the milestone PR convention (e.g. title `M46a: HDR environment pipeline (IBL foundation)`).

---

## Self-review notes (plan author)

- **Spec coverage:** M46a row of the spec table = `.hdr` equirect loader (Task 4), equirect→cube compute (Tasks 2+4), `RGBA16F` cube path (Task 3), `loadHdrSkybox()` API (Task 5), `IblBaker` foundation (Task 4). Visual gate (Task 6) matches the spec's "real HDR environment visible in skybox." Covered.
- **Out of M46a scope (deferred to b/c, per spec):** irradiance/prefilter/BRDF-LUT bakes, the descriptor-binding growth (9→12) and bones-UBO move, and the shader split-sum rewire. This plan deliberately does NOT touch `StandardLitShader.h` — the existing crude reflection simply samples the new HDR cube, which is the intended M46a behavior.
- **Type consistency:** `createHdr(VkContext&, int, int)`, `equirectFileToCubemap(VkContext&, VkCubemapStore&, const std::string&, int)`, `kEquirectToCubeComputeSrc()`, `loadHdrSkybox(const std::string&, int)`, and `VkCubemapResource::storageViews` are referenced consistently across tasks.
- **Placeholder scan:** no TBD/TODO; every code step shows complete code. The two cross-task forward references (`kEquirectToCubeComputeSrc` used in Task 2's test before it's defined in Task 4; the test going red until Task 4) are intentional and explained inline.
- **Context to confirm during implementation (not assumptions to hardcode):** the exact local name for the context accessor in `VulkanRenderer.cpp` (`context_` vs `context()`), the precise location of the `cubemaps_.init` / `destroyAll` calls, the sandbox CMake target name, and how existing large assets are tracked in git (Task 6 Step 4). Each is called out at its step.
