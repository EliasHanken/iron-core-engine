#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

namespace iron {

class VkContext;

class VkSwapchain {
public:
    bool init(VkContext& ctx, int width, int height);
    void destroy(VkContext& ctx);

    // Tear down + recreate at new size. Caller must vkDeviceWaitIdle first.
    bool recreate(VkContext& ctx, int width, int height);

    VkSwapchainKHR handle()      const { return swapchain_; }
    VkFormat       colorFormat() const { return colorFormat_; }
    VkFormat       depthFormat() const { return VK_FORMAT_D32_SFLOAT; }
    VkExtent2D     extent()      const { return extent_; }
    std::uint32_t  imageCount()  const { return static_cast<std::uint32_t>(images_.size()); }
    VkImageView    colorView(std::uint32_t i) const { return imageViews_[i]; }
    VkImageView    depthView()   const { return depthView_; }

private:
    bool createSwapchain(VkContext& ctx, int width, int height);
    bool createImageViews(VkContext& ctx);
    bool createDepth(VkContext& ctx);
    void destroyDepth(VkContext& ctx);

    VkSwapchainKHR             swapchain_   = VK_NULL_HANDLE;
    VkFormat                   colorFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D                 extent_      = {0, 0};
    std::vector<VkImage>       images_;        // owned by swapchain
    std::vector<VkImageView>   imageViews_;    // owned by us
    VkImage                    depthImage_  = VK_NULL_HANDLE;
    VmaAllocation              depthAlloc_  = VK_NULL_HANDLE;
    VkImageView                depthView_   = VK_NULL_HANDLE;
};

}  // namespace iron
