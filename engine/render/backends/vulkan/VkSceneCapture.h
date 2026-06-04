#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkSceneCapture is Vulkan-only."
#endif

#include "render/Handles.h"
#include "render/Renderer.h"   // DrawCall lives here (not a standalone DrawCall.h)
#include "math/Vec.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <vector>

namespace iron {

class VkContext;
class VkCubemapStore;
class VkMeshStore;
class VkTextureStore;

// Renders the scene's 6 cube faces from a world position into an RGBA16F cube
// (allocated via VkCubemapStore::createColorCube). Simplified shading: sun +
// ambient + diffuse texture. Faces cleared to a neutral sky color; skybox not
// drawn (v1). On-demand/blocking; owns all its resources (command pool,
// descriptor pool, UBO buffer) — does not touch VkFrameRing.
class VkSceneCapture {
public:
    // Returns false on failure; caller must still call destroy() to release any partially-created objects.
    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    CubemapHandle capture(VkContext& ctx, VkCubemapStore& cubes, VkMeshStore& meshes,
                          VkTextureStore& textures,
                          const std::vector<DrawCall>& sceneDraws,
                          Vec3 sunDir, Vec3 sunColor, Vec3 ambient,
                          Vec3 position, int faceSize);

private:
    bool ensureDepth(VkContext& ctx, std::uint32_t faceSize);

    VkRenderPass          renderPass_     = VK_NULL_HANDLE;  // 1 color (RGBA16F) + depth
    VkImage               depthImage_     = VK_NULL_HANDLE;
    VmaAllocation         depthAlloc_     = VK_NULL_HANDLE;
    VkImageView           depthView_      = VK_NULL_HANDLE;
    std::uint32_t         depthSize_      = 0;               // recreate if faceSize grows
    VkDescriptorSetLayout setLayout_      = VK_NULL_HANDLE;  // UBO@0 + sampler@1
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    ::VkPipeline          pipeline_       = VK_NULL_HANDLE;
    VkDescriptorPool      descPool_       = VK_NULL_HANDLE;
    VkBuffer              uboBuffer_      = VK_NULL_HANDLE;   // host-visible linear allocator
    VmaAllocation         uboAlloc_       = VK_NULL_HANDLE;
    void*                 uboMapped_      = nullptr;
    VkShaderModule        vert_           = VK_NULL_HANDLE;
    VkShaderModule        frag_           = VK_NULL_HANDLE;
};

}  // namespace iron
