// VkIblBaker.cpp — equirectangular .hdr -> RGBA16F cubemap via a compute
// pass. The shared IBL bake foundation (M46a); irradiance/prefilter passes
// (M46b/c) build on the same compute-to-cubemap pattern.

#include "render/backends/vulkan/VkIblBaker.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkCubemap.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <stb_image.h>

#include <cstring>
#include <vector>

namespace iron {

const char* kEquirectToCubeComputeSrc() {
    return R"(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D uEquirect;
// A cubemap is written through a 2D-array view (one layer per face); imageCube
// is for sampling, not storing. Hence image2DArray here, gid.z = face index.
layout(binding = 1, rgba16f) uniform writeonly image2DArray uOut;

const float kPi = 3.14159265358979323846;

// Cube-face direction, matching ProceduralSky / Ibl.h face convention.
vec3 faceDir(int face, float u, float v) {
    vec3 d;
    if      (face == 0) d = vec3( 1.0, -v,   -u);   // +X
    else if (face == 1) d = vec3(-1.0, -v,    u);   // -X
    else if (face == 2) d = vec3( u,    1.0,  v);   // +Y
    else if (face == 3) d = vec3( u,   -1.0, -v);   // -Y
    else if (face == 4) d = vec3( u,   -v,    1.0); // +Z
    else                d = vec3(-u,   -v,   -1.0); // -Z
    return normalize(d);
}

void main() {
    ivec3 gid = ivec3(gl_GlobalInvocationID);
    ivec2 size = imageSize(uOut).xy;
    if (gid.x >= size.x || gid.y >= size.y) return;

    float u = 2.0 * (float(gid.x) + 0.5) / float(size.x) - 1.0;
    float v = 2.0 * (float(gid.y) + 0.5) / float(size.y) - 1.0;
    vec3 dir = faceDir(gid.z, u, v);

    vec2 uv = vec2(atan(dir.z, dir.x) / (2.0 * kPi) + 0.5,
                   asin(clamp(dir.y, -1.0, 1.0)) / kPi + 0.5);

    vec3 color = textureLod(uEquirect, uv, 0.0).rgb;
    imageStore(uOut, gid, vec4(color, 1.0));
}
)";
}

bool VkIblBaker::init(VkContext& ctx) {
    // Descriptor layout: equirect sampler (0) + cube storage image (1).
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding         = 0;
    b[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding         = 1;
    b[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo slInfo{};
    slInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slInfo.bindingCount = 2;
    slInfo.pBindings    = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &slInfo, nullptr, &setLayout_));

    auto spirv = compileGlsl(VK_SHADER_STAGE_COMPUTE_BIT, kEquirectToCubeComputeSrc());
    if (spirv.empty()) {
        Log::error("VkIblBaker: equirect->cube compute compile failed");
        return false;
    }
    if (!pipeline_.init(ctx, spirv, setLayout_)) return false;

    // Nearest sampler for the equirect source (avoids a filterable-format
    // requirement on the temp R32G32B32A32_SFLOAT image).
    VkSamplerCreateInfo sInfo{};
    sInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sInfo.magFilter    = VK_FILTER_NEAREST;
    sInfo.minFilter    = VK_FILTER_NEAREST;
    sInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;        // yaw wraps
    sInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // pitch clamps
    sInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device(), &sInfo, nullptr, &equirectSampler_));
    return true;
}

void VkIblBaker::destroy(VkContext& ctx) {
    pipeline_.destroy(ctx);
    if (equirectSampler_) {
        vkDestroySampler(ctx.device(), equirectSampler_, nullptr);
        equirectSampler_ = VK_NULL_HANDLE;
    }
    if (setLayout_) {
        vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
}

CubemapHandle VkIblBaker::equirectFileToCubemap(
        VkContext& ctx, VkCubemapStore& store,
        const std::string& hdrPath, int faceSize) {
    // 1. Load the equirect .hdr as 4-channel float. No vertical flip: equirect
    //    convention has +Y at the top, which our v-mapping expects.
    stbi_set_flip_vertically_on_load(0);
    int w = 0, h = 0, ch = 0;
    float* pixels = stbi_loadf(hdrPath.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        Log::error("VkIblBaker: failed to load HDR '%s'", hdrPath.c_str());
        return kInvalidHandle;
    }
    // Equirectangular maps are 2:1. A non-2:1 source still bakes but will look
    // distorted; warn rather than fail since this is a setup-time convenience.
    if (w != 2 * h) {
        Log::warn("VkIblBaker: '%s' is %dx%d, not 2:1 equirectangular; "
                  "cubemap may look distorted", hdrPath.c_str(), w, h);
    }
    const VkDeviceSize srcBytes =
        static_cast<VkDeviceSize>(w) * h * 4 * sizeof(float);

    // 2. Temp equirect image (R32G32B32A32_SFLOAT, sampled).
    VkImage       eqImg   = VK_NULL_HANDLE;
    VmaAllocation eqAlloc = VK_NULL_HANDLE;
    VkImageView   eqView  = VK_NULL_HANDLE;
    {
        VkImageCreateInfo ii{};
        ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType     = VK_IMAGE_TYPE_2D;
        ii.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
        ii.extent        = {static_cast<std::uint32_t>(w),
                            static_cast<std::uint32_t>(h), 1};
        ii.mipLevels     = 1;
        ii.arrayLayers   = 1;
        ii.samples       = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ii.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateImage(ctx.allocator(), &ii, &ai, &eqImg, &eqAlloc, nullptr));

        VkImageViewCreateInfo vi{};
        vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image    = eqImg;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &eqView));
    }

    // 3. Staging buffer for the equirect float data.
    VkBuffer      staging      = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = srcBytes;
        bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai, &staging, &stagingAlloc, &info));
        std::memcpy(info.pMappedData, pixels, static_cast<std::size_t>(srcBytes));
        vmaFlushAllocation(ctx.allocator(), stagingAlloc, 0, srcBytes);
    }
    stbi_image_free(pixels);

    // 4. Allocate the destination cube (RGBA16F, 1 mip).
    const CubemapHandle handle = store.createHdr(ctx, faceSize, /*mipLevels=*/1);
    if (handle == kInvalidHandle) {
        vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);
        vkDestroyImageView(ctx.device(), eqView, nullptr);
        vmaDestroyImage(ctx.allocator(), eqImg, eqAlloc);
        return kInvalidHandle;
    }
    const VkCubemapResource& cube = store.get(handle);

    // 5. One-shot command buffer: upload equirect, transition layouts,
    //    dispatch, transition cube to shader-read.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pInfo{};
    pInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pInfo.queueFamilyIndex = ctx.graphicsFamily();
    pInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VK_CHECK(vkCreateCommandPool(ctx.device(), &pInfo, nullptr, &pool));

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbInfo{};
    cbInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbInfo.commandPool        = pool;
    cbInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbInfo, &cb));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));

    auto barrier = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS,
                       std::uint32_t layers) {
        VkImageMemoryBarrier mb{};
        mb.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mb.oldLayout           = oldL;
        mb.newLayout           = newL;
        mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mb.image               = img;
        mb.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        mb.srcAccessMask       = srcA;
        mb.dstAccessMask       = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &mb);
    };

    // Equirect: UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ.
    barrier(eqImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent      = {static_cast<std::uint32_t>(w),
                            static_cast<std::uint32_t>(h), 1};
    vkCmdCopyBufferToImage(cb, staging, eqImg,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(eqImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);

    // Cube: UNDEFINED -> GENERAL (for imageStore), all 6 layers.
    barrier(cube.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 6);

    // One-shot descriptor pool/set.
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = 1;
    sizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.maxSets       = 1;
    dpInfo.poolSizeCount = 2;
    dpInfo.pPoolSizes    = sizes;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpInfo, nullptr, &dpool));

    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool     = dpool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts        = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsInfo, &set));

    VkDescriptorImageInfo eqInfo{};
    eqInfo.sampler     = equirectSampler_;
    eqInfo.imageView   = eqView;
    eqInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo cubeInfo{};
    cubeInfo.imageView   = cube.storageViews[0];
    cubeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &eqInfo;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &cubeInfo;
    vkUpdateDescriptorSets(ctx.device(), 2, writes, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.pipeline());
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_.pipelineLayout(), 0, 1, &set, 0, nullptr);
    const std::uint32_t groups = (static_cast<std::uint32_t>(faceSize) + 7u) / 8u;
    vkCmdDispatch(cb, groups, groups, 6);

    // Cube: GENERAL -> SHADER_READ_ONLY for the lit/skybox passes.
    barrier(cube.image, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 6);

    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

    // Cleanup transient resources.
    vkDestroyDescriptorPool(ctx.device(), dpool, nullptr);
    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    vkDestroyImageView(ctx.device(), eqView, nullptr);
    vmaDestroyImage(ctx.allocator(), eqImg, eqAlloc);
    vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);

    return handle;
}

}  // namespace iron
