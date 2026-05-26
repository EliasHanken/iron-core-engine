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
