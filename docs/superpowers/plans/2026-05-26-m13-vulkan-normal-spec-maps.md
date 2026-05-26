# M13 Vulkan Normal Maps + Specular Maps + UV Scale Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the Vulkan lit pass with normal-map TBN math + Blinn-Phong specular highlights + UV-scale tiling, so the Vulkan-side material surface matches OpenGL parity for everything that doesn't require a separate render pass.

**Architecture:** `LitUbo` grows from 192 to 224 bytes (adds `cameraPos` + `materialParams`). The hardcoded descriptor set layout in `VkShader.cpp` grows from 2 to 4 bindings (adds normal + spec samplers). `VulkanRenderer::submit` writes all three texture samplers per draw with built-in fallback textures (white / flat-normal / no-spec). Spinning-cube and net-shooter Vulkan shaders are rewritten to compute the TBN basis, sample the maps, and combine Lambertian + Blinn-Phong + ambient + emissive.

**Tech Stack:** C++23, Vulkan 1.3, glslang (GLSL 450 → SPIR-V), VMA, MSVC, CMake.

---

## File Structure

### Modified files
- `engine/render/backends/vulkan/VulkanRenderer.h` — adds `pendingCameraPos_` private field
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — extends `LitUbo` (224 bytes), adds `extractCameraPos`, updates `beginFrame`, rewrites `submit`'s descriptor-write block to write 4 descriptors instead of 2
- `engine/render/backends/vulkan/VkShader.cpp` — descriptor set layout from 2 to 4 bindings
- `engine/render/backends/vulkan/VkFrameRing.cpp` — bumps COMBINED_IMAGE_SAMPLER capacity from `kMaxDescriptorSetsPerFrame` to `3 * kMaxDescriptorSetsPerFrame`
- `games/01-spinning-cube/main.cpp` — Vulkan-branch shaders rewritten with TBN + Blinn-Phong + uvScale
- `games/07-net-shooter/main.cpp` — same shader rewrite + updated runtime warning
- `docs/engine/rhi-abstraction.md` — appended M13 section

### New files
None.

---

## Task 1: Engine plumbing — LitUbo extension + descriptor layout + pool sizing

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/render/backends/vulkan/VkShader.cpp`
- Modify: `engine/render/backends/vulkan/VkFrameRing.cpp`

This task lands all the engine-side changes in one commit. Backward-compatible with the M12 Vulkan shaders only if you ALSO update the shaders in this commit — otherwise the descriptor set layout grows from 2 to 4 bindings while existing shaders only declare 2, which Vulkan validation layers will not tolerate. Therefore: **bundle the spinning-cube + net-shooter shader rewrites with this task to keep the tree green between commits.**

Actually — the cleaner approach: do the engine plumbing FIRST without the descriptor layout growth (just LitUbo extension), then in Task 2 do the descriptor layout growth + shader rewrites together. Use the M12 shaders' "ignore unused UBO fields" trick: a 224-byte UBO with the M12 shaders still reading only the first 192 bytes is fine. So Task 1 just expands the UBO; descriptor count stays at 2.

That means: **Task 1 expands LitUbo only. Task 2 grows the descriptor set layout AND rewrites both shaders together.**

- [ ] **Step 1: Add `pendingCameraPos_` field to `VulkanRenderer.h`**

In `engine/render/backends/vulkan/VulkanRenderer.h`, find the M12 pending-light block (`pendingSunDir_`, `pendingSunColor_`, `pendingAmbient_`). Add one new field immediately after:

```cpp
    // M13 — camera world position, extracted from view matrix at beginFrame.
    // Used by submit() for Blinn-Phong specular highlights in the lit shader.
    Vec3 pendingCameraPos_ = {0.0f, 0.0f, 0.0f};
```

- [ ] **Step 2: Extend `LitUbo` struct in `VulkanRenderer.cpp`**

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, find the anonymous-namespace `LitUbo` struct from M12. Replace it with:

```cpp
namespace {

// M12+M13 — per-draw UBO uploaded by submit. std140 layout: all members
// are mat4 (64-byte aligned) or vec4 (16-byte aligned), so no
// straddling. Total 224 bytes.
struct LitUbo {
    Mat4 mvp;             // 64 — projection * view * model
    Mat4 model;           // 64 — for mat3(model) transforms in the vertex shader
    Vec4 sunDir;          // 16 — xyz direction; w padding
    Vec4 sunColor;        // 16 — xyz color; w padding
    Vec4 ambient;         // 16 — xyz pre-multiplied ambient color; w padding
    Vec4 emissive;        // 16 — xyz from call.material.emissive; w padding
    Vec4 cameraPos;       // 16 — xyz world-space camera; w padding (M13)
    Vec4 materialParams;  // 16 — x=uvScale, y=specPower, z=reflectivity, w=padding (M13)
};
static_assert(sizeof(LitUbo) == 224, "LitUbo std140 layout");

// Extracts the camera's world-space position from a view matrix.
// Assumes view is a pure rigid transform [R | t; 0 0 0 1] (rotation +
// translation, no scale) — true for all engine cameras (lookAt /
// first-person / free-fly). For column-major Mat4 storage where
// m[col*4+row] = at(row, col), the camera world position is -R^T * t.
Vec3 extractCameraPos(const Mat4& view) {
    // R^T * t (component-wise dot of each row of R with t).
    const float tx = view.at(0, 3);
    const float ty = view.at(1, 3);
    const float tz = view.at(2, 3);
    return Vec3{
        -(view.at(0, 0) * tx + view.at(1, 0) * ty + view.at(2, 0) * tz),
        -(view.at(0, 1) * tx + view.at(1, 1) * ty + view.at(2, 1) * tz),
        -(view.at(0, 2) * tx + view.at(1, 2) * ty + view.at(2, 2) * tz),
    };
}

}  // namespace
```

If your file's existing anonymous namespace already contains the M12 LitUbo, replace it in place. Otherwise place this block right after the file's `#include` block.

- [ ] **Step 3: Populate `pendingCameraPos_` in `beginFrame`**

Locate the M12 block in `VulkanRenderer::beginFrame` that assigns `pendingSunDir_`, `pendingSunColor_`, `pendingAmbient_`. Add immediately after that block:

```cpp
    pendingCameraPos_ = extractCameraPos(view);
```

- [ ] **Step 4: Extend `submit`'s LitUbo population**

In `VulkanRenderer::submit`, find the M12 `LitUbo` populate block (sets `ubo.mvp`, `ubo.model`, `ubo.sunDir`, etc.). Add two more assignments right after the existing `ubo.emissive = …;` line:

```cpp
    ubo.cameraPos = Vec4{pendingCameraPos_.x,
                         pendingCameraPos_.y,
                         pendingCameraPos_.z,
                         0.0f};
    ubo.materialParams = Vec4{
        call.material.uvScale,
        call.material.specPower,
        call.material.reflectivity,
        0.0f
    };
```

`Material::uvScale`, `Material::specPower`, `Material::reflectivity` are all `float` members on the existing `Material` struct (verified — see `engine/render/Material.h`).

- [ ] **Step 5: Build under both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both. Visual output is identical to M12 (shaders still ignore the new UBO fields).

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M13 Task 1: extend LitUbo with cameraPos + materialParams (engine plumbing)"
```

---

## Task 2: Descriptor set layout grows to 4 bindings + shader rewrites (atomic)

**Files:**
- Modify: `engine/render/backends/vulkan/VkShader.cpp`
- Modify: `engine/render/backends/vulkan/VkFrameRing.cpp`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `games/01-spinning-cube/main.cpp`
- Modify: `games/07-net-shooter/main.cpp`

This task is atomic — descriptor layout, sampler-write block in submit, AND both game shaders must change together to keep the tree compiling and validation clean. Multiple files, one commit.

- [ ] **Step 1: Grow descriptor set layout in `VkShader.cpp`**

In `engine/render/backends/vulkan/VkShader.cpp`, find the descriptor set layout block currently sized for 2 bindings. Replace with 4 bindings:

```cpp
    // M13 — descriptor set layout: UBO + 3 samplers (diffuse, normal, spec).
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;  // diffuse
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;  // normal
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;  // spec
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 4;
    dslInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &s.setLayout));
```

(Adjust to match the surrounding variable names — `s.setLayout` is the existing one; `ctx.device()` is the existing context handle.)

- [ ] **Step 2: Bump descriptor pool sampler capacity in `VkFrameRing.cpp`**

In `engine/render/backends/vulkan/VkFrameRing.cpp::initFrame`, find the `VkDescriptorPoolSize sizes[]` block. Update the COMBINED_IMAGE_SAMPLER line:

```cpp
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxDescriptorSetsPerFrame},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * kMaxDescriptorSetsPerFrame},  // M13: 3 samplers per lit-pass set
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kMaxDescriptorSetsPerFrame},
    };
```

- [ ] **Step 3: Rewrite `submit`'s descriptor-write block**

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, find the existing M12 descriptor-write block in `submit` (currently writes 2 descriptors: UBO + 1 sampler). Replace the entire block with:

```cpp
    // M13 — write 4 descriptors per draw: UBO + diffuse + normal + spec.
    // Fallback textures used for invalid handles.
    const auto& diffuse = textures_.has(call.material.texture)
        ? textures_.get(call.material.texture)
        : textures_.get(textures_.whiteTexture());
    const auto& normal = textures_.has(call.material.normalMap)
        ? textures_.get(call.material.normalMap)
        : textures_.get(textures_.flatNormalTexture());
    const auto& spec = textures_.has(call.material.specularMap)
        ? textures_.get(call.material.specularMap)
        : textures_.get(textures_.noSpecularTexture());

    VkDescriptorImageInfo imgInfos[3]{};
    imgInfos[0].sampler     = diffuse.sampler;
    imgInfos[0].imageView   = diffuse.view;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].sampler     = normal.sampler;
    imgInfos[1].imageView   = normal.view;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[2].sampler     = spec.sampler;
    imgInfos[2].imageView   = spec.view;
    imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[4]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imgInfos[0];
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &imgInfos[1];
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &imgInfos[2];
    vkUpdateDescriptorSets(context_.device(), 4, writes, 0, nullptr);
```

Delete the old block's `VkDescriptorImageInfo imgInfo{};` + `VkWriteDescriptorSet writes[2]{};` etc. The variables `bufInfo` and `set` are unchanged from M12 (the UBO bufInfo prep block above this comes before — leave it alone).

- [ ] **Step 4: Rewrite the spinning-cube Vulkan shaders**

In `games/01-spinning-cube/main.cpp`, find the `#ifdef IRON_RENDER_BACKEND_VULKAN` block. Replace BOTH shader strings (`kVertexShader` and `kFragmentShader`) with:

```cpp
const char* kVertexShader = R"(#version 450
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
    vec4 materialParams;
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
)";

const char* kFragmentShader = R"(#version 450
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
    vec4 materialParams;  // x=uvScale, y=specPower, z=reflectivity
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;

void main() {
    float uvScale   = u.materialParams.x;
    float specPower = u.materialParams.y;
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

    float diffuse = max(dot(perturbedN, L), 0.0);
    float spec    = pow(max(dot(perturbedN, H), 0.0), specPower);
    float specMask = texture(uSpecularMap, uv).r;

    vec3 lighting = u.sunColor.xyz * (diffuse + spec * specMask) + u.ambient.xyz;
    vec3 diff = texture(uDiffuse, uv).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;
    outColor = vec4(lit, 1.0);
}
)";
```

Leave the OpenGL `#else` branch untouched.

- [ ] **Step 5: Rewrite the net-shooter Vulkan shaders**

In `games/07-net-shooter/main.cpp`, find the `#ifdef IRON_RENDER_BACKEND_VULKAN` block. Replace BOTH shader strings with the EXACT SAME `kVertexShader` and `kFragmentShader` code from Step 4. (The two games share identical Vulkan shaders.)

- [ ] **Step 6: Update net-shooter's startup warning**

In `games/07-net-shooter/main.cpp`, find the existing `Log::warn` block under `#ifdef IRON_RENDER_BACKEND_VULKAN`. Replace the warning text:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path: sun + ambient + emissive "
                    "+ normal/spec maps (Blinn-Phong) lit. Still missing "
                    "point lights, fog, shadows, cubemap reflections. "
                    "Full parity ships in future milestones.");
#endif
```

- [ ] **Step 7: Build both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both. Compile errors at this stage are most likely:
- Mismatched UBO struct between C++ `LitUbo` and the GLSL `LitUbo` (offsets must match exactly — verify member order)
- Missing `Vec3` operator usage if you copy-pasted incorrectly

- [ ] **Step 8: Smoke-test the Vulkan builds**

Launch `build-vk/games/01-spinning-cube/Debug/01-spinning-cube.exe`. Expected: cube still renders. Visible output is identical to M12 because spinning-cube's Material doesn't assign a normal map or specular map — the flat-normal fallback means `perturbedN == N`, and the no-spec fallback means `spec * specMask == 0`. Same Lambertian + ambient + emissive output as M12.

Launch `build-vk/games/07-net-shooter/Debug/07-net-shooter.exe --listen`. Expected: walls + floor render with the new pipeline. Visual output is also similar to M12 unless wall meshes have a `normalMap` and/or `specularMap` set on their Material (most likely they don't, since the engine's `appendBox` doesn't assign maps — the actual wiring of CC0 PBR textures is game-side and may or may not be present). PowerShell should show the new warning string about "normal/spec maps (Blinn-Phong) lit".

If the scene appears all white or all black, the most likely cause is descriptor binding mismatch — check that the 4 writes in `submit` match the 4 bindings in `VkShader.cpp` exactly (bindings 0, 1, 2, 3 in order).

- [ ] **Step 9: Commit**

```bash
git add engine/render/backends/vulkan/VkShader.cpp engine/render/backends/vulkan/VkFrameRing.cpp engine/render/backends/vulkan/VulkanRenderer.cpp games/01-spinning-cube/main.cpp games/07-net-shooter/main.cpp
git commit -m "M13 Task 2: descriptor layout grows to 4 bindings + shader rewrites (atomic)"
```

---

## Task 3: Docs append

**Files:**
- Modify: `docs/engine/rhi-abstraction.md`

- [ ] **Step 1: Append the M13 section**

Append at the end of `docs/engine/rhi-abstraction.md`:

```markdown
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
```

- [ ] **Step 2: Run full test suite on both backends**

```
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both.

- [ ] **Step 3: Commit**

```bash
git add docs/engine/rhi-abstraction.md
git commit -m "M13 Task 3: docs — Vulkan normal + specular maps + UV scale"
```

---

## Final verification

- [ ] **Step 1: Full test suite on both backends**

```
ctest --test-dir build    -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both.

- [ ] **Step 2: Manual smoke matrix**

| Game | Backend | Expected |
|------|---------|----------|
| `01-spinning-cube` | OpenGL | unchanged |
| `01-spinning-cube` | Vulkan | identical to M12 (no normal/spec map assigned — fallbacks make output match M12) |
| `07-net-shooter` | OpenGL | unchanged |
| `07-net-shooter` | Vulkan | new warning string; identical to M12 unless wall materials wire normalMap/specularMap |
| `08-particle-storm` | Vulkan | unchanged (own pipelines) |

- [ ] **Step 3: Push branch + open PR**

```
git push -u origin feat/m13-vulkan-normal-spec-maps
gh pr create --title "M13: Vulkan normal + specular maps + UV scale" --body "..."
```

PR body should summarize: LitUbo extension, descriptor layout grew to 4 bindings, shader rewrites, no visible change to spinning-cube (no maps assigned), net-shooter waiting on game-side map wiring (separate follow-up if visible improvement is desired).

---

## Self-review notes

- **Spec coverage:** every "In scope" item maps to a task. LitUbo expansion + extractCameraPos in Task 1. Descriptor set layout (2→4 bindings) in Task 2 Step 1. Pool size bump in Task 2 Step 2. submit write block in Task 2 Step 3. Shader rewrites in Task 2 Steps 4-5. Net-shooter warning update in Task 2 Step 6. Docs append in Task 3.
- **No placeholders:** every code block contains the actual content.
- **Type consistency:** `LitUbo` field order matches between Task 1 (C++ struct), Task 2 (GLSL UBOs in shaders). `pendingCameraPos_` named consistently. `extractCameraPos` signature consistent.
- **Atomic Task 2:** the descriptor-layout change requires shader rewrites in the same commit to keep validation clean. Task 2 explicitly bundles all 5 file changes in one commit.
- **Tree-green between tasks:** Task 1 is backward-compatible with M12 shaders (UBO grows, shaders read only the first 192 bytes); the descriptor layout stays at 2 bindings until Task 2 grows it together with the shader rewrites.
