#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>

namespace iron {

class VkContext;

// Temporal anti-aliasing. The camera's projection is jittered by a sub-pixel offset
// every frame, so over a few frames each pixel sees several points of the geometry.
// This pass blends the new frame with the accumulated history, reprojected along the
// motion vectors:
//   - the history is sampled with a 5-tap Catmull-Rom filter, so it stays sharp;
//   - it's clipped to the spread of the new frame's 3x3 neighbourhood (in YCoCg), so
//     what's no longer there can't ghost;
//   - colours are blended after a reversible tonemap, so bright speculars don't flicker;
//   - the motion vector comes from the nearest of the 3x3, so edges move with the
//     object in front.
// Particles and fog write no motion vectors, so the pass also compares the frame with
// a copy taken before they were drawn: wherever they changed it, the new frame counts
// for more (a reactive mask, made without touching their pipelines).
//
// The result is copied back into the scene image, so everything after it reads the
// anti-aliased frame without knowing this pass exists.
class VkTemporalAA {
public:
    bool init(VkContext& ctx, VkFormat colorFormat);
    void destroy(VkContext& ctx);

    bool createTargets(VkContext& ctx, VkExtent2D extent, VkImage sceneColor, VkImageView sceneColorView,
                       VkImageView depthView, VkImageView velocityView);
    void destroyTargets(VkContext& ctx);

    // After the opaque pass: keeps a copy of the frame before transparents.
    void captureOpaque(VkCommandBuffer cb) const;
    // After the transparent pass: blends, then writes the result into the scene image.
    void resolve(VkCommandBuffer cb);

    // Drops the history, for a cut or a jump; the next frame starts fresh.
    void reset() { reset_ = true; }

private:
    struct Image {
        VkImage       image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView   view = VK_NULL_HANDLE;
    };

    Image makeImage(VkContext& ctx, VkImageUsageFlags usage) const;
    void  freeImage(VkContext& ctx, Image& image) const;

    VkFormat   format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkImage    sceneColor_ = VK_NULL_HANDLE;
    std::array<Image, 2>         history_;
    std::array<VkFramebuffer, 2> framebuffers_{};
    std::array<VkDescriptorSet, 2> sets_{};  // sets_[i] writes history_[i] and reads the other.
    Image opaque_;
    int   current_ = 0;
    bool  reset_ = true;

    VkRenderPass          pass_ = VK_NULL_HANDLE;
    VkSampler             linear_ = VK_NULL_HANDLE;
    VkSampler             nearest_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkPipelineLayout      layout_ = VK_NULL_HANDLE;
    ::VkPipeline          pipeline_ = VK_NULL_HANDLE;
};

}  // namespace iron
