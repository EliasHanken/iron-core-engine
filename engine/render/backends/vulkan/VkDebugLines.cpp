// VkDebugLines.cpp — Vulkan line-list pipeline + per-frame queue/record.

#include "render/backends/vulkan/VkDebugLines.h"
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
layout(location = 1) in vec3 aColor;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProjection;
} u;

layout(location = 0) out vec3 vColor;

void main() {
    vColor = aColor;
    gl_Position = u.viewProjection * vec4(aPos, 1.0);
}
)";

const char* kFrag = R"(#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor, 1.0);
}
)";

// Fragment shader for the translucent filled-triangle overlay (gizmo plane /
// center handles). Low constant alpha so the flats read as a tint, not a wall.
const char* kFragTri = R"(#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor, 0.30);
}
)";

}  // namespace

bool VkDebugLines::init(VkContext& ctx, VkRenderPass scenePass) {
    // Compile shaders.
    auto vspv = compileGlsl(VK_SHADER_STAGE_VERTEX_BIT, kVert);
    auto fspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kFrag);
    if (vspv.empty() || fspv.empty()) {
        Log::error("VkDebugLines: shader compile failed");
        return false;
    }
    VkShaderModule vsm, fsm;
    VkShaderModuleCreateInfo smInfo{};
    smInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smInfo.codeSize = vspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = vspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &vsm));
    smInfo.codeSize = fspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = fspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &fsm));

    // Descriptor set layout: binding 0 UBO (Mat4 viewProjection), vertex stage.
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

    // Pipeline state.
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = offsetof(Vertex, position);
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;  // ignored — lineWidth is dynamic (set per draw group)

    // Thick gizmo lines use this width when the device supports wideLines;
    // otherwise everything stays 1px (the only legal width without the feature).
    thickWidth_ = ctx.wideLines() ? 3.0f : 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    att.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &att;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                   VK_DYNAMIC_STATE_LINE_WIDTH };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 3;
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

    // Overlay pipeline: identical state but depth-test disabled, so gizmo lines
    // draw on top of geometry. `ds` and `pInfo` are reused with one field flipped.
    ds.depthTestEnable = VK_FALSE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &pInfo, nullptr, &overlayPipeline_));

    // Triangle overlay pipeline: filled, alpha-blended, depth-off (ds already
    // off) — for the translucent gizmo plane / center handles. Reuses the vertex
    // shader + layout; swaps to TRIANGLE_LIST + the low-alpha fragment shader.
    auto tspv = compileGlsl(VK_SHADER_STAGE_FRAGMENT_BIT, kFragTri);
    if (tspv.empty()) { Log::error("VkDebugLines: tri shader compile failed"); return false; }
    VkShaderModule tfsm = VK_NULL_HANDLE;
    smInfo.codeSize = tspv.size() * sizeof(std::uint32_t);
    smInfo.pCode = tspv.data();
    VK_CHECK(vkCreateShaderModule(ctx.device(), &smInfo, nullptr, &tfsm));

    VkPipelineShaderStageCreateInfo triStages[2] = { stages[0], stages[1] };
    triStages[1].module = tfsm;

    VkPipelineInputAssemblyStateCreateInfo triIa{};
    triIa.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    triIa.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineColorBlendAttachmentState triAtt{};
    triAtt.colorWriteMask = 0xF;
    triAtt.blendEnable = VK_TRUE;
    triAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    triAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    triAtt.colorBlendOp = VK_BLEND_OP_ADD;
    triAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    triAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    triAtt.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo triCb{};
    triCb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    triCb.attachmentCount = 1;
    triCb.pAttachments = &triAtt;

    VkGraphicsPipelineCreateInfo triInfo = pInfo;  // copy: shares vi/vp/rs/ms/ds/dyn/layout/pass
    triInfo.pStages = triStages;
    triInfo.pInputAssemblyState = &triIa;
    triInfo.pColorBlendState = &triCb;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &triInfo, nullptr, &triPipeline_));

    vkDestroyShaderModule(ctx.device(), vsm, nullptr);
    vkDestroyShaderModule(ctx.device(), fsm, nullptr);
    vkDestroyShaderModule(ctx.device(), tfsm, nullptr);
    ok_ = true;
    return true;
}

void VkDebugLines::destroy(VkContext& ctx) {
    if (pipeline_)        { vkDestroyPipeline(ctx.device(), pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (triPipeline_)     { vkDestroyPipeline(ctx.device(), triPipeline_, nullptr); triPipeline_ = VK_NULL_HANDLE; }
    if (overlayPipeline_) { vkDestroyPipeline(ctx.device(), overlayPipeline_, nullptr); overlayPipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_)  { vkDestroyPipelineLayout(ctx.device(), pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (setLayout_)      { vkDestroyDescriptorSetLayout(ctx.device(), setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
    ok_ = false;
}

void VkDebugLines::queue(Vec3 a, Vec3 b, Vec3 color) {
    queued_.push_back({a, color});
    queued_.push_back({b, color});
}

void VkDebugLines::queueOverlay(Vec3 a, Vec3 b, Vec3 color) {
    queuedOverlay_.push_back({a, color});
    queuedOverlay_.push_back({b, color});
}

void VkDebugLines::queueOverlayThick(Vec3 a, Vec3 b, Vec3 color) {
    queuedOverlayThick_.push_back({a, color});
    queuedOverlayThick_.push_back({b, color});
}

void VkDebugLines::queueTri(Vec3 a, Vec3 b, Vec3 c, Vec3 color) {
    queuedTris_.push_back({a, color});
    queuedTris_.push_back({b, color});
    queuedTris_.push_back({c, color});
}

void VkDebugLines::record(VkCommandBuffer cb, VkDevice device, VkFrameRing& frames,
                          const Mat4& view, const Mat4& projection) {
    if (!ok_ || cb == VK_NULL_HANDLE ||
        (queued_.empty() && queuedOverlay_.empty() && queuedOverlayThick_.empty() &&
         queuedTris_.empty())) {
        queued_.clear();
        queuedOverlay_.clear();
        queuedOverlayThick_.clear();
        queuedTris_.clear();
        return;
    }

    // One UBO + descriptor set shared by both passes (same view-projection).
    CameraUbo ubo;
    const Mat4 vp = projection * view;
    std::memcpy(ubo.viewProjection, vp.m, sizeof(float) * 16);
    const VkDeviceSize uboOffset = frames.allocateUbo(&ubo, sizeof(ubo));

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
    uboInfo.range = sizeof(ubo);

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &uboInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &set, 0, nullptr);

    // Draw one queue with the given pipeline + line width. Allocates its own
    // vertex range. lineWidth is dynamic state (the pipeline ignores its baked
    // value), so each group can pick its own thickness.
    auto drawQueue = [&](std::vector<Vertex>& q, ::VkPipeline pipe, float lineWidth) {
        if (q.empty()) return;
        VkDeviceSize voff = 0;
        VkBuffer vb = frames.allocateVertices(q.data(), q.size() * sizeof(Vertex), voff);
        if (vb == VK_NULL_HANDLE) {
            Log::warn("VkDebugLines: vertex sub-allocator overflow, skipping a queue");
            return;
        }
        vkCmdSetLineWidth(cb, lineWidth);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindVertexBuffers(cb, 0, 1, &vb, &voff);
        vkCmdDraw(cb, static_cast<std::uint32_t>(q.size()), 1, 0, 0);
    };

    drawQueue(queuedTris_, triPipeline_, 1.0f);                // translucent fills (under the lines)
    drawQueue(queued_, pipeline_, 1.0f);                       // depth-tested, thin
    drawQueue(queuedOverlay_, overlayPipeline_, 1.0f);         // always-on-top, thin (outlines)
    drawQueue(queuedOverlayThick_, overlayPipeline_, thickWidth_);  // always-on-top, thick (gizmo)

    queued_.clear();
    queuedOverlay_.clear();
    queuedOverlayThick_.clear();
    queuedTris_.clear();
}

}  // namespace iron
