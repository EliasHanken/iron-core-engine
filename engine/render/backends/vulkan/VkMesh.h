#pragma once

#include "render/Handles.h"
#include "scene/Mesh.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <unordered_map>

namespace iron {

class VkContext;

struct VkMeshResource {
    VkBuffer      vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc  = VK_NULL_HANDLE;
    VkBuffer      indexBuffer  = VK_NULL_HANDLE;
    VmaAllocation indexAlloc   = VK_NULL_HANDLE;
    std::uint32_t indexCount   = 0;
    std::uint32_t vertexBytes  = 0;
    std::uint32_t indexBytes   = 0;
};

class VkMeshStore {
public:
    MeshHandle create(VkContext& ctx, const MeshData& data);
    void       update(VkContext& ctx, MeshHandle h, const MeshData& data);
    const VkMeshResource& get(MeshHandle h) const;
    bool       has(MeshHandle h) const { return meshes_.count(h) != 0; }
    void       destroyAll(VkContext& ctx);

private:
    std::unordered_map<MeshHandle, VkMeshResource> meshes_;
    MeshHandle nextHandle_ = 1;

    static VkBuffer makeBuffer(VkContext& ctx, VkDeviceSize size,
                               VkBufferUsageFlags usage, VmaAllocation& outAlloc);
};

}  // namespace iron
