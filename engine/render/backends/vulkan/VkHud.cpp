// VkHud.cpp — Vulkan screen-space HUD pipeline + per-group draws.

#include "render/backends/vulkan/VkHud.h"
#include "render/backends/vulkan/VkContext.h"
#include "render/backends/vulkan/VkFrameRing.h"
#include "render/backends/vulkan/VkShader.h"
#include "render/backends/vulkan/VkTexture.h"
#include "render/backends/vulkan/VkUtils.h"
#include "core/Log.h"

#include <cstring>

namespace iron {

namespace {

const char* kVert = R"(#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(set = 0, binding = 0) uniform ScreenUbo {
    vec4 screenSize;  // x = width, y = height (z/w unused)
} u;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    float ndcX = aPos.x / u.screenSize.x * 2.0 - 1.0;
    // Vulkan clip-Y points down: top-left pixel origin maps to clip Y = -1.
    // Compare OpenGL: ndcY = 1 - aPos.y/h*2. In Vulkan we want the OPPOSITE
    // sign because the Vulkan renderer uses a negative-height viewport (M9)
    // which makes clip-Y behave like OpenGL. So this formula matches GL.
    float ndcY = 1.0 - aPos.y / u.screenSize.y * 2.0;
    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);
}
)";

const char* kFrag = R"(#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1) uniform sampler2D uTex;

void main() {
    outColor = texture(uTex, vUV) * vColor;
}
)";

}  // namespace

bool VkHud::init(VkContext& ctx, VkRenderPass scenePass) {
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkHud: shader compile failed");
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
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm));

    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 2;
    dslInfo.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslInfo, nullptr, &setLayout_));

    VkPipelineLayoutCreateInfo plInfo{};
    plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &plInfo, nullptr, &pipelineLayout_));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm; stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(HudVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = offsetof(HudVertex, position);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset = offsetof(HudVertex, uv);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[2].offset = offsetof(HudVertex, color);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 3;
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
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.colorBlendOp        = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    att.alphaBlendOp        = VK_BLEND_OP_ADD;

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
    pInfo.pVertexInputState   = &vi;
    pInfo.pInputAssemblyState = &ia;
    pInfo.pViewportState      = &vp;
    pInfo.pRasterizationState = &rs;
    pInfo.pMultisampleState   = &ms;
    pInfo.pDepthStencilState  = &ds;
    pInfo.pColorBlendState    = &cb;
    pInfo.pDynamicState       = &dyn;
    pInfo.layout              = pipelineLayout_;
    pInfo.renderPass          = scenePass;
    pInfo.subpass             = 0;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    ok_ = true;
    return true;
}

void VkHud::destroy(VkContext& ctx) {
    if (pipeline_)       { vkDestroyPipeline(ctx.device(), pipeline_, nullptr);              pipeline_       = VK_NULL_HANDLE; }
    if (pipelineLayout_) { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr);  pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr);  setLayout_      = VK_NULL_HANDLE; }
    ok_ = false;
}

void VkHud::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                   VkTextureStore& textures,
                   const HudBatch& batch, int fbW, int fbH) {
    if (!ok_ || cb == VK_NULL_HANDLE || batch.empty()) return;

    ScreenUbo ubo{};
    ubo.screenSize[0] = static_cast<float>(fbW);
    ubo.screenSize[1] = static_cast<float>(fbH);
    ubo.screenSize[2] = 0.0f;
    ubo.screenSize[3] = 0.0f;
    const VkDeviceSize uboOffset = frames.allocateUbo(&ubo, sizeof(ubo));

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    for (const HudDrawGroup& group : batch) {
        if (group.vertices.empty()) continue;

        VkDeviceSize voff = 0;
        VkBuffer vb = frames.allocateVertices(
            group.vertices.data(),
            group.vertices.size() * sizeof(HudVertex),
            voff);
        if (vb == VK_NULL_HANDLE) {
            Log::warn("VkHud: vertex sub-allocator overflow, skipping group");
            continue;
        }

        const VkTextureResource& tex = textures.get(group.texture);

        VkDescriptorSetAllocateInfo daInfo{};
        daInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        daInfo.descriptorPool = frames.current().descriptorPool;
        daInfo.descriptorSetCount = 1;
        daInfo.pSetLayouts = &setLayout_;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VK_CHECK(vkAllocateDescriptorSets(device, &daInfo, &set));

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = frames.current().uboBuffer;
        uboInfo.offset = uboOffset;
        uboInfo.range  = sizeof(ubo);

        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView   = tex.view;
        imgInfo.sampler     = tex.sampler;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &uboInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 0, 1, &set, 0, nullptr);
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &voff);
        vkCmdDraw(cb, static_cast<std::uint32_t>(group.vertices.size()), 1, 0, 0);
    }
}

}  // namespace iron
