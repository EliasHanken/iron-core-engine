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
