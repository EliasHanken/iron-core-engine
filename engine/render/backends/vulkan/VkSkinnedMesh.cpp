// VkSkinnedMesh.cpp — host-visible VMA buffers for SkinnedVertex + indices.
// Mirrors VkMesh's host-visible approach: M9-style simple upload, no
// staging-to-device-local optimization yet. Each SkinnedVertex is 76 bytes
// (3+3+2+3 floats + 4 uint32 + 4 floats).

#include "render/backends/vulkan/VkSkinnedMesh.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <cstring>

namespace iron {

namespace {

VkBuffer makeBuffer(VkContext& ctx, VkDeviceSize size,
                    VkBufferUsageFlags usage, VmaAllocation& outAlloc) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size  = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &buf, &outAlloc, nullptr));
    return buf;
}

}  // namespace

SkinnedMeshHandle VkSkinnedMeshStore::create(VkContext& ctx,
                                              const SkinnedMeshData& data) {
    if (data.vertices.empty() || data.indices.empty()) return kInvalidSkinnedMesh;

    VkSkinnedMeshResource res;
    const VkDeviceSize vertBytes = data.vertices.size() * sizeof(SkinnedVertex);
    const VkDeviceSize idxBytes  = data.indices.size()  * sizeof(std::uint32_t);

    res.vertexBuffer = makeBuffer(ctx, vertBytes,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, res.vertexAlloc);
    res.indexBuffer  = makeBuffer(ctx, idxBytes,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT, res.indexAlloc);
    res.indexCount   = static_cast<std::uint32_t>(data.indices.size());
    res.boneCount    = static_cast<std::uint32_t>(data.skeleton.bones.size());

    void* p = nullptr;
    VK_CHECK(vmaMapMemory(ctx.allocator(), res.vertexAlloc, &p));
    std::memcpy(p, data.vertices.data(), vertBytes);
    vmaUnmapMemory(ctx.allocator(), res.vertexAlloc);

    VK_CHECK(vmaMapMemory(ctx.allocator(), res.indexAlloc, &p));
    std::memcpy(p, data.indices.data(), idxBytes);
    vmaUnmapMemory(ctx.allocator(), res.indexAlloc);

    const SkinnedMeshHandle h = nextHandle_++;
    meshes_[h] = res;
    return h;
}

const VkSkinnedMeshResource& VkSkinnedMeshStore::get(SkinnedMeshHandle h) const {
    static const VkSkinnedMeshResource empty;
    auto it = meshes_.find(h);
    return it != meshes_.end() ? it->second : empty;
}

void VkSkinnedMeshStore::destroyAll(VkContext& ctx) {
    for (auto& [h, res] : meshes_) {
        if (res.vertexBuffer) vmaDestroyBuffer(ctx.allocator(), res.vertexBuffer, res.vertexAlloc);
        if (res.indexBuffer)  vmaDestroyBuffer(ctx.allocator(), res.indexBuffer, res.indexAlloc);
    }
    meshes_.clear();
}

}  // namespace iron
