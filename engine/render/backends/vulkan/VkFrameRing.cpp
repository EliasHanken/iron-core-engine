// VkFrameRing.cpp — per-frame command, sync, descriptor pool, and UBO
// linear sub-allocator. Two frames in flight.

#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <cassert>
#include <cstring>

namespace iron {

bool VkFrameRing::init(VkContext& ctx) {
    for (auto& f : frames_) {
        if (!initFrame(ctx, f)) return false;
    }
    return true;
}

void VkFrameRing::destroy(VkContext& ctx) {
    for (auto& f : frames_) destroyFrame(ctx, f);
}

bool VkFrameRing::initFrame(VkContext& ctx, Frame& f) {
    // Command pool + buffer
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = ctx.graphicsFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &poolInfo, nullptr, &f.commandPool));

    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool = f.commandPool;
    cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &f.commandBuffer));

    // Sync
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VK_CHECK(vkCreateSemaphore(ctx.device(), &semInfo, nullptr, &f.imageAvailable));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(ctx.device(), &fenceInfo, nullptr, &f.inFlight));

    // Descriptor pool — sized for 128 sets, capacity for each descriptor
    // type used by any pipeline that allocates from this pool. M10's
    // ParticleSystem render pipeline reads from an SSBO (vertex stage),
    // so STORAGE_BUFFER capacity is required here.
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         2 * kMaxDescriptorSetsPerFrame},  // M23: skinned draws use 2 UBOs (scene + bones)
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 * kMaxDescriptorSetsPerFrame},  // M50a: 12 samplers per lit set (added height map at binding 13)
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kMaxDescriptorSetsPerFrame},
    };
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets = kMaxDescriptorSetsPerFrame;
    dpInfo.poolSizeCount = 3;
    dpInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &f.descriptorPool));

    // Per-frame UBO buffer (host-visible coherent linear allocator)
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = kUboBytesPerFrame;
    bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bufInfo, &alloc,
                             &f.uboBuffer, &f.uboAlloc, &allocInfo));
    f.uboMapped = allocInfo.pMappedData;
    f.uboCursor = 0;

    // Per-frame vertex buffer (host-visible, used by VkHud + VkDebugLines).
    // 1 MB is enough for ~16 K HudVertices or ~31 K LineVertices — well
    // above realistic per-frame use.
    VkBufferCreateInfo vbInfo{};
    vbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbInfo.size = kVertexBytesPerFrame;
    vbInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo vbAlloc{};
    vbAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    vbAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo vbAllocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &vbInfo, &vbAlloc,
                             &f.vertexBuffer, &f.vertexAlloc, &vbAllocInfo));
    f.vertexMapped = vbAllocInfo.pMappedData;
    f.vertexCursor = 0;
    return true;
}

void VkFrameRing::destroyFrame(VkContext& ctx, Frame& f) {
    if (f.vertexBuffer)   { vmaDestroyBuffer(ctx.allocator(), f.vertexBuffer, f.vertexAlloc); f.vertexBuffer = VK_NULL_HANDLE; }
    if (f.uboBuffer)      { vmaDestroyBuffer(ctx.allocator(), f.uboBuffer, f.uboAlloc); f.uboBuffer = VK_NULL_HANDLE; }
    if (f.descriptorPool) { vkDestroyDescriptorPool(ctx.device(), f.descriptorPool, nullptr); f.descriptorPool = VK_NULL_HANDLE; }
    if (f.inFlight)       { vkDestroyFence(ctx.device(), f.inFlight, nullptr); f.inFlight = VK_NULL_HANDLE; }
    if (f.imageAvailable) { vkDestroySemaphore(ctx.device(), f.imageAvailable, nullptr); f.imageAvailable = VK_NULL_HANDLE; }
    if (f.commandPool)    { vkDestroyCommandPool(ctx.device(), f.commandPool, nullptr); f.commandPool = VK_NULL_HANDLE; }
    f.commandBuffer = VK_NULL_HANDLE;
}

void VkFrameRing::resetCurrentFrame(VkContext& ctx) {
    Frame& f = current();
    vkResetCommandPool(ctx.device(), f.commandPool, 0);
    vkResetDescriptorPool(ctx.device(), f.descriptorPool, 0);
    f.uboCursor = 0;
    f.vertexCursor = 0;
}

VkDeviceSize VkFrameRing::allocateUbo(const void* data, VkDeviceSize size) {
    assert(size > 0 && size <= kUboBytesPerFrame && data != nullptr);
    // Align to 256 bytes (matches typical minUniformBufferOffsetAlignment;
    // safe upper bound — we can tune per-device later.)
    constexpr VkDeviceSize kAlign = 256;
    Frame& f = current();
    const VkDeviceSize aligned = (f.uboCursor + kAlign - 1) & ~(kAlign - 1);
    f.uboCursor = aligned + size;
    std::memcpy(static_cast<char*>(f.uboMapped) + aligned, data, size);
    return aligned;
}

VkBuffer VkFrameRing::allocateVertices(const void* data, VkDeviceSize size,
                                       VkDeviceSize& outOffset) {
    constexpr VkDeviceSize kAlign = 16;
    Frame& f = current();
    if (size == 0 || size > kVertexBytesPerFrame || data == nullptr) {
        outOffset = 0;
        return VK_NULL_HANDLE;
    }
    const VkDeviceSize aligned = (f.vertexCursor + kAlign - 1) & ~(kAlign - 1);
    if (aligned + size > kVertexBytesPerFrame) {
        outOffset = 0;
        return VK_NULL_HANDLE;
    }
    f.vertexCursor = aligned + size;
    std::memcpy(static_cast<char*>(f.vertexMapped) + aligned, data, size);
    outOffset = aligned;
    return f.vertexBuffer;
}

}  // namespace iron
