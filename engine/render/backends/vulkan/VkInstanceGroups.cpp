#include "render/backends/vulkan/VkInstanceGroups.h"

#include "core/Log.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iron {

namespace {

// Per-instance data on the GPU: the transform, and a bounding sphere in world space.
struct GpuInstance {
    Mat4  model;
    float sphere[4];
};
static_assert(sizeof(GpuInstance) == 80, "std430 layout of Instance in the shaders");

struct GpuCommand {
    std::uint32_t indexCount;
    std::uint32_t instanceCount;
    std::uint32_t firstIndex;
    std::int32_t  vertexOffset;
    std::uint32_t firstInstance;
};
static_assert(sizeof(GpuCommand) == sizeof(VkDrawIndexedIndirectCommand), "matches Vulkan's indirect command");

struct CullUbo {
    float planes[6][4];
    float shadowPlanes[6][4];
    Mat4  pyramidViewProjection;
    float eye[4];      // xyz position, w pixels per unit at a distance of one
    float pyramid[4];  // level 0 width and height, levels, 1 if it holds a frame
};

struct CullPush {
    std::uint32_t count;
    std::uint32_t levels;
    float         minPixels;
    std::uint32_t view;  // 0 the camera, then the sun's views
    float         lodPixels[4];
};

struct PyramidPush {
    std::int32_t sourceSize[2];
    std::int32_t targetSize[2];
    std::int32_t sourceLevel;
};

const char* const kCullShader = R"GLSL(#version 450
layout(local_size_x = 64) in;
struct Instance { mat4 model; vec4 sphere; };
layout(std430, set = 0, binding = 0) readonly buffer Instances { Instance inst[]; };
layout(std430, set = 0, binding = 1) writeonly buffer Visible { uint visible[]; };
struct Command { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; };
layout(std430, set = 0, binding = 2) buffer Commands { Command commands[]; };
layout(std430, set = 0, binding = 3) buffer Stats { uint stats[]; };
layout(std140, set = 0, binding = 4) uniform Cull {
    vec4 planes[6];
    vec4 shadowPlanes[6];
    mat4 pyramidViewProjection;
    vec4 eye;      // xyz position, w pixels per unit at a distance of one
    vec4 pyramid;  // level 0 size, levels, 1 if it holds a frame
} c;
layout(set = 0, binding = 5) uniform sampler2D uPyramid;
layout(push_constant) uniform Push { uint count; uint levels; float minPixels; uint view; vec4 lodPixels; } pc;

// Behind what was drawn last frame, from where the camera was then.
bool occluded(vec3 center, float radius) {
    if (c.pyramid.w < 0.5) return false;
    vec2 lo = vec2(1e9);
    vec2 hi = vec2(-1e9);
    float nearest = 1e9;
    for (int i = 0; i < 8; ++i) {
        vec3 corner = center + radius * vec3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0,
                                             (i & 4) != 0 ? 1.0 : -1.0);
        vec4 clip = c.pyramidViewProjection * vec4(corner, 1.0);
        if (clip.w <= 1e-4) return false;  // Reaches behind that camera.
        vec3 ndc = clip.xyz / clip.w;
        // The scene is drawn with y flipped: clip y points up the screen, uv y down.
        vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        lo = min(lo, uv);
        hi = max(hi, uv);
        nearest = min(nearest, ndc.z);
    }
    if (nearest <= 0.0) return false;  // Nearer than the near plane.
    // Not all of it was on screen then, so nothing can say it was hidden.
    if (lo.x < 0.0 || lo.y < 0.0 || hi.x > 1.0 || hi.y > 1.0) return false;
    // Pick the level where the rectangle is at most a texel across, so the four
    // texels around it cover it, and hold the farthest depth drawn anywhere inside.
    vec2 size = (hi - lo) * c.pyramid.xy;
    int level = int(clamp(ceil(log2(max(max(size.x, size.y), 1.0))), 0.0, c.pyramid.z - 1.0));
    ivec2 dims = textureSize(uPyramid, level);
    ivec2 a = clamp(ivec2(lo * vec2(dims)), ivec2(0), dims - 1);
    ivec2 b = clamp(ivec2(hi * vec2(dims)), ivec2(0), dims - 1);
    float farthest = max(max(texelFetch(uPyramid, a, level).r, texelFetch(uPyramid, ivec2(b.x, a.y), level).r),
                         max(texelFetch(uPyramid, ivec2(a.x, b.y), level).r, texelFetch(uPyramid, b, level).r));
    return nearest > farthest;
}

void main() {
    uint id = gl_GlobalInvocationID.x;
    if (id >= pc.count) return;
    bool sun = pc.view != 0u;
    if (id == 0 && !sun) atomicAdd(stats[8], pc.count);
    vec4 s = inst[id].sphere;
    for (int i = 0; i < 6; ++i) {
        vec4 plane = sun ? c.shadowPlanes[i] : c.planes[i];
        if (dot(plane.xyz, s.xyz) + plane.w < -s.w) {
            if (!sun) atomicAdd(stats[1], 1u);
            return;
        }
    }
    // Size on the camera's screen picks the level, for its shadow too.
    float pixels = 2.0 * s.w * c.eye.w / max(distance(s.xyz, c.eye.xyz), 1e-3);
    if (pixels < (sun ? max(pc.minPixels, 4.0) : pc.minPixels)) {
        if (!sun) atomicAdd(stats[3], 1u);
        return;
    }
    if (!sun && occluded(s.xyz, s.w)) {
        atomicAdd(stats[2], 1u);
        return;
    }
    uint lod = 0u;
    while (lod + 1u < pc.levels && pixels < pc.lodPixels[lod]) ++lod;
    if (sun) lod = min(lod + 1u, pc.levels - 1u);  // A shadow needs less detail.
    uint slot = atomicAdd(commands[lod].instanceCount, 1u);
    visible[lod * pc.count + slot] = id;
    if (!sun) {
        atomicAdd(stats[0], 1u);
        atomicAdd(stats[4 + lod], 1u);
    }
}
)GLSL";

// Each texel the farthest of the four below it, or along an odd edge, of six or nine.
const char* const kPyramidShader = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D uSource;
layout(set = 0, binding = 1, r32f) uniform writeonly image2D uTarget;
layout(push_constant) uniform Push { ivec2 sourceSize; ivec2 targetSize; int sourceLevel; } pc;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= pc.targetSize.x || p.y >= pc.targetSize.y) return;
    ivec2 base = p * 2;
    ivec2 reach = ivec2(2) + ivec2(p.x == pc.targetSize.x - 1 ? (pc.sourceSize.x & 1) : 0,
                                   p.y == pc.targetSize.y - 1 ? (pc.sourceSize.y & 1) : 0);
    float d = 0.0;
    for (int y = 0; y < reach.y; ++y) {
        for (int x = 0; x < reach.x; ++x) {
            ivec2 at = min(base + ivec2(x, y), pc.sourceSize - 1);
            d = max(d, texelFetch(uSource, at, pc.sourceLevel).r);
        }
    }
    imageStore(uTarget, p, vec4(d));
}
)GLSL";

::VkPipeline makeComputePipeline(VkDevice device, const char* source, VkPipelineLayout layout) {
    const auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, source);
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi{};
    mi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = spirv.size() * sizeof(std::uint32_t);
    mi.pCode    = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
    VkComputePipelineCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName  = "main";
    info.layout       = layout;
    ::VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline));
    vkDestroyShaderModule(device, module, nullptr);
    return pipeline;
}

Vec3 point(const Mat4& m, Vec3 p) {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {r.x, r.y, r.z};
}

}  // namespace

bool VkInstanceGroups::init(VkContext& ctx, VkFrameRing& frames) {
    ctx_ = &ctx;
    frames_ = &frames;
    for (auto& b : statBuffers_) {
        b = makeBuffer(ctx, kStatWords * sizeof(std::uint32_t),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod       = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(ctx.device(), &si, nullptr, &nearestSampler_));
    return createPipelines(ctx);
}

bool VkInstanceGroups::createPipelines(VkContext& ctx) {
    const VkDevice device = ctx.device();
    {
        VkDescriptorSetLayoutBinding b[6]{};
        for (std::uint32_t i = 0; i < 6; ++i) {
            b[i].binding         = i;
            b[i].descriptorCount = 1;
            b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType  = i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                 : i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                          : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 6;
        li.pBindings    = b;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &cullSetLayout_));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush)};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount         = 1;
        pl.pSetLayouts            = &cullSetLayout_;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges    = &push;
        VK_CHECK(vkCreatePipelineLayout(device, &pl, nullptr, &cullLayout_));
        cullPipeline_ = makeComputePipeline(device, kCullShader, cullLayout_);
    }
    {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings    = b;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &pyramidSetLayout_));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PyramidPush)};
        VkPipelineLayoutCreateInfo pl{};
        pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount         = 1;
        pl.pSetLayouts            = &pyramidSetLayout_;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges    = &push;
        VK_CHECK(vkCreatePipelineLayout(device, &pl, nullptr, &pyramidLayout_));
        pyramidPipeline_ = makeComputePipeline(device, kPyramidShader, pyramidLayout_);
    }
    if (!cullPipeline_ || !pyramidPipeline_) {
        Log::error("VkInstanceGroups: compute shaders failed to compile");
        return false;
    }
    return true;
}

VkInstanceGroups::Buffer VkInstanceGroups::makeBuffer(VkContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                                                      bool hostVisible) {
    Buffer b;
    b.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = b.size;
    bi.usage       = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) {
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    return b;
}

void VkInstanceGroups::retire(Buffer& b) {
    if (b.buffer) retired_.push_back({b, frameCount_});
    b = Buffer{};
}

void VkInstanceGroups::destroy(VkContext& ctx) {
    const auto free = [&](Buffer& b) {
        if (b.buffer) vmaDestroyBuffer(ctx.allocator(), b.buffer, b.alloc);
        b = Buffer{};
    };
    for (Group& g : groups_) {
        free(g.instances);
        for (auto& view : g.visible) for (auto& b : view) free(b);
        for (auto& view : g.indirect) for (auto& b : view) free(b);
    }
    groups_.clear();
    for (Retired& r : retired_) free(r.buffer);
    retired_.clear();
    for (auto& b : statBuffers_) free(b);
    destroyPyramid(ctx);
    const VkDevice device = ctx.device();
    if (cullPipeline_) vkDestroyPipeline(device, cullPipeline_, nullptr);
    if (pyramidPipeline_) vkDestroyPipeline(device, pyramidPipeline_, nullptr);
    if (cullLayout_) vkDestroyPipelineLayout(device, cullLayout_, nullptr);
    if (pyramidLayout_) vkDestroyPipelineLayout(device, pyramidLayout_, nullptr);
    if (cullSetLayout_) vkDestroyDescriptorSetLayout(device, cullSetLayout_, nullptr);
    if (pyramidSetLayout_) vkDestroyDescriptorSetLayout(device, pyramidSetLayout_, nullptr);
    if (nearestSampler_) vkDestroySampler(device, nearestSampler_, nullptr);
    cullPipeline_ = pyramidPipeline_ = VK_NULL_HANDLE;
    cullLayout_ = pyramidLayout_ = VK_NULL_HANDLE;
    cullSetLayout_ = pyramidSetLayout_ = VK_NULL_HANDLE;
    nearestSampler_ = VK_NULL_HANDLE;
}

InstanceGroupHandle VkInstanceGroups::create(VkContext& ctx, const InstanceGroupDesc& desc,
                                             const VkMeshStore& meshes) {
    if (desc.lods.empty() || desc.lods.size() > 4) {
        Log::error("VkInstanceGroups: a group needs one to four levels of detail");
        return kInvalidInstanceGroup;
    }
    Group g;
    g.desc = desc;
    for (std::size_t i = 0; i < desc.lods.size(); ++i) {
        if (!meshes.has(desc.lods[i])) {
            Log::error("VkInstanceGroups: level %zu's mesh doesn't exist", i);
            return kInvalidInstanceGroup;
        }
        g.indexCounts[i] = meshes.get(desc.lods[i]).indexCount;
    }
    g.center = (desc.bounds.min + desc.bounds.max) * 0.5f;
    g.radius = length(desc.bounds.max - desc.bounds.min) * 0.5f;
    for (auto& view : g.indirect) {
        for (auto& b : view) {
            b = makeBuffer(ctx, sizeof(GpuCommand) * 4,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           false);
        }
    }
    groups_.push_back(std::move(g));
    return static_cast<InstanceGroupHandle>(groups_.size());
}

void VkInstanceGroups::forEachOccluder(const std::function<void(MeshHandle, const Mat4&, const Material&)>& visit) const {
    for (const Group& g : groups_) {
        if (!g.desc.castShadows || g.count == 0 || !g.instances.mapped || g.desc.lods.empty()) continue;
        const auto* in = static_cast<const GpuInstance*>(g.instances.mapped);
        for (std::uint32_t i = 0; i < g.count; ++i) visit(g.desc.lods.front(), in[i].model, g.desc.material);
    }
}

void VkInstanceGroups::setInstances(VkContext& ctx, InstanceGroupHandle handle, std::span<const Mat4> transforms) {
    if (handle == kInvalidInstanceGroup || handle > groups_.size()) return;
    Group& g = groups_[handle - 1];
    ++generation_;
    // The old buffers may still be read by a frame in flight, so they're retired, not freed.
    retire(g.instances);
    for (auto& view : g.visible) for (auto& b : view) retire(b);
    g.count = static_cast<std::uint32_t>(transforms.size());
    if (g.count == 0) return;

    g.instances = makeBuffer(ctx, sizeof(GpuInstance) * g.count, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    auto* out = static_cast<GpuInstance*>(g.instances.mapped);
    for (std::uint32_t i = 0; i < g.count; ++i) {
        const Mat4& m = transforms[i];
        const Vec3 c = point(m, g.center);
        // The largest axis scale, so that the sphere still holds the mesh.
        const float sx = length(Vec3{m.at(0, 0), m.at(1, 0), m.at(2, 0)});
        const float sy = length(Vec3{m.at(0, 1), m.at(1, 1), m.at(2, 1)});
        const float sz = length(Vec3{m.at(0, 2), m.at(1, 2), m.at(2, 2)});
        out[i].model = m;
        out[i].sphere[0] = c.x;
        out[i].sphere[1] = c.y;
        out[i].sphere[2] = c.z;
        out[i].sphere[3] = g.radius * std::max(sx, std::max(sy, sz));
    }
    vmaFlushAllocation(ctx.allocator(), g.instances.alloc, 0, VK_WHOLE_SIZE);
    const VkDeviceSize visibleBytes = sizeof(std::uint32_t) * g.count * g.desc.lods.size();
    for (auto& view : g.visible) {
        for (auto& b : view) b = makeBuffer(ctx, visibleBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    }
}

void VkInstanceGroups::cull(VkCommandBuffer cb, int frame, const Mat4& viewProjection,
                            std::span<const Mat4> lightViews, Vec3 eye, float pixelsPerUnit,
                            VkExtent2D sceneExtent, std::uint32_t shadowMask, bool occlusion) {
    ++frameCount_;
    // Free what no frame in flight can still be reading.
    std::erase_if(retired_, [&](Retired& r) {
        if (frameCount_ - r.frame <= VkFrameRing::kFramesInFlight + 1) return false;
        vmaDestroyBuffer(ctx_->allocator(), r.buffer.buffer, r.buffer.alloc);
        return true;
    });

    const auto slot = static_cast<std::size_t>(frame);
    // This slot's counts from last time: its fence has signalled.
    if (statsWritten_[slot]) {
        vmaInvalidateAllocation(ctx_->allocator(), statBuffers_[slot].alloc, 0, VK_WHOLE_SIZE);
        const auto* w = static_cast<const std::uint32_t*>(statBuffers_[slot].mapped);
        stats_.drawn = w[0];
        stats_.outsideView = w[1];
        stats_.hidden = w[2];
        stats_.tiny = w[3];
        for (int i = 0; i < 4; ++i) stats_.perLod[static_cast<std::size_t>(i)] = w[4 + i];
        stats_.instances = w[8];
    }
    if (groups_.empty() || !cullPipeline_) return;

    // The pyramid is (re)made here, before anything this frame refers to it. Culling
    // reads it even before it holds a frame, so it must exist and be in its layout.
    if (pyramid_ == VK_NULL_HANDLE || sceneExtent.width != pyramidSource_.width ||
        sceneExtent.height != pyramidSource_.height) {
        createPyramid(*ctx_, sceneExtent);
    }
    if (!pyramidLayoutReady_) {
        VkImageMemoryBarrier init{};
        init.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        init.dstAccessMask               = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        init.oldLayout                   = VK_IMAGE_LAYOUT_UNDEFINED;
        init.newLayout                   = VK_IMAGE_LAYOUT_GENERAL;
        init.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        init.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        init.image                       = pyramid_;
        init.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        init.subresourceRange.levelCount = static_cast<std::uint32_t>(pyramidLevels_.size());
        init.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &init);
        pyramidLayoutReady_ = true;
    }

    // Freezing keeps the camera culling sees, and stops the pyramid moving with it.
    if (frozenRequest_ && !frozen_) {
        frozenViewProjection_ = viewProjection;
        frozenEye_ = eye;
    }
    frozen_ = frozenRequest_;
    const Mat4& cullViewProjection = frozen_ ? frozenViewProjection_ : viewProjection;
    const Vec3 cullEye = frozen_ ? frozenEye_ : eye;

    // Reset this frame's counters and draw commands.
    vkCmdFillBuffer(cb, statBuffers_[slot].buffer, 0, VK_WHOLE_SIZE, 0);
    for (Group& g : groups_) {
        GpuCommand commands[4]{};
        for (std::size_t l = 0; l < g.desc.lods.size(); ++l) {
            commands[l].indexCount = g.indexCounts[l];
            commands[l].firstInstance = static_cast<std::uint32_t>(l) * g.count;
        }
        for (auto& view : g.indirect) vkCmdUpdateBuffer(cb, view[slot].buffer, 0, sizeof(commands), commands);
    }
    {
        // The resets, and last frame's pyramid, before culling reads them.
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    CullUbo ubo{};
    const Frustum frustum = frustumFromViewProjection(cullViewProjection);
    for (std::size_t i = 0; i < 6; ++i) {
        ubo.planes[i][0] = frustum[i].x;
        ubo.planes[i][1] = frustum[i].y;
        ubo.planes[i][2] = frustum[i].z;
        ubo.planes[i][3] = frustum[i].w;
    }
    ubo.pyramidViewProjection = pyramidViewProjection_;
    ubo.eye[0] = cullEye.x;
    ubo.eye[1] = cullEye.y;
    ubo.eye[2] = cullEye.z;
    ubo.eye[3] = pixelsPerUnit;
    ubo.pyramid[0] = static_cast<float>(pyramidExtent_.width);
    ubo.pyramid[1] = static_cast<float>(pyramidExtent_.height);
    ubo.pyramid[2] = static_cast<float>(pyramidLevels_.size());
    occlusion_ = occlusion;
    if (!occlusion) pyramidValid_ = false;  // Not reused when occlusion comes back: it would be stale.
    ubo.pyramid[3] = pyramidValid_ ? 1.0f : 0.0f;
    // One copy per view, each with that shadow view's planes. A light's matrix gives
    // depth in 0..1 already, so its near plane is clip z = 0 too.
    const std::size_t views = 1 + std::min(lightViews.size(), kShadowViews);
    std::array<VkDeviceSize, kViews> uboOffsets{};
    for (std::size_t view = 0; view < views; ++view) {
        if (view > 0) {
            const Frustum sun = frustumFromViewProjection(lightViews[view - 1]);
            for (std::size_t i = 0; i < 6; ++i) {
                ubo.shadowPlanes[i][0] = sun[i].x;
                ubo.shadowPlanes[i][1] = sun[i].y;
                ubo.shadowPlanes[i][2] = sun[i].z;
                ubo.shadowPlanes[i][3] = sun[i].w;
            }
        }
        uboOffsets[view] = frames_->allocateUbo(&ubo, sizeof(ubo));
    }

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    for (std::size_t view = 0; view < views; ++view)
    for (Group& g : groups_) {
        if (g.count == 0 || (view > 0 && !g.desc.castShadows)) continue;
        if (view > 0 && (shadowMask & (1u << (view - 1))) == 0) continue;
        const VkDeviceSize uboOffset = uboOffsets[view];
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = frames_->current().descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &cullSetLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(ctx_->device(), &ai, &set));

        const VkDescriptorBufferInfo buffers[5] = {
            {g.instances.buffer, 0, VK_WHOLE_SIZE},
            {g.visible[view][slot].buffer, 0, VK_WHOLE_SIZE},
            {g.indirect[view][slot].buffer, 0, VK_WHOLE_SIZE},
            {statBuffers_[slot].buffer, 0, VK_WHOLE_SIZE},
            {frames_->current().uboBuffer, uboOffset, sizeof(CullUbo)},
        };
        // Before the pyramid holds a frame the shader doesn't read it, but it must be bound.
        const VkDescriptorImageInfo image{nearestSampler_, pyramidView_, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[6]{};
        for (std::uint32_t i = 0; i < 6; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = set;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = i < 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                      : i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (i < 5) writes[i].pBufferInfo = &buffers[i];
            else       writes[i].pImageInfo = &image;
        }
        vkUpdateDescriptorSets(ctx_->device(), 6, writes, 0, nullptr);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cullLayout_, 0, 1, &set, 0, nullptr);

        CullPush push{};
        push.count = g.count;
        push.levels = static_cast<std::uint32_t>(g.desc.lods.size());
        push.minPixels = g.desc.minPixels;
        push.view = static_cast<std::uint32_t>(view);
        for (int i = 0; i < 3; ++i) push.lodPixels[i] = g.desc.lodPixels[static_cast<std::size_t>(i)];
        vkCmdPushConstants(cb, cullLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cb, (g.count + 63) / 64, 1, 1);
    }

    // The draws read the counts and lists; the CPU reads the stats once the fence signals.
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    statsWritten_[slot] = true;
    lastViewProjection_ = viewProjection;
}

bool VkInstanceGroups::createPyramid(VkContext& ctx, VkExtent2D source) {
    destroyPyramid(ctx);
    pyramidSource_ = source;
    // Halving rounds down, as Vulkan's own mip sizes do; an odd edge's last texel takes
    // the extra row or column (see the pyramid shader).
    pyramidExtent_ = {std::max(source.width / 2, 1u), std::max(source.height / 2, 1u)};
    std::uint32_t levels = 1;
    for (std::uint32_t s = std::max(pyramidExtent_.width, pyramidExtent_.height); s > 1; s /= 2) ++levels;

    VkImageCreateInfo ii{};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = VK_FORMAT_R32_SFLOAT;
    ii.extent        = {pyramidExtent_.width, pyramidExtent_.height, 1};
    ii.mipLevels     = levels;
    ii.arrayLayers   = 1;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &ii, &ai, &pyramid_, &pyramidAlloc_, nullptr));

    VkImageViewCreateInfo vi{};
    vi.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image                       = pyramid_;
    vi.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    vi.format                      = VK_FORMAT_R32_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = levels;
    vi.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &pyramidView_));
    pyramidLevels_.resize(levels);
    for (std::uint32_t l = 0; l < levels; ++l) {
        vi.subresourceRange.baseMipLevel = l;
        vi.subresourceRange.levelCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &pyramidLevels_[l]));
    }
    pyramidValid_ = false;
    pyramidLayoutReady_ = false;
    return true;
}

void VkInstanceGroups::destroyPyramid(VkContext& ctx) {
    if (pyramid_ == VK_NULL_HANDLE) return;
    // A frame in flight may still be reading it: this only happens on a resize.
    vkDeviceWaitIdle(ctx.device());
    for (VkImageView v : pyramidLevels_) vkDestroyImageView(ctx.device(), v, nullptr);
    pyramidLevels_.clear();
    if (pyramidView_) vkDestroyImageView(ctx.device(), pyramidView_, nullptr);
    vmaDestroyImage(ctx.allocator(), pyramid_, pyramidAlloc_);
    pyramid_ = VK_NULL_HANDLE;
    pyramidAlloc_ = VK_NULL_HANDLE;
    pyramidView_ = VK_NULL_HANDLE;
    pyramidValid_ = false;
}

void VkInstanceGroups::buildDepthPyramid(VkContext& ctx, VkCommandBuffer cb, VkImageView sceneDepth) {
    // Culling made the pyramid at the scene's size; without that, there's nothing to fill.
    if (groups_.empty() || !pyramidPipeline_ || frozen_ || !occlusion_ || pyramid_ == VK_NULL_HANDLE) return;
    const VkExtent2D extent = pyramidSource_;

    // Culling earlier this frame read the pyramid; wait for that before writing.
    VkImageMemoryBarrier toWrite{};
    toWrite.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toWrite.srcAccessMask               = VK_ACCESS_SHADER_READ_BIT;
    toWrite.dstAccessMask               = VK_ACCESS_SHADER_WRITE_BIT;
    toWrite.oldLayout                   = VK_IMAGE_LAYOUT_GENERAL;
    toWrite.newLayout                   = VK_IMAGE_LAYOUT_GENERAL;
    toWrite.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    toWrite.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
    toWrite.image                       = pyramid_;
    toWrite.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toWrite.subresourceRange.levelCount = static_cast<std::uint32_t>(pyramidLevels_.size());
    toWrite.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &toWrite);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pyramidPipeline_);
    VkExtent2D sourceSize = extent;
    for (std::size_t level = 0; level < pyramidLevels_.size(); ++level) {
        const VkExtent2D target{std::max(sourceSize.width / 2, 1u), std::max(sourceSize.height / 2, 1u)};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = frames_->current().descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &pyramidSetLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &ai, &set));
        const VkDescriptorImageInfo source = level == 0
            ? VkDescriptorImageInfo{nearestSampler_, sceneDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}
            : VkDescriptorImageInfo{nearestSampler_, pyramidView_, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo targetInfo{VK_NULL_HANDLE, pyramidLevels_[level], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &source;
        writes[1]                 = writes[0];
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo      = &targetInfo;
        vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pyramidLayout_, 0, 1, &set, 0, nullptr);

        PyramidPush push{};
        push.sourceSize[0] = static_cast<std::int32_t>(sourceSize.width);
        push.sourceSize[1] = static_cast<std::int32_t>(sourceSize.height);
        push.targetSize[0] = static_cast<std::int32_t>(target.width);
        push.targetSize[1] = static_cast<std::int32_t>(target.height);
        push.sourceLevel = level == 0 ? 0 : static_cast<std::int32_t>(level - 1);
        vkCmdPushConstants(cb, pyramidLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cb, (target.width + 7) / 8, (target.height + 7) / 8, 1);

        // Each level is read by the next.
        VkImageMemoryBarrier written = toWrite;
        written.srcAccessMask                 = VK_ACCESS_SHADER_WRITE_BIT;
        written.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;
        written.oldLayout                     = VK_IMAGE_LAYOUT_GENERAL;
        written.subresourceRange.baseMipLevel = static_cast<std::uint32_t>(level);
        written.subresourceRange.levelCount   = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &written);
        sourceSize = target;
    }
    pyramidViewProjection_ = lastViewProjection_;
    pyramidValid_ = true;
}

void VkInstanceGroups::forEachDraw(int frame, const VkMeshStore& meshes,
                                   const std::function<void(const Draw&)>& fn) const {
    const auto slot = static_cast<std::size_t>(frame);
    for (const Group& g : groups_) {
        if (g.count == 0) continue;
        for (std::size_t l = 0; l < g.desc.lods.size(); ++l) {
            const auto& mesh = meshes.get(g.desc.lods[l]);
            fn(Draw{&g.desc, static_cast<std::uint32_t>(l), mesh.vertexBuffer, mesh.indexBuffer, g.instances.buffer,
                    g.visible[0][slot].buffer, g.indirect[0][slot].buffer, sizeof(GpuCommand) * l});
        }
    }
}

std::vector<VkInstanceGroups::Draw> VkInstanceGroups::shadowDraws(int frame, const VkMeshStore& meshes,
                                                                   std::size_t shadowView) const {
    const auto slot = static_cast<std::size_t>(frame);
    const std::size_t view = 1 + std::min(shadowView, kShadowViews - 1);
    std::vector<Draw> out;
    for (const Group& g : groups_) {
        if (g.count == 0 || !g.desc.castShadows) continue;
        for (std::size_t l = 0; l < g.desc.lods.size(); ++l) {
            const auto& mesh = meshes.get(g.desc.lods[l]);
            out.push_back(Draw{&g.desc, static_cast<std::uint32_t>(l), mesh.vertexBuffer, mesh.indexBuffer,
                               g.instances.buffer, g.visible[view][slot].buffer, g.indirect[view][slot].buffer,
                               sizeof(GpuCommand) * l});
        }
    }
    return out;
}

}  // namespace iron
