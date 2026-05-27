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
    VkDescriptorImageInfo descriptorImageInfo() const;

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
