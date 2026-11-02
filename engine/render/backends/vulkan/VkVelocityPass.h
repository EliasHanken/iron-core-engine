#pragma once

#include "math/Mat4.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <span>

namespace iron {

class VkContext;

// One moving object for the velocity pass: its geometry, and where it was drawn
// this frame and last (each a full model-view-projection).
struct VelocityObject {
    VkBuffer      vertexBuffer = VK_NULL_HANDLE;
    VkBuffer      indexBuffer = VK_NULL_HANDLE;
    std::uint32_t indexCount = 0;
    Mat4          current = Mat4::identity();
    Mat4          previous = Mat4::identity();
    // A skinned object's bones, staged with stageBones; -1 for a rigid one. Its
    // vertex buffer holds SkinnedVertex.
    int           boneSlot = -1;
};

// The motion vector of every scene pixel: how far it moved on screen since the
// last frame, in UV units (this frame's UV minus last frame's). Runs after the
// opaque pass and touches none of its pipelines. A full-screen pass first works
// out the camera's motion from depth, which is right for everything that stands
// still; then the moving objects are drawn over it with their own two transforms,
// depth-tested against the scene so only their visible pixels change. This is
// what temporal anti-aliasing and per-object motion blur read.
class VkVelocityPass {
public:
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16_SFLOAT;

    bool init(VkContext& ctx, VkFormat depthFormat);
    void destroy(VkContext& ctx);

    // (Re)creates the target at `extent`, reading `depthView` (the scene's depth,
    // left in DEPTH_STENCIL_READ_ONLY_OPTIMAL by the opaque pass).
    bool createTargets(VkContext& ctx, VkExtent2D extent, VkImageView depthView);
    void destroyTargets(VkContext& ctx);

    // Copies a skinned object's bone palettes, this frame's and last frame's, for
    // `frame` of the frame ring. Returns the slot to put in its VelocityObject, or
    // -1 when this frame has no room left (it then moves with the camera only).
    int stageBones(int frame, std::span<const Mat4> current, std::span<const Mat4> previous);

    // `reprojection` maps this frame's clip space to last frame's for the camera
    // (previous view-projection times the inverse of this one).
    void record(VkCommandBuffer cb, int frame, const Mat4& reprojection,
                std::span<const VelocityObject> objects);

    VkImageView view() const { return view_; }

private:
    bool createPipelines(VkContext& ctx);

    VkExtent2D            extent_{};
    VkFormat              depthFormat_ = VK_FORMAT_UNDEFINED;
    VkImage               image_ = VK_NULL_HANDLE;
    VmaAllocation         alloc_ = VK_NULL_HANDLE;
    VkImageView           view_ = VK_NULL_HANDLE;
    VkFramebuffer         framebuffer_ = VK_NULL_HANDLE;
    VkRenderPass          pass_ = VK_NULL_HANDLE;
    VkSampler             depthSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       set_ = VK_NULL_HANDLE;
    VkPipelineLayout      cameraLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          cameraPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout      objectLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          objectPipeline_ = VK_NULL_HANDLE;

    // Skinned objects: both palettes per slot, in a mapped buffer per frame.
    static constexpr int kBoneSlots = 16;
    static constexpr int kBonesPerPalette = 128;
    struct BoneBuffer {
        VkBuffer      buffer = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        void*         mapped = nullptr;
        int           used = 0;
    };
    std::array<BoneBuffer, 2>      boneBuffers_{};
    std::array<VkDescriptorSet, 2> boneSets_{};
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      bonePool_ = VK_NULL_HANDLE;
    VkPipelineLayout      skinnedLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          skinnedPipeline_ = VK_NULL_HANDLE;
};

}  // namespace iron
