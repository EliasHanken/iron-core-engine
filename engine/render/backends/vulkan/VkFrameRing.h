#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>

namespace iron {

class VkContext;

class VkFrameRing {
public:
    static constexpr int kFramesInFlight = 2;
    static constexpr int kMaxDescriptorSetsPerFrame = 128;
    static constexpr VkDeviceSize kUboBytesPerFrame = 256 * 1024;  // 256 KB

    struct Frame {
        VkCommandPool     commandPool   = VK_NULL_HANDLE;
        VkCommandBuffer   commandBuffer = VK_NULL_HANDLE;
        VkSemaphore       imageAvailable = VK_NULL_HANDLE;
        VkSemaphore       renderFinished = VK_NULL_HANDLE;
        VkFence           inFlight       = VK_NULL_HANDLE;
        VkDescriptorPool  descriptorPool = VK_NULL_HANDLE;
        VkBuffer          uboBuffer      = VK_NULL_HANDLE;
        VmaAllocation     uboAlloc       = VK_NULL_HANDLE;
        void*             uboMapped      = nullptr;
        VkDeviceSize      uboCursor      = 0;
    };

    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    Frame&        current()    { return frames_[index_]; }
    int           currentIndex() const { return index_; }
    void          advance()    { index_ = (index_ + 1) % kFramesInFlight; }

    // Reset the current frame for re-recording: zero out the per-frame
    // UBO sub-allocator cursor + reset the descriptor pool + reset the
    // command pool.
    void resetCurrentFrame(VkContext& ctx);

    // Allocate `size` bytes from the current frame's UBO sub-allocator.
    // Writes `data` and returns the offset (used in VkDescriptorBufferInfo).
    VkDeviceSize allocateUbo(const void* data, VkDeviceSize size);

private:
    bool initFrame(VkContext& ctx, Frame& f);
    void destroyFrame(VkContext& ctx, Frame& f);

    std::array<Frame, kFramesInFlight> frames_{};
    int index_ = 0;
};

}  // namespace iron
