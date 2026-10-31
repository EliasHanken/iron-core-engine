#include "render/backends/vulkan/VkVolumeImage.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

namespace iron {

bool VkVolumeImage::create(VkContext& ctx, VolumeSize size, VkFormat format,
                           VkImageUsageFlags usage) {
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_3D;
    info.format        = format;
    info.extent        = {size.x, size.y, size.z};
    info.mipLevels     = 1;
    info.arrayLayers   = 1;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.usage         = usage;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &alloc, &image_, &alloc_, nullptr));

    VkImageViewCreateInfo view{};
    view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image            = image_;
    view.viewType         = VK_IMAGE_VIEW_TYPE_3D;
    view.format           = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device(), &view, nullptr, &view_));

    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return view_ != VK_NULL_HANDLE;
}

void VkVolumeImage::destroy(VkContext& ctx) {
    if (view_) vkDestroyImageView(ctx.device(), view_, nullptr);
    if (image_) vmaDestroyImage(ctx.allocator(), image_, alloc_);
    view_  = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    alloc_ = VK_NULL_HANDLE;
    layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VkVolumeImage::transition(VkCommandBuffer cb, VkImageLayout newLayout,
                               VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask       = dstAccess;
    barrier.oldLayout           = layout_;
    barrier.newLayout           = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image_;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                         dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    layout_ = newLayout;
}

}  // namespace iron
