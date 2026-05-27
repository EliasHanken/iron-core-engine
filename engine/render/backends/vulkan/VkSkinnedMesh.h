#pragma once

#ifndef IRON_RENDER_BACKEND_VULKAN
#error "VkSkinnedMesh is Vulkan-only."
#endif

#include "render/Handles.h"
#include "scene/SkinnedMesh.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <unordered_map>

namespace iron {

class VkContext;

// GPU-side resource for one skinned mesh: a vertex buffer of SkinnedVertex
// (76 bytes/vertex) + a uint32 index buffer. boneCount mirrors the
// skeleton size at load time; the actual per-frame bone matrices are
// uploaded via SkinnedDrawCall + the frame UBO ring, not stored here.
struct VkSkinnedMeshResource {
    VkBuffer      vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc  = VK_NULL_HANDLE;
    VkBuffer      indexBuffer  = VK_NULL_HANDLE;
    VmaAllocation indexAlloc   = VK_NULL_HANDLE;
    std::uint32_t indexCount   = 0;
    std::uint32_t boneCount    = 0;
};

class VkSkinnedMeshStore {
public:
    SkinnedMeshHandle create(VkContext& ctx, const SkinnedMeshData& data);
    const VkSkinnedMeshResource& get(SkinnedMeshHandle h) const;
    bool has(SkinnedMeshHandle h) const { return meshes_.count(h) != 0; }
    void destroyAll(VkContext& ctx);

private:
    std::unordered_map<SkinnedMeshHandle, VkSkinnedMeshResource> meshes_;
    SkinnedMeshHandle nextHandle_ = 1;
};

}  // namespace iron
