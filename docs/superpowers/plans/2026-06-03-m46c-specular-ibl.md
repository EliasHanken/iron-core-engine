# M46c — Specular IBL (Prefilter + BRDF LUT + Split-Sum) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the split-sum IBL: bake a roughness-prefiltered specular cubemap + a scene-independent BRDF integration LUT from the environment, and rewire the lit ambient to `(kD·irradiance·albedo + prefiltered·(F·brdf.x + brdf.y))·ao` — so metals reflect the environment with roughness-driven blur (truly metallic), replacing the M16 crude reflection.

**Architecture:** Two more compute passes on the M46a/b `VkIblBaker` foundation. `prefilterEnv.comp` GGX-importance-samples the env per mip (roughness = mip/(mips−1)) into a multi-mip `RGBA16F` cube; `brdfLut.comp` integrates the split-sum scale/bias into a 512² `RG16F` 2D image (baked once at init, env-independent). The lit shader gains bindings 11 (prefiltered cube) + 12 (BRDF LUT) and the split-sum term.

**Tech Stack:** Vulkan compute (glslang), the M46a/b `VkIblBaker`/`VkCubemapStore::createHdr` (already supports per-mip storage views), `VkComputePipeline` (extended with an optional push-constant range for per-mip roughness).

---

## Background & conventions (read first)

- Branch `m46c-specular-ibl` off `main` (M46a + M46b are merged in). `VkIblBaker` has `init/destroy/equirectFileToCubemap/bakeIrradiance` + the equirect/irradiance shaders; `VkCubemapStore::createHdr(faceSize, mipLevels)` already creates an `RGBA16F` cube with **one 2D-array storage view per mip** (`storageViews[m]`) + a cube sampling view; the lit shader has binding 10 = irradiance + the `iblEnabled` flag in `materialParams2.w`.
- **Lit bindings now:** 0 = UBO, 1–8 = samplers, 9 = bones (skinned only), 10 = irradiance. M46c adds **11 = prefiltered specular cube**, **12 = BRDF LUT (sampler2D)**. Bones at 9 does not collide — **no bones move**.
- **Env clamp:** the garden HDR peaks at 66560 > the `RGBA16F` ceiling 65504, so the sun is `+Inf` in the env cube. M46b clamps `min(env, 50.0)` in the irradiance convolution; **the prefilter must do the same** or it gets fireflies.
- **Shared cube sampler** (`VkCubemap.cpp` `init`) has `maxLod = 0.0` — fine for 1-mip cubes but it would clamp prefiltered-mip sampling to mip 0. M46c raises `maxLod` so `textureLod(cube, R, roughness*maxMip)` works.
- One-shot GPU submit pattern (transient pool → record → submit → `vkQueueWaitIdle`) per `equirectFileToCubemap`/`bakeIrradiance`. Shaders are runtime glslang-compiled embedded strings.
- The `faceDir(face,u,v)` helper in every bake shader must stay byte-identical to the existing ones (+X,-X,+Y,-Y,+Z,-Z).

## File structure

**Modify:**
- `engine/render/Ibl.h` — add `hammersley`, `importanceSampleGGX`, `geometrySchlickGgx`, and `integrateBrdf` (CPU ports, lockstep with the GLSL); a couple are used to test the LUT endpoints.
- `tests/test_ibl.cpp` — BRDF-LUT endpoint test + compile-checks for the 2 new shaders.
- `engine/render/backends/vulkan/VkComputePipeline.h` / `.cpp` — add an optional push-constant byte size to `init` (default 0; backward-compatible).
- `engine/render/backends/vulkan/VkIblBaker.h` / `.cpp` — `kPrefilteredSpecularComputeSrc()`, `kBrdfIntegrationComputeSrc()`, `bakePrefiltered(...)`, `initBrdfLut(...)` + `brdfLutView()`/`brdfLutSampler()`, the prefilter+LUT pipelines, and a persistent BRDF-LUT image.
- `engine/render/backends/vulkan/VkCubemap.cpp` — raise the shared sampler `maxLod`.
- `engine/render/StandardLitShader.h` — `fresnelSchlickRoughness` helper, bindings 11/12, split-sum ambient rewire, gate the M16 crude reflection behind `!iblEnabled`.
- `engine/render/backends/vulkan/VkShader.cpp` — bindings 11/12 in both lit layouts.
- `engine/render/backends/vulkan/VulkanRenderer.h` / `.cpp` — `pendingPrefiltered_` member; bake prefilter in `setSkybox` + LUT at init; bind bindings 11/12 in both per-draw paths.
- `engine/render/backends/vulkan/VkFrameRing.cpp` — bump the COMBINED_IMAGE_SAMPLER pool count 9→11 per set.

---

## Task 1: CPU BRDF math + LUT endpoint test

**Files:** Modify `engine/render/Ibl.h`, `tests/test_ibl.cpp`.

- [ ] **Step 1: Write the failing test**

In `tests/test_ibl.cpp`, add `using iron::integrateBrdf;` near the other usings, and add before `std::puts("test_ibl: OK");`:

```cpp
    // (BRDF LUT) Split-sum scale/bias endpoints. At NdotV=1, roughness=0 the
    // surface is a perfect mirror: scale ~= 1, bias ~= 0 (specular == F0).
    {
        iron::Vec2 e = integrateBrdf(1.0f, 0.0f, 1024);
        assert(approx(e.x, 1.0f, 0.02f));
        assert(approx(e.y, 0.0f, 0.02f));
        // Mid-roughness stays in [0,1] and finite.
        iron::Vec2 m = integrateBrdf(0.5f, 0.5f, 1024);
        assert(m.x >= 0.0f && m.x <= 1.0f && m.y >= 0.0f && m.y <= 1.0f);
    }
```

- [ ] **Step 2: Run, verify it fails (undeclared `integrateBrdf`)**

Run: `cmake --build build-vk --config Debug --target test_ibl`
Expected: FAIL — `integrateBrdf` not declared.

- [ ] **Step 3: Implement the helpers in `Ibl.h`**

Before the closing `}  // namespace iron`, add (uses `iron::Vec2`/`Vec3`; the file already includes `<cmath>`/`<algorithm>`; add `#include <cstdint>` if not present). NOTE: a `cross`/`normalize`/`dot` for `Vec3` — check `math/Vec.h` provides them; if not, inline the arithmetic as shown:

```cpp
// --- M46c: split-sum BRDF integration (CPU port of brdfLut.comp) ---
// Van der Corput radical inverse + Hammersley point set.
inline Vec2 hammersley(std::uint32_t i, std::uint32_t n) {
    std::uint32_t bits = i;
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    const float rdi = static_cast<float>(bits) * 2.3283064365386963e-10f;  // / 2^32
    return Vec2{static_cast<float>(i) / static_cast<float>(n), rdi};
}

// GGX importance-sampled half-vector in tangent space (N = +Z).
inline Vec3 importanceSampleGGXLocal(Vec2 xi, float roughness) {
    const float a = roughness * roughness;
    const float phi = 2.0f * kIblPi * xi.x;
    const float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    const float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
    return Vec3{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

// Smith geometry (IBL k = a^2/2) — Schlick-GGX, one direction.
inline float geometrySchlickGgx(float nDotX, float roughness) {
    const float a = roughness;
    const float k = (a * a) / 2.0f;
    return nDotX / (nDotX * (1.0f - k) + k);
}

// Integrates the split-sum scale (.x) + bias (.y) for given NdotV + roughness.
// V is in the tangent frame with N = +Z; everything stays in that frame.
inline Vec2 integrateBrdf(float nDotV, float roughness, std::uint32_t samples) {
    nDotV = std::max(nDotV, 1e-4f);
    const Vec3 V{std::sqrt(1.0f - nDotV * nDotV), 0.0f, nDotV};  // sin, 0, cos
    float A = 0.0f, B = 0.0f;
    for (std::uint32_t i = 0; i < samples; ++i) {
        const Vec2 xi = hammersley(i, samples);
        const Vec3 H  = importanceSampleGGXLocal(xi, roughness);
        const float vDotH = std::max(V.x * H.x + V.y * H.y + V.z * H.z, 0.0f);
        // L = reflect(-V, H) = 2*(V.H)*H - V
        const Vec3 L{2.0f * vDotH * H.x - V.x, 2.0f * vDotH * H.y - V.y, 2.0f * vDotH * H.z - V.z};
        const float nDotL = std::max(L.z, 0.0f);
        const float nDotH = std::max(H.z, 0.0f);
        if (nDotL > 0.0f) {
            const float g  = geometrySchlickGgx(nDotL, roughness) * geometrySchlickGgx(nDotV, roughness);
            const float gv = (g * vDotH) / std::max(nDotH * nDotV, 1e-4f);
            const float fc = std::pow(1.0f - vDotH, 5.0f);
            A += (1.0f - fc) * gv;
            B += fc * gv;
        }
    }
    return Vec2{A / static_cast<float>(samples), B / static_cast<float>(samples)};
}
```

- [ ] **Step 4: Run, verify it passes**

Run: `cmake --build build-vk --config Debug --target test_ibl && ctest --test-dir build-vk -C Debug -R "^test_ibl$" --output-on-failure`
Expected: PASS — `test_ibl: OK`.

- [ ] **Step 5: Commit**

```bash
git add engine/render/Ibl.h tests/test_ibl.cpp
git commit -m "M46c: CPU split-sum BRDF integration + LUT endpoint test"
```

---

## Task 2: Push-constant support in `VkComputePipeline`

The prefilter pass needs a per-mip `roughness` (and mip face size); pass it via a push constant. Extend `VkComputePipeline::init` with an optional push-constant byte size (default 0 → existing callers unchanged).

**Files:** Modify `engine/render/backends/vulkan/VkComputePipeline.h`, `.cpp`.

- [ ] **Step 1: Update the header signature**

In `VkComputePipeline.h`, change the `init` declaration to:
```cpp
    bool init(VkContext& ctx,
              const std::vector<std::uint32_t>& spirv,
              VkDescriptorSetLayout setLayout,
              std::uint32_t pushConstantBytes = 0);
```

- [ ] **Step 2: Wire the push-constant range into the pipeline layout**

In `VkComputePipeline.cpp` `init`, where the `VkPipelineLayoutCreateInfo` is built, add a push-constant range when `pushConstantBytes > 0`:
```cpp
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = pushConstantBytes;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &setLayout;
    layoutInfo.pushConstantRangeCount = pushConstantBytes > 0 ? 1u : 0u;
    layoutInfo.pPushConstantRanges    = pushConstantBytes > 0 ? &pcRange : nullptr;
```
(Adapt to the existing local variable names — read the current `init` first and match how it builds the layout; only add the push-constant range plumbing.)

- [ ] **Step 3: Build to confirm existing callers still compile**

Run: `cmake --build build-vk --config Debug --target ironcore`
Expected: clean (VkParticleSystem + the equirect/irradiance pipelines pass no `pushConstantBytes` → default 0 → unchanged).

- [ ] **Step 4: Commit**

```bash
git add engine/render/backends/vulkan/VkComputePipeline.h engine/render/backends/vulkan/VkComputePipeline.cpp
git commit -m "M46c: optional push-constant range in VkComputePipeline::init"
```

---

## Task 3: BRDF LUT — shader + bake + accessors

**Files:** Modify `engine/render/backends/vulkan/VkIblBaker.h`, `.cpp`, `tests/test_ibl.cpp`.

- [ ] **Step 1: Header — declare the shader, the bake, and accessors + members**

In `VkIblBaker.h`: after the existing `kIrradianceConvolveComputeSrc()` declaration add:
```cpp
const char* kBrdfIntegrationComputeSrc();
```
In the class `public:` section add:
```cpp
    // Bakes the scene-independent split-sum BRDF LUT (512x512 RG16F) once.
    bool initBrdfLut(VkContext& ctx);
    VkImageView brdfLutView()    const { return brdfLutView_; }
    VkSampler   brdfLutSampler() const { return brdfLutSampler_; }
```
In `private:` add:
```cpp
    VkDescriptorSetLayout brdfSetLayout_  = VK_NULL_HANDLE;
    VkComputePipeline     brdfPipeline_;
    VkImage               brdfLutImage_   = VK_NULL_HANDLE;
    VmaAllocation         brdfLutAlloc_   = VK_NULL_HANDLE;
    VkImageView           brdfLutView_    = VK_NULL_HANDLE;  // sampled (sampler2D)
    VkImageView           brdfLutStorage_ = VK_NULL_HANDLE;  // image2D write
    VkSampler             brdfLutSampler_ = VK_NULL_HANDLE;
```
Add `#include <vk_mem_alloc.h>` if not already included in the header (it's needed for `VmaAllocation`; if the header avoids VMA, store the alloc as `VmaAllocation` only in the .cpp via a struct — but simplest is to include it; check what `VkCubemap.h` does and match).

- [ ] **Step 2: `.cpp` — the BRDF integration shader**

After `kIrradianceConvolveComputeSrc()`'s definition add:
```cpp
const char* kBrdfIntegrationComputeSrc() {
    return R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, rg16f) uniform writeonly image2D uLut;

const float PI = 3.14159265358979323846;
const uint  SAMPLES = 1024u;

float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) { return vec2(float(i) / float(n), radicalInverseVdC(i)); }

vec3 importanceSampleGGX(vec2 xi, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a*a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    return vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);  // tangent space, N=+Z
}
float geomSchlickGGX(float nDotX, float roughness) {
    float k = (roughness * roughness) / 2.0;
    return nDotX / (nDotX * (1.0 - k) + k);
}

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(uLut);
    if (gid.x >= size.x || gid.y >= size.y) return;

    float nDotV     = max((float(gid.x) + 0.5) / float(size.x), 1e-4);
    float roughness =      (float(gid.y) + 0.5) / float(size.y);
    vec3 V = vec3(sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);

    float A = 0.0, B = 0.0;
    for (uint i = 0u; i < SAMPLES; ++i) {
        vec2 xi = hammersley(i, SAMPLES);
        vec3 H  = importanceSampleGGX(xi, roughness);
        float vDotH = max(dot(V, H), 0.0);
        vec3  L = 2.0 * vDotH * H - V;
        float nDotL = max(L.z, 0.0);
        float nDotH = max(H.z, 0.0);
        if (nDotL > 0.0) {
            float g  = geomSchlickGGX(nDotL, roughness) * geomSchlickGGX(nDotV, roughness);
            float gv = (g * vDotH) / max(nDotH * nDotV, 1e-4);
            float fc = pow(1.0 - vDotH, 5.0);
            A += (1.0 - fc) * gv;
            B += fc * gv;
        }
    }
    imageStore(uLut, gid, vec4(A / float(SAMPLES), B / float(SAMPLES), 0.0, 0.0));
}
)";
}
```

- [ ] **Step 3: `.cpp` — `initBrdfLut`**

Add the method. It creates the persistent `RG16F` storage+sampled image, its two views (sampled + storage) and a LINEAR/CLAMP sampler, a 1-binding compute layout (binding 0 = STORAGE_IMAGE), compiles + inits `brdfPipeline_`, then bakes once via a one-shot submit (UNDEFINED→GENERAL, dispatch `(512+7)/8` squared, GENERAL→SHADER_READ_ONLY). Mirror the `bakeIrradiance` one-shot/barrier scaffold (no env input — only the storage image). Use `VK_FORMAT_R16G16_SFLOAT`, extent 512×512, usage `STORAGE|SAMPLED`. Call it from the renderer at init (Task 7). Destroy all 6 handles in `VkIblBaker::destroy`.

```cpp
bool VkIblBaker::initBrdfLut(VkContext& ctx) {
    const std::uint32_t kSize = 512;

    // Image (RG16F, storage + sampled).
    VkImageCreateInfo ii{};
    ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ii.imageType=VK_IMAGE_TYPE_2D;
    ii.format=VK_FORMAT_R16G16_SFLOAT; ii.extent={kSize,kSize,1};
    ii.mipLevels=1; ii.arrayLayers=1; ii.samples=VK_SAMPLE_COUNT_1_BIT;
    ii.tiling=VK_IMAGE_TILING_OPTIMAL;
    ii.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{}; ai.usage=VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &ii, &ai, &brdfLutImage_, &brdfLutAlloc_, nullptr));

    VkImageViewCreateInfo vi{};
    vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image=brdfLutImage_;
    vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=VK_FORMAT_R16G16_SFLOAT;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &brdfLutView_));
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &brdfLutStorage_));

    VkSamplerCreateInfo si{};
    si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter=VK_FILTER_LINEAR; si.minFilter=VK_FILTER_LINEAR;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device(), &si, nullptr, &brdfLutSampler_));

    // Compute layout: binding 0 = storage image.
    VkDescriptorSetLayoutBinding b{};
    b.binding=0; b.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b.descriptorCount=1; b.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo slInfo{};
    slInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slInfo.bindingCount=1; slInfo.pBindings=&b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &slInfo, nullptr, &brdfSetLayout_));

    auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, kBrdfIntegrationComputeSrc());
    if (spirv.empty()) { Log::error("VkIblBaker: BRDF LUT compute compile failed"); return false; }
    if (!brdfPipeline_.init(ctx, spirv, brdfSetLayout_)) return false;

    // One-shot bake.
    VkCommandPool pool=VK_NULL_HANDLE; VkCommandPoolCreateInfo pInfo{};
    pInfo.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pInfo.queueFamilyIndex=ctx.graphicsFamily();
    pInfo.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pInfo, nullptr, &pool));
    VkCommandBuffer cb=VK_NULL_HANDLE; VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cbInfo.commandPool=pool;
    cbInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbInfo.commandBufferCount=1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));
    VkCommandBufferBeginInfo begin{}; begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));

    auto barrier=[&](VkImageLayout o,VkImageLayout n,VkAccessFlags sa,VkAccessFlags da,
                     VkPipelineStageFlags ss,VkPipelineStageFlags ds){
        VkImageMemoryBarrier mb{}; mb.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb.oldLayout=o; mb.newLayout=n; mb.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        mb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; mb.image=brdfLutImage_;
        mb.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        mb.srcAccessMask=sa; mb.dstAccessMask=da;
        vkCmdPipelineBarrier(cb,ss,ds,0,0,nullptr,0,nullptr,1,&mb);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    VkDescriptorPoolSize ps{}; ps.type=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; ps.descriptorCount=1;
    VkDescriptorPoolCreateInfo dpInfo{}; dpInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets=1; dpInfo.poolSizeCount=1; dpInfo.pPoolSizes=&ps;
    VkDescriptorPool dpool=VK_NULL_HANDLE; VK_CHECK(vkCreateDescriptorPool(ctx.device(),&dpInfo,nullptr,&dpool));
    VkDescriptorSetAllocateInfo dsInfo{}; dsInfo.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool=dpool; dsInfo.descriptorSetCount=1; dsInfo.pSetLayouts=&brdfSetLayout_;
    VkDescriptorSet set=VK_NULL_HANDLE; VK_CHECK(vkAllocateDescriptorSets(ctx.device(),&dsInfo,&set));
    VkDescriptorImageInfo info{}; info.imageView=brdfLutStorage_; info.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w{}; w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=set;
    w.dstBinding=0; w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w.descriptorCount=1; w.pImageInfo=&info;
    vkUpdateDescriptorSets(ctx.device(),1,&w,0,nullptr);

    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,brdfPipeline_.pipeline());
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,brdfPipeline_.pipelineLayout(),0,1,&set,0,nullptr);
    const std::uint32_t g=(kSize+7u)/8u; vkCmdDispatch(cb,g,g,1);

    barrier(VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{}; submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount=1; submit.pCommandBuffers=&cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(),1,&submit,VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
    vkDestroyDescriptorPool(ctx.device(),dpool,nullptr);
    vkDestroyCommandPool(ctx.device(),pool,nullptr);
    return true;
}
```

- [ ] **Step 4: `.cpp` — destroy the LUT in `VkIblBaker::destroy`**

Add (before the existing teardown), guarding each handle:
```cpp
    brdfPipeline_.destroy(ctx);
    if (brdfSetLayout_)  { vkDestroyDescriptorSetLayout(ctx.device(), brdfSetLayout_, nullptr); brdfSetLayout_=VK_NULL_HANDLE; }
    if (brdfLutSampler_) { vkDestroySampler(ctx.device(), brdfLutSampler_, nullptr); brdfLutSampler_=VK_NULL_HANDLE; }
    if (brdfLutView_)    { vkDestroyImageView(ctx.device(), brdfLutView_, nullptr); brdfLutView_=VK_NULL_HANDLE; }
    if (brdfLutStorage_) { vkDestroyImageView(ctx.device(), brdfLutStorage_, nullptr); brdfLutStorage_=VK_NULL_HANDLE; }
    if (brdfLutImage_)   { vmaDestroyImage(ctx.allocator(), brdfLutImage_, brdfLutAlloc_); brdfLutImage_=VK_NULL_HANDLE; }
```

- [ ] **Step 5: Test compile-check + build**

In `tests/test_ibl.cpp`, inside the `#ifdef IRON_RENDER_BACKEND_VULKAN` block, add:
```cpp
    {  // BRDF integration shader compiles.
        const auto spv = iron::compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, iron::kBrdfIntegrationComputeSrc());
        assert(!spv.empty());
        assert(spv.front() == 0x07230203u);
    }
```
Run: `cmake --build build-vk --config Debug --target ironcore && cmake --build build-vk --config Debug --target test_ibl && ctest --test-dir build-vk -C Debug -R "^test_ibl$" --output-on-failure`
Expected: builds; test passes.

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkIblBaker.h engine/render/backends/vulkan/VkIblBaker.cpp tests/test_ibl.cpp
git commit -m "M46c: BRDF integration LUT (brdfLut.comp + persistent RG16F image)"
```

---

## Task 4: Prefiltered specular — shader + `bakePrefiltered`

**Files:** Modify `engine/render/backends/vulkan/VkIblBaker.h`, `.cpp`, `tests/test_ibl.cpp`.

- [ ] **Step 1: Header — declare shader + method + members**

In `VkIblBaker.h`: add `const char* kPrefilteredSpecularComputeSrc();`. In `public:`:
```cpp
    // Prefilters env specular into a multi-mip RGBA16F cube (roughness = mip/(mips-1)).
    CubemapHandle bakePrefiltered(VkContext& ctx, VkCubemapStore& store,
                                  CubemapHandle envCube, int faceSize, int mipLevels);
```
In `private:`:
```cpp
    VkDescriptorSetLayout prefilterSetLayout_ = VK_NULL_HANDLE;
    VkComputePipeline     prefilterPipeline_;
```

- [ ] **Step 2: `.cpp` — the prefilter shader**

`faceDir` byte-identical to the others. `roughness` comes from a push constant. Clamps env (`min(.,50)`). For roughness 0 it samples the env directly (mirror).
```cpp
const char* kPrefilteredSpecularComputeSrc() {
    return R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0) uniform samplerCube uEnv;
layout(binding = 1, rgba16f) uniform writeonly image2DArray uOut;
layout(push_constant) uniform PC { float roughness; } pc;

const float PI = 3.14159265358979323846;
const uint  SAMPLES = 256u;
const float MAX_RADIANCE = 50.0;

vec3 faceDir(int face, float u, float v) {
    vec3 d;
    if      (face == 0) d = vec3( 1.0, -v,   -u);
    else if (face == 1) d = vec3(-1.0, -v,    u);
    else if (face == 2) d = vec3( u,    1.0,  v);
    else if (face == 3) d = vec3( u,   -1.0, -v);
    else if (face == 4) d = vec3( u,   -v,    1.0);
    else                d = vec3(-u,   -v,   -1.0);
    return normalize(d);
}
float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint n) { return vec2(float(i)/float(n), radicalInverseVdC(i)); }
vec3 importanceSampleGGX(vec2 xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a*a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    vec3 H = vec3(cos(phi)*sinT, sin(phi)*sinT, cosT);
    vec3 up = abs(N.z) < 0.999 ? vec3(0,0,1) : vec3(1,0,0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent*H.x + bitangent*H.y + N*H.z);
}
void main() {
    ivec3 gid = ivec3(gl_GlobalInvocationID);
    ivec2 size = imageSize(uOut).xy;
    if (gid.x >= size.x || gid.y >= size.y) return;
    float u = 2.0*(float(gid.x)+0.5)/float(size.x) - 1.0;
    float v = 2.0*(float(gid.y)+0.5)/float(size.y) - 1.0;
    vec3 N = faceDir(gid.z, u, v);
    vec3 R = N; vec3 V = N;  // split-sum assumption: V = R = N
    float rough = pc.roughness;
    vec3 prefiltered = vec3(0.0);
    float totalW = 0.0;
    for (uint i = 0u; i < SAMPLES; ++i) {
        vec2 xi = hammersley(i, SAMPLES);
        vec3 H = importanceSampleGGX(xi, N, rough);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float nDotL = max(dot(N, L), 0.0);
        if (nDotL > 0.0) {
            vec3 c = min(textureLod(uEnv, L, 0.0).rgb, vec3(MAX_RADIANCE));
            prefiltered += c * nDotL;
            totalW += nDotL;
        }
    }
    prefiltered = totalW > 0.0 ? prefiltered / totalW : min(textureLod(uEnv, N, 0.0).rgb, vec3(MAX_RADIANCE));
    imageStore(uOut, gid, vec4(prefiltered, 1.0));
}
)";
}
```

- [ ] **Step 3: `.cpp` — init the prefilter pipeline (push constant = `sizeof(float)`)**

In `VkIblBaker::init`, after the irradiance pipeline block, add a prefilter pipeline block. Same 2-binding layout as irradiance (binding 0 samplerCube env, binding 1 storage image2DArray), but `prefilterPipeline_.init(ctx, spirv, prefilterSetLayout_, /*pushConstantBytes=*/sizeof(float))`. Use distinct local names (`pfB`/`pfSlInfo`/`pfSpirv`) to avoid the `/W4` C4456 shadowing the earlier blocks would trigger.

- [ ] **Step 4: `.cpp` — destroy prefilter in `destroy`**

```cpp
    prefilterPipeline_.destroy(ctx);
    if (prefilterSetLayout_) { vkDestroyDescriptorSetLayout(ctx.device(), prefilterSetLayout_, nullptr); prefilterSetLayout_=VK_NULL_HANDLE; }
```

- [ ] **Step 5: `.cpp` — `bakePrefiltered`**

Mirror `bakeIrradiance`, but: allocate `store.createHdr(ctx, faceSize, mipLevels)`; transition ALL mips (use `levelCount = mipLevels` in the barrier subresourceRange, `layerCount = 6`); then **loop over mips** `m = 0..mipLevels-1`: compute `mipSize = max(1, faceSize >> m)`, `roughness = mipLevels>1 ? float(m)/float(mipLevels-1) : 0.0`, allocate a descriptor set binding `out.storageViews[m]` (GENERAL) + env (SHADER_READ), `vkCmdPushConstants(cb, prefilterPipeline_.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float), &roughness)`, dispatch `groups=(mipSize+7)/8` squared × 6. After the loop, transition all mips GENERAL→SHADER_READ_ONLY. One descriptor pool sized for `mipLevels` sets (maxSets=mipLevels, COMBINED_IMAGE_SAMPLER count=mipLevels, STORAGE_IMAGE count=mipLevels). Submit + waitIdle. Return the handle.

Key barrier detail: the cube-image barrier subresourceRange must cover `levelCount = static_cast<uint32_t>(mipLevels)` (not 1) so all mips transition. Capture env view/sampler before `createHdr` (as in `bakeIrradiance`).

- [ ] **Step 6: Test compile-check + build**

Add to `tests/test_ibl.cpp` (inside the Vulkan `#ifdef`):
```cpp
    {  // Prefiltered specular shader compiles.
        const auto spv = iron::compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, iron::kPrefilteredSpecularComputeSrc());
        assert(!spv.empty());
        assert(spv.front() == 0x07230203u);
    }
```
Run: `cmake --build build-vk --config Debug --target ironcore && cmake --build build-vk --config Debug --target test_ibl && ctest --test-dir build-vk -C Debug -R "^test_ibl$" --output-on-failure`
Expected: builds; passes (green proves the prefilter GLSL — incl. the push_constant block — compiles).

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VkIblBaker.h engine/render/backends/vulkan/VkIblBaker.cpp tests/test_ibl.cpp
git commit -m "M46c: prefiltered specular bake (prefilterEnv.comp, per-mip roughness)"
```

---

## Task 5: Raise the shared cube sampler maxLod

**Files:** Modify `engine/render/backends/vulkan/VkCubemap.cpp`.

- [ ] **Step 1: Allow mip sampling**

In `VkCubemapStore::init`, change the shared sampler's `sInfo.maxLod = 0.0f;` to:
```cpp
    sInfo.maxLod = VK_LOD_CLAMP_NONE;  // M46c — prefiltered specular cube has mips
```
(`VK_LOD_CLAMP_NONE` = 1000.0. Safe for 1-mip cubes — sampling clamps to available mips.)

- [ ] **Step 2: Build**

Run: `cmake --build build-vk --config Debug --target ironcore`
Expected: clean.

- [ ] **Step 3: Commit**

```bash
git add engine/render/backends/vulkan/VkCubemap.cpp
git commit -m "M46c: raise shared cube sampler maxLod for prefiltered mips"
```

---

## Task 6: Lit shader split-sum + descriptor layouts

**Files:** Modify `engine/render/StandardLitShader.h`, `engine/render/backends/vulkan/VkShader.cpp`.

- [ ] **Step 1: Add the roughness-aware Fresnel helper**

In `standardLitFragSource()`, after the existing `fresnelSchlick`/`distributionGGX`/`geometrySmith`/`pbrContrib` helpers, add:
```glsl
vec3 fresnelSchlickRoughness(float cosT, vec3 F0, float rough) {
    return F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0);
}
```

- [ ] **Step 2: Declare bindings 11 + 12**

After `layout(set = 0, binding = 10) uniform samplerCube uIrradianceCube;` add:
```glsl
layout(set = 0, binding = 11) uniform samplerCube uPrefiltered;   // M46c specular IBL
layout(set = 0, binding = 12) uniform sampler2D   uBrdfLut;       // M46c split-sum LUT
```

- [ ] **Step 3: Replace the M46b diffuse-only ambient with the split-sum term**

Replace the M46b ambient block:
```glsl
    vec3 ambient;
    if (u.materialParams2.w > 0.5) {
        ambient = texture(uIrradianceCube, N_).rgb * albedo * ao;
    } else {
        ambient = u.ambient.xyz * albedo * ao;
    }
```
with:
```glsl
    vec3 ambient;
    if (u.materialParams2.w > 0.5) {
        // M46c — split-sum IBL: diffuse irradiance + prefiltered specular.
        float nDotV = max(dot(N_, V), 0.0);
        vec3  F     = fresnelSchlickRoughness(nDotV, F0, roughness);
        vec3  kD    = (vec3(1.0) - F) * (1.0 - metallic);
        vec3  diffuseIBL  = texture(uIrradianceCube, N_).rgb * albedo;
        vec3  R           = reflect(-V, N_);
        float maxMip      = float(textureQueryLevels(uPrefiltered) - 1);
        vec3  prefiltered = textureLod(uPrefiltered, R, roughness * maxMip).rgb;
        vec2  brdf        = texture(uBrdfLut, vec2(nDotV, roughness)).rg;
        vec3  specularIBL = prefiltered * (F * brdf.x + brdf.y);
        ambient = (kD * diffuseIBL + specularIBL) * ao;
    } else {
        ambient = u.ambient.xyz * albedo * ao;
    }
```

- [ ] **Step 4: Gate the M16 crude cubemap reflection behind `!iblEnabled`**

In the reflection block, leave the M17 planar branch (`if (u.reflectionParams.x > 0.5)`) UNCHANGED, but change the M16 cubemap fallback condition from:
```glsl
    } else if (reflectivity > 0.0) {
```
to:
```glsl
    } else if (reflectivity > 0.0 && u.materialParams2.w < 0.5) {
```
so the crude sky-cube reflection only runs when IBL is OFF (IBL specular replaces it otherwise). Add a short comment noting the M16 reflection is superseded by IBL when a skybox is present.

- [ ] **Step 5: Add bindings 11/12 to both descriptor set layouts**

In `VkShader.cpp`:
- **Non-skinned** (`bindings[10]`, `bindingCount=10`): grow to `bindings[12]`; add at array index 10 a `binding=11` COMBINED_IMAGE_SAMPLER/FRAGMENT and at index 11 a `binding=12` COMBINED_IMAGE_SAMPLER/FRAGMENT; `bindingCount=12`.
- **Skinned** (`bindings[11]`, `bindingCount=11`; bones at index containing `binding=9`, irradiance at the entry with `binding=10`): grow to `bindings[13]`; add `binding=11` and `binding=12` entries at the next two array indices; `bindingCount=13`.
Match the exact initializer style of the existing entries. Update the layout comments.

- [ ] **Step 6: Build**

Run: `cmake --build build-vk --config Debug --target ironcore`
Expected: clean (C++ builds; lit GLSL validated at runtime/visual gate).

- [ ] **Step 7: Commit**

```bash
git add engine/render/StandardLitShader.h engine/render/backends/vulkan/VkShader.cpp
git commit -m "M46c: lit shader split-sum specular (bindings 11/12) + gate M16 reflection"
```

---

## Task 7: Renderer wiring

**Files:** Modify `engine/render/backends/vulkan/VulkanRenderer.h`, `.cpp`, `engine/render/backends/vulkan/VkFrameRing.cpp`.

- [ ] **Step 1: Member**

In `VulkanRenderer.h`, next to `pendingIrradiance_`, add:
```cpp
    CubemapHandle pendingPrefiltered_ = kInvalidHandle;  // M46c — baked from the skybox
```

- [ ] **Step 2: Bake the LUT at init + prefilter on setSkybox**

In `VulkanRenderer.cpp`, right after the existing `iblBaker_.init(context_)` success check, add:
```cpp
    if (!iblBaker_.initBrdfLut(context_)) {
        Log::error("VulkanRenderer: BRDF LUT bake failed");
        return false;
    }
```
In `setSkybox`, alongside the irradiance bake, add the prefilter bake:
```cpp
        pendingPrefiltered_ = cubemaps_.has(sky)
            ? iblBaker_.bakePrefiltered(context_, cubemaps_, sky, /*faceSize=*/128, /*mipLevels=*/5)
            : kInvalidHandle;
```

- [ ] **Step 3: Bind bindings 11 + 12 in the SCENE per-draw path**

Grow `imgInfos` 9→11 and `writes` 10→12. After the irradiance `imgInfos[8]`:
```cpp
    // M46c — prefiltered specular (binding 11).
    const CubemapHandle prefiltHandle = cubemaps_.has(pendingPrefiltered_)
        ? pendingPrefiltered_ : cubemaps_.blackCubemap();
    const auto& prefiltTex = cubemaps_.get(prefiltHandle);
    imgInfos[9].sampler     = prefiltTex.sampler;
    imgInfos[9].imageView   = prefiltTex.view;
    imgInfos[9].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // M46c — BRDF LUT (binding 12).
    imgInfos[10].sampler     = iblBaker_.brdfLutSampler();
    imgInfos[10].imageView   = iblBaker_.brdfLutView();
    imgInfos[10].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
```
Add the two writes after the binding-10 write (next free `writes[]` indices — scene: `writes[10]` binding 11, `writes[11]` binding 12), each COMBINED_IMAGE_SAMPLER pointing at `&imgInfos[9]` / `&imgInfos[10]`. Update the scene `vkUpdateDescriptorSets` count 10→12.

- [ ] **Step 4: Bind bindings 11 + 12 in the SKINNED per-draw path**

Grow its `imgInfos` 9→11 and `writes` 11→13. Add the same `imgInfos[9]`/`imgInfos[10]` blocks. The bones write is at `writes[9]`, the irradiance write at `writes[10]`; add binding-11 at `writes[11]` and binding-12 at `writes[12]`. Update the skinned `vkUpdateDescriptorSets` count 11→13.

- [ ] **Step 5: Descriptor pool capacity**

In `VkFrameRing.cpp`, bump the COMBINED_IMAGE_SAMPLER pool `descriptorCount` from `9 * kMaxDescriptorSetsPerFrame` to `11 * kMaxDescriptorSetsPerFrame` (the lit set now binds 11 image samplers: bindings 1–8, 10, 11, 12). Read the actual expression and adjust.

- [ ] **Step 6: Build both backends**

Run: `cmake --build build-vk --config Debug --target ironcore` and `cmake --build build --config Debug --target ironcore`.
Expected: both clean (no Renderer interface change this milestone, but build both to be safe per the clean-build lesson).

- [ ] **Step 7: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/backends/vulkan/VkFrameRing.cpp
git commit -m "M46c: bake prefilter + LUT, bind bindings 11/12 in both draw paths"
```

---

## Task 8: Visual gate

**Files:** none (sandbox already sets the HDR skybox; the PBR sphere grid sweeps metallic × roughness).

- [ ] **Step 1: Build the sandbox**

Run: `cmake --build build-vk --config Debug --target sandbox`

- [ ] **Step 2: Run + observe** (human gate)

Run `build-vk\games\11-sandbox\Debug\sandbox.exe`. Expected:
- The **metal** spheres (right side of the grid, metallic→1) now **reflect the garden environment**, sharp at low roughness (front) and progressively blurred toward the back (high roughness) — they look genuinely metallic, not flat grey.
- **Dielectric** spheres (left, metallic 0) show a subtle environment specular sheen at grazing angles (Fresnel) over the diffuse irradiance.
- The DamagedHelmet's metal reads as metal reflecting the garden.
- No firefly speckles (env is clamped in the prefilter); no validation errors about bindings 11/12, mip sampling, or image layouts.

> If metals are black/dark: prefilter handle invalid or maxLod not raised (mip sampling clamped). If metals look identical to M46b (no sharp reflection): confirm bindings 11/12 reached the shader and `textureQueryLevels(uPrefiltered)` > 1.

- [ ] **Step 3: (verification only, no commit)**

---

## Task 9: Full verification + review

- [ ] **Step 1: Build ALL targets + full suite** (per the clean-build lesson)

Run: `cmake --build build-vk --config Debug` then `ctest --test-dir build-vk -C Debug --output-on-failure`.
Expected: all targets compile; all tests pass (incl. `test_ibl` with the BRDF endpoint + 2 new compile-checks).

- [ ] **Step 2: Branch review**

`git diff main...HEAD`. Check: prefilter clamps env (`min(.,50)`); prefilter barrier covers ALL mips (`levelCount=mipLevels`); push constant set per mip; both per-draw paths bind 11+12 AND counts match; the LUT image destroyed in `destroy`; `faceDir` identical across all bake shaders; M17 planar reflection untouched, M16 gated on `!iblEnabled`; sampler maxLod raised; no leaks in `initBrdfLut`/`bakePrefiltered`.

- [ ] **Step 3: PR** (after visual gate passes)

Title `M46c: specular IBL (prefilter + BRDF LUT + split-sum)`, base `main`.

---

## Self-review notes (plan author)

- **Spec coverage (M46c row):** prefiltered specular mips (Task 4), BRDF LUT (Task 3), split-sum rewire + bindings 11/12 (Tasks 6–7). The spec's "move bones to a fixed binding" is NOT needed — 11/12 don't collide with bones at 9 (confirmed across M46b too). Deliberate simplification.
- **Deviations from the M46-design's binding table:** design said bones→15; we keep bones at 9 (no collision). Design said clamp/maxLod as carry-forwards from M46b — both addressed (Tasks 4 + 5).
- **Type/name consistency:** `bakePrefiltered(VkContext&, VkCubemapStore&, CubemapHandle, int, int)`, `initBrdfLut(VkContext&)`, `brdfLutView()`/`brdfLutSampler()`, `kPrefilteredSpecularComputeSrc()`, `kBrdfIntegrationComputeSrc()`, `integrateBrdf(float,float,uint32_t)`, `pendingPrefiltered_`, bindings 11=prefiltered/12=BRDF — consistent across tasks.
- **Fallback safety:** no-skybox → `pendingPrefiltered_` invalid → black-cube fallback + `iblEnabled=0` → shader uses flat ambient + (M16 gate now off→) no crude reflection change for non-skybox games beyond what M46b already did. Existing non-IBL games unaffected.
- **The CPU/GLSL lockstep:** `integrateBrdf` mirrors `brdfLut.comp` (same Hammersley/GGX/Schlick-GGX `k=a²/2`), so the endpoint test guards the GLSL constants. The prefilter's GGX importance sampling shares the same Hammersley/`importanceSampleGGX` math (different `k`/use), so the `test_ibl` compile-checks plus the endpoint test cover the constants; the prefilter's visual correctness is the gate.
- **Context to confirm during implementation:** the exact `init`/`destroy`/`setSkybox` local names + the per-draw `imgInfos`/`writes` array declarations and indices in `VulkanRenderer.cpp` (mirror M46b); the `VkFrameRing` pool expression; whether `VkIblBaker.h` already transitively includes VMA (for `VmaAllocation` members). Each step says to inspect and match.
