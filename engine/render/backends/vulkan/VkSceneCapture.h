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
// ambient + diffuse texture. When a valid skyboxCube is provided the HDR sky
// is drawn into each face first (as background), then geometry on top.
// On-demand/blocking; owns all its resources (command pool, descriptor pool,
// UBO buffer) — does not touch VkFrameRing.
class VkSceneCapture {
public:
    // Returns false on failure; caller must still call destroy() to release any partially-created objects.
    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // skyboxCube: pass the renderer's current skybox handle (pendingSkybox_).
    // Pass kInvalidHandle to keep the flat-color clear as fallback.
    CubemapHandle capture(VkContext& ctx, VkCubemapStore& cubes, VkMeshStore& meshes,
                          VkTextureStore& textures,
                          const std::vector<DrawCall>& sceneDraws,
                          Vec3 sunDir, Vec3 sunColor, Vec3 ambient,
                          Vec3 position, int faceSize,
                          CubemapHandle skyboxCube);

private:
    bool ensureDepth(VkContext& ctx, std::uint32_t faceSize);

    VkRenderPass          renderPass_         = VK_NULL_HANDLE;  // 1 color (RGBA16F) + depth
    VkImage               depthImage_         = VK_NULL_HANDLE;
    VmaAllocation         depthAlloc_         = VK_NULL_HANDLE;
    VkImageView           depthView_          = VK_NULL_HANDLE;
    std::uint32_t         depthSize_          = 0;               // recreate if faceSize grows

    // --- Scene geometry pipeline ---
    VkDescriptorSetLayout setLayout_          = VK_NULL_HANDLE;  // UBO@0 + sampler2D@1
    VkPipelineLayout      pipelineLayout_     = VK_NULL_HANDLE;
    ::VkPipeline          pipeline_           = VK_NULL_HANDLE;
    VkShaderModule        vert_               = VK_NULL_HANDLE;
    VkShaderModule        frag_               = VK_NULL_HANDLE;

    // --- Skybox pipeline (owned; mirrors VkSkybox but no VkFrameRing) ---
    VkDescriptorSetLayout skySetLayout_       = VK_NULL_HANDLE;  // UBO@0(vert) + samplerCube@1(frag)
    VkPipelineLayout      skyPipelineLayout_  = VK_NULL_HANDLE;
    ::VkPipeline          skyPipeline_        = VK_NULL_HANDLE;
    VkBuffer              skyVertexBuffer_    = VK_NULL_HANDLE;
    VmaAllocation         skyVertexAlloc_     = VK_NULL_HANDLE;
    VkBuffer              skyIndexBuffer_     = VK_NULL_HANDLE;
    VmaAllocation         skyIndexAlloc_      = VK_NULL_HANDLE;

    // --- Shared descriptor pool + UBO (reset each capture) ---
    VkDescriptorPool      descPool_           = VK_NULL_HANDLE;
    VkBuffer              uboBuffer_          = VK_NULL_HANDLE;   // host-visible linear allocator
    VmaAllocation         uboAlloc_           = VK_NULL_HANDLE;
    void*                 uboMapped_          = nullptr;
};

}  // namespace iron
