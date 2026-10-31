#pragma once

#include "render/GpuParticles.h"
#include "render/backends/vulkan/VkComputePipeline.h"
#include "render/backends/vulkan/VkComputeStage.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace iron {

class VulkanRenderer;

// Vulkan-backed iron::GpuParticles. Owns one device-local SSBO plus a compute
// and a graphics pipeline sharing one descriptor set layout:
//   binding 0 = particles, binding 1 = Frame UBO, binding 2 = Params UBO.
class VkGpuParticles : public GpuParticles {
public:
    VkGpuParticles() = default;
    ~VkGpuParticles() override;

    bool init(VulkanRenderer& renderer, const GpuParticlesDesc& desc);

    void setParams(const void* params) override;
    void simulate(float dtSec) override;
    void render(const Mat4& view, const Mat4& projection) override;
    void injectLight(LightVolume& volume) override;
    void setDensityVolume(const FluidVolume* fluid, float density) override;
    void reset() override { needsClear_ = true; }
    std::uint32_t count() const override { return desc_.count; }

private:
    struct FrameUbo;

    bool createBuffer();
    bool uploadInitialData();
    bool createSetLayout();
    bool createRenderPipeline(const GpuParticleSources& sources);
    VkDescriptorSet allocateSet(const FrameUbo& frame);

    VulkanRenderer*  renderer_ = nullptr;
    GpuParticlesDesc desc_;

    VkBuffer      ssbo_      = VK_NULL_HANDLE;
    VmaAllocation ssboAlloc_ = VK_NULL_HANDLE;
    bool          needsClear_ = true;
    VkBuffer      trail_      = VK_NULL_HANDLE;
    VmaAllocation trailAlloc_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkComputePipeline     computePipeline_;
    VkPipelineLayout      renderLayout_   = VK_NULL_HANDLE;
    ::VkPipeline          renderPipeline_ = VK_NULL_HANDLE;
    VkComputeStage        injectStage_;
    bool                  hasInject_ = false;

    const FluidVolume* densityVolume_ = nullptr;
    float              densityScale_ = 0.0f;

    std::vector<unsigned char> params_;
    float         time_       = 0.0f;
    float         lastDt_     = 0.0f;
    std::uint32_t frameIndex_ = 0;
};

}  // namespace iron
