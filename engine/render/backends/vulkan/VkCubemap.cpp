// VkCubemap.cpp — Vulkan cubemap image storage (6 array layers,
// VK_IMAGE_VIEW_TYPE_CUBE) with shared sampler and built-in 1x1x6
// black fallback.

#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>
#include <vector>

namespace iron {

bool VkCubemapStore::init(VkContext& ctx) {
    // Shared sampler.
    VkSamplerCreateInfo sInfo{};
    sInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sInfo.magFilter = VK_FILTER_LINEAR;
    sInfo.minFilter = VK_FILTER_LINEAR;
    sInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sInfo.minLod = 0.0f;
    sInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(ctx.device(), &sInfo, nullptr, &sharedSampler_));

    // 1x1x6 black fallback.
    const unsigned char blackPixel[4] = {0, 0, 0, 255};
    std::array<const unsigned char*, 6> blackFaces{
        blackPixel, blackPixel, blackPixel,
        blackPixel, blackPixel, blackPixel,
    };
    black_ = createFromFaces(ctx, 1, 1, blackFaces);
    if (black_ == kInvalidHandle) {
        Log::error("VkCubemapStore: black fallback creation failed");
        return false;
    }
    return true;
}

void VkCubemapStore::destroyAll(VkContext& ctx) {
    for (auto& [h, res] : cubemaps_) {
        if (res.view)  { vkDestroyImageView(ctx.device(), res.view, nullptr); }
        if (res.image) { vmaDestroyImage(ctx.allocator(), res.image, res.alloc); }
    }
    cubemaps_.clear();
    if (sharedSampler_) {
        vkDestroySampler(ctx.device(), sharedSampler_, nullptr);
        sharedSampler_ = VK_NULL_HANDLE;
    }
    black_ = kInvalidHandle;
}

CubemapHandle VkCubemapStore::createFromFaces(
        VkContext& ctx, int width, int height,
        const std::array<const unsigned char*, 6>& faces) {
    if (width <= 0 || height <= 0) return kInvalidHandle;
    for (int i = 0; i < 6; ++i) {
        if (faces[i] == nullptr) return kInvalidHandle;
    }
    if (sharedSampler_ == VK_NULL_HANDLE) return kInvalidHandle;

    VkCubemapResource res{};
    res.width  = static_cast<std::uint32_t>(width);
    res.height = static_cast<std::uint32_t>(height);
    res.sampler = sharedSampler_;

    // Cube-compatible image.
    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imgInfo.extent = {res.width, res.height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 6;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aInfo{};
    aInfo.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &imgInfo, &aInfo,
                            &res.image, &res.alloc, nullptr));

    // Cube image view.
    VkImageViewCreateInfo vInfo{};
    vInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vInfo.image = res.image;
    vInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vInfo.subresourceRange.baseMipLevel = 0;
    vInfo.subresourceRange.levelCount = 1;
    vInfo.subresourceRange.baseArrayLayer = 0;
    vInfo.subresourceRange.layerCount = 6;
    VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &res.view));

    uploadFaces(ctx, res, width, height, faces);

    const CubemapHandle h = nextHandle_++;
    cubemaps_[h] = res;
    return h;
}

const VkCubemapResource& VkCubemapStore::get(CubemapHandle h) const {
    auto it = cubemaps_.find(h);
    return it->second;  // caller-checked via has()
}

void VkCubemapStore::uploadFaces(VkContext& ctx, VkCubemapResource& res,
                                 int width, int height,
                                 const std::array<const unsigned char*, 6>& faces) {
    const std::size_t faceBytes = static_cast<std::size_t>(width) * height * 4;
    const std::size_t totalBytes = faceBytes * 6;

    // Staging buffer holding all 6 faces concatenated.
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = totalBytes;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocInfo{};
    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VmaAllocation stagingAllocHandle = VK_NULL_HANDLE;
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bufInfo, &stagingAlloc,
                             &stagingBuf, &stagingAllocHandle, &allocInfo));

    auto* mapped = static_cast<unsigned char*>(allocInfo.pMappedData);
    for (int i = 0; i < 6; ++i) {
        std::memcpy(mapped + i * faceBytes, faces[i], faceBytes);
    }
    vmaFlushAllocation(ctx.allocator(), stagingAllocHandle, 0, totalBytes);

    // One-shot cmd buffer for the upload.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = ctx.graphicsFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &pool));

    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));

    // UNDEFINED -> TRANSFER_DST_OPTIMAL on all 6 layers.
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = res.image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 6;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    // Six copy regions, one per face.
    VkBufferImageCopy regions[6]{};
    for (int i = 0; i < 6; ++i) {
        regions[i].bufferOffset = i * faceBytes;
        regions[i].bufferRowLength = 0;
        regions[i].bufferImageHeight = 0;
        regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[i].imageSubresource.mipLevel = 0;
        regions[i].imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(i);
        regions[i].imageSubresource.layerCount = 1;
        regions[i].imageOffset = {0, 0, 0};
        regions[i].imageExtent = {res.width, res.height, 1};
    }
    vkCmdCopyBufferToImage(cb, stagingBuf, res.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);

    // TRANSFER_DST_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL.
    VkImageMemoryBarrier toShaderRead = toTransfer;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

    VK_CHECK(vkEndCommandBuffer(cb));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    vmaDestroyBuffer(ctx.allocator(), stagingBuf, stagingAllocHandle);
}

}  // namespace iron
