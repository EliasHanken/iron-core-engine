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
    std::string occluders;
    std::string upsampleVertex;
    std::string upsampleFragment;
};
VolumeRendererSources volumeRendererSources(const VolumeRendererDesc& desc = {});

class VkVolumeRenderer : public VolumeRenderer {
public:
    VkVolumeRenderer() = default;
    ~VkVolumeRenderer() override;

    bool init(VulkanRenderer& renderer, const VolumeRendererDesc& desc);

    void render(const Mat4& view, const Mat4& projection, const LightVolume& light,
                const FluidVolume* fluid, const VolumeRenderSettings& settings) override;
    void setOccluders(Vec3 light, std::span<const Vec4> spheres, float strength,
                      std::span<const Vec3> columns) override;

private:
    bool createUpsamplePipeline();
    bool ensureTarget();

    VulkanRenderer* renderer_ = nullptr;

    VkVolumeImage target_;
    VkExtent2D    targetExtent_{};

    VkComputeStage raymarchStage_;
    VkComputeStage occluderStage_;
    VkVolumeImage  shadowMap_;
    bool           shadowsOn_ = false;

    VkDescriptorSetLayout upsampleSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      upsampleLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          upsamplePipeline_ = VK_NULL_HANDLE;

    std::uint32_t frameIndex_ = 0;
};

}  // namespace iron
