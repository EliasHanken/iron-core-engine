// VkTexture.cpp — staging-upload RGBA8 image + shared linear sampler.
// Builtins (white, flatNormal, noSpec) created in init().

#include "render/backends/vulkan/VkTexture.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <stb_image.h>

#include <cstring>

namespace iron {

namespace {

VkSampler createLinearRepeatSampler(VkContext& ctx) {
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.minLod = 0.0f;
    info.maxLod = VK_LOD_CLAMP_NONE;
    info.anisotropyEnable = VK_FALSE;
    VkSampler s = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(ctx.device(), &info, nullptr, &s));
    return s;
}

VkBuffer createStagingBuffer(VkContext& ctx, VkDeviceSize size,
                             VmaAllocation& outAlloc, void*& outMap) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocationInfo aInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &buf, &outAlloc, &aInfo));
    outMap = aInfo.pMappedData;
    return buf;
}

}  // namespace

bool VkTextureStore::init(VkContext& ctx) {
    sharedSampler_ = createLinearRepeatSampler(ctx);
    if (!sharedSampler_) return false;

    // Builtins.
    const unsigned char white[4]      = {255, 255, 255, 255};
    const unsigned char flatNormal[4] = {128, 128, 255, 255};
    const unsigned char noSpec[4]     = {0,   0,   0,   255};
    white_      = createFromRgba(ctx, 1, 1, white);
    flatNormal_ = createFromRgba(ctx, 1, 1, flatNormal);
    noSpec_     = createFromRgba(ctx, 1, 1, noSpec);
    return white_ != kInvalidHandle && flatNormal_ != kInvalidHandle && noSpec_ != kInvalidHandle;
}

void VkTextureStore::destroyAll(VkContext& ctx) {
    for (auto& [h, t] : textures_) {
        if (t.view)  vkDestroyImageView(ctx.device(), t.view, nullptr);
        if (t.image) vmaDestroyImage(ctx.allocator(), t.image, t.alloc);
    }
    textures_.clear();
    if (sharedSampler_) {
        vkDestroySampler(ctx.device(), sharedSampler_, nullptr);
        sharedSampler_ = VK_NULL_HANDLE;
    }
}

void VkTextureStore::uploadRgba(VkContext& ctx, VkTextureResource& tex,
                                int width, int height, const unsigned char* rgba) {
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    void* stagingMap = nullptr;
    VkBuffer staging = createStagingBuffer(ctx, size, stagingAlloc, stagingMap);
    std::memcpy(stagingMap, rgba, size);

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imgInfo.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &imgInfo, &alloc,
                            &tex.image, &tex.alloc, nullptr));

    // One-shot command buffer for the copy.
    VkCommandPool pool;
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = ctx.graphicsFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &pool);

    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = pool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);

    VkImageMemoryBarrier toDst{};
    toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = tex.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toDst.srcAccessMask = 0;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toDst);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = imgInfo.extent;
    vkCmdCopyBufferToImage(cb, staging, tex.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier toShader = toDst;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toShader);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.graphicsQueue());

    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);

    // Image view + sampler.
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &tex.view));
    tex.sampler = sharedSampler_;
    tex.width  = static_cast<std::uint32_t>(width);
    tex.height = static_cast<std::uint32_t>(height);
}

TextureHandle VkTextureStore::createFromRgba(VkContext& ctx,
                                             int width, int height,
                                             const unsigned char* rgba) {
    VkTextureResource tex{};
    uploadRgba(ctx, tex, width, height, rgba);
    const TextureHandle h = nextHandle_++;
    textures_[h] = tex;
    return h;
}

TextureHandle VkTextureStore::loadFromFile(VkContext& ctx, const std::string& path) {
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!pixels) {
        Log::error("VkTextureStore: stbi_load failed for %s", path.c_str());
        return kInvalidHandle;
    }
    const TextureHandle handle = createFromRgba(ctx, w, h, pixels);
    stbi_image_free(pixels);
    return handle;
}

const VkTextureResource& VkTextureStore::get(TextureHandle h) const {
    return textures_.find(h)->second;
}

}  // namespace iron
