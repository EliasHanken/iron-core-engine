#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkShadowMap is Vulkan-only."
#endif

#include "math/Mat4.h"
#include "render/Renderer.h"
#include "render/backends/vulkan/VkInstanceGroups.h"

#include <span>

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
    // The map is an atlas: one view fills it whole; cascades take a quarter each.
    static constexpr int kResolution = 4096;
    static constexpr int kTile = kResolution / 2;

    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // Records the entire shadow pass into the active command buffer:
    // begin depth-only render pass, replay `draws` with the depth-only
    // pipeline, end render pass. The render pass's final layout is
    // SHADER_READ_ONLY_OPTIMAL so subsequent passes can sample.
    // `instanced` are instance groups as the sun's culling left them: indirect draws.
    // `views` is one light view-projection (the whole map) or one per cascade
    // (a 2x2 atlas: cascade i in column i % 2, row i / 2). `instanced[i]` are the
    // instance groups as view i's culling left them: indirect draws.
    // Only the views whose bit is set in `redraw` are cleared and drawn; the others
    // keep what they held, so far cascades can be refreshed every other frame.
    void record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                VkMeshStore& meshes,
                std::span<const Mat4> views,
                const std::vector<DrawCall>& draws,
                std::span<const std::vector<VkInstanceGroups::Draw>> instanced = {},
                std::uint32_t redraw = ~0u);

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
    VkDescriptorSetLayout instancedSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout instancedLayout_ = VK_NULL_HANDLE;
    ::VkPipeline    instancedPipeline_ = VK_NULL_HANDLE;
    VkSampler       sampler_        = VK_NULL_HANDLE;
};

}  // namespace iron
