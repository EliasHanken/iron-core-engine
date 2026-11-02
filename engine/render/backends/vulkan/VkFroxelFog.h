#pragma once

#include "math/Mat4.h"
#include "render/ShadowCascades.h"
#include "render/VolumeBounds.h"
#include "render/VolumetricFog.h"
#include "render/backends/vulkan/VkComputeStage.h"
#include "render/backends/vulkan/VkVolumeImage.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <span>
#include <vector>

namespace iron {

class VkContext;
class VulkanRenderer;

// Froxel volumetric fog, as Frostbite and Unreal do it. A grid of cells fitted to
// the view (160 x 90 tiles by 64 slices spaced logarithmically in depth) is:
//   1. filled and lit: density from height fog, drifting noise, fog boxes,
//      fluids' smoke and disturbances (wakes and blasts pushing it aside, smoke);
//      light from the sun through the cascaded shadows, the clustered point and
//      spot lights, and ambient; then blended with last frame's grid, reprojected,
//      over a jittered sample point, so it's smooth without many samples;
//   2. integrated front to back into what each ray has gathered and how much
//      light still gets through;
//   3. applied over the opaque scene by a blend pass: colour * transmittance +
//      in-scattered light.
// Fluids' smoke is too fine for the grid: a pass marches it per pixel through
// the fluids' own density, lit from a second grid the light pass fills (the
// light there, as smoke scatters it), and the apply pass lays it over the fog.
class VkFroxelFog {
public:
    static constexpr std::uint32_t kX = 160;
    static constexpr std::uint32_t kY = 90;
    static constexpr std::uint32_t kZ = 64;
    static constexpr float kNear = 0.4f;
    static constexpr std::uint32_t kMaxVolumes = 32;  // The shader's Shapes block holds this many boxes.
    static constexpr std::uint32_t kMaxDisturbances = 128;

    bool init(VkContext& ctx, VkFormat sceneFormat);
    void destroy(VkContext& ctx);
    // The scene image it blends into, and the depth it reads.
    bool createTargets(VkContext& ctx, VkExtent2D extent, VkImageView sceneColor, VkImageView sceneDepth);
    void destroyTargets(VkContext& ctx);

    struct Frame {
        VolumetricFog fog;
        std::span<const FogVolume> volumes;
        std::span<const FogDisturbance> disturbances;
        // Fluids' smoke: each one's density field (R) in VK_IMAGE_LAYOUT_GENERAL.
        struct Fluid {
            VkImageView  density = VK_NULL_HANDLE;
            VolumeBounds bounds{};
            float        scale = 1.0f;
            float        albedo = 0.6f;
        };
        std::array<Fluid, 2> fluids{};
        std::size_t fluidCount = 0;
        Mat4 view;
        Mat4 projection;          // Unjittered.
        Mat4 previousViewProjection;  // Unjittered, for reprojecting last frame's grid.
        bool hasPrevious = false;
        Vec3 cameraPos;
        Vec3 sunDirection;        // The way the light travels.
        Vec3 sunColor;
        const ShadowCascades* cascades = nullptr;  // Null: the sun casts no shadow in the fog.
        float casterReach = 60.0f;
        VkImageView shadowAtlas = VK_NULL_HANDLE;
        VkSampler   shadowSampler = VK_NULL_HANDLE;
        // The clusters' buffers for this frame: lights reach the fog through them.
        VkBuffer clusterParams = VK_NULL_HANDLE, clusterLights = VK_NULL_HANDLE, clusterCells = VK_NULL_HANDLE,
                 clusterItems = VK_NULL_HANDLE;
        float time = 0.0f;
        std::uint32_t frameIndex = 0;
        float depthA = 0.0f, depthB = 0.0f;  // projection(2,2) and (2,3), to linearise depth.
    };
    // Fills, lights and integrates the grid. Record outside any render pass, after
    // the shadow pass and the cluster build, before apply().
    void record(VulkanRenderer& renderer, VkCommandBuffer cb, const Frame& frame);
    // Blends the scene through the fog. Record outside any render pass, after the
    // opaque pass, while the scene image is SHADER_READ_ONLY_OPTIMAL.
    void apply(VkCommandBuffer cb, const Frame& frame);

private:
    // Marches fluids' smoke per pixel into smoke_, after the grid is integrated.
    void marchSmoke(VulkanRenderer& renderer, VkCommandBuffer cb, const Frame& f, std::size_t slot, VkImageView spare);
    bool smokeThisFrame_ = false;

    struct Buffer {
        VkBuffer      buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void*         mapped = nullptr;
    };

    VkComputeStage lightStage_;
    VkComputeStage integrateStage_;
    std::array<VkVolumeImage, 2> scattering_;  // Ping-pong: this frame's, and last frame's to reproject.
    VkVolumeImage integrated_;
    VkVolumeImage smokeLight_;   // The light at each cell, as smoke scatters it.
    VkComputeStage smokeStage_;
    VkVolumeImage smoke_;        // Per pixel: smoke's light (rgb) and what it lets through (a).
    bool smokeReady_ = false;    // In VK_IMAGE_LAYOUT_GENERAL, so the apply set can always read it.
    int current_ = 0;
    bool historyValid_ = false;
    std::array<Buffer, 2> params_;   // Per frame in flight: the uniform block.
    std::array<Buffer, 2> volumes_;  // Per frame in flight: the fog boxes, then the disturbances.
    VkSampler linear_ = VK_NULL_HANDLE;
    VkSampler nearest_ = VK_NULL_HANDLE;

    // The apply pass.
    VkFormat              sceneFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D            extent_{};
    VkRenderPass          applyPass_ = VK_NULL_HANDLE;
    VkFramebuffer         applyFramebuffer_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout applySetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      applyPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       applySet_ = VK_NULL_HANDLE;
    VkPipelineLayout      applyLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          applyPipeline_ = VK_NULL_HANDLE;
    VkImageView           sceneDepth_ = VK_NULL_HANDLE;
};

}  // namespace iron
