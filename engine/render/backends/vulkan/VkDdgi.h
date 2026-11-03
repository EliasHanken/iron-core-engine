#pragma once

// The DDGI probe volume (see render/GlobalIllumination.h) on Vulkan: the
// probes' rays traced with ray queries against VkRayTracing's scene, blended
// into an irradiance atlas and a distance atlas that every lit shader reads
// through the frame's set (set 1, bindings 6-8).
//
// The atlases always exist (a single probe's worth before a volume is set), so
// the lit shaders' descriptors are always valid; with no live volume they say so
// in the parameters and surfaces keep the environment's light.

#include "math/Vec.h"
#include "render/GlobalIllumination.h"
#include "render/backends/vulkan/VkComputeStage.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkVolumeImage.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>

namespace iron {

class VkContext;
class VulkanRenderer;

class VkDdgi {
public:
    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // A volume to light, or none (the atlases shrink back to one probe).
    void setVolume(VkContext& ctx, const GiVolume& volume);
    void clearVolume(VkContext& ctx);
    bool live() const { return live_; }
    void setView(GiView view) { view_ = view; }
    void setWipe(float x) { wipe_ = x; }

    // What the frame's lighting is, for the probes' rays to see by.
    struct Lighting {
        Vec3         towardSun{0.0f, 1.0f, 0.0f};
        Vec3         sunColor{1.0f, 1.0f, 1.0f};
        VkBuffer     lights = VK_NULL_HANDLE;  // VkClusters' light list.
        std::uint32_t lightCount = 0;
        VkImageView  sky = VK_NULL_HANDLE;     // A cube, for rays that miss everything.
        VkSampler    skySampler = VK_NULL_HANDLE;
    };
    // Writes this frame's parameters, and (with a live volume and a scene)
    // traces and blends the probes. Outside any render pass; ends with the
    // atlases ready for fragment shaders.
    void update(VulkanRenderer& renderer, VkCommandBuffer cb, std::uint32_t frame, VkAccelerationStructureKHR scene,
                VkBuffer instanceInfo, const Lighting& lighting);

    // For the lit shaders' set: the parameters (one buffer per frame in flight),
    // and the atlases (always in GENERAL).
    VkBuffer    params(std::uint32_t frame) const { return params_[frame].buffer; }
    VkDeviceSize paramsBytes() const;
    VkImageView irradianceView() const { return irradiance_.view(); }
    VkImageView distanceView() const { return distance_.view(); }
    VkSampler   sampler() const { return sampler_; }
    // A vec4 per probe: how far relocation moved it off its grid point.
    VkBuffer    offsets() const { return offsets_.buffer; }
    // Bumped whenever the atlases are remade, so sets that hold them are rewritten.
    std::uint32_t generation() const { return generation_; }

private:
    struct Buffer {
        VkBuffer      buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void*         mapped = nullptr;
    };
    bool makeImages(VkContext& ctx, std::uint32_t probesX, std::uint32_t probesY, std::uint32_t probesZ,
                    std::uint32_t rays);
    void freeImages(VkContext& ctx);

    GiVolume       volume_;
    bool           live_ = false;
    GiView         view_ = GiView::On;
    float          wipe_ = -1.0f;
    std::uint32_t  counts_[3] = {2, 2, 2};
    std::uint32_t  rays_ = 32;
    std::uint32_t  framesSinceReset_ = 0;
    std::uint32_t  frameNumber_ = 0;
    std::uint32_t  generation_ = 0;

    VkVolumeImage  rayData_;     // Radiance and distance, a ray per texel, a probe per row.
    VkVolumeImage  irradiance_;  // 8x8 texels per probe (6x6 inside a border).
    VkVolumeImage  distance_;    // 16x16 texels per probe: mean distance and its square.
    VkSampler      sampler_ = VK_NULL_HANDLE;
    std::array<Buffer, VkFrameRing::kFramesInFlight> params_{};
    Buffer         offsets_;  // Device-local: written by relocation, read by everything else.

    VkComputeStage trace_;
    VkComputeStage blendIrradiance_;
    VkComputeStage blendDistance_;
    VkComputeStage border_;
    VkComputeStage relocate_;
    bool           stagesReady_ = false;
};

}  // namespace iron
