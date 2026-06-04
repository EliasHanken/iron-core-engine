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
#include <vector>

namespace iron {

class VkContext;

struct VkCubemapResource {
    VkImage       image     = VK_NULL_HANDLE;
    VmaAllocation alloc     = VK_NULL_HANDLE;
    VkImageView   view      = VK_NULL_HANDLE;  // cube view, for sampling
    VkSampler     sampler   = VK_NULL_HANDLE;  // shared with store
    VkFormat      format    = VK_FORMAT_R8G8B8A8_SRGB;
    std::uint32_t width     = 0;
    std::uint32_t height    = 0;
    std::uint32_t mipLevels = 1;
    // Per-mip 2D-array views (6 layers) for compute imageStore. Empty for
    // sampled-only LDR cubemaps created via createFromFaces.
    std::vector<VkImageView> storageViews;
    // Per-face single-layer 2D views (6) for color-attachment rendering.
    // Empty unless created via createColorCube.
    std::vector<VkImageView> faceViews;
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

    // Allocates an RGBA16F cube-compatible image (faceSize x faceSize, 6
    // layers, `mipLevels` mips) with STORAGE+SAMPLED usage. The returned
    // resource has a cube sampling `view` plus one 2D-array `storageViews`
    // entry per mip for compute writes. The image is left in
    // VK_IMAGE_LAYOUT_UNDEFINED; the caller transitions it.
    CubemapHandle createHdr(VkContext& ctx, int faceSize, int mipLevels);

    // Allocates an RGBA16F cube-compatible image (faceSize^2, 6 layers, 1 mip)
    // with COLOR_ATTACHMENT + SAMPLED usage, a cube sampling `view`, and 6
    // single-layer `faceViews` for rendering each face. Left in
    // VK_IMAGE_LAYOUT_UNDEFINED; the render pass transitions it.
    CubemapHandle createColorCube(VkContext& ctx, int faceSize);

    // Destroys one baked cube (image/alloc/views). Safe no-op on kInvalidHandle
    // or an unknown handle. Does NOT destroy the shared sampler or the black fallback.
    void destroy(VkContext& ctx, CubemapHandle h);

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
