#pragma once

#include "math/Mat4.h"
#include "render/Culling.h"
#include "render/InstanceGroup.h"
#include "render/backends/vulkan/VkFrameRing.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace iron {

class VkContext;
class VkMeshStore;

// GPU-driven drawing for instance groups. Each frame, before anything draws, one
// compute pass per group tests every instance against the view frustum and against
// a depth pyramid of the last frame (occlusion), picks its level of detail from its
// size on screen, and appends it to that level's visible list, counting it into an
// indirect draw. So a group is one draw per level, however many instances it has,
// and the CPU never looks at an instance.
//
// Occlusion uses last frame's depth from last frame's camera, so something that
// steps out from behind a wall can appear one frame late. Anything that was off
// screen last frame counts as visible, which covers turning.
//
// The sun's shadow map is a second view with lists of its own: culled against the
// light's frustum instead, one level coarser, and without anything too small on
// screen for its shadow to matter.
class VkInstanceGroups {
public:
    bool init(VkContext& ctx, VkFrameRing& frames);
    void destroy(VkContext& ctx);

    InstanceGroupHandle create(VkContext& ctx, const InstanceGroupDesc& desc, const VkMeshStore& meshes);
    void setInstances(VkContext& ctx, InstanceGroupHandle group, std::span<const Mat4> transforms);
    // Every instance of every group that casts shadows, with its finest mesh: what
    // blocks light, for ray tracing.
    void forEachOccluder(const std::function<void(MeshHandle, const Mat4&)>& visit) const;
    bool empty() const { return groups_.empty(); }

    // Freezes the camera culling sees, so that walking away shows what it skipped.
    void setFrozen(bool frozen) { frozenRequest_ = frozen; }

    // Culls every group for this frame. Record outside any render pass, before the
    // passes that draw groups. `pixelsPerUnit` is half the screen height times the
    // projection's y scale.
    // `lightViews` are the sun's shadow views, one per cascade (up to kShadowViews);
    // each culls its own list, so a cascade draws only what falls in it.
    // Shadow views whose bit is clear in `shadowMask` keep last frame's lists.
    // Without `occlusion` nothing is tested against the depth pyramid, and the
    // pyramid isn't rebuilt (its depth would be another camera's).
    void cull(VkCommandBuffer cb, int frame, const Mat4& viewProjection, std::span<const Mat4> lightViews,
              Vec3 eye, float pixelsPerUnit, VkExtent2D sceneExtent, std::uint32_t shadowMask = ~0u,
              bool occlusion = true);

    // Builds the depth pyramid from this frame's scene depth, for next frame's
    // occlusion test. Record after the opaque pass, outside any render pass.
    void buildDepthPyramid(VkContext& ctx, VkCommandBuffer cb, VkImageView sceneDepth);

    struct Draw {
        const InstanceGroupDesc* desc;
        std::uint32_t  lod;
        VkBuffer       vertexBuffer;
        VkBuffer       indexBuffer;
        VkBuffer       instances;
        VkBuffer       visible;
        VkBuffer       indirect;
        VkDeviceSize   indirectOffset;
    };
    // Every group's draws for this frame, one per level of detail.
    void forEachDraw(int frame, const VkMeshStore& meshes, const std::function<void(const Draw&)>& fn) const;

    // A shadow view's draws for this frame: what its culling kept of each
    // shadow-casting group.
    std::vector<Draw> shadowDraws(int frame, const VkMeshStore& meshes, std::size_t shadowView = 0) const;

    static constexpr std::size_t kShadowViews = 4;

    // What culling did, a couple of frames ago.
    const CullStats& stats() const { return stats_; }

private:
    static constexpr std::uint32_t kStatWords = 9;  // drawn, outside, hidden, tiny, lod 0..3, total
    static constexpr std::size_t kViews = 1 + kShadowViews;  // The camera, then the sun's views.

    struct Buffer {
        VkBuffer      buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void*         mapped = nullptr;
        VkDeviceSize  size = 0;
    };

    struct Group {
        InstanceGroupDesc desc;
        std::array<std::uint32_t, 4> indexCounts{};
        float         radius = 0.0f;       // Of the bounds, in the mesh's space.
        Vec3          center{};
        std::uint32_t count = 0;
        Buffer        instances;           // Written by the CPU when the instances change.
        // Per view, per frame in flight.
        std::array<std::array<Buffer, VkFrameRing::kFramesInFlight>, kViews> visible;
        std::array<std::array<Buffer, VkFrameRing::kFramesInFlight>, kViews> indirect;
    };

    Buffer makeBuffer(VkContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void retire(Buffer& b);
    bool createPipelines(VkContext& ctx);
    bool createPyramid(VkContext& ctx, VkExtent2D sceneExtent);
    void destroyPyramid(VkContext& ctx);

    VkContext*   ctx_ = nullptr;
    VkFrameRing* frames_ = nullptr;
    std::vector<Group> groups_;  // Handle h is groups_[h - 1].

    // Buffers that may still be in use by a frame in flight, and the frame they were left in.
    struct Retired { Buffer buffer; std::uint64_t frame; };
    std::vector<Retired> retired_;
    std::uint64_t frameCount_ = 0;

    std::array<Buffer, VkFrameRing::kFramesInFlight> statBuffers_;
    std::array<bool, VkFrameRing::kFramesInFlight> statsWritten_{};
    CullStats stats_;

    VkDescriptorSetLayout cullSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      cullLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          cullPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout pyramidSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pyramidLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          pyramidPipeline_ = VK_NULL_HANDLE;
    VkSampler             nearestSampler_ = VK_NULL_HANDLE;

    // The depth pyramid: each level the farthest depth of four below it.
    VkImage                  pyramid_ = VK_NULL_HANDLE;
    VmaAllocation            pyramidAlloc_ = VK_NULL_HANDLE;
    VkImageView              pyramidView_ = VK_NULL_HANDLE;  // All levels, for sampling.
    std::vector<VkImageView> pyramidLevels_;                 // One each, for writing.
    VkExtent2D               pyramidExtent_{};
    VkExtent2D               pyramidSource_{};
    bool                     pyramidValid_ = false;          // Holds a frame's depth.
    bool                     pyramidLayoutReady_ = false;
    Mat4                     pyramidViewProjection_ = Mat4::identity();

    // The camera the pyramid was built from, and the one culling uses.
    Mat4 lastViewProjection_ = Mat4::identity();
    bool occlusion_ = true;  // This frame's culling tests occlusion.
    bool frozenRequest_ = false;
    bool frozen_ = false;
    Mat4 frozenViewProjection_ = Mat4::identity();
    Vec3 frozenEye_{};
};

}  // namespace iron
