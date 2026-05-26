// VkSkybox.cpp — cube mesh + skybox pipeline. Drawn first inside the
// scene render pass; samples a cubemap and writes z=1.

#include "render/backends/vulkan/VkSkybox.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>

namespace iron {

namespace {

const char* kVert = R"(#version 450
layout(location = 0) in vec3 aPos;

layout(set = 0, binding = 0) uniform SkyUbo {
    mat4 viewProjection;
} u;

layout(location = 0) out vec3 vDir;

void main() {
    vDir = aPos;
    vec4 pos = u.viewProjection * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
)";

const char* kFrag = R"(#version 450
layout(location = 0) in vec3 vDir;
layout(set = 0, binding = 1) uniform samplerCube uSkyCubemap;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(uSkyCubemap, vDir).rgb, 1.0);
}
)";

// 8 unique cube corners, 36 indices for 12 triangles.
constexpr float kCubeVerts[24] = {
    -1.0f, -1.0f, -1.0f,  // 0
     1.0f, -1.0f, -1.0f,  // 1
     1.0f,  1.0f, -1.0f,  // 2
    -1.0f,  1.0f, -1.0f,  // 3
    -1.0f, -1.0f,  1.0f,  // 4
     1.0f, -1.0f,  1.0f,  // 5
     1.0f,  1.0f,  1.0f,  // 6
    -1.0f,  1.0f,  1.0f,  // 7
};
constexpr std::uint32_t kCubeIdx[36] = {
    // -Z face
    0, 2, 1,   0, 3, 2,
    // +Z face
    4, 5, 6,   4, 6, 7,
    // -X face
    0, 4, 7,   0, 7, 3,
    // +X face
    1, 2, 6,   1, 6, 5,
    // -Y face
    0, 1, 5,   0, 5, 4,
    // +Y face
    3, 7, 6,   3, 6, 2,
};

}  // namespace

bool VkSkybox::init(VkContext& ctx, VkRenderPass scenePass) {
    // Upload cube vertex + index buffers to device-local memory via staging.
    auto uploadDeviceLocal = [&](const void* src, VkDeviceSize size,
                                 VkBufferUsageFlags usage,
                                 VkBuffer& outBuffer, VmaAllocation& outAlloc) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo ai{};
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &bi, &ai,
                                 &outBuffer, &outAlloc, nullptr));

        // Staging.
        VkBufferCreateInfo sbi = bi;
        sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo sai{};
        sai.usage = VMA_MEMORY_USAGE_AUTO;
        sai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo sallocInfo{};
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;
        VK_CHECK(vmaCreateBuffer(ctx.allocator(), &sbi, &sai,
                                 &staging, &stagingAlloc, &sallocInfo));
        std::memcpy(sallocInfo.pMappedData, src, size);
        vmaFlushAllocation(ctx.allocator(), stagingAlloc, 0, size);

        // One-shot copy.
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = ctx.graphicsFamily();
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkCommandPool pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateCommandPool(ctx.device(), &pi, nullptr, &pool));
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = pool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &cbi, &cb));

        VkCommandBufferBeginInfo bb{};
        bb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cb, &bb));
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cb, staging, outBuffer, 1, &copy);
        VK_CHECK(vkEndCommandBuffer(cb));

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(ctx.graphicsQueue(), 1, &submit, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(ctx.graphicsQueue()));

        vkDestroyCommandPool(ctx.device(), pool, nullptr);
        vmaDestroyBuffer(ctx.allocator(), staging, stagingAlloc);
    };

    uploadDeviceLocal(kCubeVerts, sizeof(kCubeVerts),
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      vertexBuffer_, vertexAlloc_);
    uploadDeviceLocal(kCubeIdx, sizeof(kCubeIdx),
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      indexBuffer_, indexAlloc_);

    // Shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkSkybox: shader compile failed");
        return false;
    }
    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = fspv.data();
    if (vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx.device(), vsm, nullptr);
        Log::error("VkSkybox: fragment shader module creation failed");
        return false;
    }

    // Descriptor set layout (binding 0 = UBO VS, binding 1 = samplerCube FS).
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 2;
    dslInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &setLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &pipelineLayout_));

    // Pipeline.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm; stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(float) * 3;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{};
    attr.location = 0; attr.binding = 0;
    attr.format = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset = 0;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount = 2;
    pInfo.pStages = stages;
    pInfo.pVertexInputState = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState = &ms;
    pInfo.pDepthStencilState = &ds;
    pInfo.pColorBlendState = &cb;
    pInfo.pDynamicState = &dyn;
    pInfo.layout = pipelineLayout_;
    pInfo.renderPass = scenePass;
    pInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    ok_ = true;
    return true;
}

void VkSkybox::destroy(VkContext& ctx) {
    if (pipeline_)       { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
    if (indexBuffer_)    { vmaDestroyBuffer(ctx.allocator(), indexBuffer_, indexAlloc_); indexBuffer_ = VK_NULL_HANDLE; }
    if (vertexBuffer_)   { vmaDestroyBuffer(ctx.allocator(), vertexBuffer_, vertexAlloc_); vertexBuffer_ = VK_NULL_HANDLE; }
    ok_ = false;
}

void VkSkybox::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                     const VkCubemapResource& cubemap,
                     const Mat4& viewProjection) {
    if (!ok_ || cb == VK_NULL_HANDLE) return;

    SkyUbo ubo;
    std::memcpy(ubo.viewProjection, viewProjection.m, sizeof(ubo.viewProjection));
    const VkDeviceSize uboOffset = frames.allocateUbo(&ubo, sizeof(ubo));

    VkDescriptorSetAllocateInfo dsInfo{};
    dsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsInfo.descriptorPool = frames.current().descriptorPool;
    dsInfo.descriptorSetCount = 1;
    dsInfo.pSetLayouts = &setLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &dsInfo, &set));

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = frames.current().uboBuffer;
    bufInfo.offset = uboOffset;
    bufInfo.range  = sizeof(ubo);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = cubemap.sampler;
    imgInfo.imageView   = cubemap.view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &set, 0, nullptr);
    VkDeviceSize offsets[1] = {0};
    vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer_, offsets);
    vkCmdBindIndexBuffer(cb, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, 36, 1, 0, 0, 0);
}

}  // namespace iron
