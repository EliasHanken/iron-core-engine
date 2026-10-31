#pragma once

#include "render/VolumeRenderer.h"
#include "render/backends/vulkan/VkComputeStage.h"
#include "render/backends/vulkan/VkVolumeImage.h"

#include <vulkan/vulkan.h>

#include <string>

namespace iron {

class VulkanRenderer;

// The GLSL for the raymarch and upsample, exposed for compile-checking.
struct VolumeRendererSources {
    std::string raymarch;
    std::string upsampleVertex;
    std::string upsampleFragment;
};
VolumeRendererSources volumeRendererSources();

class VkVolumeRenderer : public VolumeRenderer {
public:
    VkVolumeRenderer() = default;
    ~VkVolumeRenderer() override;

    bool init(VulkanRenderer& renderer);

    void render(const Mat4& view, const Mat4& projection, const LightVolume& light,
                const FluidVolume* fluid, const VolumeRenderSettings& settings) override;

private:
    bool createUpsamplePipeline();
    bool ensureTarget();

    VulkanRenderer* renderer_ = nullptr;

    VkVolumeImage target_;
    VkExtent2D    targetExtent_{};

    VkComputeStage raymarchStage_;

    VkDescriptorSetLayout upsampleSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      upsampleLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          upsamplePipeline_ = VK_NULL_HANDLE;

    VkSampler     linearSampler_ = VK_NULL_HANDLE;
    std::uint32_t frameIndex_ = 0;
};

}  // namespace iron
