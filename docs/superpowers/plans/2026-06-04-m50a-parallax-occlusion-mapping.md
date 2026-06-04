# M50a Parallax Occlusion Mapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Parallax Occlusion Mapping so height-mapped surfaces show convincing apparent depth, with a flat-vs-POM side-by-side demo.

**Architecture:** A height map (binding 13, white-fallback no-op) on the material system; `heightScale` packed into the spare `LitUbo.baseColorFactor.w` (no UBO growth). The lit fragment shader ray-marches the height field (steep parallax + occlusion interpolation, adaptive steps) using the tangent-space view direction to produce an offset UV that ALL other maps then sample. A CPU port locks the math; the look is verified in a sandbox demo.

**Tech Stack:** C++17, Vulkan (Vulkan-only backend), glslang, ImGui, CTest. Branched from `main` (`ec3862b`); independent of the M49 reflection-probe branch.

---

## Background: exact code this plan builds on (verbatim references)

- **Shared fragment source:** `engine/render/StandardLitShader.h` → `standardLitFragSource()` is used by BOTH the lit and skinned-lit shaders (only the vertex stage differs). One edit covers both.
- **Frag sampler bindings** (`StandardLitShader.h:118-128`): `uDiffuse`=1, `uNormalMap`=2, `uMetallicRoughnessMap`=3, `uShadowMap`=4, `uSkyCubemap`=5, `uReflection`=6, `uAoMap`=7, `uEmissiveMap`=8, `uIrradianceCube`=10, `uPrefiltered`=11, `uBrdfLut`=12. **Highest = 12; binding 13 is free.**
- **Frag `main()` TBN + sampling** (`StandardLitShader.h:206-224`):
  ```glsl
  void main() {
      float uvScale = u.materialParams.x;
      float bias    = u.materialParams.w;
      vec2 uv = vUV * uvScale;

      vec3 N = normalize(vNormal);
      vec3 T = normalize(vTangent);
      vec3 B = cross(N, T);
      mat3 TBN = mat3(T, B, N);
      float normalScale = u.materialParams2.z;
      vec3 tangentNormal = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
      tangentNormal.xy *= normalScale;
      vec3 perturbedN = normalize(TBN * tangentNormal);

      vec3  albedo    = texture(uDiffuse, uv).rgb * u.baseColorFactor.xyz;
      float roughness = clamp(u.materialParams.y * texture(uMetallicRoughnessMap, uv).g, 0.04, 1.0);
      float metallic  = clamp(u.materialParams2.x * texture(uMetallicRoughnessMap, uv).b, 0.0, 1.0);
      float ao        = u.materialParams2.y * texture(uAoMap, uv).r;
  ```
  `vWorldPos` and `u.cameraPos.xyz` are both available in the frag shader.
- **C++ `LitUbo`** (`VulkanRenderer.cpp:318-338`): 960 bytes, `static_assert(sizeof(LitUbo) == 960, ...)`, `baseColorFactor.w` unused. `recordSceneDraw` sets it (`:587-588`): `ubo.baseColorFactor = Vec4{...x, ...y, ...z, 0.0f};`.
- **recordSceneDraw descriptor block** (`VulkanRenderer.cpp:644-754`): `VkDescriptorImageInfo imgInfos[11]`, `VkWriteDescriptorSet writes[12]`, final `vkUpdateDescriptorSets(context_.device(), 12, writes, 0, nullptr)`. Sampler fallback pattern: `textures_.has(h) ? textures_.get(h) : textures_.get(textures_.whiteTexture())`. `recordSkinnedDraw` mirrors this (with a bones UBO at binding 9).
- **Descriptor set layouts** (`engine/render/backends/vulkan/VkShader.cpp`): non-skinned `VkDescriptorSetLayoutBinding bindings[12]` / `bindingCount = 12` (`:99-152`); skinned `bindings[13]` / `bindingCount = 13` (`:202-232`, includes bones UBO at binding 9).
- **Frame descriptor pool** (`VkFrameRing.cpp:53-62`): `{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 11 * kMaxDescriptorSetsPerFrame}`. `kMaxDescriptorSetsPerFrame = 1024` (`VkFrameRing.h:15`).
- **`Material`** (`engine/render/Material.h:12-28`), **`MaterialDef`** (`engine/scene/SceneFormat.h:31-45`), **reflection** (`engine/scene/MaterialDef.reflect.cpp:6-21`), **sandbox resolve** (`games/11-sandbox/main.cpp:210-285`), **`ProceduralTextures.h`** (`generateWoodNormalMap(size, planks)` / `generateMetalSpecularMap(size)` returning `std::vector<unsigned char>` RGBA).

## File structure

**Create:**
- `engine/render/Parallax.h` — CPU port of the POM ray-march (header-only), lockstep with the GLSL.
- `tests/test_parallax.cpp` — unit tests for the CPU port.

**Modify:**
- `engine/render/Material.h` — `heightMap` field.
- `engine/scene/SceneFormat.h` (`MaterialDef`) + `engine/scene/MaterialDef.reflect.cpp` — `heightPath` + `heightScale`.
- `engine/render/StandardLitShader.h` — binding 13 + POM in the shared frag source.
- `engine/render/backends/vulkan/VkShader.cpp` — binding 13 in both layouts.
- `engine/render/backends/vulkan/VkFrameRing.cpp` — sampler pool `11*`→`12*`.
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — binding-13 write in `recordSceneDraw` + `recordSkinnedDraw`; `baseColorFactor.w = heightScale`.
- `engine/render/ProceduralTextures.h` — procedural height generator.
- `games/11-sandbox/main.cpp` — resolve `heightPath`/`heightScale`; flat-vs-POM demo.
- `tests/CMakeLists.txt` — register `test_parallax`.

---

## Task 1: Material + MaterialDef height fields (data plumbing)

**Files:** Modify `engine/render/Material.h`, `engine/scene/SceneFormat.h`, `engine/scene/MaterialDef.reflect.cpp`, `games/11-sandbox/main.cpp`.

- [ ] **Step 1: Add `heightMap` to `Material`**

In `engine/render/Material.h`, after the `aoMap` line:
```cpp
    TextureHandle heightMap = kInvalidHandle;             // linear; grayscale height field for POM (white=peak)
```

- [ ] **Step 2: Add `heightPath` + `heightScale` to `MaterialDef`**

In `engine/scene/SceneFormat.h` `MaterialDef`, after `normalScale`:
```cpp
    std::string heightPath;                         // grayscale height/displacement map (linear)
    float       heightScale  = 0.05f;               // POM depth; 0 = POM off
```

- [ ] **Step 3: Register the new fields for reflection**

In `engine/scene/MaterialDef.reflect.cpp`, after the `normalScale` field line:
```cpp
        .field("heightPath",  &MaterialDef::heightPath)
        .field("heightScale", &MaterialDef::heightScale, {.min = 0.0f, .max = 0.2f, .slider = true})
```

- [ ] **Step 4: Resolve `heightPath` → `heightMap` in the sandbox**

In `games/11-sandbox/main.cpp`, in the MaterialDef→Material resolve block (near `:281-285`), after the `aoMap` line:
```cpp
        out.material.heightMap = resolveTexture(e.material.heightPath, iron::kInvalidHandle, /*srgb=*/false);
```

- [ ] **Step 5: Build to verify it compiles**

Run: `cmake --build build-vk --target ironcore sandbox`
Expected: PASS (data only; not yet consumed by the shader).

- [ ] **Step 6: Commit**

```bash
git add engine/render/Material.h engine/scene/SceneFormat.h engine/scene/MaterialDef.reflect.cpp games/11-sandbox/main.cpp
git commit -m "M50a: height map + heightScale on Material/MaterialDef (data plumbing)"
```

---

## Task 2: CPU port of the POM ray-march + unit test (TDD)

**Files:** Create `engine/render/Parallax.h`, `tests/test_parallax.cpp`; modify `tests/CMakeLists.txt`.

This is the canonical algorithm; the GLSL in Task 4 must match it line-for-line.

- [ ] **Step 1: Write the failing test**

Create `tests/test_parallax.cpp`:
```cpp
#include "render/Parallax.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace iron;

static bool approx2(Vec2 a, Vec2 b, float eps = 1e-3f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps;
}

int main() {
    // Flat height field at peak (1.0 everywhere) => depth 0 => no offset.
    auto flatPeak = [](Vec2) { return 1.0f; };
    Vec2 off = parallaxOcclusionOffset(flatPeak, Vec2{0.5f, 0.5f}, Vec3{0.0f, 0.0f, 1.0f}, 0.1f);
    assert(approx2(off, Vec2{0.5f, 0.5f}));

    // Head-on view (viewTS.z=1) on a full-depth field (height 0 => depth 1):
    // the UV shifts by the full parallax P = viewTS.xy/viewTS.z * scale, subtracted.
    // With viewTS.xy = 0, there is no lateral shift regardless of depth.
    auto fullDepth = [](Vec2) { return 0.0f; };
    Vec2 off2 = parallaxOcclusionOffset(fullDepth, Vec2{0.5f, 0.5f}, Vec3{0.0f, 0.0f, 1.0f}, 0.1f);
    assert(approx2(off2, Vec2{0.5f, 0.5f}));

    // View tilted toward +U on a full-depth field: UV must shift in -U (parallax
    // moves texels opposite the view direction) and not move V.
    Vec3 vt = Vec3{0.6f, 0.0f, 0.8f};  // normalized-ish, +x lean
    Vec2 off3 = parallaxOcclusionOffset(fullDepth, Vec2{0.5f, 0.5f}, vt, 0.1f);
    assert(off3.x < 0.5f - 1e-3f);          // shifted in -U
    assert(std::fabs(off3.y - 0.5f) < 1e-4f); // V unchanged

    // Grazing view uses more layers than head-on (adaptive step count).
    assert(parallaxLayerCount(Vec3{0.0f, 0.0f, 1.0f}) <= parallaxLayerCount(Vec3{0.9f, 0.0f, 0.1f}));

    std::printf("test_parallax: OK\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails (header missing)**

Run: `cmake --build build-vk --target test_parallax`
Expected: FAIL — `render/Parallax.h` not found / target not registered.

- [ ] **Step 3: Write `engine/render/Parallax.h`**

```cpp
#pragma once

#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace iron {

// CPU port of the parallax-occlusion ray-march in StandardLitShader.h.
// Keep in lockstep with the GLSL. Height convention: sampled value in [0,1]
// is HEIGHT (white=peak); depth = 1 - height. viewTS is the tangent-space
// direction from the surface toward the camera (normalized). Returns the
// parallax-offset UV.
inline constexpr int kPomMinLayers = 8;
inline constexpr int kPomMaxLayers = 32;

// Adaptive layer count: more layers at grazing angles (small viewTS.z).
inline float parallaxLayerCount(Vec3 viewTS) {
    const float t = std::fabs(viewTS.z);  // 1 head-on, ~0 grazing
    return static_cast<float>(kPomMaxLayers)
         + (static_cast<float>(kPomMinLayers) - static_cast<float>(kPomMaxLayers)) * t;
}

// sampleHeight: callable Vec2 -> float height in [0,1].
template <typename SampleHeight>
inline Vec2 parallaxOcclusionOffset(SampleHeight sampleHeight, Vec2 uv, Vec3 viewTS, float heightScale) {
    const float numLayers = parallaxLayerCount(viewTS);
    const float layerDepth = 1.0f / numLayers;
    const float vz = (std::fabs(viewTS.z) < 1e-3f) ? 1e-3f : viewTS.z;
    const Vec2 P{viewTS.x / vz * heightScale, viewTS.y / vz * heightScale};
    const Vec2 deltaUV{P.x / numLayers, P.y / numLayers};

    float currentLayerDepth = 0.0f;
    Vec2 curUV = uv;
    float curDepth = 1.0f - sampleHeight(curUV);
    int guard = 0;
    while (currentLayerDepth < curDepth && guard < kPomMaxLayers + 1) {
        curUV = Vec2{curUV.x - deltaUV.x, curUV.y - deltaUV.y};
        curDepth = 1.0f - sampleHeight(curUV);
        currentLayerDepth += layerDepth;
        ++guard;
    }

    // Occlusion interpolation between the last two layers.
    const Vec2 prevUV{curUV.x + deltaUV.x, curUV.y + deltaUV.y};
    const float afterDepth = curDepth - currentLayerDepth;
    const float beforeDepth = (1.0f - sampleHeight(prevUV)) - (currentLayerDepth - layerDepth);
    const float denom = afterDepth - beforeDepth;
    const float weight = (std::fabs(denom) < 1e-6f) ? 0.0f
                       : std::clamp(afterDepth / denom, 0.0f, 1.0f);
    return Vec2{curUV.x * (1.0f - weight) + prevUV.x * weight,
                curUV.y * (1.0f - weight) + prevUV.y * weight};
}

}  // namespace iron
```

NOTE: confirm `Vec2`/`Vec3` field names (`.x/.y/.z`) match `engine/math/Vec.h`.

- [ ] **Step 4: Register the test**

In `tests/CMakeLists.txt`, matching the `test_ibl`/`test_reflection_probe` style (READ the file for the exact macro — may be `iron_add_test(...)`):
```cmake
add_executable(test_parallax test_parallax.cpp)
target_link_libraries(test_parallax PRIVATE ironcore)
add_test(NAME test_parallax COMMAND test_parallax)
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build-vk --target test_parallax && ctest --test-dir build-vk -C Debug -R test_parallax --output-on-failure`
Expected: PASS — `test_parallax: OK`.

- [ ] **Step 6: Commit**

```bash
git add engine/render/Parallax.h tests/test_parallax.cpp tests/CMakeLists.txt
git commit -m "M50a: CPU port of POM ray-march + unit test"
```

---

## Task 3: Procedural height generator

**Files:** Modify `engine/render/ProceduralTextures.h`.

- [ ] **Step 1: Add a tiling cobblestone-ish height generator**

In `engine/render/ProceduralTextures.h`, add (matching the existing `std::vector<unsigned char>` RGBA style):
```cpp
// Generates an RGBA8 grayscale HEIGHT map (white=peak, dark valleys) of a
// tiling grid of rounded stones separated by deep recessed mortar lines.
// Strong, well-defined depth so parallax/displacement reads clearly. `cells`
// = stones across the texture. Output is `size*size*4` bytes (R=G=B=height).
inline std::vector<unsigned char> generateStoneHeightMap(int size, int cells) {
    std::vector<unsigned char> out(static_cast<std::size_t>(size) * size * 4);
    const float cellPx = static_cast<float>(size) / static_cast<float>(cells);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Position within the current cell, centered in [-1, 1].
            const float cx = (std::fmod(static_cast<float>(x), cellPx) / cellPx) * 2.0f - 1.0f;
            const float cy = (std::fmod(static_cast<float>(y), cellPx) / cellPx) * 2.0f - 1.0f;
            const float r = std::sqrt(cx * cx + cy * cy);
            // Rounded dome: ~1 at the center, falling to 0 toward the cell edge
            // (the mortar gap). smoothstep gives a soft top + sharp valley.
            float h = 1.0f - r;                  // cone
            h = std::clamp(h, 0.0f, 1.0f);
            h = h * h * (3.0f - 2.0f * h);       // smoothstep-ish dome
            const unsigned char v = static_cast<unsigned char>(std::clamp(h, 0.0f, 1.0f) * 255.0f);
            const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
            out[i + 0] = v; out[i + 1] = v; out[i + 2] = v; out[i + 3] = 255;
        }
    }
    return out;
}
```
NOTE: confirm `<cmath>` (`std::fmod`, `std::sqrt`) and `<algorithm>` (`std::clamp`) are included in the header (add if missing).

- [ ] **Step 2: Build to verify it compiles**

Run: `cmake --build build-vk --target ironcore sandbox`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add engine/render/ProceduralTextures.h
git commit -m "M50a: procedural stone height-map generator for the POM demo"
```

---

## Task 4: Renderer binding 13 (height map) + heightScale upload

**Files:** Modify `engine/render/backends/vulkan/VkShader.cpp`, `VkFrameRing.cpp`, `VulkanRenderer.cpp`.

- [ ] **Step 1: Add binding 13 to the non-skinned layout**

In `VkShader.cpp` (the non-skinned lit layout, `bindings[12]`/`bindingCount = 12`): change the array to `bindings[13]`, append after the binding-12 entry:
```cpp
    bindings[12].binding         = 13;  // M50a — height map (POM)
    bindings[12].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
```
and set `dslInfo.bindingCount = 13;`.

- [ ] **Step 2: Add binding 13 to the skinned layout**

In `VkShader.cpp` (the skinned layout, `bindings[13]`/`bindingCount = 13`): change to `bindings[14]`, append:
```cpp
    bindings[13].binding         = 13;  // M50a — height map (POM)
    bindings[13].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
```
and set `dslInfo.bindingCount = 14;`.

- [ ] **Step 3: Bump the descriptor pool sampler capacity**

In `VkFrameRing.cpp:53-62`, change the sampler pool size `11 * kMaxDescriptorSetsPerFrame` → `12 * kMaxDescriptorSetsPerFrame` (update the trailing comment to mention the M50a height map).

- [ ] **Step 4: Write binding 13 + heightScale in `recordSceneDraw`**

In `VulkanRenderer.cpp` `recordSceneDraw`:
1. Add the height texture selection alongside the other fallbacks (`:644-664` area):
```cpp
    const auto& height = textures_.has(call.material.heightMap)
        ? textures_.get(call.material.heightMap)
        : textures_.get(textures_.whiteTexture());  // white = peak => depth 0 => POM no-op
```
2. Grow `VkDescriptorImageInfo imgInfos[11]` → `imgInfos[12]` and add:
```cpp
    imgInfos[11].sampler     = height.sampler;
    imgInfos[11].imageView   = height.view;
    imgInfos[11].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
```
3. Grow `VkWriteDescriptorSet writes[12]` → `writes[13]` and add (after the binding-12 write):
```cpp
    // M50a — binding 13: height map (POM).
    writes[12].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[12].dstSet          = set;
    writes[12].dstBinding      = 13;
    writes[12].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[12].descriptorCount = 1;
    writes[12].pImageInfo      = &imgInfos[11];
```
4. Change the update count: `vkUpdateDescriptorSets(context_.device(), 13, writes, 0, nullptr);`
5. Set `baseColorFactor.w` to the height scale (replace the `, 0.0f}` at `:587-588`):
```cpp
    const float pomScale = textures_.has(call.material.heightMap) ? call.material.heightScale : 0.0f;
    ubo.baseColorFactor = Vec4{call.material.baseColorFactor.x, call.material.baseColorFactor.y,
                               call.material.baseColorFactor.z, pomScale};
```

- [ ] **Step 5: Mirror the binding-13 write in `recordSkinnedDraw`**

`recordSkinnedDraw` has its own descriptor block mirroring `recordSceneDraw` (with the bones UBO at binding 9). Apply the SAME four changes there: the `height` selection, the extra `imgInfos[...]` entry, the extra `writes[...]` entry for binding 13, the incremented `vkUpdateDescriptorSets` count, and the `baseColorFactor.w = pomScale`. Match its existing array sizes/indices (read the function — its imgInfos/writes counts mirror recordSceneDraw; grow each by one and add binding 13 the same way).

- [ ] **Step 6: Build (all targets) to verify it compiles**

Run: `cmake --build build-vk`
Expected: PASS. (Shader still ignores binding 13 until Task 5 — but the descriptor is now bound, so no validation error.)

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VkShader.cpp engine/render/backends/vulkan/VkFrameRing.cpp engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M50a: bind height map at descriptor 13 + upload heightScale (baseColorFactor.w)"
```

---

## Task 5: POM in the lit fragment shader

**Files:** Modify `engine/render/StandardLitShader.h` (shared `standardLitFragSource`).

- [ ] **Step 1: Declare the height sampler**

In `standardLitFragSource`, after the `uBrdfLut` (binding 12) declaration:
```glsl
layout(set = 0, binding = 13) uniform sampler2D uHeightMap;  // M50a — POM (white=peak)
```

- [ ] **Step 2: Add the POM function + apply it to the UV**

In `standardLitFragSource`, add this function above `main()`:
```glsl
// M50a — Parallax Occlusion Mapping. Mirrors engine/render/Parallax.h.
// viewTS = tangent-space dir from surface to camera. Height: white=peak,
// depth = 1 - height. Returns the parallax-offset UV.
const int POM_MIN_LAYERS = 8;
const int POM_MAX_LAYERS = 32;
vec2 parallaxOcclusionUV(vec2 uv, vec3 viewTS, float heightScale) {
    float numLayers = mix(float(POM_MAX_LAYERS), float(POM_MIN_LAYERS), abs(viewTS.z));
    float layerDepth = 1.0 / numLayers;
    float vz = (abs(viewTS.z) < 1e-3) ? 1e-3 : viewTS.z;
    vec2 P = viewTS.xy / vz * heightScale;
    vec2 deltaUV = P / numLayers;

    float currentLayerDepth = 0.0;
    vec2 curUV = uv;
    float curDepth = 1.0 - texture(uHeightMap, curUV).r;
    for (int i = 0; i < POM_MAX_LAYERS + 1; ++i) {
        if (currentLayerDepth >= curDepth) break;
        curUV -= deltaUV;
        curDepth = 1.0 - texture(uHeightMap, curUV).r;
        currentLayerDepth += layerDepth;
    }
    vec2 prevUV = curUV + deltaUV;
    float afterDepth = curDepth - currentLayerDepth;
    float beforeDepth = (1.0 - texture(uHeightMap, prevUV).r) - (currentLayerDepth - layerDepth);
    float denom = afterDepth - beforeDepth;
    float weight = (abs(denom) < 1e-6) ? 0.0 : clamp(afterDepth / denom, 0.0, 1.0);
    return mix(curUV, prevUV, weight);
}
```

- [ ] **Step 3: Compute the offset UV before sampling the maps**

Replace the top of `main()` (`:206-219` region) so the TBN is built first, then POM offsets the UV, then the maps sample at the offset UV:
```glsl
void main() {
    float uvScale = u.materialParams.x;
    float bias    = u.materialParams.w;
    vec2 uv = vUV * uvScale;

    vec3 N = normalize(vNormal);
    vec3 T = normalize(vTangent);
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    // M50a — Parallax Occlusion Mapping: offset the UV along the tangent-space
    // view direction before sampling any map. heightScale is packed in baseColorFactor.w.
    float heightScale = u.baseColorFactor.w;
    if (heightScale > 0.0) {
        vec3 viewWS = u.cameraPos.xyz - vWorldPos;
        vec3 viewTS = normalize(transpose(TBN) * viewWS);
        uv = parallaxOcclusionUV(uv, viewTS, heightScale);
    }

    float normalScale = u.materialParams2.z;
    vec3 tangentNormal = texture(uNormalMap, uv).rgb * 2.0 - 1.0;
    tangentNormal.xy *= normalScale;
    vec3 perturbedN = normalize(TBN * tangentNormal);

    vec3  albedo    = texture(uDiffuse, uv).rgb * u.baseColorFactor.xyz;
    float roughness = clamp(u.materialParams.y * texture(uMetallicRoughnessMap, uv).g, 0.04, 1.0);
    float metallic  = clamp(u.materialParams2.x * texture(uMetallicRoughnessMap, uv).b, 0.0, 1.0);
    float ao        = u.materialParams2.y * texture(uAoMap, uv).r;
```
(Everything below this point already uses `uv` and is unchanged. The emissive map sample later in `main()` also uses `uv`, so it inherits the offset automatically — confirm it references `uv` and not `vUV`.)

- [ ] **Step 4: Build engine + a game to compile the shaders**

Run: `cmake --build build-vk --target ironcore sandbox`
Expected: PASS — glslang compiles the lit + skinned-lit variants (shared frag). No layout/UBO errors (heightScale rides in the existing `baseColorFactor.w`).

- [ ] **Step 5: Commit**

```bash
git add engine/render/StandardLitShader.h
git commit -m "M50a: parallax occlusion mapping in the lit fragment shader"
```

---

## Task 6: Flat-vs-POM demo + visual gate

**Files:** Modify `games/11-sandbox/main.cpp`.

- [ ] **Step 1: Build the demo surface + two quads**

In the sandbox scene setup:
1. Upload the procedural height map (and reuse an existing CC0 albedo/normal, e.g. `assets/cc0/brick`, OR a procedural albedo) — create a texture from `iron::generateStoneHeightMap(512, 6)` via the renderer's create-from-RGBA path (match how other procedural textures are uploaded; grep for an existing `generateWoodNormalMap`/`createTexture`/`createTextureRgba` usage to copy the upload call + format).
2. Add two quads (use the existing quad/plane mesh builder; grep `appendQuad`), placed side by side and **tilted toward the camera** (~45–60° from facing) so grazing-angle depth is visible. Both use the same albedo + normal + the procedural height map and a higher `uvScale` (e.g. 3) so the stones tile.
   - **Left quad:** `material.heightScale = 0.0f` (flat — normal map only).
   - **Right quad:** `material.heightScale = 0.06f` (POM).
3. Add HUD labels under/over each quad: "Normal map" (left) and "POM" (right) via the existing HUD text path (grep how the sandbox draws HUD labels).
4. Position the initial camera to frame both quads at a grazing angle (so the POM depth pops as the camera moves).

NOTE: place this demo in a clear area (offset from other content, like the M49 room was) so it's isolated. Keep it gated to Edit/initial view as appropriate.

- [ ] **Step 2: Build + run the visual gate**

Run: `cmake --build build-vk --target sandbox` then run `build-vk\games\11-sandbox\Debug\sandbox.exe`.
Expected (visual gate — confirm with user):
- Left quad looks flat; **right quad shows recessed depth** between the stones (mortar gaps sink in).
- The depth **parallax-shifts correctly** as the camera moves / as the grazing angle changes.
- At the quad's edge the POM surface stays flat (expected POM limitation — the thing M50b/tessellation will fix).
- Other scene materials (no height map) are visually unchanged.

- [ ] **Step 3: Commit**

```bash
git add games/11-sandbox/main.cpp
git commit -m "M50a: flat-vs-POM side-by-side demo (visual gate)"
```

---

## Self-review notes (spec coverage)

- Height map slot + scale (Material/MaterialDef + reflection + resolve) → Task 1. Full POM (steep + interpolation + adaptive layers) → Tasks 2 (CPU) + 5 (GLSL). Binding 13 + white fallback + heightScale in `baseColorFactor.w` (no LitUbo growth) → Task 4. Procedural height → Task 3. Flat-vs-POM demo → Task 6. CPU-port unit test → Task 2. All spec sections covered.
- Out-of-scope items (self-shadowing, silhouette clipping, tessellation) appear in no task — correct.

## Risks / verification reminders

- **GLSL/CPU lockstep:** the Task 5 GLSL `parallaxOcclusionUV` must match the Task 2 `parallaxOcclusionOffset` exactly (layer count, `P = viewTS.xy/viewTS.z*scale`, subtract `deltaUV`, occlusion interpolation). They share the convention; if you change one, change both.
- **Vec field names / math:** confirm `Vec2`/`Vec3` and `transpose(mat3)` availability (GLSL has `transpose`; the CPU port uses explicit math).
- **`recordSkinnedDraw`:** easy to forget — it has its own descriptor block; binding 13 + `baseColorFactor.w` must be added there too, or skinned meshes break/validate-error. Build ALL targets.
- **Emissive sample uses `uv`:** confirm the later emissive-map sample in `main()` references `uv` (so it inherits the POM offset), not `vUV`.
- **Clean build before CI:** `cmake --build build-vk` (all targets) + `ctest --test-dir build-vk -C Debug` at the end ([[verify-clean-build-before-ci]]).
- OpenGL backend is frozen; these are Vulkan-only shader/material changes (the `Material`/`MaterialDef` field additions are backend-agnostic data and compile on both).
