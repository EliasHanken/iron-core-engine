# M16 Vulkan Cubemap Skybox + Cubemap Reflection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring cubemap skybox + cubemap-reflection sampling to the Vulkan backend — un-stubs `createCubemap` + `setSkybox`, draws the cubemap behind the scene, and lets reflective materials sample it via `reflect(view, normal)`.

**Architecture:** Two new subsystems share the cubemap storage: `VkCubemapStore` (image creation + 6-face staging upload + sampler + 1×1×6 black fallback) and `VkSkybox` (cube mesh + skybox pipeline drawn first inside the scene pass with `gl_Position.xyww` z=1 trick). The lit pass descriptor set layout grows from 5 to 6 bindings (cubemap sampler at binding 5); reflective materials gated on `materialParams.z > 0` mix `texture(uSkyCubemap, reflect(view, perturbedN))` into the final color.

**Tech Stack:** C++23, Vulkan 1.3, glslang (GLSL 450 → SPIR-V), VMA, MSVC, CMake.

---

## File Structure

### New files
- `engine/render/backends/vulkan/VkCubemap.h`
- `engine/render/backends/vulkan/VkCubemap.cpp`
- `engine/render/backends/vulkan/VkSkybox.h`
- `engine/render/backends/vulkan/VkSkybox.cpp`

### Modified files
- `engine/render/backends/vulkan/VulkanRenderer.h` — adds `VkCubemapStore cubemaps_;`, `VkSkybox skybox_;`, `CubemapHandle pendingSkybox_;`
- `engine/render/backends/vulkan/VulkanRenderer.cpp` — un-stubs `createCubemap`/`setSkybox`; init/destroy lifecycle; `endFrame` draws skybox; `recordSceneDraw` writes 6 descriptors
- `engine/render/backends/vulkan/VkShader.cpp` — descriptor set layout 5 → 6 bindings
- `engine/render/backends/vulkan/VkFrameRing.cpp` — sampler pool capacity `4*` → `5*` `kMaxDescriptorSetsPerFrame`
- `engine/CMakeLists.txt` — registers `VkCubemap.cpp` + `VkSkybox.cpp` under the Vulkan branch
- `games/01-spinning-cube/main.cpp` — Vulkan shaders gain the binding=5 sampler + reflection block
- `games/07-net-shooter/main.cpp` — same; warning string updated
- `docs/engine/rhi-abstraction.md` — appended M16 section

---

## Task 1: `VkCubemapStore` subsystem (image + 6-face upload + sampler + black fallback)

**Files:**
- Create: `engine/render/backends/vulkan/VkCubemap.h`
- Create: `engine/render/backends/vulkan/VkCubemap.cpp`
- Modify: `engine/CMakeLists.txt`

Standalone subsystem. Compiles but isn't used yet (Task 3 integrates).

- [ ] **Step 1: Write `VkCubemap.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkCubemap is Vulkan-only."
#endif

#include "render/Handles.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <unordered_map>

namespace iron {

class VkContext;

struct VkCubemapResource {
    VkImage       image   = VK_NULL_HANDLE;
    VmaAllocation alloc   = VK_NULL_HANDLE;
    VkImageView   view    = VK_NULL_HANDLE;
    VkSampler     sampler = VK_NULL_HANDLE;  // shared with store
    std::uint32_t width   = 0;
    std::uint32_t height  = 0;
};

// Vulkan cubemap storage. Mirrors VkTextureStore pattern: shared
// sampler, in-memory creation from six RGBA face arrays, built-in
// 1x1x6 black fallback for the lit pass's reflection sampler when no
// skybox is set.
class VkCubemapStore {
public:
    bool init(VkContext& ctx);
    void destroyAll(VkContext& ctx);

    CubemapHandle createFromFaces(VkContext& ctx, int width, int height,
                                  const std::array<const unsigned char*, 6>& faces);

    CubemapHandle blackCubemap() const { return black_; }
    const VkCubemapResource& get(CubemapHandle h) const;
    bool has(CubemapHandle h) const { return cubemaps_.count(h) != 0; }

private:
    void uploadFaces(VkContext& ctx, VkCubemapResource& res,
                     int width, int height,
                     const std::array<const unsigned char*, 6>& faces);

    std::unordered_map<CubemapHandle, VkCubemapResource> cubemaps_;
    CubemapHandle nextHandle_ = 1;
    VkSampler     sharedSampler_ = VK_NULL_HANDLE;
    CubemapHandle black_ = kInvalidHandle;
};

}  // namespace iron
```

- [ ] **Step 2: Write `VkCubemap.cpp`**

```cpp
// VkCubemap.cpp — Vulkan cubemap image storage (6 array layers,
// VK_IMAGE_VIEW_TYPE_CUBE) with shared sampler and built-in 1x1x6
// black fallback.

#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>
#include <vector>

namespace iron {

bool VkCubemapStore::init(VkContext& ctx) {
    // Shared sampler.
    VkSamplerCreateInfo sInfo{};
    sInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sInfo.magFilter = VK_FILTER_LINEAR;
    sInfo.minFilter = VK_FILTER_LINEAR;
    sInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.minLod = 0.0f;
    sInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(ctx.device(), &sInfo, nullptr, &sharedSampler_));

    // 1x1x6 black fallback.
    const unsigned char blackPixel[4] = {0, 0, 0, 255};
    std::array<const unsigned char*, 6> blackFaces{
        blackPixel, blackPixel, blackPixel,
        blackPixel, blackPixel, blackPixel,
    };
    black_ = createFromFaces(ctx, 1, 1, blackFaces);
    if (black_ == kInvalidHandle) {
        Log::error("VkCubemapStore: black fallback creation failed");
        return false;
    }
    return true;
}

void VkCubemapStore::destroyAll(VkContext& ctx) {
    for (auto& [h, res] : cubemaps_) {
        if (res.view)  { vkDestroyImageView(ctx.device(), res.view, nullptr); }
        if (res.image) { vmaDestroyImage(ctx.allocator(), res.image, res.alloc); }
    }
    cubemaps_.clear();
    if (sharedSampler_) {
        vkDestroySampler(ctx.device(), sharedSampler_, nullptr);
        sharedSampler_ = VK_NULL_HANDLE;
    }
    black_ = kInvalidHandle;
}

CubemapHandle VkCubemapStore::createFromFaces(
        VkContext& ctx, int width, int height,
        const std::array<const unsigned char*, 6>& faces) {
    if (width <= 0 || height <= 0) return kInvalidHandle;
    for (int i = 0; i < 6; ++i) {
        if (faces[i] == nullptr) return kInvalidHandle;
    }

    VkCubemapResource res{};
    res.width  = static_cast<std::uint32_t>(width);
    res.height = static_cast<std::uint32_t>(height);
    res.sampler = sharedSampler_;

    // Cube-compatible image.
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imgInfo.extent = {res.width, res.height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 6;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aInfo{};
    aInfo.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &imgInfo, &aInfo,
                            &res.image, &res.alloc, nullptr));

    // Cube image view.
    VkImageViewCreateInfo vInfo{};
    vInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vInfo.image = res.image;
    vInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vInfo.subresourceRange.baseMipLevel = 0;
    vInfo.subresourceRange.levelCount = 1;
    vInfo.subresourceRange.baseArrayLayer = 0;
    vInfo.subresourceRange.layerCount = 6;
    VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &res.view));

    uploadFaces(ctx, res, width, height, faces);

    const CubemapHandle h = nextHandle_++;
    cubemaps_[h] = res;
    return h;
}

const VkCubemapResource& VkCubemapStore::get(CubemapHandle h) const {
    auto it = cubemaps_.find(h);
    return it->second;  // caller-checked via has()
}

void VkCubemapStore::uploadFaces(VkContext& ctx, VkCubemapResource& res,
                                 int width, int height,
                                 const std::array<const unsigned char*, 6>& faces) {
    const std::size_t faceBytes = static_cast<std::size_t>(width) * height * 4;
    const std::size_t totalBytes = faceBytes * 6;

    // Staging buffer holding all 6 faces concatenated.
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = totalBytes;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocInfo{};
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAllocHandle = VK_NULL_HANDLE;
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bufInfo, &stagingAlloc,
                             &stagingBuf, &stagingAllocHandle, &allocInfo));

    auto* mapped = static_cast<unsigned char*>(allocInfo.pMappedData);
    for (int i = 0; i < 6; ++i) {
        std::memcpy(mapped + i * faceBytes, faces[i], faceBytes);
    }
    vmaFlushAllocation(ctx.allocator(), stagingAllocHandle, 0, totalBytes);

    // One-shot cmd buffer for the upload.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = ctx.graphicsFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &pool));

    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));

    // UNDEFINED -> TRANSFER_DST_OPTIMAL on all 6 layers.
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = res.image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 6;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    // Six copy regions, one per face.
    VkBufferImageCopy regions[6]{};
    for (int i = 0; i < 6; ++i) {
        regions[i].bufferOffset = i * faceBytes;
        regions[i].bufferRowLength = 0;
        regions[i].bufferImageHeight = 0;
        regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[i].imageSubresource.mipLevel = 0;
        regions[i].imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(i);
        regions[i].imageSubresource.layerCount = 1;
        regions[i].imageOffset = {0, 0, 0};
        regions[i].imageExtent = {res.width, res.height, 1};
    }
    vkCmdCopyBufferToImage(cb, stagingBuf, res.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);

    // TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL.
    VkImageMemoryBarrier toShaderRead = toTransfer;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    vmaDestroyBuffer(ctx.allocator(), stagingBuf, stagingAllocHandle);
}

}  // namespace iron
```

- [ ] **Step 3: Register the new file in `engine/CMakeLists.txt`**

Inside the `if (IRON_RENDER_BACKEND STREQUAL "vulkan")` block, append to `target_sources(...)`:

```
      render/backends/vulkan/VkCubemap.cpp
```

- [ ] **Step 4: Build both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both. The new file compiles but isn't used yet — Task 3 integrates.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkCubemap.h engine/render/backends/vulkan/VkCubemap.cpp engine/CMakeLists.txt
git commit -m "M16 Task 1: VkCubemapStore subsystem (6-face upload + sampler + black fallback)"
```

---

## Task 2: `VkSkybox` subsystem (cube mesh + skybox pipeline + record API)

**Files:**
- Create: `engine/render/backends/vulkan/VkSkybox.h`
- Create: `engine/render/backends/vulkan/VkSkybox.cpp`
- Modify: `engine/CMakeLists.txt`

Standalone subsystem. Not yet integrated.

- [ ] **Step 1: Write `VkSkybox.h`**

```cpp
#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkSkybox is Vulkan-only."
#endif

#include "math/Mat4.h"
#include "render/backends/vulkan/VkCubemap.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace iron {

class VkContext;
class VkFrameRing;

// Vulkan skybox renderer. Owns a one-time-uploaded cube mesh + a
// graphics pipeline that samples a cubemap and writes z=1 via the
// gl_Position.xyww trick. Drawn inside the scene render pass before
// the geometry replay so the GPU can early-z-reject sky fragments
// behind opaque geometry.
class VkSkybox {
public:
    bool init(VkContext& ctx, VkRenderPass scenePass);
    void destroy(VkContext& ctx);

    // Records the skybox into the active command buffer. `viewProjection`
    // must be the translation-stripped view-projection matrix
    // (caller computes it).
    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                const VkCubemapResource& cubemap,
                const Mat4& viewProjection);

private:
    struct SkyUbo { float viewProjection[16]; };

    bool ok_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkBuffer       vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation  vertexAlloc_  = VK_NULL_HANDLE;
    VkBuffer       indexBuffer_  = VK_NULL_HANDLE;
    VmaAllocation  indexAlloc_   = VK_NULL_HANDLE;
};

}  // namespace iron
```

- [ ] **Step 2: Write `VkSkybox.cpp`**

```cpp
// VkSkybox.cpp — cube mesh + skybox pipeline. Drawn first inside the
// scene render pass; samples a cubemap and writes z=1.

#include "render/backends/vulkan/VkSkybox.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>

namespace iron {

namespace {

const char* kVert = R"(#version 450
layout(location = 0) in vec3 aPos;

layout(set = 0, binding = 0) uniform SkyUbo {
    mat4 viewProjection;
} u;

layout(location = 0) out vec3 vDir;

void main() {
    vDir = aPos;
    vec4 pos = u.viewProjection * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
)";

const char* kFrag = R"(#version 450
layout(location = 0) in vec3 vDir;
layout(set = 0, binding = 1) uniform samplerCube uSkyCubemap;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(uSkyCubemap, vDir).rgb, 1.0);
}
)";

// 8 unique cube corners, 36 indices for 12 triangles. Counter-clockwise
// winding when viewed from inside (the cube is drawn with cull = NONE,
// so winding doesn't matter, but stay consistent).
constexpr float kCubeVerts[24] = {
    -1.0f, -1.0f, -1.0f,  // 0
     1.0f, -1.0f, -1.0f,  // 1
     1.0f,  1.0f, -1.0f,  // 2
    -1.0f,  1.0f, -1.0f,  // 3
    -1.0f, -1.0f,  1.0f,  // 4
     1.0f, -1.0f,  1.0f,  // 5
     1.0f,  1.0f,  1.0f,  // 6
    -1.0f,  1.0f,  1.0f,  // 7
};
constexpr std::uint32_t kCubeIdx[36] = {
    // -Z face
    0, 2, 1,   0, 3, 2,
    // +Z face
    4, 5, 6,   4, 6, 7,
    // -X face
    0, 4, 7,   0, 7, 3,
    // +X face
    1, 2, 6,   1, 6, 5,
    // -Y face
    0, 1, 5,   0, 5, 4,
    // +Y face
    3, 7, 6,   3, 6, 2,
};

}  // namespace

bool VkSkybox::init(VkContext& ctx, VkRenderPass scenePass) {
    // Upload cube vertex + index buffers to device-local memory via staging.
    auto uploadDeviceLocal = [&](const void* src, VkDeviceSize size,
                                 VkBufferUsageFlags usage,
                                 VkBuffer& outBuffer, VmaAllocation& outAlloc) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai,
                                 &outBuffer, &outAlloc, nullptr));

        // Staging.
        VkBufferCreateInfo sbi = bi;
        sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo sai{};
        sai.usage = VMA_MEMORY_USAGE_AUTO;
        sai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo sallocInfo{};
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &sbi, &sai,
                                 &staging, &stagingAlloc, &sallocInfo));
        std::memcpy(sallocInfo.pMappedData, src, size);
        vmaFlushAllocation(ctx.allocator(), stagingAlloc, 0, size);

        // One-shot copy.
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = ctx.graphicsFamily();
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkCommandPool pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbi, &cb));

        VkCommandBufferBeginInfo bb{};
        bb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bb));
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cb, staging, outBuffer, 1, &copy);
        VK_CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

        vkDestroyCommandPool(ctx.device(), pool, nullptr);
        vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);
    };

    uploadDeviceLocal(kCubeVerts, sizeof(kCubeVerts),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      vertexBuffer_, vertexAlloc_);
    uploadDeviceLocal(kCubeIdx, sizeof(kCubeIdx),
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      indexBuffer_, indexAlloc_);

    // Shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkSkybox: shader compile failed");
        return false;
    }
    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkSkybox: fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout (binding 0 = UBO VS, binding 1 = samplerCube FS).
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 2;
    dslInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &setLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &pipelineLayout_));

    // Pipeline.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm; stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(float) * 3;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{};
    attr.location = 0; attr.binding = 0;
    attr.format = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset = 0;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount = 2;
    pInfo.pStages = stages;
    pInfo.pVertexInputState = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState = &ms;
    pInfo.pDepthStencilState = &ds;
    pInfo.pColorBlendState = &cb;
    pInfo.pDynamicState = &dyn;
    pInfo.layout = pipelineLayout_;
    pInfo.renderPass = scenePass;
    pInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    ok_ = true;
    return true;
}

void VkSkybox::destroy(VkContext& ctx) {
    if (pipeline_)       { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
    if (indexBuffer_)    { vmaDestroyBuffer(ctx.allocator(), indexBuffer_, indexAlloc_); indexBuffer_ = VK_NULL_HANDLE; }
    if (vertexBuffer_)   { vmaDestroyBuffer(ctx.allocator(), vertexBuffer_, vertexAlloc_); vertexBuffer_ = VK_NULL_HANDLE; }
    ok_ = false;
}

void VkSkybox::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                     const VkCubemapResource& cubemap,
                     const Mat4& viewProjection) {
    if (!ok_ || cb == VK_NULL_HANDLE) return;

    SkyUbo ubo;
    std::memcpy(ubo.viewProjection, viewProjection.m, sizeof(ubo.viewProjection));
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

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = cubemap.sampler;
    imgInfo.imageView   = cubemap.view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
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
    writes[1].pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &set, 0, nullptr);
    VkDeviceSize offsets[1] = {0};
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer_, offsets);
    vkCmdBindIndexBuffer(cb, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, 36, 1, 0, 0, 0);
}

}  // namespace iron
```

- [ ] **Step 3: Register the new file in `engine/CMakeLists.txt`**

Inside the Vulkan `target_sources(...)` block, append:

```
      render/backends/vulkan/VkSkybox.cpp
```

- [ ] **Step 4: Build both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 pass on both. New file compiles but isn't used yet — Task 3 integrates.

- [ ] **Step 5: Commit**

```bash
git add engine/render/backends/vulkan/VkSkybox.h engine/render/backends/vulkan/VkSkybox.cpp engine/CMakeLists.txt
git commit -m "M16 Task 2: VkSkybox subsystem (cube mesh + skybox pipeline + record API)"
```

---

## Task 3: Integration + descriptor layout + shader rewrites (atomic)

**Files:**
- Modify: `engine/render/backends/vulkan/VulkanRenderer.h`
- Modify: `engine/render/backends/vulkan/VulkanRenderer.cpp`
- Modify: `engine/render/backends/vulkan/VkShader.cpp`
- Modify: `engine/render/backends/vulkan/VkFrameRing.cpp`
- Modify: `games/01-spinning-cube/main.cpp`
- Modify: `games/07-net-shooter/main.cpp`

Atomic — all 6 file changes in one commit so the tree stays compiling + validating.

- [ ] **Step 1: Add new private state to `VulkanRenderer.h`**

Add the includes:

```cpp
#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkSkybox.h"
```

In the `private:` section, alongside the M14/M15 backend subsystems (`shadowMap_`, etc.):

```cpp
    // M16 — cubemap storage + skybox subsystem + currently-set skybox.
    VkCubemapStore cubemaps_;
    VkSkybox       skybox_;
    CubemapHandle  pendingSkybox_ = kInvalidHandle;
```

- [ ] **Step 2: Wire init/destroy in `VulkanRenderer.cpp`**

In `VulkanRenderer::init`, after `textures_.init(context_)` and BEFORE `shadowMap_.init(...)`:

```cpp
    if (!cubemaps_.init(context_)) {
        Log::error("VulkanRenderer: VkCubemapStore init failed");
        return false;
    }
```

After `shadowMap_.init(context_)`:

```cpp
    if (!skybox_.init(context_, scenePass())) {
        Log::error("VulkanRenderer: VkSkybox init failed");
        return false;
    }
```

In the destructor (`~VulkanRenderer()`), BEFORE `shadowMap_.destroy(context_)`:

```cpp
    skybox_.destroy(context_);
```

After `textures_.destroyAll(context_)` and BEFORE `pipelines_.destroy(context_)`:

```cpp
    cubemaps_.destroyAll(context_);
```

- [ ] **Step 3: Un-stub `createCubemap` and `setSkybox`**

Find the M9 stubs:

```cpp
CubemapHandle VulkanRenderer::createCubemap(int, int,
    const std::array<const unsigned char*, 6>&) {
    warnOnce("createCubemap");
    return kInvalidHandle;
}
void VulkanRenderer::setSkybox(CubemapHandle) { warnOnce("setSkybox"); }
```

Replace with:

```cpp
CubemapHandle VulkanRenderer::createCubemap(int width, int height,
        const std::array<const unsigned char*, 6>& faces) {
    return cubemaps_.createFromFaces(context_, width, height, faces);
}

void VulkanRenderer::setSkybox(CubemapHandle sky) {
    pendingSkybox_ = sky;
}
```

- [ ] **Step 4: Draw the skybox in `endFrame`**

In `VulkanRenderer::endFrame`, inside the scene render pass (after the existing M14 shadow pass + render pass begin + setSceneViewport call) and BEFORE the `for (const DrawCall& call : sceneDraws_)` loop, add:

```cpp
        // M16 — draw skybox first inside the scene pass.
        if (cubemaps_.has(pendingSkybox_)) {
            Mat4 v = pendingView_;
            v.at(0, 3) = 0.0f;
            v.at(1, 3) = 0.0f;
            v.at(2, 3) = 0.0f;
            const Mat4 vp = pendingProjection_ * v;
            skybox_.record(cb, context_.device(), frames_,
                          cubemaps_.get(pendingSkybox_), vp);
        }
```

- [ ] **Step 5: Extend `recordSceneDraw` to write a 6th descriptor**

In `VulkanRenderer::recordSceneDraw`, find the existing imgInfos array (M14 has `imgInfos[4]{}` covering diffuse / normal / spec / shadow). Change the array size to 5 and add the cubemap entry:

```cpp
    // M16 — also bind the active skybox (or black fallback) at binding 5.
    const CubemapHandle skyHandle = cubemaps_.has(pendingSkybox_)
        ? pendingSkybox_
        : cubemaps_.blackCubemap();
    const auto& skyTex = cubemaps_.get(skyHandle);

    VkDescriptorImageInfo imgInfos[5]{};
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
```

Replace the M14 `VkWriteDescriptorSet writes[5]{}` with `writes[6]{}`. Add a 6th write at binding 5 pointing to `imgInfos[4]`:

```cpp
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = set;
    writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].descriptorCount = 1;
    writes[5].pImageInfo = &imgInfos[4];
```

Update the `vkUpdateDescriptorSets` call from `..., 5, writes, ...` to `..., 6, writes, ...`.

- [ ] **Step 6: Grow descriptor set layout in `VkShader.cpp`**

In `engine/render/backends/vulkan/VkShader.cpp`, find the M14 5-binding layout. Grow to 6 bindings — add binding 5 for the cubemap sampler:

```cpp
    VkDescriptorSetLayoutBinding bindings[6]{};
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

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 6;
    dslInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &s.setLayout));
```

- [ ] **Step 7: Bump descriptor pool sampler capacity in `VkFrameRing.cpp`**

In `VkFrameRing.cpp::initFrame`, update the COMBINED_IMAGE_SAMPLER line:

```cpp
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxDescriptorSetsPerFrame},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * kMaxDescriptorSetsPerFrame},  // M16: 5 samplers per lit set
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kMaxDescriptorSetsPerFrame},
    };
```

- [ ] **Step 8: Rewrite spinning-cube Vulkan shaders**

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
    vec4 materialParams;
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
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
    vec4 fogColor;
    vec4 lightCounts;
    vec4 pointPositions[16];
    vec4 pointColors[16];
} u;

layout(set = 0, binding = 1) uniform sampler2D uDiffuse;
layout(set = 0, binding = 2) uniform sampler2D uNormalMap;
layout(set = 0, binding = 3) uniform sampler2D uSpecularMap;
layout(set = 0, binding = 4) uniform sampler2D uShadowMap;
layout(set = 0, binding = 5) uniform samplerCube uSkyCubemap;

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

    // Point lights (M15).
    int plCount = int(u.lightCounts.x);
    for (int i = 0; i < plCount; ++i) {
        vec3 toLight = u.pointPositions[i].xyz - vWorldPos;
        float dist  = length(toLight);
        float range = u.pointColors[i].w;
        if (dist < 0.0001 || dist >= range) continue;
        vec3 Lp = toLight / dist;
        float falloff   = 1.0 - smoothstep(0.0, range, dist);
        float intensity = u.pointPositions[i].w;
        float diffusePL = max(dot(perturbedN, Lp), 0.0);
        vec3  Hp        = normalize(Lp + V);
        float specPL    = pow(max(dot(perturbedN, Hp), 0.0), specPower);
        lighting += u.pointColors[i].xyz * intensity * falloff
                  * (diffusePL + specPL * specMask);
    }

    vec3 diff = texture(uDiffuse, uv).rgb;
    vec3 lit = diff * lighting + u.emissive.xyz;

    // M16 — cubemap reflection (planar reflection comes in M17).
    float reflectivity = u.materialParams.z;
    if (reflectivity > 0.0) {
        vec3 viewDir = normalize(vWorldPos - u.cameraPos.xyz);
        vec3 reflectDir = reflect(viewDir, perturbedN);
        vec3 reflectColor = texture(uSkyCubemap, reflectDir).rgb;
        lit = mix(lit, reflectColor, reflectivity);
    }

    // Fog (M15).
    float distFromCamera = length(u.cameraPos.xyz - vWorldPos);
    float fogFactor = 1.0 - exp(-u.fogColor.w * distFromCamera);
    vec3 finalColor = mix(lit, u.fogColor.xyz, clamp(fogFactor, 0.0, 1.0));
    outColor = vec4(finalColor, 1.0);
}
)";
```

Leave the OpenGL `#else` branch untouched.

- [ ] **Step 9: Rewrite net-shooter Vulkan shaders + update warning**

In `games/07-net-shooter/main.cpp`, replace the `#ifdef IRON_RENDER_BACKEND_VULKAN` shader block with the EXACT SAME `kVertexShader` and `kFragmentShader` code from Step 8 (both games share identical Vulkan shaders).

Update the warning string:

```cpp
#ifdef IRON_RENDER_BACKEND_VULKAN
    iron::Log::warn("net-shooter Vulkan path: full lit pass (sun + "
                    "ambient + emissive + normal/spec + shadow + point "
                    "lights + fog + cubemap reflections; Blinn-Phong, "
                    "3x3 PCF). Still missing planar reflection. Full "
                    "parity ships in M17.");
#endif
```

- [ ] **Step 10: Build both backends + run tests**

```
cmake --build build --config Debug
cmake --build build-vk --config Debug
ctest --test-dir build -C Debug --output-on-failure
ctest --test-dir build-vk -C Debug --output-on-failure
```

Expected: 35/35 on both. Likely issues if it fails:
- Mismatched descriptor count (writes[6] vs bindingCount=6)
- LitUbo field-order mismatch between C++ struct and GLSL declaration
- Cubemap-sampler write missing → validation layer error at allocate-descriptor-set time

- [ ] **Step 11: Smoke-test the Vulkan builds**

```
.\build-vk\games\01-spinning-cube\Debug\01-spinning-cube.exe
.\build-vk\games\07-net-shooter\Debug\net-shooter.exe --listen
```

Expected:
- **Spinning-cube**: unchanged visually (no skybox set, no reflective materials).
- **Net-shooter**: sunset cubemap visible behind the arena (currently a flat blue clear color). Brick walls + ground + cubes still rendered correctly. PowerShell shows the new warning string.
- **OpenGL net-shooter**: unchanged (was already drawing the sunset cubemap via GL backend).

If the sky doesn't show on Vulkan, most likely causes:
- `setSkybox` not actually called — confirm by reading net-shooter's main.cpp around the existing `iron::CubemapHandle sky = renderer.createCubemap(...)` line.
- The skybox draw is being clipped — check the `gl_Position.xyww` swizzle compiled correctly.
- The cubemap image layout mismatch — check validation layer output.

- [ ] **Step 12: Commit (atomic — all 6 files in one commit)**

```bash
git add engine/render/backends/vulkan/VulkanRenderer.h engine/render/backends/vulkan/VulkanRenderer.cpp engine/render/backends/vulkan/VkShader.cpp engine/render/backends/vulkan/VkFrameRing.cpp games/01-spinning-cube/main.cpp games/07-net-shooter/main.cpp
git commit -m "M16 Task 3: skybox + cubemap reflection integration (6 bindings + shaders)"
```

---

## Task 4: Docs append

**Files:**
- Modify: `docs/engine/rhi-abstraction.md`

- [ ] **Step 1: Append the M16 section**

Append at the end of `docs/engine/rhi-abstraction.md`:

```markdown
## Vulkan cubemap skybox + cubemap reflection (M16)

Two cubemap-dependent features bundled because they share the
underlying cubemap storage.

### VkCubemapStore subsystem

`engine/render/backends/vulkan/VkCubemap.cpp` owns cube-compatible
images (6 array layers, `VK_IMAGE_VIEW_TYPE_CUBE`), a shared linear
sampler with CLAMP_TO_EDGE address modes, and a built-in 1×1×6 black
fallback. `createFromFaces(width, height, faces[6])` does a single
staging-buffer upload of all 6 faces with one `vkCmdCopyBufferToImage`
call + layout transitions.

The black fallback is used by the lit pass when no skybox is set:
`mix(lit, blackReflection, reflectivity)` → no contribution, matches
the "no skybox" OpenGL behavior.

### VkSkybox subsystem

`engine/render/backends/vulkan/VkSkybox.cpp` owns a one-time-uploaded
cube mesh (8 vertices, 36 indices) + a graphics pipeline:
- Vertex shader emits `gl_Position = (vp * pos).xyww` — clip-Z = clip-W
  → after perspective divide → NDC-Z = 1.0
- depthCompareOp = `LESS_OR_EQUAL` so z=1 passes against the cleared
  depth of 1.0 (sky fragments pass only where geometry hasn't written
  closer depth)
- Fragment shader samples the cubemap with `vDir = aPos` (the
  vertex position IS the direction from the origin)
- cull = NONE (we're inside the cube)
- depth write = OFF (cube doesn't contribute to depth)

`VulkanRenderer::endFrame` draws the skybox FIRST inside the scene
render pass — before geometry — so the GPU can early-z-reject sky
fragments behind opaque geometry. The translation is stripped from
the view matrix CPU-side so the cube stays centered on the camera.

### createCubemap + setSkybox un-stubbed

`VulkanRenderer::createCubemap` (was a `warnOnce` stub since M9) now
delegates to `VkCubemapStore::createFromFaces`. `setSkybox` stores
the handle in `pendingSkybox_`. `endFrame` skips the skybox draw if
`pendingSkybox_ == kInvalidHandle`.

### Cubemap reflection in the lit shader

The lit pass descriptor set layout grew from 5 to 6 bindings (cubemap
sampler at binding 5). `VulkanRenderer::recordSceneDraw` writes the
active skybox (or black fallback) as the 6th descriptor per draw.
`VkFrameRing` descriptor pool sampler capacity bumped to `5 *
kMaxDescriptorSetsPerFrame` (= 1280 / frame).

The lit fragment shader gains a reflection block guarded by
`materialParams.z` (reflectivity, present in LitUbo since M13):

```glsl
float reflectivity = u.materialParams.z;
if (reflectivity > 0.0) {
    vec3 viewDir = normalize(vWorldPos - u.cameraPos.xyz);
    vec3 reflectDir = reflect(viewDir, perturbedN);
    vec3 reflectColor = texture(uSkyCubemap, reflectDir).rgb;
    lit = mix(lit, reflectColor, reflectivity);
}
```

The branch is uniform across the draw call, so the GPU resolves it
efficiently. When reflectivity = 0 (default for most materials), the
block is skipped entirely.

### What's still missing

- Planar reflection (M17 — RTT pass with mirrored camera + clip plane).

After M17 lands, the Vulkan backend reaches full parity with the
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
git commit -m "M16 Task 4: docs — Vulkan cubemap skybox + cubemap reflection"
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
| `01-spinning-cube` | Vulkan | unchanged (no skybox, no reflective materials) |
| `07-net-shooter` | OpenGL | unchanged |
| `07-net-shooter` | Vulkan | visible sunset cubemap behind arena |
| `08-particle-storm` | Vulkan | unchanged (no skybox, no scene draws) |

- [ ] **Step 3: Push branch + open PR**

```
git push -u origin feat/m16-vulkan-cubemap-skybox-reflection
gh pr create --title "M16: Vulkan cubemap skybox + cubemap reflection" --body "..."
```

---

## Self-review notes

- **Spec coverage:** every "In scope" item maps to a task. VkCubemapStore (spec §VkCubemapStore) = Task 1. VkSkybox (spec §VkSkybox) = Task 2. VulkanRenderer integration + descriptor layout + shader rewrites + warning (spec §VulkanRenderer changes, §VkShader.cpp, §VkFrameRing.cpp, §Shader updates, §Net-shooter startup warning) = Task 3 (atomic). Docs = Task 4.
- **No placeholders:** every code block contains the actual content.
- **Type consistency:** `VkCubemapResource` field names (`image`, `alloc`, `view`, `sampler`, `width`, `height`) match between Task 1 declaration and Task 3 use. `pendingSkybox_` consistent across declaration (Task 3 Step 1), use (Task 3 Steps 3-5). Shader UBO field order matches the C++ LitUbo struct exactly (M15 layout — no new fields in M16; reflectivity sits at `materialParams.z` since M13).
- **Atomic Task 3:** the descriptor layout change requires shader rewrites + descriptor pool bump + recordSceneDraw 6-descriptor write all in the same commit to keep validation clean.
- **Skybox draws first:** explicitly ordered before the geometry replay loop in Task 3 Step 4 for early-z efficiency.
- **Net-shooter wiring:** spec calls out that net-shooter already calls `createCubemap` (the sunset cubemap from the `generateSunsetFace` block) and `setSkybox(sky)` in its existing setup (~line 465). These calls were no-ops on Vulkan since M9 — M16 makes them functional. No game-side change needed.
