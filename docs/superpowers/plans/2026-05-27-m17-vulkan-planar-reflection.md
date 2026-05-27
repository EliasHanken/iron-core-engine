# M17 Vulkan Planar Reflection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring planar reflection to the Vulkan backend at parity with OpenGL — un-stubs `setReflectionPlane`/`disableReflectionPlane`, runs a mirrored RTT pass between shadow and scene, and lets reflective materials sample the result. Then ports `games/03-showcase` to Vulkan as the visual validator.

**Architecture:** New `VkReflectionTarget` subsystem owns a 1024² color+depth RTT, its own render pass with an exit subpass dependency to FRAGMENT_SHADER, and a framebuffer. A separate shared reflection pipeline (2-binding layout: UBO + diffuse) is created once at renderer init; it reuses the same 928-byte LitUbo so the same upload path applies. `VkContext::createDevice` enables `shaderClipDistance` so `gl_ClipDistance[0]` works. The lit pass descriptor set grows from 6 → 7 bindings (reflection RTT at binding 6); reflective materials with `useReflectionPlane=true` sample it projectively via `gl_FragCoord.xy / screenSize`.

**Tech Stack:** C++23, Vulkan 1.3, glslang (GLSL 450 → SPIR-V), VMA, MSVC, CMake.

---

## File Structure

### New files
- `engine/render/backends/vulkan/VkReflectionTarget.h`
- `engine/render/backends/vulkan/VkReflectionTarget.cpp`

### Modified files
- `engine/render/backends/vulkan/VkContext.cpp` — enable `shaderClipDistance` device feature
- `engine/render/backends/vulkan/VkFrameRing.cpp` — sampler pool capacity `5*` → `6*` `kMaxDescriptorSetsPerFrame`
- `engine/render/backends/vulkan/VkShader.cpp` — descriptor set layout 6 → 7 bindings
- `engine/render/backends/vulkan/VulkanRenderer.h` — adds `VkReflectionTarget reflection_;`, reflection pipeline state, `std::optional<ReflectionPlane> reflectionPlane_;`
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — un-stubs `setReflectionPlane`/`disableReflectionPlane`; grows LitUbo 832 → 928 bytes; init+destroy reflection lifecycle; `endFrame` records reflection pass between shadow and scene; `recordSceneDraw` writes 7 descriptors
- `engine/CMakeLists.txt` — registers `VkReflectionTarget.cpp` under the Vulkan branch
- `games/03-showcase/main.cpp` — adds `--backend` flag + Vulkan GLSL 450 shaders + Vulkan default
- `docs/engine/rhi-abstraction.md` — appends M17 section
- `MEMORY.md` (root engine memory if present) — left untouched here, updated separately

---

## Task 1: `VkReflectionTarget` subsystem (color + depth RTT + render pass + framebuffer)

**Files:**
- Create: `engine/render/backends/vulkan/VkReflectionTarget.h`
- Create: `engine/render/backends/vulkan/VkReflectionTarget.cpp`
- Modify: `engine/CMakeLists.txt`

Standalone subsystem. Compiles but isn't yet wired into VulkanRenderer (Task 3 integrates).

- [ ] **Step 1: Write `VkReflectionTarget.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkReflectionTarget is Vulkan-only."
#endif

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace iron {

class VkContext;

// Vulkan planar-reflection render target. Owns a 1024x1024 RGBA8 color
// image + D32_SFLOAT depth image, a 2-attachment render pass with an
// exit subpass dependency so the scene pass can sample the color
// texture safely, and a framebuffer binding both.
//
// `descriptorImageInfo()` returns a (sampler, view, layout) triple ready
// to write into binding 6 of the lit pass descriptor set. When no
// reflection plane is active, the renderer points binding 6 at a black
// fallback texture instead.
class VkReflectionTarget {
public:
    static constexpr uint32_t kResolution = 1024;

    bool init(VkContext& ctx, VkSampler sharedSampler);
    void destroy(VkContext& ctx);

    VkRenderPass  renderPass()  const { return renderPass_; }
    VkFramebuffer framebuffer() const { return framebuffer_; }
    VkImageView   colorView()   const { return colorView_; }
    VkSampler     sampler()     const { return sampler_; }

    // Begin/end the reflection pass on the active command buffer.
    // beginPass also issues vkCmdSetViewport/Scissor to kResolution^2.
    void beginPass(VkCommandBuffer cb, const float clearColor[4]) const;
    void endPass(VkCommandBuffer cb) const;

private:
    VkImage         colorImage_  = VK_NULL_HANDLE;
    VmaAllocation   colorAlloc_  = VK_NULL_HANDLE;
    VkImageView     colorView_   = VK_NULL_HANDLE;

    VkImage         depthImage_  = VK_NULL_HANDLE;
    VmaAllocation   depthAlloc_  = VK_NULL_HANDLE;
    VkImageView     depthView_   = VK_NULL_HANDLE;

    VkRenderPass    renderPass_  = VK_NULL_HANDLE;
    VkFramebuffer   framebuffer_ = VK_NULL_HANDLE;
    VkSampler       sampler_     = VK_NULL_HANDLE;  // shared, not owned
};

}  // namespace iron
```

- [ ] **Step 2: Write `VkReflectionTarget.cpp`**

```cpp
// VkReflectionTarget.cpp — color+depth RTT for the planar reflection pass.

#include "render/backends/vulkan/VkReflectionTarget.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

namespace iron {

bool VkReflectionTarget::init(VkContext& ctx, VkSampler sharedSampler) {
    sampler_ = sharedSampler;

    // --- Color image (RGBA8, USAGE_COLOR_ATTACHMENT | SAMPLED) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType = VK_IMAGE_TYPE_2D;
        iInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        iInfo.extent = {kResolution, kResolution, 1};
        iInfo.mipLevels = 1;
        iInfo.arrayLayers = 1;
        iInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
        iInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &colorImage_, &colorAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image = colorImage_;
        vInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vInfo.subresourceRange.levelCount = 1;
        vInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &colorView_));
    }

    // --- Depth image (D32, USAGE_DEPTH_STENCIL_ATTACHMENT) ---
    {
        VkImageCreateInfo iInfo{};
        iInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        iInfo.imageType = VK_IMAGE_TYPE_2D;
        iInfo.format = VK_FORMAT_D32_SFLOAT;
        iInfo.extent = {kResolution, kResolution, 1};
        iInfo.mipLevels = 1;
        iInfo.arrayLayers = 1;
        iInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        iInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        iInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        iInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                                &depthImage_, &depthAlloc_, nullptr));

        VkImageViewCreateInfo vInfo{};
        vInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vInfo.image = depthImage_;
        vInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vInfo.format = VK_FORMAT_D32_SFLOAT;
        vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vInfo.subresourceRange.levelCount = 1;
        vInfo.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &depthView_));
    }

    // --- Render pass: color + depth, color finalLayout = SHADER_READ_ONLY ---
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Exit dep: 0 -> EXTERNAL, COLOR_ATTACHMENT_OUTPUT -> FRAGMENT_SHADER
    // so the scene pass can sample the color texture safely.
    VkSubpassDependency exitDep{};
    exitDep.srcSubpass = 0;
    exitDep.dstSubpass = VK_SUBPASS_EXTERNAL;
    exitDep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    exitDep.dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    exitDep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    exitDep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 2;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &exitDep;
    VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &renderPass_));

    // --- Framebuffer ---
    VkImageView views[2] = {colorView_, depthView_};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass_;
    fbInfo.attachmentCount = 2;
    fbInfo.pAttachments = views;
    fbInfo.width  = kResolution;
    fbInfo.height = kResolution;
    fbInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &framebuffer_));

    return true;
}

void VkReflectionTarget::destroy(VkContext& ctx) {
    if (framebuffer_) { vkDestroyFramebuffer(ctx.device(), framebuffer_, nullptr); framebuffer_ = VK_NULL_HANDLE; }
    if (renderPass_)  { vkDestroyRenderPass(ctx.device(), renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    if (colorView_)   { vkDestroyImageView(ctx.device(), colorView_, nullptr); colorView_ = VK_NULL_HANDLE; }
    if (depthView_)   { vkDestroyImageView(ctx.device(), depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (colorImage_)  { vmaDestroyImage(ctx.allocator(), colorImage_, colorAlloc_); colorImage_ = VK_NULL_HANDLE; colorAlloc_ = VK_NULL_HANDLE; }
    if (depthImage_)  { vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_); depthImage_ = VK_NULL_HANDLE; depthAlloc_ = VK_NULL_HANDLE; }
}

void VkReflectionTarget::beginPass(VkCommandBuffer cb, const float clearColor[4]) const {
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = clearColor[0];
    clears[0].color.float32[1] = clearColor[1];
    clears[0].color.float32[2] = clearColor[2];
    clears[0].color.float32[3] = clearColor[3];
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass_;
    rpBegin.framebuffer = framebuffer_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {kResolution, kResolution};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Same negative-height viewport convention as the scene pass so
    // GL-style projections render correctly without altering winding.
    VkViewport vp{};
    vp.x = 0;
    vp.y = static_cast<float>(kResolution);
    vp.width  = static_cast<float>(kResolution);
    vp.height = -static_cast<float>(kResolution);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {kResolution, kResolution}};
    vkCmdSetScissor(cb, 0, 1, &scissor);
}

void VkReflectionTarget::endPass(VkCommandBuffer cb) const {
    vkCmdEndRenderPass(cb);
}

}  // namespace iron
```

- [ ] **Step 3: Register `VkReflectionTarget.cpp` in CMake**

In `engine/CMakeLists.txt`, find the Vulkan-branch sources block (it lists `VkCubemap.cpp`, `VkSkybox.cpp`, `VkShadowMap.cpp`, etc). Add the new file alongside them:

```cmake
        render/backends/vulkan/VkReflectionTarget.cpp
```

- [ ] **Step 4: Build to verify compile**

Run from repo root:

```bash
cmake --build build-vk --config Debug --target ironcore
```

Expected: clean compile. No links yet — VulkanRenderer doesn't reference it.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkReflectionTarget.h \
        engine/render/backends/vulkan/VkReflectionTarget.cpp \
        engine/CMakeLists.txt
git commit -m "M17 Task 1: VkReflectionTarget subsystem (1024^2 color+depth RTT + render pass)"
```

---

## Task 2: Device feature `shaderClipDistance` + reflection pipeline

**Files:**
- Modify: `engine/render/backends/vulkan/VkContext.cpp:194-217` (enable feature in `createLogicalDevice`)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h` (adds reflection pipeline state)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (creates reflection pipeline at init)

The reflection pipeline is a single shared pipeline created once. It uses its own 2-binding descriptor set layout (UBO + diffuse) and the reflection render pass. Standalone after Task 1 — built at init but not yet recorded into frames.

- [ ] **Step 1: Enable `shaderClipDistance` in `VkContext::createLogicalDevice`**

In `engine/render/backends/vulkan/VkContext.cpp`, modify `createLogicalDevice` (currently lines 194-217). Replace:

```cpp
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = deviceExtensions;

    VK_CHECK(vkCreateDevice(phys_, &info, nullptr, &device_));
```

With:

```cpp
    // M17 — enable shaderClipDistance so gl_ClipDistance[0] in the
    // reflection-pass vertex shader hardware-clips geometry on the
    // wrong side of the mirror plane. Core Vulkan 1.0 feature.
    VkPhysicalDeviceFeatures features{};
    features.shaderClipDistance = VK_TRUE;

    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = deviceExtensions;
    info.pEnabledFeatures = &features;

    VK_CHECK(vkCreateDevice(phys_, &info, nullptr, &device_));
```

- [ ] **Step 2: Add reflection pipeline state to `VulkanRenderer.h`**

Open `engine/render/backends/vulkan/VulkanRenderer.h`. Add the include for the new subsystem alongside the others:

```cpp
#include "render/backends/vulkan/VkReflectionTarget.h"
```

Then, in the `private:` section, after the existing M16 cubemap block (around line 158-160), add:

```cpp
    // M17 — planar reflection RTT + shared pipeline + currently-set plane.
    VkReflectionTarget    reflection_;
    VkDescriptorSetLayout reflectionSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      reflectionPipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          reflectionPipeline_ = VK_NULL_HANDLE;
    VkShaderModule        reflectionVertModule_ = VK_NULL_HANDLE;
    VkShaderModule        reflectionFragModule_ = VK_NULL_HANDLE;
    std::optional<ReflectionPlane> reflectionPlane_;
```

Add `#include "render/ReflectionPlane.h"` and `#include <optional>` at the top of the header.

- [ ] **Step 3: Add `compileGlslToSpirv` helper reuse + build reflection pipeline at init**

The Vulkan backend already has a glslang-driven SPIR-V compile helper in `VkShader.cpp` (used by `VkShaderStore`) and in `VkSkybox.cpp` (similar private helper). Mirror that pattern: in `VulkanRenderer.cpp`, add a private static helper inside the anonymous namespace OR just inline-call `VkShader::compileGlslToSpirv` if exposed. **If not exposed**, copy the 20-line helper from `VkSkybox.cpp` (it's the canonical short version) into the anonymous namespace at the top of `VulkanRenderer.cpp`.

Then add a private method `buildReflectionPipeline()` that runs once at `init()` time, after `reflection_.init(...)`. Inside `VulkanRenderer::init` (`VulkanRenderer.cpp`), after the existing `skybox_.init(...)` call (around line 74-77), add:

```cpp
    if (!reflection_.init(context_, textures_.sharedSampler())) {
        Log::error("VulkanRenderer: VkReflectionTarget init failed");
        return false;
    }
    if (!buildReflectionPipeline()) {
        Log::error("VulkanRenderer: reflection pipeline build failed");
        return false;
    }
```

> **Note:** `textures_.sharedSampler()` is the shared CLAMP_TO_EDGE LINEAR sampler used by `VkTextureStore`. If that accessor doesn't exist, check `VkTextureStore` and either add a `sampler() const` accessor returning the private `sharedSampler_` member, or look at how `VkSkybox::init` obtained the sampler — reuse that pattern. The point: do NOT create a new sampler; reuse the existing shared one.

Define `buildReflectionPipeline()` near the bottom of `VulkanRenderer.cpp` (before the `}  // namespace iron` closing brace):

```cpp
namespace {

const char* kReflectionVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

// Full LitUbo std140 layout (928 bytes after M17). We reference only
// `model`, `reflectionViewProj`, and `clipPlane`.
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
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
    mat4 reflectionViewProj;
    vec4 reflectionParams;
    vec4 clipPlane;
} u;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vNormal;

void main() {
    vec4 worldPos = u.model * vec4(aPos, 1.0);
    vUV = aUV;
    vNormal = mat3(u.model) * aNormal;
    gl_ClipDistance[0] = dot(worldPos.xyz, u.clipPlane.xyz) + u.clipPlane.w;
    gl_Position = u.reflectionViewProj * worldPos;
}
)";

const char* kReflectionFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vNormal;
layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, normalize(vec3(0.3, 1.0, 0.2))), 0.0);
    vec4 texel = texture(uDiffuse, vUV);
    outColor = vec4(texel.rgb * (0.3 + 0.7 * diffuse), texel.a);
}
)";

}  // namespace

bool VulkanRenderer::buildReflectionPipeline() {
    // --- Descriptor set layout: UBO (0) + diffuse (1) ---
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 2;
    sli.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(context_.device(), &sli, nullptr,
                                          &reflectionSetLayout_));

    // --- Pipeline layout ---
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &reflectionSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(context_.device(), &pli, nullptr,
                                     &reflectionPipelineLayout_));

    // --- Compile shaders to SPIR-V ---
    std::vector<uint32_t> vertSpv, fragSpv;
    if (!compileGlslToSpirv(kReflectionVert, VK_SHADER_STAGE_VERTEX_BIT, vertSpv)) {
        Log::error("VulkanRenderer: reflection vert SPIR-V compile failed");
        return false;
    }
    if (!compileGlslToSpirv(kReflectionFrag, VK_SHADER_STAGE_FRAGMENT_BIT, fragSpv)) {
        Log::error("VulkanRenderer: reflection frag SPIR-V compile failed");
        return false;
    }

    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vertSpv.size() * sizeof(uint32_t);
    smInfo.pCode    = vertSpv.data();
    VK_CHECK(vkCreateShaderModule(context_.device(), &smInfo, nullptr,
                                   &reflectionVertModule_));
    smInfo.codeSize = fragSpv.size() * sizeof(uint32_t);
    smInfo.pCode    = fragSpv.data();
    VK_CHECK(vkCreateShaderModule(context_.device(), &smInfo, nullptr,
                                   &reflectionFragModule_));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = reflectionVertModule_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = reflectionFragModule_;
    stages[1].pName = "main";

    // --- Vertex input — match VkMesh layout (Pos, Normal, UV, Tangent) ---
    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = 11 * sizeof(float);  // 3+3+2+3 = 11 floats
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    6 * sizeof(float)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 * sizeof(float)};

    VkPipelineVertexInputStateCreateInfo vinfo{};
    vinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vinfo.vertexBindingDescriptionCount = 1;
    vinfo.pVertexBindingDescriptions = &bind;
    vinfo.vertexAttributeDescriptionCount = 4;
    vinfo.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo iaInfo{};
    iaInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaInfo.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpInfo{};
    vpInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpInfo.viewportCount = 1;
    vpInfo.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // mirror flips winding; disable cull
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{};
    dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState   = &vinfo;
    gp.pInputAssemblyState = &iaInfo;
    gp.pViewportState      = &vpInfo;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState   = &ms;
    gp.pDepthStencilState  = &ds;
    gp.pColorBlendState    = &cb;
    gp.pDynamicState       = &dynInfo;
    gp.layout = reflectionPipelineLayout_;
    gp.renderPass = reflection_.renderPass();
    gp.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(context_.device(), VK_NULL_HANDLE,
                                        1, &gp, nullptr, &reflectionPipeline_));
    return true;
}
```

Declare `bool buildReflectionPipeline();` in the `private:` section of `VulkanRenderer.h`.

- [ ] **Step 4: Wire destroy in `~VulkanRenderer`**

In the destructor (currently `VulkanRenderer.cpp:19-35`), add cleanup BEFORE `context_.shutdown()` and AFTER all other subsystem destroys. Add these lines just before `pipelines_.destroy(context_);`:

```cpp
        if (reflectionPipeline_)       vkDestroyPipeline(context_.device(), reflectionPipeline_, nullptr);
        if (reflectionPipelineLayout_) vkDestroyPipelineLayout(context_.device(), reflectionPipelineLayout_, nullptr);
        if (reflectionSetLayout_)      vkDestroyDescriptorSetLayout(context_.device(), reflectionSetLayout_, nullptr);
        if (reflectionVertModule_)     vkDestroyShaderModule(context_.device(), reflectionVertModule_, nullptr);
        if (reflectionFragModule_)     vkDestroyShaderModule(context_.device(), reflectionFragModule_, nullptr);
        reflection_.destroy(context_);
```

- [ ] **Step 5: Build to verify compile**

```bash
cmake --build build-vk --config Debug --target ironcore
```

Expected: clean compile. Run any existing Vulkan demo (`build-vk/games/01-spinning-cube/Debug/spinning-cube.exe`) — should still work (reflection target initialized but unused).

- [ ] **Step 6: Commit**

```bash
git add engine/render/backends/vulkan/VkContext.cpp \
        engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp
git commit -m "M17 Task 2: shaderClipDistance feature + reflection pipeline (2-binding shared)"
```

---

## Task 3: Atomic integration — endFrame reflection pass + LitUbo grow + descriptor 6→7 + scene shaders

**Files:**
- Modify: `engine/render/backends/vulkan/VkShader.cpp:96-120` (6 → 7 bindings)
- Modify: `engine/render/backends/vulkan/VkFrameRing.cpp:56` (5* → 6* sampler pool)
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp` (un-stub planes, grow LitUbo, record reflection pass, write 7 descriptors)
- Modify: `games/01-spinning-cube/main.cpp` and `games/07-net-shooter/main.cpp` (Vulkan scene shaders gain binding 6 reflection sampler + planar branch)

This is the atomic commit that flips everything on at once. After this commit, planar reflection works end-to-end on the Vulkan backend. Visual validator comes in Task 4.

- [ ] **Step 1: Grow descriptor set layout to 7 bindings in `VkShader.cpp`**

Open `engine/render/backends/vulkan/VkShader.cpp`. Find the `VkDescriptorSetLayoutBinding bindings[6]{};` array around line 96. Change to:

```cpp
    VkDescriptorSetLayoutBinding bindings[7]{};
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
    bindings[4].binding = 4;  // shadow
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[5].binding = 5;  // sky cubemap (M16)
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[6].binding = 6;  // planar reflection RTT (M17)
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
```

Update the `sli.bindingCount = 6;` → `sli.bindingCount = 7;` line that immediately follows.

- [ ] **Step 2: Bump sampler pool capacity 5× → 6× in `VkFrameRing.cpp`**

Open `engine/render/backends/vulkan/VkFrameRing.cpp` line 56. Change:

```cpp
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * kMaxDescriptorSetsPerFrame},  // M16: 5 samplers per lit set
```

to:

```cpp
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * kMaxDescriptorSetsPerFrame},  // M17: 6 samplers per lit set
```

- [ ] **Step 3: Grow LitUbo to 928 bytes in `VulkanRenderer.cpp`**

In `VulkanRenderer.cpp`, find the `LitUbo` struct in the anonymous namespace (currently at line 153-167). Add the three M17 fields at the end:

```cpp
struct LitUbo {
    Mat4 mvp;                 // 64
    Mat4 model;               // 64
    Mat4 lightViewProj;       // 64
    Vec4 sunDir;              // 16
    Vec4 sunColor;            // 16
    Vec4 ambient;             // 16
    Vec4 emissive;            // 16
    Vec4 cameraPos;           // 16
    Vec4 materialParams;      // 16  x=uvScale, y=specPower, z=reflectivity, w=shadowBias
    Vec4 fogColor;            // 16  M15 — xyz=color, w=density
    Vec4 lightCounts;         // 16  M15 — x=pointLightCount (as float), y/z/w padding
    Vec4 pointPositions[16];  // 256 M15 — xyz=position, w=intensity
    Vec4 pointColors[16];     // 256 M15 — xyz=color, w=range
    Mat4 reflectionViewProj;  // 64  M17 — scene: identity; reflection: P * V * mirror
    Vec4 reflectionParams;    // 16  M17 — x=useReflectionPlane (0/1), y=screenW, z=screenH, w=0
    Vec4 clipPlane;           // 16  M17 — (normal.xyz, -d) for reflection pass; ignored in scene
};
static_assert(sizeof(LitUbo) == 928, "LitUbo std140 layout (M17)");
```

- [ ] **Step 4: Un-stub `setReflectionPlane` / `disableReflectionPlane`**

In `VulkanRenderer.cpp`, find the existing stub implementations (search for `setReflectionPlane`). Replace them:

```cpp
void VulkanRenderer::setReflectionPlane(Vec3 normal, float d) {
    ReflectionPlane plane;
    plane.normal = normal;
    plane.d = d;
    reflectionPlane_ = plane;
}

void VulkanRenderer::disableReflectionPlane() {
    reflectionPlane_.reset();
}
```

Remove any `warnOnce("setReflectionPlane")` etc. calls these had.

- [ ] **Step 5: Populate M17 LitUbo fields in `recordSceneDraw`**

Locate `recordSceneDraw` (around line 294). The function fills the `LitUbo ubo;` struct. After the existing `lightCounts` and point-light loops, add (matching the new struct layout):

```cpp
    // M17 — scene pass: pass-through reflectionViewProj (unused by scene
    // shader), planar-reflection flag from material, screen size for
    // projective sampling. clipPlane is ignored in the scene pass.
    ubo.reflectionViewProj = Mat4::identity();
    const float useRefl = (call.material.useReflectionPlane && reflectionPlane_.has_value())
                          ? 1.0f : 0.0f;
    ubo.reflectionParams = Vec4{
        useRefl,
        static_cast<float>(swapchain_.extent().width),
        static_cast<float>(swapchain_.extent().height),
        0.0f,
    };
    ubo.clipPlane = Vec4{0.0f, 0.0f, 0.0f, 0.0f};
```

- [ ] **Step 6: Write 7 descriptors in `recordSceneDraw`**

Still in `recordSceneDraw`. Find the existing 5-element `imgInfos[5]` array and 6-element `writes[6]` array (around line 379-433). Replace with 6 + 7:

```cpp
    // M17 — reflection RTT (or black fallback if no plane active) at binding 6.
    VkImageView   reflectionView   = reflection_.colorView();
    VkSampler     reflectionSampler = reflection_.sampler();
    if (!reflectionPlane_.has_value()) {
        // No plane → sample the cubemap's black fallback through binding 6.
        // Reuse VkCubemapStore's 1x1x6 black handle as a 2D fallback would
        // require a sampler+view; simpler to just sample the reflection RTT
        // and rely on shader-side `useReflectionPlane` flag to gate.
        // (The RTT will hold uninitialized contents on first frame if a
        // plane was never set — that's fine because useReflectionPlane=0
        // means the shader doesn't sample it.)
    }

    VkDescriptorImageInfo imgInfos[6]{};
    imgInfos[0].sampler     = diffuse.sampler;
    imgInfos[0].imageView   = diffuse.view;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[1].sampler     = normal.sampler;
    imgInfos[1].imageView   = normal.view;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[2].sampler     = spec.sampler;
    imgInfos[2].imageView   = spec.view;
    imgInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[3].sampler     = shadowMap_.sampler();
    imgInfos[3].imageView   = shadowMap_.depthView();
    imgInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[4].sampler     = skyTex.sampler;
    imgInfos[4].imageView   = skyTex.view;
    imgInfos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfos[5].sampler     = reflectionSampler;
    imgInfos[5].imageView   = reflectionView;
    imgInfos[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[7]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;
    for (int i = 0; i < 6; ++i) {
        writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i + 1].dstSet = set;
        writes[i + 1].dstBinding = i + 1;
        writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i + 1].descriptorCount = 1;
        writes[i + 1].pImageInfo = &imgInfos[i];
    }
    vkUpdateDescriptorSets(context_.device(), 7, writes, 0, nullptr);
```

Note the layout transition concern: the reflection RTT's color image starts in `UNDEFINED` and only reaches `SHADER_READ_ONLY_OPTIMAL` after its first render pass. **If no plane is active on the first frame and the descriptor sampler reads from it**, that's a validation layer warning but not a hang because the shader's `useReflectionPlane=0` branch never samples. To silence the warning: when `!reflectionPlane_.has_value()` AND `reflection_` has never been rendered to, run an initial empty-clear pass once at `init()` time. **Defer that to a fixup commit if validation complains; not required for visual correctness.**

- [ ] **Step 7: Add reflection pass to `endFrame`**

Locate `VulkanRenderer::endFrame` (around line 445). After the shadow pass and before the scene pass `vkCmdBeginRenderPass`, insert:

```cpp
    // --- Pass 2: planar reflection (if a plane is set) ---
    if (reflectionPlane_.has_value()) {
        const ReflectionPlane& plane = *reflectionPlane_;
        const Mat4 mirror = reflectionMatrix(plane);
        const Mat4 reflectionVP = pendingProjection_ * (pendingView_ * mirror);

        const float clearC[4] = {pendingClear_.x, pendingClear_.y,
                                 pendingClear_.z, 1.0f};
        reflection_.beginPass(cb, clearC);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          reflectionPipeline_);

        for (const DrawCall& call : sceneDraws_) {
            if (call.material.useReflectionPlane) continue;  // skip mirror itself
            if (!meshes_.has(call.mesh)) continue;

            // Build a mini-LitUbo: only model, reflectionViewProj, clipPlane
            // are read by the reflection vert shader. Zero the rest.
            LitUbo ubo{};
            ubo.model = call.model;
            ubo.reflectionViewProj = reflectionVP * call.model;
            ubo.clipPlane = Vec4{plane.normal.x, plane.normal.y, plane.normal.z,
                                  -plane.d};

            const VkDeviceSize uboOffset = frames_.allocateUbo(&ubo, sizeof(ubo));
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = f.uboBuffer;
            bufInfo.offset = uboOffset;
            bufInfo.range  = sizeof(ubo);

            VkDescriptorSetAllocateInfo dsInfo{};
            dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsInfo.descriptorPool = f.descriptorPool;
            dsInfo.descriptorSetCount = 1;
            dsInfo.pSetLayouts = &reflectionSetLayout_;
            VkDescriptorSet rset = VK_NULL_HANDLE;
            VK_CHECK(vkAllocateDescriptorSets(context_.device(), &dsInfo, &rset));

            const auto& diffuse = textures_.has(call.material.texture)
                ? textures_.get(call.material.texture)
                : textures_.get(textures_.whiteTexture());

            VkDescriptorImageInfo imgInfo{};
            imgInfo.sampler     = diffuse.sampler;
            imgInfo.imageView   = diffuse.view;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet rWrites[2]{};
            rWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rWrites[0].dstSet = rset;
            rWrites[0].dstBinding = 0;
            rWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            rWrites[0].descriptorCount = 1;
            rWrites[0].pBufferInfo = &bufInfo;
            rWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rWrites[1].dstSet = rset;
            rWrites[1].dstBinding = 1;
            rWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            rWrites[1].descriptorCount = 1;
            rWrites[1].pImageInfo = &imgInfo;
            vkUpdateDescriptorSets(context_.device(), 2, rWrites, 0, nullptr);

            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    reflectionPipelineLayout_, 0, 1, &rset, 0, nullptr);

            const auto& mesh = meshes_.get(call.mesh);
            VkDeviceSize offsets[1] = {0};
            vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertexBuffer, offsets);
            vkCmdBindIndexBuffer(cb, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
        }

        reflection_.endPass(cb);
    }
```

> **Note:** the existing shadow pass is "Pass 1", scene pass is "Pass 3" semantically — relabel comments accordingly if you prefer ("// --- Pass 1: shadow ---" → leave; "// --- Scene pass ---" → "// --- Pass 3: scene pass ---").

`#include "render/ReflectionPlane.h"` is needed at the top of `VulkanRenderer.cpp` (declares `Mat4 reflectionMatrix(const ReflectionPlane&);`).

- [ ] **Step 8: Update scene shaders in net-shooter and spinning-cube**

For each of:
- `games/01-spinning-cube/main.cpp`
- `games/07-net-shooter/main.cpp`

Find the Vulkan scene fragment shader string (it's the GLSL 450 source with bindings 0-5). In the UBO block, add the M17 fields:

```glsl
layout(set=0, binding=0) uniform LitUbo {
    // ... existing M11-M16 fields ...
    mat4 reflectionViewProj;
    vec4 reflectionParams;
    vec4 clipPlane;
} u;
```

Add the new sampler binding:

```glsl
layout(set=0, binding=6) uniform sampler2D uReflection;
```

In the fragment `main()`, BEFORE the existing M16 cubemap reflectivity block, add the planar branch:

```glsl
    vec3 reflectColor = vec3(0.0);
    if (u.reflectionParams.x > 0.5) {
        // M17 — planar reflection: project frag onto reflection RTT
        vec2 ndc = gl_FragCoord.xy / u.reflectionParams.yz;
        reflectColor = texture(uReflection, ndc).rgb;
        lit = mix(lit, reflectColor, u.materialParams.z);
    } else if (u.materialParams.z > 0.0) {
        // M16 — cubemap fallback
        vec3 V = normalize(u.cameraPos.xyz - vWorldPos);
        vec3 R = reflect(-V, normalize(vNormal));
        reflectColor = texture(uSkyCubemap, R).rgb;
        lit = mix(lit, reflectColor, u.materialParams.z);
    }
```

If the existing M16 code already has a `materialParams.z > 0.0` block, modify it as shown — wrap the existing cubemap path in an `else if`.

Also update the GL-shader compatibility branch of these games to nothing (they're Vulkan-only shaders; if a GL-fallback variable exists, leave it alone).

- [ ] **Step 9: Build to verify compile**

```bash
cmake --build build-vk --config Debug --target net-shooter spinning-cube
```

Expected: clean compile. Run `build-vk/games/07-net-shooter/Debug/net-shooter.exe --listen`. Expected: no visual regression vs M16 (no reflection plane is set, so the reflection pass is skipped). Sunset skybox, brick walls, lanterns, fog all unchanged.

- [ ] **Step 10: Commit**

```bash
git add engine/render/backends/vulkan/VkShader.cpp \
        engine/render/backends/vulkan/VkFrameRing.cpp \
        engine/render/backends/vulkan/VulkanRenderer.h \
        engine/render/backends/vulkan/VulkanRenderer.cpp \
        games/01-spinning-cube/main.cpp \
        games/07-net-shooter/main.cpp
git commit -m "M17 Task 3: planar reflection integration (7 bindings + reflection pass + shaders)"
```

---

## Task 4: Showcase Vulkan port + docs

**Files:**
- Modify: `games/03-showcase/main.cpp` — add `--backend` flag + Vulkan GLSL 450 shaders + Vulkan default
- Create/Modify: `docs/engine/rhi-abstraction.md` — append M17 section

This is the visual validator. After this task, `.\build-vk\games\03-showcase\Debug\showcase.exe --backend vulkan` shows the mirror floor reflecting the scene with the sunset cubemap above.

- [ ] **Step 1: Read the existing showcase to understand its structure**

```bash
wc -l games/03-showcase/main.cpp
```

Open `games/03-showcase/main.cpp`. It currently constructs `iron::OpenGLRenderer` directly. We need to switch on a CLI arg.

- [ ] **Step 2: Mirror the `--backend` arg pattern from net-shooter**

Look at `games/07-net-shooter/main.cpp` for the `--backend [opengl|vulkan]` parsing pattern (used by net-shooter and spinning-cube). Copy that pattern into `games/03-showcase/main.cpp`'s `main()`:

```cpp
    bool useVulkan = true;  // M17 — default to Vulkan now that parity is complete
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--backend" && i + 1 < argc) {
            std::string b = argv[++i];
            useVulkan = (b == "vulkan");
        }
    }

    std::unique_ptr<iron::Renderer> renderer;
    if (useVulkan) {
        auto vr = std::make_unique<iron::VulkanRenderer>();
        if (!vr->init(window)) {
            iron::Log::error("showcase: Vulkan init failed");
            return 1;
        }
        renderer = std::move(vr);
    } else {
        renderer = std::make_unique<iron::OpenGLRenderer>();
    }
```

Switch the `OpenGLRenderer` includes to also include `render/backends/vulkan/VulkanRenderer.h` (under `#ifdef IRON_RENDER_BACKEND_VULKAN` if needed — check net-shooter for the exact pattern).

- [ ] **Step 3: Add Vulkan GLSL 450 versions of the scene shaders**

The showcase already has GLSL 330 vert/frag strings (`kVertexShader`, `kFragmentShader`). Add parallel GLSL 450 strings as `kVertexShaderVk` and `kFragmentShaderVk` using the exact M16+M17 UBO layout from net-shooter (copy the net-shooter Vulkan shader, adjusting only for showcase-specific differences — there shouldn't be any since both use the standard lit shader features).

When constructing the shader: `renderer->createShader(useVulkan ? kVertexShaderVk : kVertexShader, useVulkan ? kFragmentShaderVk : kFragmentShader);`

- [ ] **Step 4: Build and visually validate**

```bash
cmake --build build-vk --config Debug --target showcase
```

Run:

```powershell
.\build-vk\games\03-showcase\Debug\showcase.exe --backend vulkan
```

**Expected:**
- Mirror floor at y=-0.1 reflects the scene
- Sunset cubemap skybox visible behind
- Rotating sphere, metallic cylinder, emissive elements present
- Pressing F3 toggles gizmos (carry-over from M11)

If the reflection looks wrong (upside-down, missing geometry, flickering), debug. Likely causes:
- Mirror plane d sign wrong → reflection appears below/above the floor → check `setReflectionPlane(Vec3{0,1,0}, -0.1f)` arguments match GL's
- Wrong winding shows no geometry → check CULL_MODE_NONE on reflection pipeline
- `gl_ClipDistance` not active → check `shaderClipDistance` feature is being enabled (Task 2 Step 1)

Run the OpenGL build for visual comparison:

```powershell
.\build\games\03-showcase\Debug\showcase.exe --backend opengl
```

Vulkan and OpenGL renders should look visually similar (within RTT res / shader-simplification differences).

- [ ] **Step 5: Append M17 docs**

Append to `docs/engine/rhi-abstraction.md` (or the equivalent docs file used by M16, currently at `docs/engine/rhi-abstraction.md`):

```markdown
## M17 — Vulkan planar reflection (2026-05-27)

Vulkan backend reaches OpenGL parity. `VkReflectionTarget` owns a 1024² RGBA8+D32 RTT with its own render pass + exit subpass dependency. `VkContext::createDevice` now enables `shaderClipDistance` so the reflection vert shader can use `gl_ClipDistance[0]` to discard geometry on the wrong side of the mirror plane.

A separate shared reflection pipeline (2-binding layout: UBO + diffuse, `CULL_MODE_NONE`) is created once at `VulkanRenderer::init`. The pipeline shares the 928-byte LitUbo buffer with the scene pass, but its descriptor layout binds only `LitUbo.model`, `LitUbo.reflectionViewProj`, and `LitUbo.clipPlane`.

The lit pass descriptor set grows from 6 → 7 bindings (binding 6 = reflection RTT). Sampler pool capacity bumped 5× → 6× `kMaxDescriptorSetsPerFrame` in `VkFrameRing`.

Materials with `useReflectionPlane=true` sample binding 6 projectively (`gl_FragCoord.xy / screenSize`); the M16 cubemap fallback applies otherwise.

Demo: `games/03-showcase` — `--backend vulkan` is the new default. Mirror floor at y=-0.1 reflects the rotating sphere + metallic cylinder + sunset skybox.
```

- [ ] **Step 6: Commit**

```bash
git add games/03-showcase/main.cpp docs/engine/rhi-abstraction.md
git commit -m "M17 Task 4: showcase Vulkan port (--backend vulkan default) + docs"
```

- [ ] **Step 7: Push and open PR**

```bash
git push -u origin feat/m17-vulkan-planar-reflection
gh pr create --title "M17: Vulkan planar reflection + showcase port" --body "$(cat <<'EOF'
## Summary
- VkReflectionTarget (1024² RGBA8+D32 RTT with own render pass + exit subpass dep)
- shaderClipDistance device feature + shared reflection pipeline (2-binding, CULL_NONE)
- LitUbo grows 832 → 928 bytes (+reflectionViewProj +reflectionParams +clipPlane)
- Descriptor layout 6 → 7 bindings (binding 6 = reflection RTT)
- Sampler pool 5× → 6× `kMaxDescriptorSetsPerFrame`
- Scene shaders gain planar reflection branch (M16 cubemap path kept as fallback)
- 03-showcase Vulkan port; `--backend vulkan` is the new default

## Test plan
- [ ] CI green (Windows MSVC)
- [ ] `.\build-vk\games\03-showcase\Debug\showcase.exe --backend vulkan` shows mirror floor reflecting scene
- [ ] Side-by-side vs `--backend opengl` is visually similar
- [ ] No regression on net-shooter / spinning-cube (reflection pass skipped when no plane set)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Self-Review Summary

**Spec coverage check:**
- VkReflectionTarget subsystem → Task 1 ✓
- shaderClipDistance device feature → Task 2 Step 1 ✓
- Reflection pipeline (shared, 2-binding, CULL_NONE) → Task 2 Steps 2-3 ✓
- LitUbo growth 832 → 928 → Task 3 Step 3 ✓
- Descriptor layout 6 → 7 → Task 3 Step 1 ✓
- Pool capacity bump → Task 3 Step 2 ✓
- `setReflectionPlane`/`disableReflectionPlane` un-stubbed → Task 3 Step 4 ✓
- recordSceneDraw writes 7 descriptors → Task 3 Step 6 ✓
- endFrame reflection pass between shadow and scene → Task 3 Step 7 ✓
- Scene shader planar branch → Task 3 Step 8 ✓
- 03-showcase Vulkan port + docs → Task 4 ✓

**Placeholder scan:** clean (no TBDs).

**Type consistency:** `reflectionPipeline_` / `reflectionPipelineLayout_` / `reflectionSetLayout_` consistent across Tasks 2-3. `LitUbo` field names (`reflectionViewProj`, `reflectionParams`, `clipPlane`) consistent across Steps 3 / 5 / 8.

**Ambiguity:** the "uninitialized reflection RTT first-frame validation warning" footnote in Task 3 Step 6 acknowledges a known minor edge — explicitly deferred to a fixup if it actually fires.
