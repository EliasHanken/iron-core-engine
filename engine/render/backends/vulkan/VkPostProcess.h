#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkPostProcess is Vulkan-only."
#endif

#include "math/Mat4.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>

namespace iron {

class VkContext;

// Owns the offscreen render targets and full-screen passes for the M36
// post-process chain. Phase A: a single offscreen scene-color target (+ depth)
// matching the swapchain, and a "copy" pipeline that blits it to the swapchain
// image. Phase C adds a mask target (R8_UINT color + D32 depth) and a mask pass
// that re-renders tagged draws (effectId != 0) writing their effectId per pixel.
// Recreated on resize. Mirrors VkReflectionTarget.
class VkPostProcess {
public:
    // Push constants for the mask pipeline. Shared with VulkanRenderer so it
    // can fill and push these without duplicating the struct.
    struct MaskPushConstants {
        Mat4     mvp;  // 64 bytes — vertex stage: transform to clip space
        uint32_t id;   // 4 bytes  — fragment stage: effectId written to R8_UINT
    };

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

    // --- Mask pass API (Phase C) ---
    VkRenderPass maskPass() const { return maskPass_; }

    // Begins the mask render pass: clears color to 0 (no effect), depth to 1.0,
    // sets the negative-height scene viewport + scissor so geometry orientation
    // matches the scene pass.
    void beginMaskPass(VkCommandBuffer cb) const;
    void endMaskPass(VkCommandBuffer cb) const;

    // Binds the mask pipeline. Call after beginMaskPass.
    void bindMaskPipeline(VkCommandBuffer cb) const;

    // Pipeline layout with push-constant range for MaskPushConstants. Exposed
    // so VulkanRenderer can call vkCmdPushConstants with the correct layout.
    VkPipelineLayout maskPipelineLayout() const { return maskPipeLayout_; }

    // Image views for the mask targets. Exposed so later passes (outline, x-ray)
    // can build descriptor sets sampling these images.
    VkImageView maskColorView() const { return maskColorView_; }
    VkImageView maskDepthView() const { return maskDepthView_; }

private:
    VkContext* ctx_ = nullptr;
    VkSampler  sampler_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    VkFormat   colorFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat   depthFormat_ = VK_FORMAT_UNDEFINED;

    // --- Scene offscreen target ---
    VkImage       sceneColor_      = VK_NULL_HANDLE;
    VmaAllocation sceneColorAlloc_ = VK_NULL_HANDLE;
    VkImageView   sceneColorView_  = VK_NULL_HANDLE;
    VkImage       sceneDepth_      = VK_NULL_HANDLE;
    VmaAllocation sceneDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView   sceneDepthView_  = VK_NULL_HANDLE;
    VkRenderPass  scenePass_       = VK_NULL_HANDLE;
    VkFramebuffer sceneFb_         = VK_NULL_HANDLE;

    // --- Mask target (R8_UINT color + D32 depth) ---
    VkImage       maskColor_      = VK_NULL_HANDLE;
    VmaAllocation maskColorAlloc_ = VK_NULL_HANDLE;
    VkImageView   maskColorView_  = VK_NULL_HANDLE;
    VkImage       maskDepth_      = VK_NULL_HANDLE;
    VmaAllocation maskDepthAlloc_ = VK_NULL_HANDLE;
    VkImageView   maskDepthView_  = VK_NULL_HANDLE;
    VkRenderPass  maskPass_       = VK_NULL_HANDLE;
    VkFramebuffer maskFb_         = VK_NULL_HANDLE;

    // --- Copy (composite) pipeline ---
    VkDescriptorSetLayout copySetLayout_  = VK_NULL_HANDLE;
    VkPipelineLayout      copyPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          copyPipeline_   = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_       = VK_NULL_HANDLE;
    VkDescriptorSet       copyDescSet_    = VK_NULL_HANDLE;

    // --- Mask pipeline (push-constant only, no descriptor sets) ---
    VkPipelineLayout maskPipeLayout_ = VK_NULL_HANDLE;
    ::VkPipeline     maskPipeline_   = VK_NULL_HANDLE;

    bool createTargets(VkContext& ctx);
    void destroyTargets(VkContext& ctx);
    bool createCopyPipeline(VkContext& ctx, VkRenderPass swapchainPass);
    bool createMaskPipeline(VkContext& ctx);
};

}  // namespace iron
