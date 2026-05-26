#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkCubemap is Vulkan-only."
#endif

#include "render/Handles.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <unordered_map>

namespace iron {

class VkContext;

struct VkCubemapResource {
    VkImage       image   = VK_NULL_HANDLE;
    VmaAllocation alloc   = VK_NULL_HANDLE;
    VkImageView   view    = VK_NULL_HANDLE;
    VkSampler     sampler = VK_NULL_HANDLE;  // shared with store
    std::uint32_t width   = 0;
    std::uint32_t height  = 0;
};

// Vulkan cubemap storage. Mirrors VkTextureStore pattern: shared
// sampler, in-memory creation from six RGBA face arrays, built-in
// 1x1x6 black fallback for the lit pass's reflection sampler when no
// skybox is set.
class VkCubemapStore {
public:
    bool init(VkContext& ctx);
    void destroyAll(VkContext& ctx);

    CubemapHandle createFromFaces(VkContext& ctx, int width, int height,
                                  const std::array<const unsigned char*, 6>& faces);

    CubemapHandle blackCubemap() const { return black_; }
    const VkCubemapResource& get(CubemapHandle h) const;
    bool has(CubemapHandle h) const { return cubemaps_.count(h) != 0; }

private:
    void uploadFaces(VkContext& ctx, VkCubemapResource& res,
                     int width, int height,
                     const std::array<const unsigned char*, 6>& faces);

    std::unordered_map<CubemapHandle, VkCubemapResource> cubemaps_;
    CubemapHandle nextHandle_ = 1;
    VkSampler     sharedSampler_ = VK_NULL_HANDLE;
    CubemapHandle black_ = kInvalidHandle;
};

}  // namespace iron
