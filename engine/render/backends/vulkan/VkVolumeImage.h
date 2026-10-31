#pragma once

#include "render/VolumeBounds.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace iron {

class VkContext;

// One 3D image and its view, remembering its layout so that a chain of compute
// stages can transition it without tracking state by hand.
class VkVolumeImage {
public:
    bool create(VkContext& ctx, VolumeSize size, VkFormat format, VkImageUsageFlags usage);
    void destroy(VkContext& ctx);

    VkImage     image() const { return image_; }
    VkImageView view() const { return view_; }

    // Records a barrier into `newLayout`, waiting on every prior compute write.
    void transition(VkCommandBuffer cb, VkImageLayout newLayout, VkPipelineStageFlags dstStage,
                    VkAccessFlags dstAccess);

    // Marks the image as discardable, so the next transition may drop its
    // contents. Use before a stage that rewrites every voxel.
    void discard() { layout_ = VK_IMAGE_LAYOUT_UNDEFINED; }

private:
    VkImage       image_ = VK_NULL_HANDLE;
    VkImageView   view_  = VK_NULL_HANDLE;
    VmaAllocation alloc_ = VK_NULL_HANDLE;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
};

}  // namespace iron
