// VkSwapchain.cpp — surface format negotiation, swapchain, image views,
// shared depth attachment. Recreates on window resize.

#include "render/backends/vulkan/VkSwapchain.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <algorithm>

namespace iron {

namespace {

VkSurfaceFormatKHR pickFormat(VkPhysicalDevice phys, VkSurfaceKHR surface) {
    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats.data());
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats.empty()
        ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
        : formats.front();
}

VkPresentModeKHR pickPresentMode(VkPhysicalDevice phys, VkSurfaceKHR surface) {
    std::uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &n, modes.data());
    for (auto m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    }
    return VK_PRESENT_MODE_FIFO_KHR;  // guaranteed available
}

}  // namespace

bool VkSwapchain::init(VkContext& ctx, int width, int height) {
    if (!createSwapchain(ctx, width, height)) return false;
    if (!createImageViews(ctx))               return false;
    if (!createDepth(ctx))                    return false;
    return true;
}

void VkSwapchain::destroy(VkContext& ctx) {
    destroyDepth(ctx);
    for (auto v : imageViews_) vkDestroyImageView(ctx.device(), v, nullptr);
    imageViews_.clear();
    images_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(ctx.device(), swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VkSwapchain::recreate(VkContext& ctx, int width, int height) {
    destroy(ctx);
    return init(ctx, width, height);
}

bool VkSwapchain::createSwapchain(VkContext& ctx, int width, int height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(), ctx.surface(), &caps);

    const VkSurfaceFormatKHR fmt = pickFormat(ctx.physicalDevice(), ctx.surface());
    colorFormat_ = fmt.format;

    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        extent_.width  = std::clamp(static_cast<std::uint32_t>(width),
                                    caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent_.height = std::clamp(static_cast<std::uint32_t>(height),
                                    caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    std::uint32_t desiredImages = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && desiredImages > caps.maxImageCount) {
        desiredImages = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = ctx.surface();
    info.minImageCount = desiredImages;
    info.imageFormat = fmt.format;
    info.imageColorSpace = fmt.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = pickPresentMode(ctx.physicalDevice(), ctx.surface());
    info.clipped = VK_TRUE;

    VK_CHECK(vkCreateSwapchainKHR(ctx.device(), &info, nullptr, &swapchain_));
    if (!swapchain_) return false;

    std::uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx.device(), swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(ctx.device(), swapchain_, &n, images_.data());
    return true;
}

bool VkSwapchain::createImageViews(VkContext& ctx) {
    imageViews_.resize(images_.size());
    for (std::size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = images_[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = colorFormat_;
        info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &info, nullptr, &imageViews_[i]));
        if (!imageViews_[i]) return false;
    }
    return true;
}

bool VkSwapchain::createDepth(VkContext& ctx) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT;
    info.extent = {extent_.width, extent_.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VK_CHECK(vmaCreateImage(ctx.allocator(), &info, &alloc,
                            &depthImage_, &depthAlloc_, nullptr));
    if (!depthImage_) return false;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_D32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &viewInfo, nullptr, &depthView_));
    return depthView_ != VK_NULL_HANDLE;
}

void VkSwapchain::destroyDepth(VkContext& ctx) {
    if (depthView_)  { vkDestroyImageView(ctx.device(), depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_) {
        vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_);
        depthImage_ = VK_NULL_HANDLE;
        depthAlloc_ = VK_NULL_HANDLE;
    }
}

}  // namespace iron
