#pragma once

#include "render/FluidVolume.h"
#include "render/backends/vulkan/VkComputeStage.h"
#include "render/backends/vulkan/VkVolumeImage.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace iron {

class VulkanRenderer;

// Semi-Lagrangian solver with vorticity confinement and Jacobi pressure, with
// MacCormack advection for density and heat. Every grid stays in
// VK_IMAGE_LAYOUT_GENERAL, so a stage only needs a memory barrier.
class VkFluidVolume : public FluidVolume {
public:
    VkFluidVolume() = default;
    ~VkFluidVolume() override;

    bool init(VulkanRenderer& renderer, const FluidVolumeDesc& desc);

    void setParams(const void* params) override;
    void setDissipation(float densityPerSecond) override { dissipation_ = densityPerSecond; }
    void step() override;
    void reset() override { needsClear_ = true; }
    const VolumeBounds& bounds() const override { return desc_.bounds; }

    // R = density, G = heat. In VK_IMAGE_LAYOUT_GENERAL.
    VkImageView scalarsView() const { return scalars_[0].view(); }
    VkImageView solidView() const { return solid_.view(); }

private:
    struct Push;

    VkDeviceSize uploadParams() const;
    void clearAll(VkCommandBuffer cb);

    VulkanRenderer* renderer_ = nullptr;
    FluidVolumeDesc desc_;

    VkVolumeImage velocity_[2];
    VkVolumeImage scalars_[3];
    VkVolumeImage pressure_[2];
    VkVolumeImage divergence_;
    VkVolumeImage curl_;
    VkVolumeImage solid_;

    VkComputeStage obstacleStage_;
    VkComputeStage sourceStage_;
    VkComputeStage advectVelocityStage_;
    VkComputeStage curlStage_;
    VkComputeStage confineStage_;
    VkComputeStage divergenceStage_;
    VkComputeStage jacobiStage_;
    VkComputeStage projectStage_;
    VkComputeStage advectForwardStage_;
    VkComputeStage advectCorrectStage_;

    std::vector<unsigned char> params_;
    float         dissipation_ = 0.0f;
    float         time_ = 0.0f;
    std::uint32_t stepIndex_ = 0;
    bool          needsClear_ = true;
};

}  // namespace iron
