#pragma once

// The scene as the GPU's ray-tracing hardware sees it: a bottom-level
// acceleration structure (BLAS) per mesh, built once, and a top-level one (TLAS)
// over every placed copy, rebuilt each frame so what moves is where it is. Ray
// queries in compute shaders trace against the TLAS (dynamic GI's probes, ray-
// traced shadows).
//
// What goes in: this frame's draws (but not glowing ones: a light doesn't block
// light) and every instance group that casts shadows. Skinned meshes don't yet.
// Only made when VkContext::rayTracing().

#include "render/Handles.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "math/Mat4.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iron {

class VkContext;
class VkMeshStore;
struct DrawCall;

class VkRayTracing {
public:
    // One thing in the TLAS: a mesh, placed. `id` comes back to a shader as the
    // instance's custom index (to look up what it's made of).
    struct Placement {
        MeshHandle    mesh;
        Mat4          transform;
        std::uint32_t id = 0;
    };

    bool init(VkContext& ctx);
    void destroy(VkContext& ctx);

    // Builds any BLAS still missing, then frame `frameIndex`'s TLAS over
    // `statics` (placed rarely: written again only when `staticGeneration`
    // changes) and `dynamics` (this frame's). Records into `cb`, ending with a
    // barrier that makes the TLAS readable by compute and fragment shaders.
    void build(VkContext& ctx, VkCommandBuffer cb, std::uint32_t frameIndex, const VkMeshStore& meshes,
               std::span<const Placement> statics, std::uint64_t staticGeneration, std::span<const Placement> dynamics);

    // The TLAS built for this frame, or null if nothing has been.
    VkAccelerationStructureKHR scene(std::uint32_t frameIndex) const { return tlas_[frameIndex].as; }
    std::uint32_t instances(std::uint32_t frameIndex) const { return tlas_[frameIndex].count; }
    std::size_t blasCount() const { return blas_.size(); }

private:
    struct Buffer {
        VkBuffer        buffer = VK_NULL_HANDLE;
        VmaAllocation   alloc = VK_NULL_HANDLE;
        void*           mapped = nullptr;
        VkDeviceSize    size = 0;
        VkDeviceAddress address = 0;
    };
    struct Blas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        Buffer                     storage;
        VkDeviceAddress            address = 0;
        VkBuffer                   source = VK_NULL_HANDLE;  // The vertex buffer it was built from.
    };
    struct Tlas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        Buffer                     storage;
        Buffer                     instances;  // Host-written VkAccelerationStructureInstanceKHR.
        Buffer                     scratch;
        std::uint32_t              capacity = 0;
        std::uint32_t              count = 0;
        std::uint64_t              staticGeneration = 0;  // Of the statics written at its front.
        std::uint32_t              staticCount = 0;
    };

    Buffer makeBuffer(VkContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void   free(VkContext& ctx, Buffer& b);
    // Frees what an earlier frame may still have been reading, once it can't be.
    void   retire(VkContext& ctx, Buffer& b);
    void   retire(VkContext& ctx, VkAccelerationStructureKHR& as);
    void   collect(VkContext& ctx);

    std::unordered_map<MeshHandle, Blas>             blas_;
    std::array<Tlas, VkFrameRing::kFramesInFlight>   tlas_{};
    struct Retired {
        Buffer                     buffer;
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        std::uint64_t              frame = 0;
    };
    std::vector<Retired> retired_;
    std::uint64_t        frameCount_ = 0;
};

}  // namespace iron
