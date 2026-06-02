#pragma once

#include "render/Handles.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace iron {

class VkContext;

struct VkTextureResource {
    VkImage        image   = VK_NULL_HANDLE;
    VmaAllocation  alloc   = VK_NULL_HANDLE;
    VkImageView    view    = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;  // shared with store
    std::uint32_t  width   = 0;
    std::uint32_t  height  = 0;
};

class VkTextureStore {
public:
    bool init(VkContext& ctx);   // creates shared sampler + builtin textures
    void destroyAll(VkContext& ctx);

    TextureHandle createFromRgba(VkContext& ctx,
                                 int width, int height,
                                 const unsigned char* rgba,
                                 bool srgb = true);
    TextureHandle loadFromFile(VkContext& ctx, const std::string& path,
                               bool srgb = true);

    TextureHandle whiteTexture()        const { return white_; }
    TextureHandle flatNormalTexture()   const { return flatNormal_; }
    TextureHandle noSpecularTexture()   const { return noSpec_; }

    const VkTextureResource& get(TextureHandle h) const;
    bool has(TextureHandle h) const { return textures_.count(h) != 0; }

    // Returns the store's shared CLAMP_TO_EDGE LINEAR sampler. Other
    // Vulkan subsystems (skybox, reflection target) reuse this rather
    // than allocating their own.
    VkSampler sampler() const { return sharedSampler_; }

private:
    void uploadRgba(VkContext& ctx, VkTextureResource& tex,
                    int width, int height, const unsigned char* rgba,
                    bool srgb = true);

    std::unordered_map<TextureHandle, VkTextureResource> textures_;
    TextureHandle nextHandle_ = 1;
    VkSampler     sharedSampler_ = VK_NULL_HANDLE;
    TextureHandle white_       = kInvalidHandle;
    TextureHandle flatNormal_  = kInvalidHandle;
    TextureHandle noSpec_      = kInvalidHandle;
};

}  // namespace iron
