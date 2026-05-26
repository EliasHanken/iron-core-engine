#pragma once

#include "render/ParticleSystem.h"
#include "render/backends/vulkan/VkComputePipeline.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>

namespace iron {

class VulkanRenderer;

// Vulkan-backed iron::ParticleSystem. Owns:
//   - One device-local SSBO holding `count` particles (64 bytes each).
//   - One compute pipeline + descriptor set layout (curl-noise update).
//   - One graphics pipeline + descriptor set layout (additive billboard
//     render via SSBO vertex pull).
//   - One small host-mapped UBO buffer used for the Sim uniform fed to
//     the compute dispatch (separate from the frame ring's UBO because
//     tick() runs outside the per-frame command buffer).
class VkParticleSystem : public ParticleSystem {
public:
    VkParticleSystem();
    ~VkParticleSystem() override;

    bool init(VulkanRenderer& renderer, const ParticleSystemConfig& cfg);

    void tick(float dtSec) override;
    void render(const Mat4& view, const Mat4& projection) override;
    // Engine-internal: called from VulkanRenderer's deferred-callback drain.
    void recordRender(VkCommandBuffer cb, const Mat4& view, const Mat4& projection);
    std::uint32_t count() const override { return cfg_.count; }

private:
    bool createSsbo();
    void uploadInitialState();
    bool initCompute();
    bool initRender();

    VulkanRenderer* renderer_ = nullptr;
    ParticleSystemConfig cfg_;
    std::uint32_t frameSeed_ = 0;

    VkBuffer       ssbo_      = VK_NULL_HANDLE;
    VmaAllocation  ssboAlloc_ = VK_NULL_HANDLE;

    // Compute (Task 5).
    VkDescriptorSetLayout computeSetLayout_ = VK_NULL_HANDLE;
    VkComputePipeline     computePipeline_;
    VkBuffer              simUbo_       = VK_NULL_HANDLE;
    VmaAllocation         simUboAlloc_  = VK_NULL_HANDLE;
    void*                 simUboMapped_ = nullptr;

    // Render (Task 6).
    VkDescriptorSetLayout renderSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      renderPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            renderPipeline_       = VK_NULL_HANDLE;
};

}  // namespace iron
