# M14 Vulkan Shadow Map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring directional sun shadows to the Vulkan backend via a depth-only pre-pass + 3×3 PCF sampling in the lit shader, restructuring the per-frame flow to defer-and-replay so multi-pass rendering becomes viable.

**Architecture:** `VulkanRenderer::submit` now queues draws into `sceneDraws_` instead of recording inline. External Vulkan subsystems (`VkParticleSystem::render`) register deferred callbacks via a new `enqueueDeferredScenePass` API. `VulkanRenderer::endFrame` orchestrates a shadow pass (via new `VkShadowMap` subsystem) → image-layout barrier → scene pass that replays the queue (now writing a 5th descriptor: shadow sampler at binding 4) → deferred callbacks → debug-lines drain → HUD drain → submit + present. `LitUbo` grows from 224 to 288 bytes (adds `lightViewProj` matrix; repurposes `materialParams.w` for shadow bias).

**Tech Stack:** C++23, Vulkan 1.3, glslang (GLSL 450 → SPIR-V), VMA, MSVC, CMake.

---

## File Structure

### New files
- `engine/render/backends/vulkan/VkShadowMap.h`
- `engine/render/backends/vulkan/VkShadowMap.cpp`

### Modified files
- `engine/render/backends/vulkan/VulkanRenderer.h` — adds frame-flow state (sceneDraws_, deferredScenePass_, pendingDebug*, pendingHud*, pendingShadow*, pendingLightViewProj_, pendingShadowBias_, shadowMap_); declares recordSceneDraw + enqueueDeferredScenePass
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — restructures submit/flushDebugLines/drawHud/beginFrame/endFrame; adds computeLightViewProj helper; un-stubs setShadowBounds; LitUbo grows to 288 bytes
- `engine/render/backends/vulkan/VkShader.cpp` — descriptor set layout 4 → 5 bindings
- `engine/render/backends/vulkan/VkFrameRing.cpp` — bump sampler pool to `4 * kMaxDescriptorSetsPerFrame`
- `engine/render/backends/vulkan/VkParticleSystem.cpp` — `render(view, proj)` becomes a thin wrapper that registers a deferred callback; existing body moves into `recordRender`
- `engine/CMakeLists.txt` — registers `VkShadowMap.cpp` in the Vulkan source list
- `games/01-spinning-cube/main.cpp` — Vulkan shaders rewritten with shadow sampling; adds `setShadowBounds`
- `games/07-net-shooter/main.cpp` — Vulkan shaders rewritten with shadow sampling; warning string updated
- `docs/engine/rhi-abstraction.md` — appended M14 section

---

## Task 1: Frame-flow restructure (defer + replay, no shadow yet)

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/render/backends/vulkan/VkParticleSystem.cpp`

This task ONLY restructures the recording flow. No shadow code. After this task, both Vulkan games (spinning-cube + net-shooter) must render visually identical to M13. The descriptor set layout, LitUbo, and pipeline state are unchanged.

- [ ] **Step 1: Add frame-flow state to `VulkanRenderer.h`**

Find the existing M13 pending-state block (`pendingClear_`, `pendingView_`, `pendingProjection_`, `pendingSunDir_`, `pendingSunColor_`, `pendingAmbient_`, `pendingCameraPos_`). Add the following block immediately after the last pending-* field. Also add the `<functional>` include at the top of the file.

In the includes section:

```cpp
#include <functional>
```

In the `private:` section:

```cpp
    // M14 — frame-flow state for defer-and-replay rendering.
    // submit() queues into sceneDraws_; external Vulkan subsystems
    // register render callbacks via enqueueDeferredScenePass; endFrame
    // orchestrates the entire pass sequence and drains all queues.
    std::vector<DrawCall> sceneDraws_;
    std::vector<std::function<void(VkCommandBuffer)>> deferredScenePass_;

    // Deferred debug-lines state (M14 — was recorded inline before).
    Mat4 pendingDebugView_       = Mat4::identity();
    Mat4 pendingDebugProj_       = Mat4::identity();
    bool pendingDebugFlush_      = false;

    // Deferred HUD state (M14 — was recorded inline before).
    HudBatch pendingHudBatch_{};
    int      pendingHudW_      = 0;
    int      pendingHudH_      = 0;
    bool     pendingHudValid_  = false;
```

Note: `<vector>` is already included (used by `pendingDraws_` historically), and `Mat4` / `HudBatch` are already pulled in via the `Renderer.h` include chain. Verify these by reading the existing header before editing.

In the `public:` section (alongside the other engine-internal accessors like `currentCommandBuffer()` / `scenePass()`):

```cpp
    // Engine-internal: external Vulkan subsystems (e.g., VkParticleSystem)
    // register a deferred render callback here. The callback fires inside
    // the scene render pass during endFrame, after the geometry replay
    // but before debug-lines and HUD.
    void enqueueDeferredScenePass(std::function<void(VkCommandBuffer)> fn);
```

In the `private:` section (with the other private helpers):

```cpp
    // Records a single submitted DrawCall into the active scene render
    // pass. Called from endFrame's replay loop. Body is the M13 submit
    // logic moved here.
    void recordSceneDraw(VkCommandBuffer cb, const DrawCall& call);
```

Also REMOVE the now-unused `void warnOnce(const char* feature)` if it's only called from the stubs that we'll be replacing — actually leave it alone since other stubs (createCubemap, setSkybox, setReflectionPlane, disableReflectionPlane) still use it.

- [ ] **Step 2: Move the existing `submit` body into a new `recordSceneDraw` helper**

In `VulkanRenderer.cpp`, find the existing `void VulkanRenderer::submit(const DrawCall& call)` (the M13 body that handles the descriptor set, image fan-out, etc.). RENAME it from `VulkanRenderer::submit` to `VulkanRenderer::recordSceneDraw`, and add a new minimal `submit` above:

The new public `submit`:

```cpp
void VulkanRenderer::submit(const DrawCall& call) {
    if (skipFrame_) return;
    sceneDraws_.push_back(call);
}
```

The renamed `recordSceneDraw` keeps the entire M13 body verbatim, but starting with `void VulkanRenderer::recordSceneDraw(VkCommandBuffer cb, const DrawCall& call) {` and using the passed `cb` and `call` parameters. Any internal `VkCommandBuffer cb = currentCommandBuffer();` line at the top of the body should be REMOVED (cb is now a parameter). Any internal `if (cb == VK_NULL_HANDLE) return;` early-out should also be removed (the caller — endFrame — handles skipFrame).

- [ ] **Step 3: Add `enqueueDeferredScenePass`**

In `VulkanRenderer.cpp`, add the new public method body (place it near the other engine-internal accessors):

```cpp
void VulkanRenderer::enqueueDeferredScenePass(
        std::function<void(VkCommandBuffer)> fn) {
    if (skipFrame_) return;
    deferredScenePass_.push_back(std::move(fn));
}
```

- [ ] **Step 4: Defer `flushDebugLines` and `drawHud`**

In `VulkanRenderer.cpp`, find the current `VulkanRenderer::flushDebugLines` (which calls `debugLines_.record(cb, ...)`). Replace its body with deferred capture:

```cpp
void VulkanRenderer::flushDebugLines(const Mat4& view, const Mat4& projection) {
    if (skipFrame_) return;
    pendingDebugView_  = view;
    pendingDebugProj_  = projection;
    pendingDebugFlush_ = true;
}
```

Find the current `VulkanRenderer::drawHud`. Replace its body with:

```cpp
void VulkanRenderer::drawHud(const HudBatch& batch, int fbW, int fbH) {
    if (skipFrame_) return;
    pendingHudBatch_ = batch;
    pendingHudW_     = fbW;
    pendingHudH_     = fbH;
    pendingHudValid_ = true;
}
```

- [ ] **Step 5: Restructure `beginFrame` to NOT open the scene render pass**

Find `VulkanRenderer::beginFrame`. After the existing acquire-image + cmd-buffer-begin logic, REMOVE the existing `vkCmdBeginRenderPass` + `setSceneViewport` block. The render pass will now be opened by `endFrame`. Add at the end of `beginFrame`:

```cpp
    sceneDraws_.clear();
    deferredScenePass_.clear();
    pendingDebugFlush_ = false;
    pendingHudValid_   = false;
```

(Existing M13 logic — storing view/projection/sun/ambient/cameraPos — stays unchanged.)

- [ ] **Step 6: Rewrite `endFrame` to open scene render pass + replay queues**

Find `VulkanRenderer::endFrame`. The existing M10-era body does `vkCmdEndRenderPass + vkEndCommandBuffer + queue submit + present`. Replace the body with:

```cpp
void VulkanRenderer::endFrame() {
    if (skipFrame_) {
        frames_.advance();
        return;
    }

    VkFrameRing::Frame& f = frames_.current();
    VkCommandBuffer cb = f.commandBuffer;

    // --- Pass 2 (scene). Pass 1 lands in M14 Task 3. ---
    {
        VkClearValue clears[2]{};
        clears[0].color.float32[0] = pendingClear_.x;
        clears[0].color.float32[1] = pendingClear_.y;
        clears[0].color.float32[2] = pendingClear_.z;
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = pipelines_.renderPass();
        rpBegin.framebuffer = pipelines_.framebufferFor(currentImageIndex_);
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = swapchain_.extent();
        rpBegin.clearValueCount = 2;
        rpBegin.pClearValues = clears;
        vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        setSceneViewport(cb, swapchain_.extent());

        for (const DrawCall& call : sceneDraws_) {
            recordSceneDraw(cb, call);
        }

        for (auto& fn : deferredScenePass_) {
            fn(cb);
        }
        deferredScenePass_.clear();

        if (pendingDebugFlush_) {
            debugLines_.record(cb, context_.device(), frames_,
                              pendingDebugView_, pendingDebugProj_);
            pendingDebugFlush_ = false;
        }
        if (pendingHudValid_) {
            hud_.record(cb, context_.device(), frames_, textures_,
                       pendingHudBatch_, pendingHudW_, pendingHudH_);
            pendingHudValid_ = false;
        }

        vkCmdEndRenderPass(cb);
    }

    VK_CHECK(vkEndCommandBuffer(cb));

    const VkPipelineStageFlags waitStages[] =
        {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.imageAvailable;
    submit.pWaitDstStageMask = waitStages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &f.renderFinished;
    VK_CHECK(vkQueueSubmit(context_.graphicsQueue(), 1, &submit, f.inFlight));

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &f.renderFinished;
    present.swapchainCount = 1;
    VkSwapchainKHR swap = swapchain_.handle();
    present.pSwapchains = &swap;
    present.pImageIndices = &currentImageIndex_;
    const VkResult presentResult = vkQueuePresentKHR(context_.graphicsQueue(), &present);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        pendingResize_ = true;
    } else {
        VK_CHECK(presentResult);
    }

    frames_.advance();
}
```

Adapt the submit/present part to match what the existing code already does — copy the existing submit/present block from the current `endFrame` after the `vkCmdEndRenderPass` if it differs from the above. The key change is just: scene render pass now begins INSIDE `endFrame`, drains `sceneDraws_` via `recordSceneDraw`, then drains the deferred callbacks, then the debug-lines + HUD.

- [ ] **Step 7: Convert `VkParticleSystem::render` to register a deferred callback**

In `engine/render/backends/vulkan/VkParticleSystem.cpp`, find `VkParticleSystem::render(const Mat4& view, const Mat4& projection)`. RENAME it to `recordRender` (and update the header `.h` accordingly). Then add a new public `render` body:

In the header `VkParticleSystem.h` find:
```cpp
void render(const Mat4& view, const Mat4& projection);
```
Add immediately after:
```cpp
// Engine-internal: called from VulkanRenderer's deferred-callback drain.
void recordRender(VkCommandBuffer cb, const Mat4& view, const Mat4& projection);
```

In the cpp, the existing body becomes `recordRender(VkCommandBuffer cb, const Mat4& view, const Mat4& projection)`. Remove the internal `VkCommandBuffer cb = renderer_->currentCommandBuffer();` and `if (cb == VK_NULL_HANDLE) return;` lines from the top — `cb` is now a parameter.

Add the new public `render`:

```cpp
void VkParticleSystem::render(const Mat4& view, const Mat4& projection) {
    renderer_->enqueueDeferredScenePass(
        [this, view, projection](VkCommandBuffer cb) {
            recordRender(cb, view, projection);
        });
}
```

- [ ] **Step 8: Build under both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both.

- [ ] **Step 9: Smoke-test all three Vulkan games**

Launch each briefly (5 seconds is enough) and confirm visual output is IDENTICAL to M13:

```
.\build-vk\games\01-spinning-cube\Debug\01-spinning-cube.exe
.\build-vk\games\07-net-shooter\Debug\07-net-shooter.exe --listen
.\build-vk\games\08-particle-storm\Debug\08-particle-storm.exe
```

- Spinning cube: textured cube rotates with Lambertian shading + ambient.
- Net-shooter: brick walls + grass floor + HUD + gizmos (red AABBs).
- Particle storm: 1M particles + free-fly camera.

If any of them crashes, fails to render, or renders differently than M13, the frame-flow restructure has a bug. Most likely causes:
- Forgot to call `setSceneViewport` in endFrame (cube appears stretched/clipped).
- Missing `pendingResize_` handling in endFrame.
- `submit` queues but `endFrame` doesn't iterate (black screen).

- [ ] **Step 10: Commit**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/backends/vulkan/VkParticleSystem.cpp engine/render/backends/vulkan/VkParticleSystem.h
git commit -m "M14 Task 1: VulkanRenderer frame-flow restructure (defer + replay)"
```

---

## Task 2: `VkShadowMap` subsystem (init/destroy only — not yet integrated)

**Files:**
- Create: `engine/render/backends/vulkan/VkShadowMap.h`
- Create: `engine/render/backends/vulkan/VkShadowMap.cpp`
- Modify: `engine/CMakeLists.txt`

This task lands the new subsystem. It compiles and can be constructed but is not yet used by VulkanRenderer (Task 3). Tests stay at 35/35; visuals unchanged.

- [ ] **Step 1: Write `VkShadowMap.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkShadowMap is Vulkan-only."
#endif

#include "math/Mat4.h"
#include "render/Renderer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <vector>

namespace iron {

class VkContext;
class VkFrameRing;
class VkMeshStore;

// Vulkan directional-light shadow map. Owns a depth image + image
// view + depth-only render pass + framebuffer + depth-only graphics
// pipeline + sampler. `record()` runs the entire shadow pass for the
// frame.
class VkShadowMap {
public:
    static constexpr int kResolution = 2048;

    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // Records the entire shadow pass into the active command buffer:
    // begin depth-only render pass, replay `draws` with the depth-only
    // pipeline, end render pass. The render pass's final layout is
    // SHADER_READ_ONLY_OPTIMAL so subsequent passes can sample.
    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                VkMeshStore& meshes,
                const Mat4& lightViewProj,
                const std::vector<DrawCall>& draws);

    VkImageView depthView() const { return depthView_; }
    VkSampler   sampler()   const { return sampler_; }

private:
    struct LightUbo {
        float lightModelViewProj[16];  // pre-multiplied per draw
    };

    bool ok_ = false;
    VkImage         depthImage_     = VK_NULL_HANDLE;
    VmaAllocation   depthAlloc_     = VK_NULL_HANDLE;
    VkImageView     depthView_      = VK_NULL_HANDLE;
    VkFramebuffer   framebuffer_    = VK_NULL_HANDLE;
    VkRenderPass    renderPass_     = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline    pipeline_       = VK_NULL_HANDLE;
    VkSampler       sampler_        = VK_NULL_HANDLE;
};

}  // namespace iron
```

- [ ] **Step 2: Write `VkShadowMap.cpp` — init**

```cpp
// VkShadowMap.cpp — depth-only shadow map for the directional sun.

#include "render/backends/vulkan/VkShadowMap.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>

namespace iron {

namespace {

const char* kVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LightUbo {
    mat4 lightModelViewProj;
} u;

void main() {
    gl_Position = u.lightModelViewProj * vec4(aPos, 1.0);
}
)";

}  // namespace

bool VkShadowMap::init(VkContext& ctx) {
    // --- Depth image ---
    VkImageCreateInfo iInfo{};
    iInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iInfo.imageType = VK_IMAGE_TYPE_2D;
    iInfo.format = VK_FORMAT_D32_SFLOAT;
    iInfo.extent = {kResolution, kResolution, 1};
    iInfo.mipLevels = 1;
    iInfo.arrayLayers = 1;
    iInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    iInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    iInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
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

    // --- Render pass: depth-only, final layout = SHADER_READ_ONLY_OPTIMAL ---
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &depthAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &renderPass_));

    // --- Framebuffer ---
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass_;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &depthView_;
    fbInfo.width = kResolution;
    fbInfo.height = kResolution;
    fbInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &framebuffer_));

    // --- Descriptor set layout (binding 0 = LightUbo, vertex stage only) ---
    VkDescriptorSetLayoutBinding b0{};
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings = &b0;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &setLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &pipelineLayout_));

    // --- Depth-only pipeline ---
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kVert);
    if (vspv.empty()) {
        Log::error("VkShadowMap: vertex shader compile failed");
        return false;
    }
    VkShaderModule vsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vsm;
    stage.pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = offsetof(Vertex, position);
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = offsetof(Vertex, normal);
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32G32_SFLOAT;    attrs[2].offset = offsetof(Vertex, uv);
    attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[3].offset = offsetof(Vertex, tangent);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;       // back-face cull reduces peter-panning
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

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 0;  // depth-only — no color attachments

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount = 1;
    pInfo.pStages = &stage;
    pInfo.pVertexInputState = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState = &ms;
    pInfo.pDepthStencilState = &ds;
    pInfo.pColorBlendState = &cb;
    pInfo.pDynamicState = &dyn;
    pInfo.layout = pipelineLayout_;
    pInfo.renderPass = renderPass_;
    pInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx.device(), vsm, nullptr);

    // --- Sampler: NEAREST, CLAMP_TO_BORDER, white border (matches GL) ---
    VkSamplerCreateInfo sInfo{};
    sInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sInfo.magFilter = VK_FILTER_NEAREST;
    sInfo.minFilter = VK_FILTER_NEAREST;
    sInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sInfo.minLod = 0.0f;
    sInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(ctx.device(), &sInfo, nullptr, &sampler_));

    ok_ = true;
    return true;
}

void VkShadowMap::destroy(VkContext& ctx) {
    if (sampler_)        { vkDestroySampler(ctx.device(), sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (pipeline_)       { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
    if (framebuffer_)    { vkDestroyFramebuffer(ctx.device(), framebuffer_, nullptr); framebuffer_ = VK_NULL_HANDLE; }
    if (renderPass_)     { vkDestroyRenderPass(ctx.device(), renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    if (depthView_)      { vkDestroyImageView(ctx.device(), depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_)     { vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_); depthImage_ = VK_NULL_HANDLE; }
    ok_ = false;
}

void VkShadowMap::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                         VkMeshStore& meshes,
                         const Mat4& lightViewProj,
                         const std::vector<DrawCall>& draws) {
    if (!ok_ || cb == VK_NULL_HANDLE) return;

    // Begin depth-only render pass.
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass_;
    rpBegin.framebuffer = framebuffer_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {kResolution, kResolution};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(kResolution);
    vp.height = static_cast<float>(kResolution);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {kResolution, kResolution}};
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    for (const DrawCall& call : draws) {
        if (!meshes.has(call.mesh)) continue;

        // lightModelViewProj = lightViewProj * model.
        const Mat4 lmvp = lightViewProj * call.model;
        LightUbo ubo;
        std::memcpy(ubo.lightModelViewProj, lmvp.m, sizeof(ubo.lightModelViewProj));
        const VkDeviceSize uboOffset = frames.allocateUbo(&ubo, sizeof(ubo));

        VkDescriptorSetAllocateInfo dsInfo{};
        dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsInfo.descriptorPool = frames.current().descriptorPool;
        dsInfo.descriptorSetCount = 1;
        dsInfo.pSetLayouts = &setLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(device, &dsInfo, &set));

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = frames.current().uboBuffer;
        bufInfo.offset = uboOffset;
        bufInfo.range  = sizeof(ubo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 0, 1, &set, 0, nullptr);

        const auto& mesh = meshes.get(call.mesh);
        VkDeviceSize offsets[1] = {0};
        vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertexBuffer, offsets);
        vkCmdBindIndexBuffer(cb, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cb);
}

}  // namespace iron
```

- [ ] **Step 3: Register the new file in `engine/CMakeLists.txt`**

Inside the Vulkan `target_sources(...)` block, append:

```
      render/backends/vulkan/VkShadowMap.cpp
```

- [ ] **Step 4: Build both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both. The new file compiles but isn't used yet — Task 3 wires it into VulkanRenderer.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkShadowMap.h engine/render/backends/vulkan/VkShadowMap.cpp engine/CMakeLists.txt
git commit -m "M14 Task 2: VkShadowMap subsystem (init/destroy + record API)"
```

---

## Task 3: Shadow integration (atomic — LitUbo grows, descriptors grow, shaders rewritten)

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/render/backends/vulkan/VkShader.cpp`
- Modify: `engine/render/backends/vulkan/VkFrameRing.cpp`
- Modify: `games/01-spinning-cube/main.cpp`
- Modify: `games/07-net-shooter/main.cpp`

Atomic — all 6 file changes go in one commit so the tree stays valid.

- [ ] **Step 1: Add shadow state to `VulkanRenderer.h`**

Add the include:

```cpp
#include "render/backends/vulkan/VkShadowMap.h"
```

In the `private:` section, add (alongside the M13 pending state):

```cpp
    // M14 — directional-light shadow.
    Vec3  pendingShadowCenter_  = {0.0f, 0.0f, 0.0f};
    float pendingShadowRadius_  = 20.0f;
    float pendingShadowBias_    = 0.002f;
    Mat4  pendingLightViewProj_ = Mat4::identity();
    VkShadowMap shadowMap_;
```

- [ ] **Step 2: Add the `computeLightViewProj` helper in `VulkanRenderer.cpp`**

In the anonymous namespace alongside `LitUbo` and `extractCameraPos`, add:

```cpp
// Builds the directional light's orthographic view-projection matrix.
// `dir` is the sun direction (the engine convention: direction the light
// travels). Eye = center - dir * 2r so the camera sits behind the
// scene relative to the sun. Matches the OpenGL backend's
// computeLightViewProj exactly.
Mat4 computeLightViewProj(Vec3 dir, Vec3 center, float radius) {
    const Vec3 dn = normalize(dir);
    const Vec3 up = (std::fabs(dn.y) > 0.99f)
        ? Vec3{0.0f, 0.0f, 1.0f}
        : Vec3{0.0f, 1.0f, 0.0f};
    const Vec3 eye{
        center.x - dn.x * (radius * 2.0f),
        center.y - dn.y * (radius * 2.0f),
        center.z - dn.z * (radius * 2.0f),
    };
    const Mat4 view = lookAt(eye, center, up);
    const Mat4 proj = orthographic(-radius, radius,
                                   -radius, radius,
                                   radius * 0.5f, radius * 3.5f);
    return proj * view;
}
```

Add the `<cmath>` include at the top of the file if not already present (for `std::fabs`). `lookAt` and `orthographic` are in `engine/math/Transform.h` — verify that header is included via the existing include chain; add `#include "math/Transform.h"` if needed.

- [ ] **Step 3: Un-stub `setShadowBounds` and populate `pendingLightViewProj_` in `beginFrame`**

In `VulkanRenderer.cpp`, find the existing stub:

```cpp
void VulkanRenderer::setShadowBounds(Vec3, float) { warnOnce("setShadowBounds"); }
```

Replace with:

```cpp
void VulkanRenderer::setShadowBounds(Vec3 center, float radius) {
    pendingShadowCenter_ = center;
    pendingShadowRadius_ = radius;
}
```

In `beginFrame`, after the existing M13 `pendingCameraPos_ = extractCameraPos(view);` line, add:

```cpp
    pendingLightViewProj_ = computeLightViewProj(
        pendingSunDir_, pendingShadowCenter_, pendingShadowRadius_);
```

- [ ] **Step 4: Initialize `VkShadowMap` in `VulkanRenderer::init`**

In `VulkanRenderer::init`, after `textures_.init(context_)` and BEFORE the `debugLines_.init` call from M11, add:

```cpp
    if (!shadowMap_.init(context_)) {
        Log::error("VulkanRenderer: VkShadowMap init failed");
        return false;
    }
```

In the destructor, BEFORE `pipelines_.destroy(context_)` and AFTER `hud_.destroy(context_)`:

```cpp
        shadowMap_.destroy(context_);
```

- [ ] **Step 5: Grow `LitUbo` to 288 bytes**

In `VulkanRenderer.cpp`, find the M13 `LitUbo` struct in the anonymous namespace. Replace with:

```cpp
struct LitUbo {
    Mat4 mvp;             // 64
    Mat4 model;           // 64
    Mat4 lightViewProj;   // 64  M14
    Vec4 sunDir;          // 16
    Vec4 sunColor;        // 16
    Vec4 ambient;         // 16
    Vec4 emissive;        // 16
    Vec4 cameraPos;       // 16
    Vec4 materialParams;  // 16  x=uvScale, y=specPower, z=reflectivity, w=shadowBias (M14)
};
static_assert(sizeof(LitUbo) == 288, "LitUbo std140 layout");
```

- [ ] **Step 6: Extend `recordSceneDraw` (the renamed M13 submit body) to populate the new fields + write the shadow sampler**

In `VulkanRenderer.cpp::recordSceneDraw`, find the M13 LitUbo populate block. Add the new field assignments after `ubo.emissive = ...`:

```cpp
    ubo.lightViewProj = pendingLightViewProj_;
```

In the materialParams populate, change the `w` argument from `0.0f` to `pendingShadowBias_`:

```cpp
    ubo.materialParams = Vec4{
        call.material.uvScale,
        call.material.specPower,
        call.material.reflectivity,
        pendingShadowBias_,
    };
```

Find the M13 image-info fan-out block (3 image infos for diffuse/normal/spec). Add a 4th image info for the shadow sampler:

```cpp
    VkDescriptorImageInfo imgInfos[4]{};
    // [0], [1], [2] = diffuse / normal / spec, unchanged from M13
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
```

Find the `VkWriteDescriptorSet writes[4]{}` array. Change it to `writes[5]{}`. Add a 5th write for binding 4:

```cpp
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = set;
    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &imgInfos[3];
```

Update the `vkUpdateDescriptorSets` call: `vkUpdateDescriptorSets(context_.device(), 5, writes, 0, nullptr);`.

- [ ] **Step 7: Run the shadow pass in `endFrame` BEFORE the scene pass**

In `VulkanRenderer::endFrame`, immediately after the `VkCommandBuffer cb = f.commandBuffer;` line and BEFORE the existing scene render pass begin (the `vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);` for the scene), add:

```cpp
    // --- Pass 1: shadow ---
    shadowMap_.record(cb, context_.device(), frames_, meshes_,
                     pendingLightViewProj_, sceneDraws_);
```

The shadow render pass's `finalLayout = SHADER_READ_ONLY_OPTIMAL` plus the subpass dependency `FRAGMENT_SHADER_BIT → EARLY_FRAGMENT_TESTS_BIT` mean no explicit `vkCmdPipelineBarrier` is needed — the layout transition and execution barrier happen at the end of the shadow render pass automatically.

- [ ] **Step 8: Grow descriptor set layout in `VkShader.cpp`**

In `VkShader.cpp`, find the M13 4-binding descriptor set layout. Grow to 5 bindings:

```cpp
    VkDescriptorSetLayoutBinding bindings[5]{};
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
    bindings[4].binding = 4;  // shadow map (M14)
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 5;
    dslInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &s.setLayout));
```

- [ ] **Step 9: Bump sampler pool capacity in `VkFrameRing.cpp`**

In `VkFrameRing.cpp::initFrame`, update the COMBINED_IMAGE_SAMPLER line:

```cpp
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxDescriptorSetsPerFrame},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kMaxDescriptorSetsPerFrame},  // M14: 4 samplers per lit-pass set
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kMaxDescriptorSetsPerFrame},
    };
```

- [ ] **Step 10: Rewrite spinning-cube Vulkan shaders**

In `games/01-spinning-cube/main.cpp`, replace the `#ifdef IRON_RENDER_BACKEND_VULKAN` shader block with:

```cpp
const char* kVertexShader = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LitUbo {
    mat4 mvp;
    mat4 model;
    mat4 lightViewProj;
    vec4 sunDir;
    vec4 sunColor;
    vec4 ambient;
    vec4 emissive;
    vec4 cameraPos;
    vec4 materialParams;  // x=uvScale, y=specPower, z=reflectivity, w=shadowBias
} u;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) out vec4 vLightSpacePos;

void main() {
    vec4 world = u.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(u.model) * aNormal;
    vTangent = mat3(u.model) * aTangent;
    vUV = aUV;
    vLightSpacePos = u.lightViewProj * world;
    gl_Position = u.mvp * vec4(aPos, 1.0);
}
)";

const char* kFragmentShader = R"(#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) in vec4 vLightSpacePos;
layout(location = 0) out vec4 outColor;

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
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;
layout(set = 0, binding = 4) uniform sampler2D uShadowMap;

float shadowFactor(vec4 lightSpacePos, float bias) {
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (proj.z > 1.0) return 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float stored = texture(uShadowMap, uv + vec2(x, y) * texel).r;
            sum += (proj.z - bias > stored) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

void main() {
    float uvScale   = u.materialParams.x;
    float specPower = u.materialParams.y;
    float bias      = u.materialParams.w;
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

    float diffuse  = max(dot(perturbedN, L), 0.0);
    float spec     = pow(max(dot(perturbedN, H), 0.0), specPower);
    float specMask = texture(uSpecularMap, uv).r;
    float shadow   = shadowFactor(vLightSpacePos, bias);

    vec3 lighting = u.sunColor.xyz * (diffuse * shadow + spec * specMask * shadow)
                  + u.ambient.xyz;
    vec3 diff = texture(uDiffuse, uv).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;
    outColor = vec4(lit, 1.0);
}
)";
```

Leave the OpenGL `#else` branch untouched.

- [ ] **Step 11: Add `setShadowBounds` call to spinning-cube**

In `games/01-spinning-cube/main.cpp`, find the existing renderer setup (after `iron::createRenderer(...)` returns). Add:

```cpp
    renderer.setShadowBounds(iron::Vec3{0.0f, 0.0f, 0.0f}, 5.0f);
```

This was previously a no-op stub on Vulkan; now it actually controls the shadow frustum.

- [ ] **Step 12: Rewrite net-shooter Vulkan shaders + update warning**

In `games/07-net-shooter/main.cpp`, replace the `#ifdef IRON_RENDER_BACKEND_VULKAN` shader block with the EXACT SAME `kVertexShader` and `kFragmentShader` code from Step 10. Net-shooter already calls `setShadowBounds(iron::Vec3{0,0,0}, 30.0f)` — leave that intact.

Update the warning string:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path: sun + ambient + emissive "
                    "+ normal/spec + shadow map (Blinn-Phong, 3x3 PCF) lit. "
                    "Still missing point lights, fog, cubemap reflections. "
                    "Full parity ships in future milestones.");
#endif
```

- [ ] **Step 13: Build both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both. If validation layers fire, most likely causes:
- Mismatched descriptor count (writes[5] vs bindingCount=5)
- LitUbo offset mismatch between C++ struct and GLSL declaration

- [ ] **Step 14: Smoke-test the Vulkan builds for actual shadows**

Launch each:

```
.\build-vk\games\01-spinning-cube\Debug\01-spinning-cube.exe
.\build-vk\games\07-net-shooter\Debug\07-net-shooter.exe --listen
```

Expected:
- Spinning cube: the cube casts a self-shadow on its own underside (visible because the cube has facets — the side opposite the sun gets darker, and where geometry overhangs, the shadow appears).
- Net-shooter: walls cast clear shadows onto the floor. Player cubes cast shadows. With the sun at `{-0.4, -1.0, -0.3}`, shadows fall to the lower-right of objects.

If the scene shows uniform lighting (no shadow contrast visible), check:
- `setShadowBounds` was actually called by the game before beginFrame.
- `pendingLightViewProj_` is being recomputed each frame.
- The shadow render pass actually drains `sceneDraws_` (e.g., 0-draw shadow pass = no shadow).
- The shadow sampler is wired to binding 4 in the descriptor write.

- [ ] **Step 15: Commit (atomic — all 6 files in one commit)**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/backends/vulkan/VkShader.cpp engine/render/backends/vulkan/VkFrameRing.cpp games/01-spinning-cube/main.cpp games/07-net-shooter/main.cpp
git commit -m "M14 Task 3: shadow integration (LitUbo 288 + 5 bindings + PCF shaders)"
```

---

## Task 4: Docs append

**Files:**
- Modify: `docs/engine/rhi-abstraction.md`

- [ ] **Step 1: Append the M14 section**

Append at the end of `docs/engine/rhi-abstraction.md`:

```markdown
## Vulkan shadow map + frame-flow restructure (M14)

The Vulkan backend's first multi-pass rendering feature, plus a
foundational restructure of the per-frame flow to make multi-pass
rendering practical going forward.

### Frame-flow restructure: defer + replay

`VulkanRenderer::submit` no longer records into the active command
buffer. It queues into a `std::vector<DrawCall> sceneDraws_`.
External Vulkan subsystems (`VkParticleSystem::render`) register
deferred render callbacks via a new
`VulkanRenderer::enqueueDeferredScenePass` API.
`flushDebugLines` and `drawHud` also defer (HUD already did on
OpenGL). `endFrame` orchestrates the entire pass sequence:

```
endFrame:
  1. Shadow pass (VkShadowMap::record)
       — depth-only render pass, replays sceneDraws_, ends with
         image in SHADER_READ_ONLY_OPTIMAL
  2. Scene render pass
       — replays sceneDraws_ via recordSceneDraw (writes 5
         descriptors per draw including binding 4 = shadow sampler)
       — drains deferredScenePass_ callbacks (particles)
       — drains pending debug-lines if any
       — drains pending HUD if any
  3. End cmd buffer + submit + present
```

### VkShadowMap subsystem

`engine/render/backends/vulkan/VkShadowMap.cpp` owns a 2048×2048
D32_SFLOAT depth image, a depth-only render pass (final layout =
SHADER_READ_ONLY_OPTIMAL via subpass dependency), a framebuffer, a
depth-only graphics pipeline (back-face cull to reduce peter-panning,
no color attachments), and a sampler with NEAREST filter +
CLAMP_TO_BORDER + `VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE` (matches the
OpenGL "out-of-bounds = lit" convention).

### LitUbo grew to 288 bytes

```cpp
struct LitUbo {
    Mat4 mvp;
    Mat4 model;
    Mat4 lightViewProj;   // M14 — for shadow map sampling
    Vec4 sunDir;
    Vec4 sunColor;
    Vec4 ambient;
    Vec4 emissive;
    Vec4 cameraPos;
    Vec4 materialParams;  // x=uvScale, y=specPower, z=reflectivity,
                          // w=shadowBias (M14 repurposes padding)
};
```

`VulkanRenderer::beginFrame` computes `pendingLightViewProj_` from
`pendingSunDir_ + pendingShadowCenter_ + pendingShadowRadius_` using
the same orthographic-from-sphere math the OpenGL backend uses.
`VulkanRenderer::setShadowBounds` (was a stub since M9) now stores
the center + radius for the next frame.

### Descriptor set layout: 5 bindings

| Binding | Type | Stage | Purpose |
|---|---|---|---|
| 0 | UNIFORM_BUFFER | VS+FS | LitUbo |
| 1 | COMBINED_IMAGE_SAMPLER | FS | Diffuse |
| 2 | COMBINED_IMAGE_SAMPLER | FS | Normal |
| 3 | COMBINED_IMAGE_SAMPLER | FS | Specular |
| 4 | COMBINED_IMAGE_SAMPLER | FS | Shadow (M14) |

`VkFrameRing` descriptor pool COMBINED_IMAGE_SAMPLER capacity bumped
to `4 * kMaxDescriptorSetsPerFrame` (= 512 / frame).

### Shader-side PCF sampling

Vertex shader emits `vLightSpacePos = u.lightViewProj * world`.
Fragment shader's `shadowFactor()` helper does 3×3 PCF:
remap clip-space XY to UV via `proj.xy * 0.5 + 0.5` (Vulkan clip-Z
is already [0,1] — no remap needed there, unlike OpenGL), then for
each of 9 texel offsets sample the shadow map and compare to
`proj.z - bias`. Average of 9 samples = soft-edged shadow factor.

### What's still missing

- Point lights (16-array with range falloff) — M15.
- Exponential distance fog — M15.
- Cubemap skybox + cubemap-based reflection — M16.
- Planar reflection — M17.

After M15-M17 land, the Vulkan backend reaches full parity with the
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
git commit -m "M14 Task 4: docs — Vulkan shadow map + frame-flow restructure"
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
| `01-spinning-cube` | Vulkan | visible self-shadowing on facets |
| `07-net-shooter` | OpenGL | unchanged |
| `07-net-shooter` | Vulkan | walls cast shadows on floor + each other; players cast shadows |
| `08-particle-storm` | Vulkan | unchanged (particles record into scene pass via deferred callback — must still render) |

- [ ] **Step 3: Push branch + open PR**

```
git push -u origin feat/m14-vulkan-shadow-map
gh pr create --title "M14: Vulkan shadow map + frame-flow restructure" --body "..."
```

---

## Self-review notes

- **Spec coverage:** every "In scope" item maps to a task. Frame-flow restructure (spec §Architecture frame-flow) is Task 1. VkShadowMap subsystem (spec §VkShadowMap) is Task 2. LitUbo growth + descriptor 5 bindings + shaders (spec §LitUbo extension / §Descriptor set layout / §Shader updates) is Task 3. setShadowBounds + computeLightViewProj (spec §VulkanRenderer::setShadowBounds) is Task 3 Steps 2-4. Docs append is Task 4.
- **No placeholders:** every code block contains the actual content. The plan includes complete inline GLSL, complete C++ struct definitions, complete pipeline create-infos.
- **Type consistency:** `LitUbo` field order matches between C++ struct (Task 3 Step 5), spinning-cube shaders (Task 3 Step 10), net-shooter shaders (Task 3 Step 12), and `recordSceneDraw` populate (Task 3 Step 6). `LightUbo` (shadow pass) uses a SEPARATE single-mat4 struct — does NOT collide with `LitUbo`. `pendingLightViewProj_` consistent across declaration (Task 3 Step 1) and use (Task 3 Steps 3, 6, 7).
- **Atomic Task 3:** descriptor layout change requires shader + recordSceneDraw + descriptor-pool changes in the same commit to keep validation clean. All 6 files bundled.
- **Order of operations in endFrame:** Task 1 establishes the queue-and-replay pattern; Task 3 adds the shadow pass BEFORE the scene pass in endFrame. The order is critical — particles + debug-lines + HUD must render INSIDE the scene pass (after scene geometry, after particles, before HUD as established in M11).
