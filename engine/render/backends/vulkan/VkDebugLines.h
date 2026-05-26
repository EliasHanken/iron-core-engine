#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkDebugLines is Vulkan-only."
#endif

#include "math/Mat4.h"
#include "math/Vec.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace iron {

class VkContext;
class VkFrameRing;

// Vulkan implementation of debug-line drawing. queue() accumulates line
// segments during the frame; record() uploads to the frame's vertex
// sub-allocator, binds the pipeline + descriptor set, draws, and clears
// the queue. Recorded inside the active scene render pass between the
// scene geometry and the HUD.
class VkDebugLines {
public:
    bool init(VkContext& ctx, VkRenderPass scenePass);
    void destroy(VkContext& ctx);

    void queue(Vec3 a, Vec3 b, Vec3 color);
    // device is the Vulkan device handle (for vkAllocateDescriptorSets +
    // vkUpdateDescriptorSets). Pass VulkanRenderer::context_.device().
    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                const Mat4& view, const Mat4& projection);

private:
    struct Vertex { Vec3 position; Vec3 color; };
    struct CameraUbo { float viewProjection[16]; };

    bool ok_ = false;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::vector<Vertex> queued_;
};

}  // namespace iron
