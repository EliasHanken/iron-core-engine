#pragma once

#include "render/LightVolume.h"
#include "render/backends/vulkan/VkComputeStage.h"
#include "render/backends/vulkan/VkVolumeImage.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace iron {

class VulkanRenderer;

// Particles can't blend into an image from compute, so injection adds to a
// fixed-point SSBO with atomics. resolve() turns that into a blurred RGBA16F
// 3D image.
class VkLightVolume : public LightVolume {
public:
    VkLightVolume() = default;
    ~VkLightVolume() override;

    bool init(VulkanRenderer& renderer, const LightVolumeDesc& desc);

    void beginInject() override;
    void resolve() override;
    const VolumeBounds& bounds() const override { return desc_.bounds; }
    VolumeSize size() const override { return desc_.size; }

    VkBuffer     accumulationBuffer() const { return accumulation_; }
    VkImageView  resolvedView() const { return images_[resolvedIndex_].view(); }

    // Fixed-point scale of the accumulation buffer, shared with injection shaders.
    static constexpr float kFixedPointScale = 1024.0f;

private:
    VulkanRenderer* renderer_ = nullptr;
    LightVolumeDesc desc_;

    VkBuffer      accumulation_ = VK_NULL_HANDLE;
    VmaAllocation accumulationAlloc_ = VK_NULL_HANDLE;

    VkVolumeImage images_[2];
    int           resolvedIndex_ = 0;

    VkComputeStage resolveStage_;
    VkComputeStage blurStage_;
};

}  // namespace iron
