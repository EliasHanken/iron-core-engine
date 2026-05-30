#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkPostProcess is Vulkan-only."
#endif

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

namespace iron {

class VkContext;

// Owns the offscreen render targets and full-screen passes for the M36
// post-process chain. Phase A: a single offscreen scene-color target (+ depth)
// matching the swapchain, and a "copy" pipeline that blits it to the swapchain
// image. Later phases add the mask target, ping-pong scratch targets, and the
// effect pipelines. Recreated on resize. Mirrors VkReflectionTarget.
class VkPostProcess {
public:
    bool init(VkContext& ctx, VkFormat colorFormat, VkFormat depthFormat,
              VkExtent2D extent, VkSampler sharedSampler,
              VkRenderPass swapchainPass);
    void destroy(VkContext& ctx);
    bool resize(VkContext& ctx, VkExtent2D extent);

    VkRenderPass  scenePass()        const { return scenePass_; }
    VkFramebuffer sceneFramebuffer() const { return sceneFb_; }
    VkExtent2D    extent()           const { return extent_; }

    void beginScenePass(VkCommandBuffer cb, const float clearColor[4]) const;
    void endScenePass(VkCommandBuffer cb) const;
    void recordComposite(VkCommandBuffer cb) const;

private:
    VkContext* ctx_ = nullptr;
    VkSampler  sampler_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat   colorFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat   depthFormat_ = VK_FORMAT_UNDEFINED;

    VkImage       sceneColor_      = VK_NULL_HANDLE;
    VmaAllocation sceneColorAlloc_ = VK_NULL_HANDLE;
    VkImageView   sceneColorView_  = VK_NULL_HANDLE;
    VkImage       sceneDepth_      = VK_NULL_HANDLE;
    VmaAllocation sceneDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView   sceneDepthView_  = VK_NULL_HANDLE;
    VkRenderPass  scenePass_       = VK_NULL_HANDLE;
    VkFramebuffer sceneFb_         = VK_NULL_HANDLE;

    VkDescriptorSetLayout copySetLayout_  = VK_NULL_HANDLE;
    VkPipelineLayout      copyPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          copyPipeline_   = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_       = VK_NULL_HANDLE;
    VkDescriptorSet       copyDescSet_    = VK_NULL_HANDLE;

    bool createTargets(VkContext& ctx);
    void destroyTargets(VkContext& ctx);
    bool createCopyPipeline(VkContext& ctx, VkRenderPass swapchainPass);
};

}  // namespace iron
