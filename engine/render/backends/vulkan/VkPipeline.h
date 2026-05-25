#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace iron {

class VkContext;
class VkSwapchain;
struct VkShader;

// Owns the foundation render pass + one VkPipeline per shader + framebuffers.
// Recreate framebuffers when the swapchain is recreated.
class VkPipeline {
public:
    bool init(VkContext& ctx, VkSwapchain& swap);
    void destroy(VkContext& ctx);

    // Recreate framebuffers only (render pass + pipeline stay valid).
    bool recreateFramebuffers(VkContext& ctx, VkSwapchain& swap);

    // Build (or fetch) a graphics pipeline for a given VkShader.
    // Cached by shader pointer.
    ::VkPipeline pipelineFor(VkContext& ctx, VkSwapchain& swap, const VkShader& sh);

    VkRenderPass  renderPass()                    const { return renderPass_; }
    VkFramebuffer framebuffer(std::uint32_t i)    const { return framebuffers_[i]; }

private:
    VkRenderPass               renderPass_   = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers_;
    // One pipeline per (shader pointer) value.
    std::vector<std::pair<const VkShader*, ::VkPipeline>> pipelines_;
};

}  // namespace iron
