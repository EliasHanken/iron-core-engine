# M46b — Diffuse IBL (Irradiance Convolution) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bake a cosine-convolved **irradiance cubemap** from the skybox environment and replace the flat ambient term in the lit shader with a direction-varying irradiance lookup, so objects are lit by the environment's color instead of a single constant.

**Architecture:** A second compute pass on the existing `VkIblBaker` foundation (M46a) convolves the skybox cube into a small `RGBA16F` irradiance cube. The renderer bakes it once when the skybox is set, binds it at a new lit descriptor binding (10), and the lit fragment shader uses `irradiance(N)·albedo·ao` as ambient when IBL is active (falling back to the existing flat `u.ambient` when there is no skybox).

**Tech Stack:** Vulkan compute (glslang runtime compile), the M46a `VkIblBaker`/`VkCubemapStore::createHdr` foundation, the shared `StandardLitShader` GLSL.

---

## Background & conventions (read first)

- Branch: `m46b-diffuse-ibl` (stacked on `m46a-hdr-ibl-foundation`, which is in unmerged PR #70 — the `VkIblBaker`, `createHdr`, and `Ibl.h` it adds are present on this branch).
- This is **Vulkan-only**. No OpenGL changes (it's frozen; the lit shader edits are in the shared `StandardLitShader.h` which only the Vulkan path compiles via glslang — confirm by checking the OpenGL backend doesn't include it for compilation; if it does, guard appropriately, but per the engine's Vulkan-only direction the OpenGL games don't use the standard lit shader).
- **Lit descriptor bindings today:** 0 = `LitUbo`, 1–8 = samplers (diffuse, normal, metallic-roughness, shadow, **5 = sky cubemap**, planar reflection, AO, emissive), **9 = bones UBO (skinned variant only)**. Binding **10 is free in both variants** — irradiance goes there. **Bones stays at 9; do NOT move it** (no collision).
- **`LitUbo`** is 960 bytes with a spare lane `materialParams2.w`. M46b uses it for an `iblEnabled` float flag — **no struct growth**.
- Cube face direction + equirect math live in `engine/render/Ibl.h` (CPU) kept in lockstep with the GLSL. The irradiance convolution math (normalization constant) gets the same treatment.
- The skybox cube is in `cubemaps_` (a `VkCubemapStore`); `setSkybox(handle)` stores `pendingSkybox_`. The store's shared sampler is LINEAR / CLAMP_TO_EDGE / maxLod=0 — correct for sampling both the env (HDR or LDR) and the 1-mip irradiance cube.
- The standard **fragment** shader `standardLitFragSource()` is shared by BOTH the non-skinned and skinned shaders — editing it once covers both.
- One-shot GPU submit pattern (transient pool → record → submit → `vkQueueWaitIdle` → destroy) is used for setup-time bakes; `VkIblBaker::equirectFileToCubemap` is the reference.

## File structure

**Modify:**
- `engine/render/Ibl.h` — add `convolveConstantIrradiance()` (CPU normalization helper, lockstep with the GLSL).
- `tests/test_ibl.cpp` — add a normalization test + a SPIR-V compile check for the new shader.
- `engine/render/backends/vulkan/VkIblBaker.h` / `.cpp` — add `kIrradianceConvolveComputeSrc()`, a `bakeIrradiance(...)` method, and its compute pipeline/layout (init/destroy).
- `engine/render/StandardLitShader.h` — fragment shader: declare `binding = 10` irradiance cube, rewire the ambient term behind an `iblEnabled` flag.
- `engine/render/backends/vulkan/VkShader.cpp` — add binding 10 to both lit descriptor set layouts.
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — `pendingIrradiance_` + `lastBakedSkybox_` members; bake on `setSkybox`; bind binding 10 + set `iblEnabled` in both the scene and skinned per-draw paths.
- `engine/render/backends/vulkan/VkFrameRing.cpp` (or `.h`) — bump the per-frame `COMBINED_IMAGE_SAMPLER` pool capacity by one sampler/set.

---

## Task 1: Irradiance normalization math + test

The bug-prone part of irradiance convolution is the normalization (`PI * sum / nrSamples`), which must return the input radiance for a constant environment (energy conservation). Lock it with a CPU port + test.

**Files:**
- Modify: `engine/render/Ibl.h`
- Modify: `tests/test_ibl.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/test_ibl.cpp`, add `using iron::convolveConstantIrradiance;` near the other `using` lines, then add this section just before `std::puts("test_ibl: OK");`:

```cpp
    // 6. Irradiance normalization: convolving a CONSTANT environment of
    //    radiance L must return ~L (cosine-weighted hemisphere integral
    //    normalized by PI/nrSamples = 1). Validates the shader's constant.
    {
        Vec3 L{0.5f, 0.3f, 0.8f};
        Vec3 e = convolveConstantIrradiance(L, 0.025f);
        assert(approx(e.x, L.x, 0.02f));
        assert(approx(e.y, L.y, 0.02f));
        assert(approx(e.z, L.z, 0.02f));
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vk --config Debug --target test_ibl`
Expected: FAIL — `convolveConstantIrradiance` not declared.

- [ ] **Step 3: Implement the helper**

In `engine/render/Ibl.h`, before the closing `}  // namespace iron`, add:

```cpp
// CPU port of the normalization in irradianceConvolve.comp. Runs the same
// cosine-weighted hemisphere loop for a UNIFORM environment of radiance L;
// the PI/nrSamples normalization makes the result == L (energy conservation).
// Used to lock the GLSL normalization constant in tests.
inline Vec3 convolveConstantIrradiance(Vec3 L, float sampleDelta = 0.025f) {
    float sum = 0.0f;
    float nrSamples = 0.0f;
    for (float phi = 0.0f; phi < 2.0f * kIblPi; phi += sampleDelta) {
        for (float theta = 0.0f; theta < 0.5f * kIblPi; theta += sampleDelta) {
            sum += std::cos(theta) * std::sin(theta);
            nrSamples += 1.0f;
        }
    }
    const float scale = kIblPi * sum / nrSamples;  // ~= 1.0
    return Vec3{L.x * scale, L.y * scale, L.z * scale};
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-vk --config Debug --target test_ibl && ctest --test-dir build-vk -C Debug -R "^test_ibl$" --output-on-failure`
Expected: PASS — `test_ibl: OK`.

- [ ] **Step 5: Commit**

```bash
git add engine/render/Ibl.h tests/test_ibl.cpp
git commit -m "M46b: irradiance normalization CPU port + test"
```

---

## Task 2: `irradianceConvolve.comp` + `VkIblBaker::bakeIrradiance`

**Files:**
- Modify: `engine/render/backends/vulkan/VkIblBaker.h`
- Modify: `engine/render/backends/vulkan/VkIblBaker.cpp`
- Modify: `tests/test_ibl.cpp` (compile-check)

- [ ] **Step 1: Declare the shader source + method + members**

In `engine/render/backends/vulkan/VkIblBaker.h`:

After the `const char* kEquirectToCubeComputeSrc();` declaration, add:

```cpp
// Returns the embedded irradiance-convolution compute shader source.
// Exposed for a compile-check unit test.
const char* kIrradianceConvolveComputeSrc();
```

In the `VkIblBaker` class `public:` section, after `equirectFileToCubemap(...)`, add:

```cpp
    // Convolves a cosine-weighted irradiance cubemap from an environment cube
    // already in `store` (e.g. the skybox). Output is an RGBA16F cube
    // (faceSize x faceSize, 1 mip). Returns kInvalidHandle on failure.
    CubemapHandle bakeIrradiance(VkContext& ctx, VkCubemapStore& store,
                                 CubemapHandle envCube, int faceSize);
```

In the `private:` section, after `VkComputePipeline pipeline_;`, add:

```cpp
    VkDescriptorSetLayout irradianceSetLayout_ = VK_NULL_HANDLE;
    VkComputePipeline     irradiancePipeline_;
```

- [ ] **Step 2: Add the shader source + init/destroy + the bake method**

In `engine/render/backends/vulkan/VkIblBaker.cpp`:

(a) After the `kEquirectToCubeComputeSrc()` definition, add the convolution shader. Note the `faceDir` helper must be IDENTICAL to the one in `kEquirectToCubeComputeSrc` (same +X..-Z convention):

```cpp
const char* kIrradianceConvolveComputeSrc() {
    return R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform samplerCube uEnv;
layout(binding = 1, rgba16f) uniform writeonly image2DArray uOut;

const float PI = 3.14159265358979323846;

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
    vec3 N = faceDir(gid.z, u, v);

    // Tangent basis around N.
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    vec3 irradiance = vec3(0.0);
    float sampleDelta = 0.025;
    float nrSamples = 0.0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            // Spherical (tangent space) -> world.
            vec3 tangentSample = vec3(sin(theta) * cos(phi),
                                      sin(theta) * sin(phi),
                                      cos(theta));
            vec3 sampleVec = tangentSample.x * right
                           + tangentSample.y * up
                           + tangentSample.z * N;
            irradiance += textureLod(uEnv, sampleVec, 0.0).rgb
                        * cos(theta) * sin(theta);
            nrSamples += 1.0;
        }
    }
    irradiance = PI * irradiance / nrSamples;
    imageStore(uOut, gid, vec4(irradiance, 1.0));
}
)";
}
```

(b) In `VkIblBaker::init`, after the existing pipeline/sampler setup (just before `return true;`), add the irradiance pipeline. It reuses the SAME 2-binding layout shape as the equirect pass (binding 0 = combined image sampler, binding 1 = storage image, both COMPUTE), so mirror that block:

```cpp
    // M46b — irradiance convolution pipeline (env samplerCube -> irradiance cube).
    {
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
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &slInfo, nullptr,
                                             &irradianceSetLayout_));

        auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT,
                                 kIrradianceConvolveComputeSrc());
        if (spirv.empty()) {
            Log::error("VkIblBaker: irradiance compute compile failed");
            return false;
        }
        if (!irradiancePipeline_.init(ctx, spirv, irradianceSetLayout_)) return false;
    }
```

(c) In `VkIblBaker::destroy`, before `setLayout_` teardown, add:

```cpp
    irradiancePipeline_.destroy(ctx);
    if (irradianceSetLayout_) {
        vkDestroyDescriptorSetLayout(ctx.device(), irradianceSetLayout_, nullptr);
        irradianceSetLayout_ = VK_NULL_HANDLE;
    }
```

(d) Add the `bakeIrradiance` method (before the closing `}  // namespace iron`). It mirrors `equirectFileToCubemap`'s one-shot/barrier/dispatch structure but the input is an existing env cube (already in `SHADER_READ_ONLY_OPTIMAL`), so there is no `.hdr` load, no temp image, and no staging buffer. The same local `barrier` lambda shape is reused:

```cpp
CubemapHandle VkIblBaker::bakeIrradiance(VkContext& ctx, VkCubemapStore& store,
                                         CubemapHandle envCube, int faceSize) {
    if (!store.has(envCube)) return kInvalidHandle;

    // Capture env view+sampler BEFORE createHdr (store insertion is safe for
    // references in std::unordered_map, but capture to be explicit).
    const VkCubemapResource& env = store.get(envCube);
    const VkImageView envView    = env.view;
    const VkSampler   envSampler = env.sampler;

    const CubemapHandle outHandle = store.createHdr(ctx, faceSize, /*mipLevels=*/1);
    if (outHandle == kInvalidHandle) return kInvalidHandle;
    const VkCubemapResource& out = store.get(outHandle);

    // One-shot command buffer.
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
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier mb{};
        mb.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb.oldLayout           = oldL;
        mb.newLayout           = newL;
        mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image               = img;
        mb.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        mb.srcAccessMask       = srcA;
        mb.dstAccessMask       = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &mb);
    };

    // Output cube: UNDEFINED -> GENERAL for imageStore.
    barrier(out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

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
    dsInfo.pSetLayouts        = &irradianceSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsInfo, &set));

    VkDescriptorImageInfo envInfo{};
    envInfo.sampler     = envSampler;
    envInfo.imageView   = envView;
    envInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo outInfo{};
    outInfo.imageView   = out.storageViews[0];
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &envInfo;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &outInfo;
    vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, irradiancePipeline_.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            irradiancePipeline_.pipelineLayout(), 0, 1, &set, 0, nullptr);
    const std::uint32_t groups = (static_cast<std::uint32_t>(faceSize) + 7u) / 8u;
    vkCmdDispatch(cb, groups, groups, 6);

    // Output cube: GENERAL -> SHADER_READ_ONLY for the lit pass.
    barrier(out.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    vkDestroyDescriptorPool(ctx.device(), dpool, nullptr);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    return outHandle;
}
```

- [ ] **Step 3: Add the compile-check to `tests/test_ibl.cpp`**

In the existing `#ifdef IRON_RENDER_BACKEND_VULKAN` block that compile-checks the equirect shader, add a second check right after it:

```cpp
    // 7. The embedded irradiance-convolution compute shader compiles to SPIR-V.
    {
        const auto spv = iron::compileGlsl(
            VK_SHADER_STAGE_COMPUTE_BIT, iron::kIrradianceConvolveComputeSrc());
        assert(!spv.empty());
        assert(spv.front() == 0x07230203u);  // SPIR-V magic
    }
```

- [ ] **Step 4: Build + run**

Run: `cmake --build build-vk --config Debug --target ironcore && cmake --build build-vk --config Debug --target test_ibl && ctest --test-dir build-vk -C Debug -R "^test_ibl$" --output-on-failure`
Expected: `ironcore` compiles; `test_ibl` passes (a green test proves the new GLSL compiles to valid SPIR-V).

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkIblBaker.h engine/render/backends/vulkan/VkIblBaker.cpp tests/test_ibl.cpp
git commit -m "M46b: VkIblBaker::bakeIrradiance + irradianceConvolve.comp"
```

---

## Task 3: Lit shader binding 10 + ambient rewire + descriptor layout

**Files:**
- Modify: `engine/render/StandardLitShader.h`
- Modify: `engine/render/backends/vulkan/VkShader.cpp`

- [ ] **Step 1: Declare the irradiance sampler in the fragment shader**

In `engine/render/StandardLitShader.h`, in `standardLitFragSource()`, after the emissive sampler line `layout(set = 0, binding = 8) uniform sampler2D uEmissiveMap;`, add:

```glsl
layout(set = 0, binding = 10) uniform samplerCube uIrradianceCube;  // M46b diffuse IBL
```

- [ ] **Step 2: Rewire the ambient term**

In the same shader's `main()`, replace this line:

```glsl
    vec3 ambient = u.ambient.xyz * albedo * ao;
```

with (uses the perturbed shading normal `N_`; `materialParams2.w` is the `iblEnabled` flag set per-draw):

```glsl
    // M46b — diffuse IBL: when an irradiance map is bound (iblEnabled), use the
    // environment irradiance; otherwise the legacy flat ambient.
    vec3 ambient;
    if (u.materialParams2.w > 0.5) {
        ambient = texture(uIrradianceCube, N_).rgb * albedo * ao;
    } else {
        ambient = u.ambient.xyz * albedo * ao;
    }
```

Also update the `materialParams2` doc comment in the UBO block (find `// M45b — x=metallic, y=ao, z=normalScale, w spare`) to:

```glsl
    vec4 materialParams2;  // x=metallic, y=ao, z=normalScale, w=iblEnabled (M46b)
```

- [ ] **Step 3: Add binding 10 to both lit descriptor set layouts**

In `engine/render/backends/vulkan/VkShader.cpp`:

**Non-skinned layout** (the `bindings[9]` array, `bindingCount = 9`): change the array size to `bindings[10]`, add a binding-10 entry alongside the others, and set `bindingCount = 10`:

```cpp
    bindings[9].binding         = 10;   // M46b — irradiance cubemap (diffuse IBL)
    bindings[9].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[9].descriptorCount = 1;
    bindings[9].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
```
(Array index 9 holds binding number 10 — the non-skinned layout has no bones binding, so index 9 was previously unused.) Then `dslInfo.bindingCount = 10;`.

**Skinned layout** (the `bindings[10]` array with `bindings[9]` = bones, `bindingCount = 10`): change the array size to `bindings[11]`, add a binding-10 entry at array index 10 (after the bones entry at index 9), and set `bindingCount = 11`:

```cpp
    bindings[10].binding         = 10;  // M46b — irradiance cubemap (diffuse IBL)
    bindings[10].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
```
Then `dslInfo.bindingCount = 11;`.

Update the descriptor-layout comments in both functions to mention binding 10 = irradiance.

- [ ] **Step 4: Build the engine to verify shaders compile + layouts are valid**

Run: `cmake --build build-vk --config Debug --target ironcore`
Expected: compiles cleanly (the lit shaders are glslang-compiled at startup, but the C++ layout/struct must build now; full shader validation happens at runtime / visual gate).

- [ ] **Step 5: Commit**

```bash
git add engine/render/StandardLitShader.h engine/render/backends/vulkan/VkShader.cpp
git commit -m "M46b: lit shader binding 10 irradiance + iblEnabled ambient rewire"
```

---

## Task 4: Renderer wiring — bake on skybox set + bind binding 10

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/render/backends/vulkan/VkFrameRing.cpp` (descriptor pool sampler capacity)

- [ ] **Step 1: Add renderer members**

In `engine/render/backends/vulkan/VulkanRenderer.h`, next to `CubemapHandle pendingSkybox_ = kInvalidHandle;`, add:

```cpp
    CubemapHandle pendingIrradiance_ = kInvalidHandle;  // M46b — baked from the skybox
    CubemapHandle lastBakedSkybox_   = kInvalidHandle;  // M46b — bake-once guard
```

- [ ] **Step 2: Bake irradiance when the skybox changes**

In `engine/render/backends/vulkan/VulkanRenderer.cpp`, replace the body of `setSkybox`:

```cpp
void VulkanRenderer::setSkybox(CubemapHandle sky) {
    pendingSkybox_ = sky;
}
```

with:

```cpp
void VulkanRenderer::setSkybox(CubemapHandle sky) {
    pendingSkybox_ = sky;
    // M46b — bake diffuse irradiance once per distinct skybox (one-shot GPU
    // submit inside bakeIrradiance; safe here since the device is initialized
    // by the time games set a skybox).
    if (sky != lastBakedSkybox_) {
        pendingIrradiance_ = cubemaps_.has(sky)
            ? iblBaker_.bakeIrradiance(context_, cubemaps_, sky, /*faceSize=*/32)
            : kInvalidHandle;
        lastBakedSkybox_ = sky;
    }
}
```

- [ ] **Step 3: Bind irradiance + set iblEnabled in the SCENE per-draw path**

In `VulkanRenderer.cpp`, in the scene-draw recording function (where `imgInfos[0..7]` are filled for bindings 1–8 and `writes[0..8]` are written — around lines 652–695):

(a) Grow the `imgInfos` array from 8 to 9 and the `writes` array from 9 to 10 (update their declared sizes).

(b) After the emissive `imgInfos[7]` is filled, add the irradiance image info at `imgInfos[8]`:

```cpp
    // M46b — irradiance cubemap (binding 10). Falls back to the black cube when
    // no skybox is baked; the shader ignores it then (iblEnabled = 0).
    const CubemapHandle irrHandle = cubemaps_.has(pendingIrradiance_)
        ? pendingIrradiance_
        : cubemaps_.blackCubemap();
    const auto& irrTex = cubemaps_.get(irrHandle);
    imgInfos[8].sampler     = irrTex.sampler;
    imgInfos[8].imageView   = irrTex.view;
    imgInfos[8].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
```

(c) Add the binding-10 write as the last `writes[]` entry (after the binding-1..8 loop fills indices 1–8 and index 0 is the UBO — so the new entry is `writes[9]`):

```cpp
    writes[9].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet          = set;
    writes[9].dstBinding      = 10;
    writes[9].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[9].descriptorCount = 1;
    writes[9].pImageInfo      = &imgInfos[8];
```

Update the `vkUpdateDescriptorSets` write count from 9 to 10.

(d) Where the `LitUbo` is populated for this draw (the `materialParams2` assignments), set the IBL flag:

```cpp
    ubo.materialParams2.w = cubemaps_.has(pendingIrradiance_) ? 1.0f : 0.0f;  // M46b iblEnabled
```

- [ ] **Step 4: Bind irradiance + set iblEnabled in the SKINNED per-draw path**

In `VulkanRenderer.cpp`, in the skinned-draw recording function (where `imgInfos[0..7]` fill bindings 1–8, `writes[0..9]` write bindings 0–9 with `writes[9]` = bones UBO — around lines 827–880):

(a) Grow `imgInfos` from 8 to 9 and `writes` from 10 to 11 (update declared sizes).

(b) Add the same `imgInfos[8]` irradiance block as in Step 3(b).

(c) The bones write currently occupies `writes[9]`. Add the binding-10 irradiance write at `writes[10]` (keep the bones write at index 9 unchanged):

```cpp
    writes[10].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet          = set;
    writes[10].dstBinding      = 10;
    writes[10].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[10].descriptorCount = 1;
    writes[10].pImageInfo      = &imgInfos[8];
```

Update the skinned `vkUpdateDescriptorSets` write count from 10 to 11.

(d) Set the same `ubo.materialParams2.w = cubemaps_.has(pendingIrradiance_) ? 1.0f : 0.0f;` where the skinned path populates `materialParams2`.

- [ ] **Step 5: Bump the descriptor pool sampler capacity**

In `engine/render/backends/vulkan/VkFrameRing.cpp`, find the descriptor pool creation (`VkDescriptorPoolSize` for `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`). It is sized as some multiple of the max sets per frame to cover the samplers each set binds. The lit set now binds **9** samplers (bindings 1–8 + 10) instead of 8. Increase that `COMBINED_IMAGE_SAMPLER` `descriptorCount` multiplier by one set's worth (e.g. if it is `N * kMaxDescriptorSetsPerFrame`, bump `N` by 1). Read the current value and adjust; if the pool is already sized generously (e.g. a large fixed constant comfortably above `9 * sets`), leave it and note that in the commit. Do NOT guess — inspect the actual code.

- [ ] **Step 6: Build**

Run: `cmake --build build-vk --config Debug --target ironcore`
Expected: compiles cleanly.

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/backends/vulkan/VkFrameRing.cpp
git commit -m "M46b: bake irradiance on setSkybox + bind binding 10 + iblEnabled"
```

---

## Task 5: Visual gate

**Files:** none (the sandbox already sets an HDR skybox from M46a; irradiance now bakes from it automatically).

- [ ] **Step 1: Build the sandbox**

Run: `cmake --build build-vk --config Debug --target sandbox`
Expected: builds; assets copied next to the exe.

- [ ] **Step 2: Run and observe**

Run the sandbox from its build dir: `build-vk\games\11-sandbox\Debug\sandbox.exe`

Expected on screen (this is the M46b acceptance gate — a human confirms):
- The **ambient/shadowed** sides of objects are no longer a single flat grey tone — they pick up the environment's color (e.g. a greenish tint from the garden ground below, a cooler blue from the sky above). The matte spheres in the sandbox grid show a subtle top-to-bottom color gradient on their unlit sides.
- No validation-layer errors about descriptor binding 10 or image layouts.
- Toggling/removing the skybox path still works: a scene with no skybox (or where `loadHdrSkybox` fell back) uses the old flat ambient (no black objects). Confirm by checking nothing renders pure-black-ambient.

> If objects go black on their ambient side: `iblEnabled` is likely set (1.0) but the irradiance cube is black/unbaked — check that `bakeIrradiance` ran (the skybox handle is valid when `setSkybox` is called) and returned a valid handle. If the ambient looks identical to before: confirm the binding-10 descriptor write reached the shader and `materialParams2.w` is actually 1.0 in the draw.

- [ ] **Step 3: (No commit — verification only.)**

---

## Task 6: Full verification + review

- [ ] **Step 1: Run the whole test suite**

Run: `ctest --test-dir build-vk -C Debug --output-on-failure`
Expected: all tests pass (existing count + the extended `test_ibl`).

- [ ] **Step 2: Best-practices review of the branch diff**

Run: `git diff main...HEAD` (or `git diff m46a-hdr-ibl-foundation...HEAD` to isolate just M46b's commits while #70 is unmerged).

Check specifically:
- Bake-once guard: `bakeIrradiance` runs only when the skybox handle changes; no per-frame re-bake.
- Both per-draw paths (scene + skinned) bind binding 10 AND set `materialParams2.w` consistently; the write counts passed to `vkUpdateDescriptorSets` match the array sizes.
- The `faceDir` in `irradianceConvolve.comp` is identical to the one in `equirectToCube.comp` (and `Ibl.h`).
- No leak in `bakeIrradiance` (command pool + descriptor pool destroyed on every return path; early returns before pool creation don't leak).
- The flat-ambient fallback is preserved for no-skybox scenes (other games that don't set a skybox are unaffected).

- [ ] **Step 3: Open the PR** (after the visual gate passes)

Title `M46b: diffuse IBL (irradiance convolution)`, base `main` (or stacked on the M46a PR until #70 merges).

---

## Self-review notes (plan author)

- **Spec coverage (M46b row of the M46 design):** irradiance cubemap bake (Task 2), diffuse-ambient rewire at binding 10 (Tasks 3–4), replaces flat `u.ambient` (Task 3). The spec's "binding 10" matches; the spec's suggested bones-UBO move is NOT needed (bones at 9 doesn't collide with 10) — deliberate simplification, noted in Background.
- **Out of M46b scope (deferred to M46c):** prefiltered specular, BRDF LUT, the specular split-sum rewire, and raising the shared cube sampler `maxLod` for mips. M46b's irradiance cube is 1 mip, so maxLod=0 is correct now.
- **Type/name consistency:** `bakeIrradiance(VkContext&, VkCubemapStore&, CubemapHandle, int)`, `kIrradianceConvolveComputeSrc()`, `convolveConstantIrradiance(Vec3, float)`, `pendingIrradiance_`, `lastBakedSkybox_`, `materialParams2.w = iblEnabled`, binding 10 = `uIrradianceCube` — all referenced consistently across tasks.
- **Fallback safety:** when no irradiance is baked, the binding falls back to the black cubemap (valid descriptor) and `iblEnabled = 0` makes the shader use flat ambient — so non-skybox scenes are visually unchanged.
- **Context to confirm during implementation (not hardcode):** exact line numbers for the scene/skinned per-draw paths and the `imgInfos`/`writes` array declarations in `VulkanRenderer.cpp`; the `VkFrameRing` sampler pool sizing form; the exact `bindings[]` array initializer style in `VkShader.cpp`. Each step says to inspect and match the surrounding code.
