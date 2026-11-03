#include "render/backends/vulkan/VkRayTracing.h"

#include "core/Log.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkUtils.h"
#include "scene/Mesh.h"

#include <algorithm>
#include <cstring>

namespace iron {

namespace {

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

struct GpuInstanceInfo {
    std::uint64_t vertices;
    std::uint64_t indices;
    float         albedo[4];
    float         emissive[4];
};
static_assert(sizeof(GpuInstanceInfo) == 48, "std430 Info in the GI trace");

}  // namespace

bool VkRayTracing::init(VkContext& ctx) { return ctx.rayTracing(); }

VkRayTracing::Buffer VkRayTracing::makeBuffer(VkContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                                              bool hostVisible) {
    Buffer b;
    b.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = b.size;
    info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo out{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &info, &alloc, &b.buffer, &b.alloc, &out));
    b.mapped = out.pMappedData;
    VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addr.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(ctx.device(), &addr);
    return b;
}

void VkRayTracing::free(VkContext& ctx, Buffer& b) {
    if (b.buffer) vmaDestroyBuffer(ctx.allocator(), b.buffer, b.alloc);
    b = {};
}

void VkRayTracing::retire(VkContext&, Buffer& b) {
    if (b.buffer) retired_.push_back({b, VK_NULL_HANDLE, frameCount_});
    b = {};
}

void VkRayTracing::retire(VkContext&, VkAccelerationStructureKHR& as) {
    if (as) retired_.push_back({{}, as, frameCount_});
    as = VK_NULL_HANDLE;
}

void VkRayTracing::collect(VkContext& ctx) {
    // A frame recorded `kFramesInFlight` frames ago has finished on the GPU.
    std::erase_if(retired_, [&](Retired& r) {
        if (frameCount_ < r.frame + VkFrameRing::kFramesInFlight + 1) return false;
        if (r.as) ctx.rt().destroy(ctx.device(), r.as, nullptr);
        free(ctx, r.buffer);
        return true;
    });
}

void VkRayTracing::destroy(VkContext& ctx) {
    if (!ctx.rayTracing()) return;
    for (auto& [mesh, b] : blas_) {
        ctx.rt().destroy(ctx.device(), b.as, nullptr);
        free(ctx, b.storage);
    }
    blas_.clear();
    for (Tlas& t : tlas_) {
        if (t.as) ctx.rt().destroy(ctx.device(), t.as, nullptr);
        free(ctx, t.storage);
        free(ctx, t.instances);
        free(ctx, t.info);
        free(ctx, t.scratch);
        t = {};
    }
    for (Retired& r : retired_) {
        if (r.as) ctx.rt().destroy(ctx.device(), r.as, nullptr);
        free(ctx, r.buffer);
    }
    retired_.clear();
}

void VkRayTracing::build(VkContext& ctx, VkCommandBuffer cb, std::uint32_t frameIndex, const VkMeshStore& meshes,
                         std::span<const Placement> statics, std::uint64_t staticGeneration,
                         std::span<const Placement> dynamics) {
    if (!ctx.rayTracing()) return;
    ++frameCount_;
    collect(ctx);
    const auto& rt = ctx.rt();
    const VkDeviceSize scratchAlign = ctx.asScratchAlignment();

    // --- Bottom level: any mesh not yet built (or rebuilt since: new buffers). ---
    struct Pending {
        MeshHandle                                  mesh;
        VkAccelerationStructureGeometryKHR          geometry{};
        VkAccelerationStructureBuildRangeInfoKHR    range{};
        VkAccelerationStructureBuildGeometryInfoKHR info{};
        VkDeviceSize                                scratchOffset = 0;
    };
    std::vector<Pending> pending;
    VkDeviceSize scratchTotal = 0;
    const auto needs = [&](const Placement& p) {
        if (!meshes.has(p.mesh)) return;
        const VkMeshResource& m = meshes.get(p.mesh);
        if (m.indexCount < 3) return;
        const auto found = blas_.find(p.mesh);
        if (found != blas_.end() && found->second.source == m.vertexBuffer) return;
        if (std::any_of(pending.begin(), pending.end(), [&](const Pending& q) { return q.mesh == p.mesh; })) return;
        if (found != blas_.end()) {
            retire(ctx, found->second.as);
            retire(ctx, found->second.storage);
            blas_.erase(found);
        }
        Pending q;
        q.mesh = p.mesh;
        VkBufferDeviceAddressInfo vaddr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        vaddr.buffer = m.vertexBuffer;
        VkBufferDeviceAddressInfo iaddr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        iaddr.buffer = m.indexBuffer;
        q.geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        q.geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        q.geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = q.geometry.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = vkGetBufferDeviceAddress(ctx.device(), &vaddr);
        tri.vertexStride = sizeof(Vertex);
        tri.maxVertex = m.vertexBytes / sizeof(Vertex) - 1;
        tri.indexType = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress = vkGetBufferDeviceAddress(ctx.device(), &iaddr);
        q.range.primitiveCount = m.indexCount / 3;
        pending.push_back(q);
    };
    // Statics only when they've changed: their meshes were seen the last time.
    Tlas& t = tlas_[frameIndex];
    const bool staticsChanged = t.staticGeneration != staticGeneration;
    if (staticsChanged) {
        for (const Placement& p : statics) needs(p);
    }
    for (const Placement& p : dynamics) needs(p);
    for (Pending& q : pending) {
        q.info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        q.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        q.info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        q.info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        q.info.geometryCount = 1;
        q.info.pGeometries = &q.geometry;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rt.buildSizes(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &q.info, &q.range.primitiveCount,
                      &sizes);
        Blas b;
        b.source = meshes.get(q.mesh).vertexBuffer;
        b.vertices = q.geometry.geometry.triangles.vertexData.deviceAddress;
        b.indices = q.geometry.geometry.triangles.indexData.deviceAddress;
        b.storage = makeBuffer(ctx, sizes.accelerationStructureSize,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = b.storage.buffer;
        create.size = sizes.accelerationStructureSize;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VK_CHECK(rt.create(ctx.device(), &create, nullptr, &b.as));
        VkAccelerationStructureDeviceAddressInfoKHR where{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        where.accelerationStructure = b.as;
        b.address = rt.address(ctx.device(), &where);
        q.info.dstAccelerationStructure = b.as;
        q.scratchOffset = scratchTotal;
        scratchTotal += alignUp(sizes.buildScratchSize, scratchAlign);
        blas_[q.mesh] = b;
    }
    if (!pending.empty()) {
        // One scratch for the lot, each build in its own aligned slice; kept till the frame's done.
        Buffer scratch = makeBuffer(ctx, scratchTotal + scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        const VkDeviceAddress base = alignUp(scratch.address, scratchAlign);
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
        for (Pending& q : pending) {
            q.info.pGeometries = &q.geometry;
            q.info.scratchData.deviceAddress = base + q.scratchOffset;
            infos.push_back(q.info);
            ranges.push_back(&q.range);
        }
        rt.cmdBuild(cb, static_cast<std::uint32_t>(infos.size()), infos.data(), ranges.data());
        retire(ctx, scratch);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, nullptr, 0,
                             nullptr);
        Log::info("VkRayTracing: built %zu BLAS (%zu in all)", pending.size(), blas_.size());
    }

    // --- Top level: the statics at the front (rewritten when they change), this frame's after. ---
    const std::uint32_t count = static_cast<std::uint32_t>(statics.size() + dynamics.size());
    // A rebuilt BLAS has a new address, which statics written before don't know.
    bool rewriteStatics = staticsChanged || !pending.empty();
    if (count > t.capacity) {
        rewriteStatics = true;
        retire(ctx, t.instances);
        retire(ctx, t.info);
        retire(ctx, t.storage);
        retire(ctx, t.scratch);
        retire(ctx, t.as);
        t.capacity = std::max(count + count / 2, 64u);
        t.instances = makeBuffer(ctx, sizeof(VkAccelerationStructureInstanceKHR) * t.capacity,
                                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
        // Ids run over statics then dynamics, so the info's as long as both.
        t.info = makeBuffer(ctx, sizeof(GpuInstanceInfo) * t.capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    auto* out = static_cast<VkAccelerationStructureInstanceKHR*>(t.instances.mapped);
    const auto write = [&](const Placement& p, std::uint32_t& written) {
        const auto found = blas_.find(p.mesh);
        if (found == blas_.end()) return;
        VkAccelerationStructureInstanceKHR& inst = out[written++];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) inst.transform.matrix[r][c] = p.transform.at(r, c);
        }
        inst.instanceCustomIndex = p.id & 0xFFFFFFu;
        inst.mask = 0xFF;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = found->second.address;
        if (p.id < t.capacity) {
            GpuInstanceInfo& in = static_cast<GpuInstanceInfo*>(t.info.mapped)[p.id];
            in.vertices = found->second.vertices;
            in.indices = found->second.indices;
            in.albedo[0] = p.albedo.x;
            in.albedo[1] = p.albedo.y;
            in.albedo[2] = p.albedo.z;
            in.albedo[3] = 1.0f;
            in.emissive[0] = p.emissive.x;
            in.emissive[1] = p.emissive.y;
            in.emissive[2] = p.emissive.z;
            in.emissive[3] = 0.0f;
        }
    };
    if (rewriteStatics) {
        t.staticCount = 0;
        for (const Placement& p : statics) write(p, t.staticCount);
        t.staticGeneration = staticGeneration;
    }
    std::uint32_t written = t.staticCount;
    for (const Placement& p : dynamics) write(p, written);
    vmaFlushAllocation(ctx.allocator(), t.instances.alloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(ctx.allocator(), t.info.alloc, 0, VK_WHOLE_SIZE);
    t.count = written;
    if (frameCount_ % 300 == 1) Log::info("VkRayTracing: %u instances in the scene", written);

    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = t.instances.address;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    const std::uint32_t maxCount = t.capacity;
    rt.buildSizes(ctx.device(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &maxCount, &sizes);
    if (!t.as) {
        // Sized for the capacity, so it's rebuilt in place until that grows.
        t.storage = makeBuffer(ctx, sizes.accelerationStructureSize,
                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        t.scratch = makeBuffer(ctx, sizes.buildScratchSize + scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        VkAccelerationStructureCreateInfoKHR create{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        create.buffer = t.storage.buffer;
        create.size = sizes.accelerationStructureSize;
        create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(rt.create(ctx.device(), &create, nullptr, &t.as));
    }
    info.dstAccelerationStructure = t.as;
    info.scratchData.deviceAddress = alignUp(t.scratch.address, scratchAlign);
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = written;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    rt.cmdBuild(cb, 1, &info, &ranges);

    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier,
                         0, nullptr, 0, nullptr);
}

}  // namespace iron
