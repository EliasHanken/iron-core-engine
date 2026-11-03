#include "render/backends/vulkan/VkClusters.h"

#include "core/Log.h"
#include "math/Transform.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace iron {

namespace {

// std140 / std430 mirrors of what the shaders read.
struct GpuParams {
    Mat4 view;
    float proj[4];    // x = tan of half the horizontal field, y = vertical, z = near, w = far
    float grid[4];    // x, y, z cluster counts; w = 1
    float counts[4];  // x = lights, y = decals, z = log(far / near)
    float screen[4];  // x, y = the scene target's size
};
struct GpuLight {
    float positionRange[4];
    float colorIntensity[4];
    float spot[4];   // xyz = direction it shines; w = cos of the cone's edge, or -2 all round
    float spot2[4];  // x = cos where the edge's fade ends inside the cone
};
struct GpuDecal {
    Mat4  worldToDecal;
    float axis[4];   // xyz = the way surfaces it covers face; w = layer
    float tint[4];   // rgb, opacity
    float glow[4];   // rgb glow; w = roughness, or < 0 to keep the surface's
    float sphere[4]; // bounding sphere, for clustering
    float extra[4];  // x = how flat it lays the surface's normal
};

constexpr std::uint32_t kStride = VkClusters::kLightsPerCluster + VkClusters::kDecalsPerCluster;

const char* const kBuild = R"GLSL(#version 450
layout(local_size_x = 64) in;
layout(std140, set = 0, binding = 0) uniform Params { mat4 view; vec4 proj; vec4 grid; vec4 counts; vec4 screen; } p;
struct Light { vec4 positionRange; vec4 colorIntensity; vec4 spot; vec4 spot2; };
struct Decal { mat4 worldToDecal; vec4 axis; vec4 tint; vec4 glow; vec4 sphere; vec4 extra; };
layout(std430, set = 0, binding = 1) readonly buffer Lights { Light lights[]; };
layout(std430, set = 0, binding = 2) readonly buffer Decals { Decal decals[]; };
layout(std430, set = 0, binding = 3) writeonly buffer Cells { uvec4 cells[]; };
layout(std430, set = 0, binding = 4) writeonly buffer Items { uint items[]; };
const uint LIGHTS = 64u;
const uint DECALS = 32u;
const uint STRIDE = 96u;

float sliceDepth(float k) { return p.proj.z * pow(p.proj.w / p.proj.z, k / p.grid.z); }
bool touches(vec3 c, float r, vec3 lo, vec3 hi) {
    vec3 d = c - clamp(c, lo, hi);
    return dot(d, d) <= r * r;
}

void main() {
    uint X = uint(p.grid.x), Y = uint(p.grid.y), Z = uint(p.grid.z);
    uint id = gl_GlobalInvocationID.x;
    if (id >= X * Y * Z) return;
    uint gx = id % X, gy = (id / X) % Y, gz = id / (X * Y);
    // The cluster's box in view space. Tile rows run down the screen, from NDC +1.
    vec2 n0 = vec2(-1.0 + 2.0 * float(gx) / float(X), 1.0 - 2.0 * float(gy) / float(Y));
    vec2 n1 = vec2(-1.0 + 2.0 * float(gx + 1u) / float(X), 1.0 - 2.0 * float(gy + 1u) / float(Y));
    float depths[2] = float[2](sliceDepth(float(gz)), sliceDepth(float(gz + 1u)));
    vec3 lo = vec3(1e30), hi = vec3(-1e30);
    for (int a = 0; a < 2; ++a) {
        for (int b = 0; b < 4; ++b) {
            vec2 n = vec2((b & 1) == 0 ? n0.x : n1.x, (b & 2) == 0 ? n0.y : n1.y);
            vec3 v = vec3(n.x * p.proj.x * depths[a], n.y * p.proj.y * depths[a], -depths[a]);
            lo = min(lo, v);
            hi = max(hi, v);
        }
    }
    uint base = id * STRIDE;
    uint nl = 0u;
    uint lightCount = uint(p.counts.x);
    for (uint i = 0u; i < lightCount && nl < LIGHTS; ++i) {
        vec3 c = (p.view * vec4(lights[i].positionRange.xyz, 1.0)).xyz;
        if (touches(c, lights[i].positionRange.w, lo, hi)) items[base + nl++] = i;
    }
    uint nd = 0u;
    uint decalCount = uint(p.counts.y);
    for (uint i = 0u; i < decalCount && nd < DECALS; ++i) {
        vec3 c = (p.view * vec4(decals[i].sphere.xyz, 1.0)).xyz;
        if (touches(c, decals[i].sphere.w, lo, hi)) items[base + LIGHTS + nd++] = i;
    }
    cells[id] = uvec4(nl, nd, 0u, 0u);
}
)GLSL";

void oneShot(VkContext& ctx, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandPoolCreateInfo cp{};
    cp.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.queueFamilyIndex = ctx.graphicsFamily();
    cp.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &cp, nullptr, &pool));
    VkCommandBufferAllocateInfo cba{};
    cba.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool        = pool;
    cba.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cba, &cb));
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));
    record(cb);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
}

}  // namespace

VkClusters::Buffer VkClusters::makeBuffer(VkContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                                          bool hostVisible) const {
    Buffer b;
    VkBufferCreateInfo bi{};
    bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size        = size;
    bi.usage       = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    return b;
}

bool VkClusters::init(VkContext& ctx, int framesInFlight) {
    const VkDevice device = ctx.device();

    // Set 1 of lit pipelines: what fragments read.
    {
        VkDescriptorSetLayoutBinding b[10]{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        for (std::uint32_t i = 1; i <= 4; ++i) {
            b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        b[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        // The GI volume (VkDdgi): its parameters and its two atlases.
        b[6] = {6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        b[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        b[8] = {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        b[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 10;
        li.pBindings    = b;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &setLayout_));
    }
    // The build's.
    {
        VkDescriptorSetLayoutBinding b[5]{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (std::uint32_t i = 1; i <= 4; ++i) {
            b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 5;
        li.pBindings    = b;
        VK_CHECK(vkCreateDescriptorSetLayout(device, &li, nullptr, &buildSetLayout_));
    }
    const auto n = static_cast<std::uint32_t>(framesInFlight);
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * n},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 * n},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * n}};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets       = 2 * n;
    pi.poolSizeCount = 3;
    pi.pPoolSizes    = sizes;
    VK_CHECK(vkCreateDescriptorPool(device, &pi, nullptr, &pool_));

    frames_.resize(static_cast<std::size_t>(framesInFlight));
    for (Frame& f : frames_) {
        f.params = makeBuffer(ctx, sizeof(GpuParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        f.lights = makeBuffer(ctx, sizeof(GpuLight) * kMaxLights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f.decals = makeBuffer(ctx, sizeof(GpuDecal) * kMaxDecals, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        f.cells = makeBuffer(ctx, sizeof(std::uint32_t) * 4 * kClusters,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        f.items = makeBuffer(ctx, sizeof(std::uint32_t) * kStride * kClusters, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             false);
        const VkDescriptorSetLayout layouts[2] = {setLayout_, buildSetLayout_};
        VkDescriptorSet sets[2] = {};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = pool_;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts        = layouts;
        VK_CHECK(vkAllocateDescriptorSets(device, &ai, sets));
        f.set = sets[0];
        f.buildSet = sets[1];
        // Until a frame is uploaded, nothing is listed: the cell buffer starts empty.
        GpuParams empty{};
        empty.view = Mat4::identity();
        std::memcpy(f.params.mapped, &empty, sizeof(empty));
    }

    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_LINEAR;
    si.minFilter    = VK_FILTER_LINEAR;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod       = VK_LOD_CLAMP_NONE;
    si.anisotropyEnable = ctx.samplerAnisotropy() ? VK_TRUE : VK_FALSE;
    si.maxAnisotropy    = 8.0f;
    VK_CHECK(vkCreateSampler(device, &si, nullptr, &decalSampler_));
    // A blank layer, so the set is whole before a game loads any decals.
    const std::vector<std::vector<unsigned char>> blank = {std::vector<unsigned char>{255, 255, 255, 0}};
    if (!makeDecalImage(ctx, 1, 1, 1, blank)) return false;
    // The cells must be zero before the first frame reads them.
    oneShot(ctx, [&](VkCommandBuffer cb) {
        for (Frame& f : frames_) vkCmdFillBuffer(cb, f.cells.buffer, 0, VK_WHOLE_SIZE, 0);
    });
    writeSets(ctx);

    VkPipelineLayoutCreateInfo pl{};
    pl.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts    = &buildSetLayout_;
    VK_CHECK(vkCreatePipelineLayout(device, &pl, nullptr, &buildLayout_));
    const auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, kBuild);
    if (spirv.empty()) {
        Log::error("VkClusters: build shader failed to compile");
        return false;
    }
    VkShaderModuleCreateInfo mi{};
    mi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = spirv.size() * sizeof(std::uint32_t);
    mi.pCode    = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &mi, nullptr, &module));
    VkComputePipelineCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName  = "main";
    ci.layout       = buildLayout_;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_));
    vkDestroyShaderModule(device, module, nullptr);
    return true;
}

void VkClusters::setGi(VkContext& ctx, std::span<const VkBuffer> params, VkDeviceSize paramsBytes,
                       VkImageView irradiance, VkImageView distance, VkSampler sampler, VkBuffer offsets) {
    for (std::size_t i = 0; i < frames_.size() && i < params.size(); ++i) {
        const VkDescriptorBufferInfo buffer{params[i], 0, paramsBytes};
        const VkDescriptorImageInfo images[2] = {{sampler, irradiance, VK_IMAGE_LAYOUT_GENERAL},
                                                 {sampler, distance, VK_IMAGE_LAYOUT_GENERAL}};
        const VkDescriptorBufferInfo offsetsInfo{offsets, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[4]{};
        w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[3].dstSet = frames_[i].set;
        w[3].dstBinding = 9;
        w[3].descriptorCount = 1;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[3].pBufferInfo = &offsetsInfo;
        for (std::uint32_t k = 0; k < 3; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = frames_[i].set;
            w[k].dstBinding = 6 + k;
            w[k].descriptorCount = 1;
            w[k].descriptorType = k == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            if (k == 0) w[k].pBufferInfo = &buffer;
            else w[k].pImageInfo = &images[k - 1];
        }
        vkUpdateDescriptorSets(ctx.device(), 4, w, 0, nullptr);
    }
}

void VkClusters::writeSets(VkContext& ctx) {
    for (Frame& f : frames_) {
        const VkDescriptorBufferInfo buffers[5] = {{f.params.buffer, 0, sizeof(GpuParams)},
                                                   {f.lights.buffer, 0, VK_WHOLE_SIZE},
                                                   {f.decals.buffer, 0, VK_WHOLE_SIZE},
                                                   {f.cells.buffer, 0, VK_WHOLE_SIZE},
                                                   {f.items.buffer, 0, VK_WHOLE_SIZE}};
        const VkDescriptorImageInfo image{decalSampler_, decalView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[11]{};
        for (std::uint32_t i = 0; i < 5; ++i) {
            for (int s = 0; s < 2; ++s) {
                VkWriteDescriptorSet& x = w[i * 2 + static_cast<std::uint32_t>(s)];
                x.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                x.dstSet          = s == 0 ? f.set : f.buildSet;
                x.dstBinding      = i;
                x.descriptorCount = 1;
                x.descriptorType  = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                x.pBufferInfo     = &buffers[i];
            }
        }
        w[10].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[10].dstSet          = f.set;
        w[10].dstBinding      = 5;
        w[10].descriptorCount = 1;
        w[10].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[10].pImageInfo      = &image;
        vkUpdateDescriptorSets(ctx.device(), 11, w, 0, nullptr);
    }
}

bool VkClusters::makeDecalImage(VkContext& ctx, std::uint32_t width, std::uint32_t height, std::uint32_t layers,
                                const std::vector<std::vector<unsigned char>>& pixels) {
    std::uint32_t levels = 1;
    for (std::uint32_t s = std::max(width, height); s > 1; s /= 2) ++levels;
    VkImageCreateInfo ii{};
    ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType     = VK_IMAGE_TYPE_2D;
    ii.format        = VK_FORMAT_R8G8B8A8_SRGB;
    ii.extent        = {width, height, 1};
    ii.mipLevels     = levels;
    ii.arrayLayers   = layers;
    ii.samples       = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ii.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    if (vmaCreateImage(ctx.allocator(), &ii, &ai, &image, &alloc, nullptr) != VK_SUCCESS) return false;

    const VkDeviceSize layerBytes = VkDeviceSize{width} * height * 4;
    Buffer staging = makeBuffer(ctx, layerBytes * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    for (std::uint32_t l = 0; l < layers; ++l) {
        std::memcpy(static_cast<unsigned char*>(staging.mapped) + layerBytes * l, pixels[l].data(), layerBytes);
    }
    oneShot(ctx, [&](VkCommandBuffer cb) {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = image;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
        copy.imageExtent      = {width, height, 1};
        vkCmdCopyBufferToImage(cb, staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        // Each level from the one above, every layer at once.
        std::int32_t w = static_cast<std::int32_t>(width), h = static_cast<std::int32_t>(height);
        for (std::uint32_t level = 0; level < levels; ++level) {
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, layers};
            b.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            b.srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
            if (level + 1 < levels) {
                const std::int32_t nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
                blit.srcOffsets[1]  = {w, h, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level + 1, 0, layers};
                blit.dstOffsets[1]  = {nw, nh, 1};
                vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                w = nw;
                h = nh;
            }
        }
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        b.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
    });
    vmaDestroyBuffer(ctx.allocator(), staging.buffer, staging.alloc);

    VkImageViewCreateInfo vi{};
    vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image            = image;
    vi.viewType         = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format           = VK_FORMAT_R8G8B8A8_SRGB;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
    VkImageView view = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &view));

    destroyDecalImage(ctx);
    decalImage_ = image;
    decalAlloc_ = alloc;
    decalView_ = view;
    return true;
}

void VkClusters::destroyDecalImage(VkContext& ctx) {
    if (decalView_) vkDestroyImageView(ctx.device(), decalView_, nullptr);
    if (decalImage_) vmaDestroyImage(ctx.allocator(), decalImage_, decalAlloc_);
    decalView_ = VK_NULL_HANDLE;
    decalImage_ = VK_NULL_HANDLE;
    decalAlloc_ = VK_NULL_HANDLE;
}

bool VkClusters::setDecalTextures(VkContext& ctx, std::span<const std::string> paths) {
    if (paths.empty()) return false;
    std::vector<std::vector<unsigned char>> layers;
    int width = 0, height = 0;
    for (const std::string& path : paths) {
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
        if (!data) {
            Log::error("VkClusters: can't load decal %s", path.c_str());
            return false;
        }
        if (layers.empty()) {
            width = w;
            height = h;
        }
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        if (w == width && h == height) {
            std::memcpy(pixels.data(), data, pixels.size());
        } else {
            // Nearest resample to the first decal's size.
            Log::warn("VkClusters: decal %s is %dx%d, not %dx%d; resampled", path.c_str(), w, h, width, height);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int sx = x * w / width, sy = y * h / height;
                    std::memcpy(&pixels[(static_cast<std::size_t>(y) * width + x) * 4],
                                &data[(static_cast<std::size_t>(sy) * w + sx) * 4], 4);
                }
            }
        }
        stbi_image_free(data);
        layers.push_back(std::move(pixels));
    }
    vkDeviceWaitIdle(ctx.device());
    if (!makeDecalImage(ctx, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                        static_cast<std::uint32_t>(layers.size()), layers)) {
        return false;
    }
    writeSets(ctx);
    return true;
}

void VkClusters::upload(int frame, std::span<const PointLight> lights, std::span<const Decal> decals, const Mat4& view,
                        const Mat4& projection, VkExtent2D screen) {
    Frame& f = frames_[static_cast<std::size_t>(frame)];
    f.lightCount = static_cast<std::uint32_t>(std::min<std::size_t>(lights.size(), kMaxLights));
    f.decalCount = static_cast<std::uint32_t>(std::min<std::size_t>(decals.size(), kMaxDecals));

    auto* gl = static_cast<GpuLight*>(f.lights.mapped);
    for (std::uint32_t i = 0; i < f.lightCount; ++i) {
        const PointLight& l = lights[i];
        GpuLight g{};
        g.positionRange[0] = l.position.x;
        g.positionRange[1] = l.position.y;
        g.positionRange[2] = l.position.z;
        g.positionRange[3] = l.range;
        g.colorIntensity[0] = l.color.x;
        g.colorIntensity[1] = l.color.y;
        g.colorIntensity[2] = l.color.z;
        g.colorIntensity[3] = l.intensity;
        const Vec3 d = normalize(l.direction);
        g.spot[0] = d.x;
        g.spot[1] = d.y;
        g.spot[2] = d.z;
        g.spot[3] = l.spotAngle > 0.0f ? std::cos(l.spotAngle) : -2.0f;
        g.spot2[0] = l.spotAngle > 0.0f ? std::cos(l.spotAngle * (1.0f - std::clamp(l.spotSoftness, 0.0f, 1.0f)))
                                        : -2.0f;
        gl[i] = g;
    }
    auto* gd = static_cast<GpuDecal*>(f.decals.mapped);
    for (std::uint32_t i = 0; i < f.decalCount; ++i) {
        const Decal& d = decals[i];
        GpuDecal g{};
        g.worldToDecal = inverse(d.transform);
        const Vec3 x{d.transform.at(0, 0), d.transform.at(1, 0), d.transform.at(2, 0)};
        const Vec3 y{d.transform.at(0, 1), d.transform.at(1, 1), d.transform.at(2, 1)};
        const Vec3 z{d.transform.at(0, 2), d.transform.at(1, 2), d.transform.at(2, 2)};
        const Vec3 axis = normalize(z);
        g.axis[0] = axis.x;
        g.axis[1] = axis.y;
        g.axis[2] = axis.z;
        g.axis[3] = static_cast<float>(d.layer);
        g.tint[0] = d.tint.x;
        g.tint[1] = d.tint.y;
        g.tint[2] = d.tint.z;
        g.tint[3] = d.opacity;
        g.glow[0] = d.glow.x;
        g.glow[1] = d.glow.y;
        g.glow[2] = d.glow.z;
        g.glow[3] = d.roughness;
        g.sphere[0] = d.transform.at(0, 3);
        g.sphere[1] = d.transform.at(1, 3);
        g.sphere[2] = d.transform.at(2, 3);
        g.sphere[3] = 0.5f * std::sqrt(dot(x, x) + dot(y, y) + dot(z, z));
        g.extra[0] = std::clamp(d.flatten, 0.0f, 1.0f);
        gd[i] = g;
    }

    GpuParams p{};
    p.view = view;
    p.proj[0] = 1.0f / projection.at(0, 0);
    p.proj[1] = 1.0f / projection.at(1, 1);
    p.proj[2] = kNear;
    p.proj[3] = kFar;
    p.grid[0] = static_cast<float>(kGridX);
    p.grid[1] = static_cast<float>(kGridY);
    p.grid[2] = static_cast<float>(kGridZ);
    p.grid[3] = 1.0f;
    p.counts[0] = static_cast<float>(f.lightCount);
    p.counts[1] = static_cast<float>(f.decalCount);
    p.counts[2] = std::log(kFar / kNear);
    p.screen[0] = static_cast<float>(screen.width);
    p.screen[1] = static_cast<float>(screen.height);
    std::memcpy(f.params.mapped, &p, sizeof(p));
}

void VkClusters::record(VkCommandBuffer cb, int frame) {
    if (!pipeline_) return;
    Frame& f = frames_[static_cast<std::size_t>(frame)];
    // Last frame's lit passes in this slot read these lists; the fence has seen them
    // done, but the barrier orders this frame's earlier passes too.
    VkMemoryBarrier before{};
    before.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    before.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &before, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, buildLayout_, 0, 1, &f.buildSet, 0, nullptr);
    vkCmdDispatch(cb, (kClusters + 63) / 64, 1, 1);
    VkMemoryBarrier after{};
    after.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &after,
                         0, nullptr, 0, nullptr);
}

void VkClusters::destroy(VkContext& ctx) {
    const VkDevice device = ctx.device();
    for (Frame& f : frames_) {
        for (Buffer* b : {&f.params, &f.lights, &f.decals, &f.cells, &f.items}) {
            if (b->buffer) vmaDestroyBuffer(ctx.allocator(), b->buffer, b->alloc);
            *b = Buffer{};
        }
    }
    frames_.clear();
    destroyDecalImage(ctx);
    if (decalSampler_) vkDestroySampler(device, decalSampler_, nullptr);
    if (pipeline_) vkDestroyPipeline(device, pipeline_, nullptr);
    if (buildLayout_) vkDestroyPipelineLayout(device, buildLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    if (buildSetLayout_) vkDestroyDescriptorSetLayout(device, buildSetLayout_, nullptr);
    decalSampler_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    buildLayout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    buildSetLayout_ = VK_NULL_HANDLE;
}

}  // namespace iron
