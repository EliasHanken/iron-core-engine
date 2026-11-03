#pragma once

#include "math/Mat4.h"
#include "render/Decal.h"
#include "render/Light.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <span>
#include <string>
#include <vector>

namespace iron {

class VkContext;

// Clustered forward lighting and decals. The view is cut into a grid of clusters
// (screen tiles by depth slices that grow with distance); a compute pass lists,
// for each cluster, the lights and decals that reach it, and lit shaders read the
// list for the cluster a pixel falls in. Shading cost follows the lights that
// actually touch a pixel, not every light in the level, and decals ride the same
// lists (as in DOOM 2016).
//
// Everything a lit shader reads from here is in one descriptor set, set 1 of every
// lit pipeline, bound once per draw beside the draw's own set 0.
class VkClusters {
public:
    static constexpr std::uint32_t kGridX = 16;
    static constexpr std::uint32_t kGridY = 9;
    static constexpr std::uint32_t kGridZ = 24;
    static constexpr std::uint32_t kClusters = kGridX * kGridY * kGridZ;
    static constexpr std::uint32_t kMaxLights = 1024;
    static constexpr std::uint32_t kMaxDecals = 1024;
    static constexpr std::uint32_t kLightsPerCluster = 64;
    static constexpr std::uint32_t kDecalsPerCluster = 32;
    static constexpr float kNear = 0.1f;
    static constexpr float kFar = 200.0f;

    bool init(VkContext& ctx, int framesInFlight);
    void destroy(VkContext& ctx);

    // The GI volume's parameters (one buffer per frame in flight) and atlases, for
    // set 1's bindings 6-8. Call before the first frame, and whenever they're remade.
    void setGi(VkContext& ctx, std::span<const VkBuffer> params, VkDeviceSize paramsBytes, VkImageView irradiance,
               VkImageView distance, VkSampler sampler, VkBuffer offsets);

    // The layout lit pipelines take as set 1.
    VkDescriptorSetLayout setLayout() const { return setLayout_; }

    // Loads the decal images as layers of one texture array, in order: a decal's
    // `layer` indexes this list. They should share a size; others are resized.
    bool setDecalTextures(VkContext& ctx, std::span<const std::string> paths);

    // This frame's lights and decals, and the camera they're clustered for. Writes
    // the frame's buffers; record() then builds the lists on the GPU.
    void upload(int frame, std::span<const PointLight> lights, std::span<const Decal> decals, const Mat4& view,
                const Mat4& projection, VkExtent2D screen);
    // Builds the lists. Record outside any render pass, before the lit passes.
    void record(VkCommandBuffer cb, int frame);

    // Set 1 for this frame's lit draws.
    VkDescriptorSet set(int frame) const { return frames_[static_cast<std::size_t>(frame)].set; }
    // The frame's buffers, for compute passes that walk the lists themselves.
    VkBuffer paramsBuffer(int frame) const { return frames_[static_cast<std::size_t>(frame)].params.buffer; }
    VkBuffer lightsBuffer(int frame) const { return frames_[static_cast<std::size_t>(frame)].lights.buffer; }
    VkBuffer cellsBuffer(int frame) const { return frames_[static_cast<std::size_t>(frame)].cells.buffer; }
    VkBuffer itemsBuffer(int frame) const { return frames_[static_cast<std::size_t>(frame)].items.buffer; }
    bool active() const { return pipeline_ != VK_NULL_HANDLE; }

private:
    struct Buffer {
        VkBuffer      buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void*         mapped = nullptr;
    };
    struct Frame {
        Buffer          params;   // Uniform: the camera and the grid.
        Buffer          lights;   // Storage, written by the CPU.
        Buffer          decals;   // Storage, written by the CPU.
        Buffer          cells;    // Storage, written by the build: offset and counts per cluster.
        Buffer          items;    // Storage, written by the build: light and decal indices.
        VkDescriptorSet set = VK_NULL_HANDLE;       // Set 1 for lit draws.
        VkDescriptorSet buildSet = VK_NULL_HANDLE;  // The build's.
        std::uint32_t   lightCount = 0;
        std::uint32_t   decalCount = 0;
    };

    Buffer makeBuffer(VkContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible) const;
    void   writeSets(VkContext& ctx);
    void   destroyDecalImage(VkContext& ctx);
    bool   makeDecalImage(VkContext& ctx, std::uint32_t width, std::uint32_t height, std::uint32_t layers,
                          const std::vector<std::vector<unsigned char>>& pixels);

    std::vector<Frame>    frames_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout buildSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkPipelineLayout      buildLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          pipeline_ = VK_NULL_HANDLE;

    VkImage       decalImage_ = VK_NULL_HANDLE;
    VmaAllocation decalAlloc_ = VK_NULL_HANDLE;
    VkImageView   decalView_ = VK_NULL_HANDLE;
    VkSampler     decalSampler_ = VK_NULL_HANDLE;
};

}  // namespace iron
