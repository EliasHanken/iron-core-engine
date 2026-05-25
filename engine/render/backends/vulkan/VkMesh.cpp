// VkMesh.cpp — host-visible vertex+index buffers via VMA. M9 sticks
// with host-visible memory for simplicity; staging-to-device-local
// upload is a future optimization.

#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkUtils.h"

#include <cstring>

namespace iron {

VkBuffer VkMeshStore::makeBuffer(VkContext& ctx, VkDeviceSize size,
                                 VkBufferUsageFlags usage,
                                 VmaAllocation& outAlloc) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                  VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &buf, &outAlloc, &allocInfo));
    return buf;
}

MeshHandle VkMeshStore::create(VkContext& ctx, const MeshData& data) {
    VkMeshResource r{};
    r.vertexBytes = static_cast<std::uint32_t>(data.vertices.size() * sizeof(Vertex));
    r.indexBytes  = static_cast<std::uint32_t>(data.indices.size()  * sizeof(std::uint32_t));
    r.indexCount  = static_cast<std::uint32_t>(data.indices.size());

    r.vertexBuffer = makeBuffer(ctx, r.vertexBytes,
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r.vertexAlloc);
    r.indexBuffer  = makeBuffer(ctx, r.indexBytes,
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  r.indexAlloc);

    void* vmap = nullptr;
    void* imap = nullptr;
    vmaMapMemory(ctx.allocator(), r.vertexAlloc, &vmap);
    std::memcpy(vmap, data.vertices.data(), r.vertexBytes);
    vmaUnmapMemory(ctx.allocator(), r.vertexAlloc);

    vmaMapMemory(ctx.allocator(), r.indexAlloc, &imap);
    std::memcpy(imap, data.indices.data(), r.indexBytes);
    vmaUnmapMemory(ctx.allocator(), r.indexAlloc);

    const MeshHandle h = nextHandle_++;
    meshes_[h] = r;
    return h;
}

void VkMeshStore::update(VkContext& ctx, MeshHandle h, const MeshData& data) {
    auto it = meshes_.find(h);
    if (it == meshes_.end()) return;
    VkMeshResource& r = it->second;

    const std::uint32_t newVertBytes =
        static_cast<std::uint32_t>(data.vertices.size() * sizeof(Vertex));
    const std::uint32_t newIdxBytes =
        static_cast<std::uint32_t>(data.indices.size()  * sizeof(std::uint32_t));

    // If sizes grew, destroy and recreate. Otherwise reuse.
    if (newVertBytes > r.vertexBytes || newIdxBytes > r.indexBytes) {
        vmaDestroyBuffer(ctx.allocator(), r.vertexBuffer, r.vertexAlloc);
        vmaDestroyBuffer(ctx.allocator(), r.indexBuffer,  r.indexAlloc);
        r = {};
        r.vertexBytes = newVertBytes;
        r.indexBytes  = newIdxBytes;
        r.indexCount  = static_cast<std::uint32_t>(data.indices.size());
        r.vertexBuffer = makeBuffer(ctx, newVertBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, r.vertexAlloc);
        r.indexBuffer  = makeBuffer(ctx, newIdxBytes,  VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  r.indexAlloc);
    } else {
        r.indexCount = static_cast<std::uint32_t>(data.indices.size());
    }

    void* vmap = nullptr;
    void* imap = nullptr;
    vmaMapMemory(ctx.allocator(), r.vertexAlloc, &vmap);
    std::memcpy(vmap, data.vertices.data(), newVertBytes);
    vmaUnmapMemory(ctx.allocator(), r.vertexAlloc);

    vmaMapMemory(ctx.allocator(), r.indexAlloc, &imap);
    std::memcpy(imap, data.indices.data(), newIdxBytes);
    vmaUnmapMemory(ctx.allocator(), r.indexAlloc);
}

const VkMeshResource& VkMeshStore::get(MeshHandle h) const {
    return meshes_.find(h)->second;
}

void VkMeshStore::destroyAll(VkContext& ctx) {
    for (auto& [h, r] : meshes_) {
        if (r.vertexBuffer) vmaDestroyBuffer(ctx.allocator(), r.vertexBuffer, r.vertexAlloc);
        if (r.indexBuffer)  vmaDestroyBuffer(ctx.allocator(), r.indexBuffer,  r.indexAlloc);
    }
    meshes_.clear();
}

}  // namespace iron
