// VkShadowMap.cpp — depth-only shadow map for the directional sun.

#include "render/backends/vulkan/VkShadowMap.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkMesh.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>

namespace iron {

namespace {

const char* kVert = R"(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;

layout(set = 0, binding = 0) uniform LightUbo {
    mat4 lightModelViewProj;
} u;

void main() {
    gl_Position = u.lightModelViewProj * vec4(aPos, 1.0);
}
)";

}  // namespace

bool VkShadowMap::init(VkContext& ctx) {
    // --- Depth image ---
    VkImageCreateInfo iInfo{};
    iInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iInfo.imageType = VK_IMAGE_TYPE_2D;
    iInfo.format = VK_FORMAT_D32_SFLOAT;
    iInfo.extent = {kResolution, kResolution, 1};
    iInfo.mipLevels = 1;
    iInfo.arrayLayers = 1;
    iInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    iInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    iInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
    iInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    iInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aInfo{};
    aInfo.usage = VMA_MEMORY_USAGE_AUTO;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &iInfo, &aInfo,
                            &depthImage_, &depthAlloc_, nullptr));

    VkImageViewCreateInfo vInfo{};
    vInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vInfo.image = depthImage_;
    vInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vInfo.format = VK_FORMAT_D32_SFLOAT;
    vInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vInfo.subresourceRange.levelCount = 1;
    vInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx.device(), &vInfo, nullptr, &depthView_));

    // --- Render pass: depth-only, final layout = SHADER_READ_ONLY_OPTIMAL ---
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &depthAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;
    VK_CHECK(vkCreateRenderPass(ctx.device(), &rpInfo, nullptr, &renderPass_));

    // --- Framebuffer ---
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass_;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &depthView_;
    fbInfo.width = kResolution;
    fbInfo.height = kResolution;
    fbInfo.layers = 1;
    VK_CHECK(vkCreateFramebuffer(ctx.device(), &fbInfo, nullptr, &framebuffer_));

    // --- Descriptor set layout (binding 0 = LightUbo, vertex stage only) ---
    VkDescriptorSetLayoutBinding b0{};
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 1;
    dslInfo.pBindings = &b0;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &setLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &pipelineLayout_));

    // --- Depth-only pipeline ---
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kVert);
    if (vspv.empty()) {
        Log::error("VkShadowMap: vertex shader compile failed");
        return false;
    }
    VkShaderModule vsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vsm;
    stage.pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = offsetof(Vertex, position);
    attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[1].offset = offsetof(Vertex, normal);
    attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32G32_SFLOAT;    attrs[2].offset = offsetof(Vertex, uv);
    attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[3].offset = offsetof(Vertex, tangent);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;       // back-face cull reduces peter-panning
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 0;  // depth-only — no color attachments

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pInfo{};
    pInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pInfo.stageCount = 1;
    pInfo.pStages = &stage;
    pInfo.pVertexInputState = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState = &ms;
    pInfo.pDepthStencilState = &ds;
    pInfo.pColorBlendState = &cb;
    pInfo.pDynamicState = &dyn;
    pInfo.layout = pipelineLayout_;
    pInfo.renderPass = renderPass_;
    pInfo.subpass = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx.device(), vsm, nullptr);

    // --- Sampler: NEAREST, CLAMP_TO_BORDER, white border (matches GL) ---
    VkSamplerCreateInfo sInfo{};
    sInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sInfo.magFilter = VK_FILTER_NEAREST;
    sInfo.minFilter = VK_FILTER_NEAREST;
    sInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sInfo.minLod = 0.0f;
    sInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(ctx.device(), &sInfo, nullptr, &sampler_));

    ok_ = true;
    return true;
}

void VkShadowMap::destroy(VkContext& ctx) {
    if (sampler_)        { vkDestroySampler(ctx.device(), sampler_, nullptr); sampler_ = VK_NULL_HANDLE; }
    if (pipeline_)       { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
    if (framebuffer_)    { vkDestroyFramebuffer(ctx.device(), framebuffer_, nullptr); framebuffer_ = VK_NULL_HANDLE; }
    if (renderPass_)     { vkDestroyRenderPass(ctx.device(), renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    if (depthView_)      { vkDestroyImageView(ctx.device(), depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_)     { vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_); depthImage_ = VK_NULL_HANDLE; }
    ok_ = false;
}

void VkShadowMap::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                         VkMeshStore& meshes,
                         const Mat4& lightViewProj,
                         const std::vector<DrawCall>& draws) {
    if (!ok_ || cb == VK_NULL_HANDLE) return;

    // Begin depth-only render pass.
    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass_;
    rpBegin.framebuffer = framebuffer_;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {kResolution, kResolution};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(kResolution);
    vp.height = static_cast<float>(kResolution);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, {kResolution, kResolution}};
    vkCmdSetScissor(cb, 0, 1, &scissor);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    for (const DrawCall& call : draws) {
        if (!meshes.has(call.mesh)) continue;

        // lightModelViewProj = lightViewProj * model.
        const Mat4 lmvp = lightViewProj * call.model;
        LightUbo ubo;
        std::memcpy(ubo.lightModelViewProj, lmvp.m, sizeof(ubo.lightModelViewProj));
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

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 0, 1, &set, 0, nullptr);

        const auto& mesh = meshes.get(call.mesh);
        VkDeviceSize offsets[1] = {0};
        vkCmdBindVertexBuffers(cb, 0, 1, &mesh.vertexBuffer, offsets);
        vkCmdBindIndexBuffer(cb, mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, mesh.indexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cb);
}

}  // namespace iron
